#pragma once

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <deque>
#include <memory>
#include <thread>
#include <vector>

#include <asio/ip/tcp.hpp>

namespace devilution {

/** A loopback stream relay: delays and fragments bytes without pretending byte loss is TCP packet loss. */
class TcpTestProxy {
public:
	TcpTestProxy(uint16_t target, uint32_t seed)
	    : acceptor_(context_, { asio::ip::address_v4::loopback(), 0 })
	    , target_(asio::ip::address_v4::loopback(), target)
	    , seed_(seed)
	{
		acceptor_.non_blocking(true);
		port_ = acceptor_.local_endpoint().port();
		thread_ = std::thread([this] { Run(); });
	}
	~TcpTestProxy()
	{
		running_ = false;
		thread_.join();
	}
	uint16_t port() const { return port_; }
	void Pause(std::chrono::milliseconds duration)
	{
		pausedUntil_ = Milliseconds() + duration.count();
	}
	void Disconnect() { disconnect_ = true; }
	uint64_t forwardedBytes() const { return forwarded_; }

private:
	using Clock = std::chrono::steady_clock;
	struct Chunk {
		std::vector<unsigned char> data;
		size_t offset = 0;
		int64_t ready;
	};
	struct Direction {
		std::deque<Chunk> queue;
		size_t queued = 0;
		uint32_t sequence = 0;
		bool eof = false;
		bool finished = false;
	};
	struct Connection {
		asio::ip::tcp::socket client;
		asio::ip::tcp::socket server;
		Direction upstream;
		Direction downstream;
		bool closed = false;
		Connection(asio::io_context &context, uint32_t seed)
		    : client(context)
		    , server(context)
		{
			upstream.sequence = seed;
			downstream.sequence = seed ^ 0x9E3779B9U;
		}
		void Close()
		{
			closed = true;
			asio::error_code ignored;
			client.close(ignored);
			server.close(ignored);
		}
	};
	static int64_t Milliseconds()
	{
		return std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now().time_since_epoch()).count();
	}
	static bool Retry(asio::error_code error)
	{
		return error == asio::error::would_block || error == asio::error::try_again;
	}
	void Relay(asio::ip::tcp::socket &source, asio::ip::tcp::socket &destination, Direction &direction)
	{
		std::array<unsigned char, 251> bytes;
		constexpr std::array<size_t, 4> Sizes { 1, 7, 31, 251 };
		asio::error_code error;
		if (!direction.eof && direction.queued < 1024 * 1024) {
			const auto count = source.read_some(asio::buffer(bytes.data(), Sizes[direction.sequence % Sizes.size()]), error);
			if (error && !Retry(error))
				direction.eof = true;
			if (count != 0) {
				const int64_t ready = Milliseconds() + 10 + (direction.sequence++ * 17U) % 21U;
				direction.queue.push_back({ { bytes.begin(), bytes.begin() + count }, 0, direction.queue.empty() ? ready : std::max(ready, direction.queue.back().ready) });
				direction.queued += count;
			}
		}
		if (direction.queue.empty()) {
			// EOF is a half-close. Forward queued bytes before propagating its FIN.
			if (direction.eof && !direction.finished) {
				asio::error_code ignored;
				destination.shutdown(asio::ip::tcp::socket::shutdown_send, ignored);
				direction.finished = true;
			}
			return;
		}
		if (direction.queue.front().ready > Milliseconds())
			return;
		auto &chunk = direction.queue.front();
		const auto written = destination.write_some(asio::buffer(chunk.data.data() + chunk.offset, chunk.data.size() - chunk.offset), error);
		if (error && !Retry(error)) {
			direction.eof = true;
			direction.queue.clear();
			direction.queued = 0;
			return;
		}
		forwarded_ += written;
		direction.queued -= written;
		chunk.offset += written;
		if (chunk.offset == chunk.data.size())
			direction.queue.pop_front();
	}
	void Run()
	{
		while (running_) {
			auto connection = std::make_unique<Connection>(context_, seed_);
			asio::error_code error;
			acceptor_.accept(connection->client, error);
			if (!error) {
				connection->server.connect(target_, error);
				if (!error) {
					connection->client.non_blocking(true);
					connection->server.non_blocking(true);
					connections_.push_back(std::move(connection));
				}
			}
			if (disconnect_.exchange(false)) {
				for (auto &existing : connections_)
					existing->Close();
			}
			if (Milliseconds() >= pausedUntil_) {
				for (auto &existing : connections_) {
					if (!existing->closed)
						Relay(existing->client, existing->server, existing->upstream);
					if (!existing->closed)
						Relay(existing->server, existing->client, existing->downstream);
					if (existing->upstream.finished && existing->downstream.finished)
						existing->Close();
				}
			}
			std::erase_if(connections_, [](const auto &existing) { return existing->closed; });
			std::this_thread::sleep_for(std::chrono::milliseconds(1));
		}
	}
	asio::io_context context_;
	asio::ip::tcp::acceptor acceptor_;
	asio::ip::tcp::endpoint target_;
	std::vector<std::unique_ptr<Connection>> connections_;
	uint16_t port_;
	uint32_t seed_;
	std::atomic<bool> running_ { true };
	std::atomic<bool> disconnect_ { false };
	std::atomic<int64_t> pausedUntil_ { 0 };
	std::atomic<uint64_t> forwarded_ { 0 };
	std::thread thread_;
};

} // namespace devilution
