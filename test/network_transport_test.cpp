#include <array>
#include <chrono>
#include <cstring>
#include <iostream>
#include <map>
#include <string_view>
#include <thread>

#include <gtest/gtest.h>

#include "dvlnet/base.h"
#include "dvlnet/frame_queue.h"
#include "dvlnet/limits.hpp"
#include "dvlnet/loopback.h"
#include "dvlnet/send_queue.hpp"
#include "dvlnet/session.hpp"
#if !defined(NONET) && !defined(DISABLE_TCP)
#include "dvlnet/tcp_client.h"
#include "dvlnet/tcp_server.h"
#endif
#include "player.h"
#include "storm/storm_net.hpp"

namespace devilution::net {
namespace {

GameData LocalGame(bool extended)
{
	GameData data {};
	data.size = sizeof(data);
	data.nTickRate = 20;
	data.rules = extended ? ExtendedSessionRules : ClassicSessionRules;
	return data;
}

buffer_t GameInfo(bool extended)
{
	GameData data = LocalGame(extended);
	SwapGameDataLE(data);
	const auto *bytes = reinterpret_cast<const unsigned char *>(&data);
	return { bytes, bytes + sizeof(data) };
}

TEST(NetworkFrames, PartialAndCoalescedFramesRetainOrder)
{
	const buffer_t first { 1, 2, 3 };
	const buffer_t second { 4, 5 };
	const auto a = frame_queue::MakeFrame(first);
	const auto b = frame_queue::MakeFrame(second, TcpErrorCodeFlags);
	ASSERT_TRUE(a.has_value());
	ASSERT_TRUE(b.has_value());
	buffer_t stream = *a;
	stream.insert(stream.end(), b->begin(), b->end());
	for (size_t split = 0; split <= a->size(); ++split) {
		frame_queue queue;
		ASSERT_TRUE(queue.Write(buffer_t(stream.begin(), stream.begin() + split)));
		if (split < a->size()) {
			EXPECT_EQ(queue.PacketReady(), false);
		}
		ASSERT_TRUE(queue.Write(buffer_t(stream.begin() + split, stream.end())));
		ASSERT_EQ(queue.PacketReady(), true);
		EXPECT_EQ(queue.ReadPacketFlags(), 0);
		EXPECT_EQ(*queue.ReadPacket(), first);
		ASSERT_EQ(queue.PacketReady(), true);
		EXPECT_EQ(queue.ReadPacketFlags(), TcpErrorCodeFlags);
		EXPECT_EQ(*queue.ReadPacket(), second);
		EXPECT_EQ(queue.PacketReady(), false);
	}
}

TEST(NetworkFrames, RejectsCorruptHeadersAndBoundsQueues)
{
	for (buffer_t header : { buffer_t { 0, 0, 0, 0 }, buffer_t { 0, 0, 0, 128 }, buffer_t { 1, 0, 1, 0 } }) {
		frame_queue queue;
		ASSERT_TRUE(queue.Write(header));
		EXPECT_FALSE(queue.PacketReady().has_value());
	}
	EXPECT_FALSE(frame_queue::MakeFrame({}).has_value());
	EXPECT_FALSE(frame_queue::MakeFrame(buffer_t(frame_queue::max_frame_size + 1)).has_value());
	EXPECT_FALSE(frame_queue::MakeFrame({ 1 }, 1).has_value());
	EXPECT_TRUE(frame_queue::MakeFrame(buffer_t(frame_queue::max_frame_size)).has_value());
	frame_queue byteLimited;
	ASSERT_TRUE(byteLimited.Write(buffer_t(MaxQueuedBytes)));
	EXPECT_FALSE(byteLimited.Write({ 1 }));
	frame_queue countLimited;
	ASSERT_TRUE(countLimited.Write({}));
	for (size_t i = 0; i < MaxQueuedPackets; ++i)
		ASSERT_TRUE(countLimited.Write({ 1 }));
	EXPECT_FALSE(countLimited.Write({ 1 }));
}

TEST(NetworkFrames, FixedPacketsRejectTrailingPayload)
{
	packet_factory factory;
	auto turn = factory.make_packet<PT_TURN>(plr_t { 0 }, PLR_BROADCAST, turn_t { 0, 1 });
	ASSERT_TRUE(turn.has_value());
	buffer_t bytes = (*turn)->Data();
	bytes.push_back(99);
	EXPECT_FALSE(factory.make_packet(bytes).has_value());
}

TEST(NetworkQueues, BoundsFramesAndKeepsOutstandingWritesAlive)
{
	send_queue queue;
	ASSERT_TRUE(queue.Push({ 1 }));
	const auto active = queue.Front();
	queue.Clear();
	EXPECT_EQ(*active, buffer_t { 1 });
	ASSERT_TRUE(queue.Push(buffer_t(MaxQueuedBytes)));
	EXPECT_FALSE(queue.Push({ 2 }));
	queue.Pop();
	for (size_t i = 0; i < MaxQueuedPackets; ++i)
		ASSERT_TRUE(queue.Push({ static_cast<unsigned char>(i) }));
	EXPECT_FALSE(queue.Push({ 1 }));
	for (size_t i = 0; i < MaxQueuedPackets; ++i) {
		ASSERT_FALSE(queue.Empty());
		EXPECT_EQ((*queue.Front())[0], static_cast<unsigned char>(i));
		queue.Pop();
	}
	EXPECT_TRUE(queue.Empty());
	EXPECT_TRUE(queue.Push({ 9 }));
}

class local_net : public base {
public:
	int create(std::string_view) override { return -1; }
	int join(std::string_view) override { return -1; }
	std::string make_default_gamename() override { return {}; }
	std::expected<void, PacketError> poll() override { return {}; }
	std::expected<void, PacketError> send(packet &) override { return {}; }
	bool IsGameHost() override { return true; }
	void Prepare(bool extended)
	{
		clear_password();
		setup_gameinfo(GameInfo(extended));
		plr_self = 0;
	}
	std::expected<void, PacketError> Receive(packet &pkt) { return RecvLocal(pkt); }
	std::expected<void, PacketError> Add(plr_t player) { return Connect(player); }
	bool Connected(plr_t player) const { return IsConnected(player); }
	void Reset(plr_t player) { ResetPlayerState(player); }
};

TEST(NetworkBase, CapacityAndArrayBoundsDoNotDependOnPlayers)
{
	Players.clear();
	for (bool extended : { false, true }) {
		local_net net;
		net.Prepare(extended);
		const unsigned capacity = extended ? 8 : 4;
		_SNETCAPS caps {};
		net.SNetGetProviderCaps(&caps);
		EXPECT_EQ(caps.maxplayers, capacity);
		for (unsigned player = 0; player <= 255; ++player)
			EXPECT_EQ(net.Add(static_cast<plr_t>(player)).has_value(), player < capacity);
		std::array<char *, 9> messages {};
		std::array<size_t, 9> sizes;
		std::array<uint32_t, 9> status;
		sizes.fill(123);
		status.fill(456);
		EXPECT_FALSE(net.SNetReceiveTurns(messages.data(), sizes.data(), status.data()));
		for (size_t i = capacity; i < sizes.size(); ++i) {
			EXPECT_EQ(sizes[i], 123);
			EXPECT_EQ(status[i], 456);
		}
		EXPECT_FALSE(net.SNetDropPlayer(-1, leaveinfo_t::LEAVE_DROP));
		EXPECT_FALSE(net.SNetDropPlayer(capacity, leaveinfo_t::LEAVE_DROP));
		EXPECT_FALSE(net.SNetSendMessage(capacity, nullptr, 0));
		EXPECT_FALSE(net.SNetSendTurn(nullptr, 4));
		EXPECT_FALSE(net.SNetSendMessage(0, nullptr, 1));
		net.get_latencies(255);
	}
}

TEST(NetworkBase, InvalidControlIDsAndSourcesCannotIndexTables)
{
	local_net net;
	net.Prepare(true);
	packet_factory factory;
	for (unsigned player = 8; player <= 255; ++player) {
		auto connect = factory.make_packet<PT_CONNECT>(PLR_MASTER, PLR_BROADCAST, static_cast<plr_t>(player));
		ASSERT_TRUE(connect.has_value());
		EXPECT_FALSE(net.Receive(**connect));
		auto disconnect = factory.make_packet<PT_DISCONNECT>(PLR_MASTER, PLR_BROADCAST, static_cast<plr_t>(player), leaveinfo_t::LEAVE_DROP);
		ASSERT_TRUE(disconnect.has_value());
		EXPECT_FALSE(net.Receive(**disconnect));
		auto turn = factory.make_packet<PT_TURN>(static_cast<plr_t>(player), PLR_BROADCAST, turn_t { 0, 1 });
		ASSERT_TRUE(turn.has_value());
		EXPECT_FALSE(net.Receive(**turn));
	}
}

TEST(NetworkBase, SaturationAndReusedSlotsDiscardOldMessagesAndTurns)
{
	local_net net;
	net.Prepare(true);
	packet_factory factory;
	auto message = factory.make_packet<PT_MESSAGE>(plr_t { 7 }, plr_t { 0 }, buffer_t { 42 });
	ASSERT_TRUE(message.has_value());
	for (size_t i = 0; i < MaxQueuedPackets; ++i)
		ASSERT_TRUE(net.Receive(**message));
	EXPECT_FALSE(net.Receive(**message));
	auto turn = factory.make_packet<PT_TURN>(plr_t { 7 }, PLR_BROADCAST, turn_t { 0, 99 });
	ASSERT_TRUE(turn.has_value());
	for (size_t i = 0; i < MaxQueuedTurns; ++i)
		ASSERT_TRUE(net.Receive(**turn));
	EXPECT_FALSE(net.Receive(**turn));
	net.Reset(7);
	EXPECT_FALSE(net.Connected(7));
	uint8_t sender;
	void *data;
	size_t size;
	EXPECT_FALSE(net.SNetReceiveMessage(&sender, &data, &size));
	ASSERT_TRUE(net.Add(7));
	uint32_t waiting = 999;
	ASSERT_TRUE(net.SNetGetOwnerTurnsWaiting(&waiting));
	EXPECT_EQ(waiting, 0);
	ASSERT_TRUE(net.Receive(**message));
	ASSERT_TRUE(net.SNetReceiveMessage(&sender, &data, &size));
	EXPECT_EQ(sender, 7);
	EXPECT_EQ(size, 1);
}

TEST(NetworkLoopback, ExtendedBufferCapacityKeepsSingleParticipantSemantics)
{
	loopback net;
	net.setup_gameinfo(GameInfo(true));
	ASSERT_EQ(net.create(""), 0);
	_SNETCAPS caps {};
	net.SNetGetProviderCaps(&caps);
	EXPECT_EQ(caps.maxplayers, 8);
	char value = 42;
	ASSERT_TRUE(net.SNetSendMessage(0, &value, 1));
	ASSERT_TRUE(net.SNetSendMessage(SNPLAYER_OTHERS, &value, 1));
	uint8_t sender;
	void *data;
	size_t size;
	ASSERT_TRUE(net.SNetReceiveMessage(&sender, &data, &size));
	EXPECT_EQ(sender, 0);
	EXPECT_EQ(size, 1);
	EXPECT_FALSE(net.SNetReceiveMessage(&sender, &data, &size));
	ASSERT_TRUE(net.SNetSendMessage(0, &value, 1));
	ASSERT_TRUE(net.SNetLeaveGame(leaveinfo_t::LEAVE_EXIT));
	EXPECT_FALSE(net.SNetReceiveMessage(&sender, &data, &size));
	EXPECT_TRUE(net.SNetSendMessage(8, &value, 1));
	EXPECT_FALSE(net.SNetReceiveMessage(&sender, &data, &size));
}

TEST(NetworkLoopback, EightTurnSlotsKeepTheArraySentinel)
{
	loopback net;
	net.setup_gameinfo(GameInfo(true));
	Players.resize(8);
	std::array<char *, 9> data {};
	std::array<size_t, 9> sizes;
	std::array<uint32_t, 9> status {};
	sizes.fill(99);
	ASSERT_TRUE(net.SNetReceiveTurns(data.data(), sizes.data(), status.data()));
	for (size_t player = 0; player < 8; ++player)
		EXPECT_EQ(sizes[player], 0);
	EXPECT_EQ(sizes[8], 99);
	Players.clear();
}

#if !defined(NONET) && !defined(DISABLE_TCP)
// The listener runs separately, so ID 0 supplies the host's initial turn sequence.
class transport_client : public tcp_client {
public:
	explicit transport_client(bool host)
	    : host_(host)
	{
	}

protected:
	bool IsGameHost() override { return host_; }

private:
	bool host_;
};

class TcpTransport : public ::testing::TestWithParam<bool> {
protected:
	void Start(bool extended, const std::string &bindAddress = "127.0.0.1")
	{
		info_ = GameInfo(extended);
		factory_ = GetParam() ? std::make_unique<packet_factory>("transport-test") : std::make_unique<packet_factory>();
		server_ = std::make_unique<tcp_server>(context_, bindAddress, 0, *factory_, info_);
		address_ = server_->LocalhostSelf();
		thread_ = std::thread([this] { context_.run(); });
	}
	void TearDown() override
	{
		if (storm_) {
			SNetDestroy();
			sgGameInitInfo = {};
		}
		clients_.clear();
		if (thread_.joinable()) {
			asio::post(context_, [this] { server_->Close(); });
			thread_.join();
		}
		server_.reset();
	}
	int Add()
	{
		auto client = std::make_unique<transport_client>(clients_.empty());
		if (GetParam())
			client->setup_password("transport-test");
		else
			client->clear_password();
		client->setup_gameinfo(info_);
		const int id = client->join(address_);
		if (id >= 0) clients_.push_back(std::move(client));
		return id;
	}
	// Join through Storm as the menu does, so its classic-first mode detection runs.
	int StormJoin(const GameData &local)
	{
		storm_ = true;
		sgGameInitInfo = local;
		if (!SNetInitializeProvider(SELCONN_TCP, &sgGameInitInfo))
			return -2;
		std::string address = address_;
		char password[] = "transport-test";
		int id = -1;
		if (!SNetJoinGame(address.data(), GetParam() ? password : nullptr, &id))
			return -1;
		return id;
	}
	void Pump()
	{
		for (auto &client : clients_) {
			if (!client) continue;
			auto result = client->poll();
			ASSERT_TRUE(result.has_value()) << result.error().what();
		}
	}
	template <class F>
	bool Until(F condition)
	{
		const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
		do {
			Pump();
			if (condition()) return true;
			std::this_thread::sleep_for(std::chrono::milliseconds(1));
		} while (std::chrono::steady_clock::now() < deadline);
		return false;
	}
	asio::io_context context_;
	std::unique_ptr<packet_factory> factory_;
	std::unique_ptr<tcp_server> server_;
	std::thread thread_;
	std::string address_;
	buffer_t info_;
	std::vector<std::unique_ptr<transport_client>> clients_;
	bool storm_ = false;
};

TEST_P(TcpTransport, EightPeersExchangeOrderedDirectedBroadcastAndTurns)
{
	Start(true);
	for (int id = 0; id < 8; ++id)
		ASSERT_EQ(Add(), id);
	EXPECT_EQ(Add(), -1);
	Pump();
	constexpr uint8_t Repeats = 16;
	for (uint8_t source = 0; source < 8; ++source) {
		for (uint8_t sequence = 0; sequence < Repeats; ++sequence) {
			for (uint8_t destination = 0; destination < 8; ++destination) {
				buffer_t message(MaxMessageSize, source);
				message[0] = 0;
				message[1] = source;
				message[2] = destination;
				message[3] = sequence;
				ASSERT_TRUE(clients_[source]->SNetSendMessage(destination, message.data(), message.size()));
			}
			buffer_t message(MaxMessageSize, source);
			message[0] = 1;
			message[1] = source;
			message[2] = PLR_BROADCAST;
			message[3] = sequence;
			ASSERT_TRUE(clients_[source]->SNetSendMessage(SNPLAYER_OTHERS, message.data(), message.size()));
		}
	}
	std::array<size_t, 8> received {};
	std::array<std::array<std::array<uint8_t, 8>, 2>, 8> next {};
	ASSERT_TRUE(Until([&] {
		for (uint8_t destination = 0; destination < 8; ++destination) {
			uint8_t sender;
			void *data;
			size_t size;
			while (clients_[destination]->SNetReceiveMessage(&sender, &data, &size)) {
				EXPECT_EQ(size, MaxMessageSize);
				const auto *message = static_cast<const uint8_t *>(data);
				if (size != MaxMessageSize || sender >= 8 || message[0] > 1) return false;
				EXPECT_EQ(message[1], sender);
				EXPECT_EQ(message[2], message[0] == 0 ? destination : PLR_BROADCAST);
				EXPECT_EQ(message[3], next[destination][message[0]][sender]++);
				++received[destination];
			}
		}
		return std::all_of(received.begin(), received.end(), [](size_t count) { return count == 15 * Repeats; });
	}));
	for (size_t player = 0; player < 8; ++player)
		std::cout << "participant=" << player << " ordered-messages=" << received[player] << '\n';
	for (int tick = 0; tick < 8; ++tick) {
		for (int player = 0; player < 8; ++player) {
			int32_t value = 100 * player + tick;
			ASSERT_TRUE(clients_[player]->SNetSendTurn(reinterpret_cast<char *>(&value), sizeof(value)));
		}
		std::array<bool, 8> arrived {};
		ASSERT_TRUE(Until([&] {
			for (size_t player = 0; player < 8; ++player) {
				if (arrived[player]) continue;
				std::array<char *, 9> data {};
				std::array<size_t, 9> size {};
				std::array<uint32_t, 9> status {};
				status[8] = 123;
				if (!clients_[player]->SNetReceiveTurns(data.data(), size.data(), status.data())) continue;
				for (int sender = 0; sender < 8; ++sender) {
					EXPECT_NE(status[sender] & PS_TURN_ARRIVED, 0);
					EXPECT_EQ(size[sender], sizeof(int32_t));
					if (data[sender] == nullptr) return false;
					int32_t value;
					std::memcpy(&value, data[sender], sizeof(value));
					EXPECT_EQ(value, 100 * sender + tick);
				}
				EXPECT_EQ(status[8], 123);
				arrived[player] = true;
			}
			return std::all_of(arrived.begin(), arrived.end(), [](bool ready) { return ready; });
		}));
	}
}

TEST_P(TcpTransport, IPv6AddressWithEphemeralPortConnects)
{
	Start(true, "::1");
	ASSERT_EQ(Add(), 0);
	ASSERT_EQ(Add(), 1);
	char marker = 9;
	ASSERT_TRUE(clients_[1]->SNetSendMessage(0, &marker, 1));
	ASSERT_TRUE(Until([&] {
		uint8_t sender;
		void *data;
		size_t size;
		return clients_[0]->SNetReceiveMessage(&sender, &data, &size) && sender == 1 && size == 1;
	}));
}

TEST_P(TcpTransport, ClassicRoomRemainsFullAtFour)
{
	Start(false);
	for (int id = 0; id < 4; ++id)
		ASSERT_EQ(Add(), id);
	EXPECT_EQ(Add(), -1);
	char message = 9;
	ASSERT_TRUE(clients_[3]->SNetSendMessage(0, &message, 1));
	ASSERT_TRUE(Until([&] {
		uint8_t sender;
		void *data;
		size_t size;
		return clients_[0]->SNetReceiveMessage(&sender, &data, &size) && sender == 3 && size == 1;
	}));
}

TEST_P(TcpTransport, EverySlotCanBeReusedAfterAbruptDisconnect)
{
	Start(true);
	for (int id = 0; id < 8; ++id)
		ASSERT_EQ(Add(), id);
	for (size_t slot = 0; slot < 8; ++slot) {
		clients_[slot].reset();
		std::this_thread::sleep_for(std::chrono::milliseconds(20));
		Pump();
		// Add appends the replacement; move it back to the slot it was assigned.
		ASSERT_EQ(Add(), slot);
		clients_[slot] = std::move(clients_.back());
		clients_.pop_back();
		char marker = static_cast<char>(slot);
		const size_t destination = (slot + 1) % 8;
		ASSERT_TRUE(clients_[slot]->SNetSendMessage(destination, &marker, 1));
		ASSERT_TRUE(Until([&] {
			uint8_t sender;
			void *data;
			size_t size;
			return clients_[destination]->SNetReceiveMessage(&sender, &data, &size) && sender == slot && size == 1 && *static_cast<char *>(data) == marker;
		}));
		std::cout << "reused-slot=" << slot << " message-delivered\n";
	}
}

class raw_peer {
public:
	explicit raw_peer(const std::string &address)
	    : socket_(context_)
	{
		const size_t colon = address.rfind(':');
		socket_.connect({ asio::ip::make_address(address.substr(0, colon)), static_cast<unsigned short>(std::stoi(address.substr(colon + 1))) });
		socket_.non_blocking(true);
	}
	bool Send(const buffer_t &frame, bool bytewise = false)
	{
		size_t sent = 0;
		const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(3);
		while (sent < frame.size() && std::chrono::steady_clock::now() < deadline) {
			asio::error_code error;
			sent += socket_.write_some(asio::buffer(frame.data() + sent, bytewise ? 1 : frame.size() - sent), error);
			if (error && error != asio::error::would_block && error != asio::error::try_again) return false;
		}
		return sent == frame.size();
	}
	std::unique_ptr<packet> Read(packet_factory &factory, packet_type type)
	{
		const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(3);
		while (std::chrono::steady_clock::now() < deadline) {
			if (!ReadAvailable()) return nullptr;
			while (queue_.PacketReady().value_or(false)) {
				auto bytes = queue_.ReadPacket();
				if (!bytes.has_value()) return nullptr;
				auto pkt = factory.make_packet(std::move(*bytes));
				if (!pkt.has_value()) return nullptr;
				if ((*pkt)->Type() == type) return std::move(*pkt);
			}
			std::this_thread::sleep_for(std::chrono::milliseconds(1));
		}
		return nullptr;
	}
	bool Closed()
	{
		const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(3);
		while (std::chrono::steady_clock::now() < deadline) {
			if (!ReadAvailable()) return true;
			std::this_thread::sleep_for(std::chrono::milliseconds(1));
		}
		return false;
	}

private:
	bool ReadAvailable()
	{
		std::array<unsigned char, 4096> bytes;
		asio::error_code error;
		const size_t count = socket_.read_some(asio::buffer(bytes), error);
		if (error) return error == asio::error::would_block || error == asio::error::try_again;
		return count != 0 && queue_.Write(buffer_t(bytes.begin(), bytes.begin() + count)).has_value();
	}
	asio::io_context context_;
	asio::ip::tcp::socket socket_;
	frame_queue queue_;
};

TEST_P(TcpTransport, FragmentedJoinAndMessagesPreserveTheStream)
{
	Start(true);
	ASSERT_EQ(Add(), 0);
	{
		// An incomplete admission is released without ever claiming a player ID.
		raw_peer partial(address_);
		ASSERT_TRUE(partial.Send({ 99, 0, 0, 0, 21 }));
	}
	raw_peer peer(address_);
	auto request = MakeJoinRequest(*factory_, 42, info_);
	ASSERT_TRUE(request.has_value());
	auto frame = frame_queue::MakeFrame((*request)->Data());
	ASSERT_TRUE(frame.has_value());
	ASSERT_TRUE(peer.Send(*frame, true));
	auto accept = peer.Read(*factory_, PT_JOIN_ACCEPT_EXTENDED);
	ASSERT_NE(accept, nullptr);
	EXPECT_EQ(*accept->NewPlayer(), 1);
	buffer_t stream;
	for (uint8_t sequence = 0; sequence < 32; ++sequence) {
		auto message = factory_->make_packet<PT_MESSAGE>(plr_t { 1 }, plr_t { 0 }, buffer_t { sequence });
		ASSERT_TRUE(message.has_value());
		auto messageFrame = frame_queue::MakeFrame((*message)->Data());
		ASSERT_TRUE(messageFrame.has_value());
		stream.insert(stream.end(), messageFrame->begin(), messageFrame->end());
	}
	ASSERT_TRUE(peer.Send(stream, true));
	uint8_t next = 0;
	ASSERT_TRUE(Until([&] {
		uint8_t sender;
		void *data;
		size_t size;
		while (clients_[0]->SNetReceiveMessage(&sender, &data, &size)) {
			EXPECT_EQ(sender, 1);
			EXPECT_EQ(size, 1);
			EXPECT_EQ(*static_cast<uint8_t *>(data), next++);
		}
		return next == 32;
	}));
}

TEST_P(TcpTransport, CorruptFramesAndForgedAddressesReleaseTheirSlots)
{
	Start(true);
	ASSERT_EQ(Add(), 0);
	for (unsigned scenario = 0; scenario < 8; ++scenario) {
		SCOPED_TRACE(scenario);
		raw_peer peer(address_);
		auto request = MakeJoinRequest(*factory_, 42, info_);
		ASSERT_TRUE(request.has_value());
		ASSERT_TRUE(peer.Send(*frame_queue::MakeFrame((*request)->Data())));
		auto accept = peer.Read(*factory_, PT_JOIN_ACCEPT_EXTENDED);
		ASSERT_NE(accept, nullptr);
		EXPECT_EQ(*accept->NewPlayer(), 1);
		if (scenario == 0) {
			ASSERT_TRUE(peer.Send({ 0, 0, 0, 128 }));
		} else if (scenario == 1) {
			ASSERT_TRUE(peer.Send({ 1, 0, 1, 0, 0 }));
		} else {
			const plr_t source = scenario == 2 ? 0 : scenario == 3 ? 8
			                                                       : 1;
			const plr_t destination = scenario == 4 ? 8 : 0;
			if (scenario < 6) {
				auto forged = factory_->make_packet<PT_MESSAGE>(source, destination, buffer_t(scenario == 5 ? MaxMessageSize + 1 : 1));
				ASSERT_TRUE(forged.has_value());
				ASSERT_TRUE(peer.Send(*frame_queue::MakeFrame((*forged)->Data())));
			} else if (scenario == 6) {
				auto forged = factory_->make_packet<PT_CONNECT>(plr_t { 1 }, PLR_BROADCAST, plr_t { 8 });
				ASSERT_TRUE(forged.has_value());
				ASSERT_TRUE(peer.Send(*frame_queue::MakeFrame((*forged)->Data())));
			} else {
				auto forged = factory_->make_packet<PT_DISCONNECT>(plr_t { 1 }, PLR_BROADCAST, plr_t { 255 }, leaveinfo_t::LEAVE_DROP);
				ASSERT_TRUE(forged.has_value());
				ASSERT_TRUE(peer.Send(*frame_queue::MakeFrame((*forged)->Data())));
			}
		}
		ASSERT_TRUE(peer.Closed());
		Pump();
		std::cout << "invalid-request=" << scenario << " disconnected, slot 1 reusable\n";
	}
	ASSERT_EQ(Add(), 1);
}

TEST_P(TcpTransport, IncompleteJoinsAndWrongPasswordsReleaseResources)
{
	Start(true);
	ASSERT_EQ(Add(), 0);
	{
		std::vector<std::unique_ptr<raw_peer>> pending;
		for (size_t i = 0; i < 8; ++i) {
			auto peer = std::make_unique<raw_peer>(address_);
			ASSERT_TRUE(peer->Send({ 99, 0, 0 }));
			pending.push_back(std::move(peer));
		}
		EXPECT_EQ(Add(), -1);
	}
	ASSERT_TRUE(Until([&] {
		const int id = Add();
		if (id < 0) return false;
		EXPECT_EQ(id, 1);
		return true;
	}));
	transport_client wrongPassword(false);
	wrongPassword.setup_gameinfo(info_);
	wrongPassword.setup_password("wrong-password");
	EXPECT_EQ(wrongPassword.join(address_), -1);
	if (GetParam()) { EXPECT_NE(std::string(SDL_GetError()).find("password"), std::string::npos); }
	ASSERT_EQ(Add(), 2);
}

TEST_P(TcpTransport, StormJoinDetectsExtendedHostFromClassicRequest)
{
	Start(true);
	ASSERT_EQ(Add(), 0);
	// The rejected classic attempt must not consume a slot before the extended retry.
	ASSERT_EQ(StormJoin(LocalGame(false)), 1);
	EXPECT_EQ(sgGameInitInfo.rules, ExtendedSessionRules);
	ASSERT_EQ(Add(), 2);
}

TEST_P(TcpTransport, JoiningClientsPublishTheGameEvenWithTheHostsOwnData)
{
	Start(true);
	ASSERT_EQ(Add(), 0);
	static unsigned created;
	created = 0;
	transport_client client(false);
	if (GetParam())
		client.setup_password("transport-test");
	else
		client.clear_password();
	client.setup_gameinfo(info_); // Identical to the host's descriptor, seeds included.
	ASSERT_TRUE(client.SNetRegisterEventHandler(EVENT_TYPE_PLAYER_CREATE_GAME, [](_SNETEVENT *) { ++created; }));
	ASSERT_EQ(client.join(address_), 1);
	// The event starts the world resync of a joining client; without it the client never loads the world.
	EXPECT_EQ(created, 1U);
}

TEST_P(TcpTransport, StormJoinKeepsClassicHostsClassic)
{
	Start(false);
	ASSERT_EQ(Add(), 0);
	ASSERT_EQ(StormJoin(LocalGame(false)), 1);
	EXPECT_EQ(sgGameInitInfo.rules, ClassicSessionRules);
}

TEST_P(TcpTransport, StormJoinFallsBackToClassicHost)
{
	Start(false);
	ASSERT_EQ(Add(), 0);
	ASSERT_EQ(StormJoin(LocalGame(true)), 1);
	EXPECT_EQ(sgGameInitInfo.rules, ClassicSessionRules);
}

TEST_P(TcpTransport, StormJoinReportsFullExtendedRoom)
{
	Start(true);
	for (int id = 0; id < 8; ++id)
		ASSERT_EQ(Add(), id);
	EXPECT_EQ(StormJoin(LocalGame(false)), -1);
	EXPECT_EQ(std::string_view(SDL_GetError()), GameDataErrorMessage(GameDataError::SessionFull));
}

TEST_P(TcpTransport, StormJoinReportsFullClassicRoom)
{
	Start(false);
	for (int id = 0; id < 4; ++id)
		ASSERT_EQ(Add(), id);
	EXPECT_EQ(StormJoin(LocalGame(false)), -1);
	EXPECT_EQ(std::string_view(SDL_GetError()), GameDataErrorMessage(GameDataError::SessionFull));
}

TEST_P(TcpTransport, StormJoinDoesNotReportIncompatibleVersionAsFull)
{
	Start(true);
	ASSERT_EQ(Add(), 0);
	GameData local = LocalGame(false);
	local.versionMajor = 9;
	EXPECT_EQ(StormJoin(local), -1);
	EXPECT_EQ(std::string_view(SDL_GetError()), GameDataErrorMessage(GameDataError::VersionMismatch));
	ASSERT_EQ(Add(), 1);
}

INSTANTIATE_TEST_SUITE_P(PlainAndEncrypted, TcpTransport, ::testing::Bool());
#endif

} // namespace
} // namespace devilution::net
