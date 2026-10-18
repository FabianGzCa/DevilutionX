#include "dvlnet/tcp_server.h"

#include <chrono>
#include <expected>
#include <functional>
#include <memory>
#include <utility>

#include "dvlnet/base.h"
#include "dvlnet/session.hpp"
#include "player.h"
#include "utils/log.hpp"

namespace devilution::net {

tcp_server::tcp_server(asio::io_context &ioc, const std::string &bindaddr,
    unsigned short port, packet_factory &pktfty, buffer_t gameInfo)
    : ioc(ioc)
    , pktfty(pktfty)
    , playerLimit_(SessionCapacity(gameInfo))
    , game_init_info(std::move(gameInfo))
{
	auto addr = asio::ip::make_address(bindaddr);
	auto ep = asio::ip::tcp::endpoint(addr, port);
	acceptor = std::make_unique<asio::ip::tcp::acceptor>(ioc, ep, true);
	StartAccept();
}

std::string tcp_server::LocalhostSelf()
{
	auto endpoint = acceptor->local_endpoint();
	auto addr = endpoint.address();
	if (addr.is_unspecified())
		addr = addr.is_v4() ? asio::ip::address(asio::ip::address_v4::loopback()) : asio::ip::address(asio::ip::address_v6::loopback());
	const std::string host = addr.is_v6() ? "[" + addr.to_string() + "]" : addr.to_string();
	return host + ":" + std::to_string(endpoint.port());
}

tcp_server::scc tcp_server::MakeConnection()
{
	return std::make_shared<client_connection>(ioc);
}

plr_t tcp_server::NextFree()
{
	for (plr_t i = 0; i < playerLimit_; ++i)
		if (!connections[i])
			return i;
	return PLR_BROADCAST;
}

void tcp_server::StartReceive(const scc &con)
{
	con->socket.async_receive(
	    asio::buffer(con->recv_buffer),
	    std::bind(&tcp_server::HandleReceive, this, con, std::placeholders::_1, std::placeholders::_2));
}

void tcp_server::HandleReceive(const scc &con, const asio::error_code &ec,
    size_t bytesRead)
{
	if (con->closed)
		return;
	if (ec || bytesRead == 0) {
		DropConnection(con);
		return;
	}
	con->recv_buffer.resize(bytesRead);
	if (auto result = con->recv_queue.Write(std::move(con->recv_buffer)); !result.has_value()) {
		DropConnection(con);
		return;
	}
	con->recv_buffer.resize(frame_queue::max_frame_size);
	while (true) {
		std::expected<bool, PacketError> ready = con->recv_queue.PacketReady();
		if (!ready.has_value()) {
			Log("PacketReady: {}", ready.error().what());
			DropConnection(con);
			return;
		}
		if (!*ready)
			break;
		if (con->recv_queue.ReadPacketFlags() != 0) {
			DropConnection(con);
			return;
		}
		std::expected<buffer_t, PacketError> pktData = con->recv_queue.ReadPacket();
		if (!pktData.has_value()) {
			Log("ReadPacket: {}", pktData.error().what());
			DropConnection(con);
			return;
		}
		std::expected<std::unique_ptr<packet>, PacketError> pkt = pktfty.make_packet(*pktData);
		if (!pkt.has_value()) {
			Log("make_packet: {}", pkt.error().what());
			if (pkt.error().code() == PacketError::ErrorCode::DecryptionFailed) {
				con->rejecting = true;
				con->closeAfterSend = true;
				if (auto result = StartSend(con, pkt.error().code()); result.has_value())
					return;
			}
			DropConnection(con);
			return;
		}
		if (con->plr == PLR_BROADCAST) {
			std::expected<void, PacketError> result = HandleReceiveNewPlayer(con, **pkt);
			if (!result.has_value()) {
				Log("HandleReceiveNewPlayer: {}", result.error().what());
				DropConnection(con);
				return;
			}
			if (con->rejecting)
				return;
		} else {
			con->timeout = timeout_active;
			std::expected<void, PacketError> result = HandleReceivePacket(con, **pkt);
			if (!result.has_value()) {
				Log("Network error: {}", result.error().what());
				DropConnection(con);
				return;
			}
		}
		if (con->closed)
			return;
	}
	StartReceive(con);
}

std::expected<void, PacketError> tcp_server::HandleReceiveNewPlayer(const scc &con, packet &inPkt)
{
	const auto info = inPkt.Info();
	if (!info.has_value())
		return std::unexpected(info.error());
	if (auto error = ValidateJoinRequest(game_init_info, inPkt)) {
		auto reply = MakeJoinReject(pktfty, PLR_MASTER, inPkt, *error);
		if (!reply.has_value())
			return std::unexpected(reply.error());
		con->rejecting = true;
		return StartSend(con, (*reply)->Data(), 0, true);
	}
	const auto newplr = NextFree();
	if (newplr == PLR_BROADCAST) {
		con->rejecting = true;
		return MakeJoinReject(pktfty, PLR_MASTER, inPkt, GameDataError::SessionFull)
		    .and_then([&](std::unique_ptr<packet> pkt) { return StartSend(con, pkt->Data(), 0, true); });
	}

	for (plr_t player = 0; player < playerLimit_; player++) {
		if (connections[player]) {
			std::expected<void, PacketError> result
			    = pktfty.make_packet<PT_CONNECT>(PLR_MASTER, PLR_BROADCAST, newplr)
			          .and_then([&](std::unique_ptr<packet> &&pkt) { return StartSend(connections[player], *pkt); })
			          .and_then([&]() { return pktfty.make_packet<PT_CONNECT>(PLR_MASTER, PLR_BROADCAST, player); })
			          .and_then([&](std::unique_ptr<packet> &&pkt) { return StartSend(con, *pkt); });
			if (!result.has_value())
				return result;
		}
	}

	std::expected<void, PacketError> result
	    = inPkt.Cookie()
	          .and_then([&](cookie_t &&cookie) { return MakeJoinAccept(pktfty, PLR_MASTER, cookie, newplr, game_init_info); })
	          .and_then([&](std::unique_ptr<packet> &&pkt) { return StartSend(con, *pkt); });
	if (!result.has_value())
		return result;
	pending_.erase(con);
	con->plr = newplr;
	connections[newplr] = con;
	con->timeout = timeout_active;
	return {};
}

std::expected<void, PacketError> tcp_server::HandleReceivePacket(const scc &con, packet &pkt)
{
	if (pkt.Source() != con->plr)
		return std::unexpected("Packet source does not match connection");
	if (auto result = ValidatePacketAddresses(pkt, playerLimit_); !result.has_value())
		return result;
	switch (pkt.Type()) {
	case PT_MESSAGE:
		if ((*pkt.Message())->size() > MaxMessageSize)
			return std::unexpected("Message exceeds provider limit");
		break;
	case PT_TURN:
	case PT_ECHO_REQUEST:
	case PT_ECHO_REPLY:
		break;
	case PT_DISCONNECT:
		return SendPacket(pkt).transform([&] { DisconnectNet(*pkt.NewPlayer()); });
	default:
		return std::unexpected("Unexpected packet from an admitted player");
	}
	return SendPacket(pkt);
}

std::expected<void, PacketError> tcp_server::SendPacket(packet &pkt)
{
	if (pkt.Destination() == PLR_BROADCAST) {
		for (size_t i = 0; i < playerLimit_; ++i) {
			if (i == pkt.Source() || !connections[i])
				continue;
			std::expected<void, PacketError> result = StartSend(connections[i], pkt);
			if (!result.has_value()) {
				LogError("Failed to send packet {} to player {}: {}", static_cast<uint8_t>(pkt.Type()), i, result.error().what());
				DropConnection(connections[i]);
			}
		}
		return {};
	}
	if (pkt.Destination() >= playerLimit_)
		return std::unexpected(ServerError());
	if (pkt.Destination() == pkt.Source() || !connections[pkt.Destination()])
		return {};
	const scc recipient = connections[pkt.Destination()];
	if (auto result = StartSend(recipient, pkt); !result.has_value())
		DropConnection(recipient);
	return { };
}

std::expected<void, PacketError> tcp_server::StartSend(const scc &con, packet &pkt)
{
	return StartSend(con, pkt.Data(), 0);
}

std::expected<void, PacketError> tcp_server::StartSend(const scc &con, PacketError::ErrorCode errorCode)
{
	buffer_t pktData;
	pktData.push_back(static_cast<unsigned char>(errorCode));
	return StartSend(con, pktData, TcpErrorCodeFlags);
}

std::expected<void, PacketError> tcp_server::StartSend(const scc &con, buffer_t pktData, uint16_t flags, bool closeAfterSend)
{
	if (con->closed)
		return std::unexpected("Connection is closed");
	auto frame = frame_queue::MakeFrame(std::move(pktData), flags);
	if (!frame.has_value())
		return std::unexpected(frame.error());
	const bool idle = con->outgoing.Empty();
	if (auto result = con->outgoing.Push(std::move(*frame)); !result.has_value())
		return result;
	con->closeAfterSend |= closeAfterSend;
	if (idle)
		WriteNext(con);
	return {};
}

void tcp_server::WriteNext(const scc &con)
{
	const auto frame = con->outgoing.Front();
	asio::async_write(con->socket, asio::buffer(*frame),
	    [this, con, frame](const asio::error_code &ec, size_t bytesSent) { HandleSend(con, ec, bytesSent); });
}

void tcp_server::HandleSend(const scc &con, const asio::error_code &ec, size_t /*bytesSent*/)
{
	if (con->closed)
		return;
	if (ec) {
		DropConnection(con);
		return;
	}
	con->outgoing.Pop();
	if (!con->outgoing.Empty())
		WriteNext(con);
	else if (con->closeAfterSend)
		DropConnection(con);
}

void tcp_server::StartAccept()
{
	auto nextcon = MakeConnection();
	acceptor->async_accept(
	    nextcon->socket,
	    std::bind(&tcp_server::HandleAccept, this, nextcon, std::placeholders::_1));
}

void tcp_server::HandleAccept(const scc &con, const asio::error_code &ec)
{
	if (closing_)
		return;
	if (ec) {
		const PacketError packetError = IoHandlerError(ec.message());
		RaiseIoHandlerError(packetError);
		return;
	}
	if (pending_.size() >= playerLimit_) {
		DropConnection(con);
	} else {
		asio::error_code errorCode;
		const asio::ip::tcp::no_delay option(true);
		con->socket.set_option(option, errorCode);
		if (errorCode)
			LogError("Server error setting socket option: {}", errorCode.message());
		pending_.insert(con);
		con->timeout = timeout_connect;
		StartReceive(con);
		StartTimeout(con);
	}
	StartAccept();
}

void tcp_server::StartTimeout(const scc &con)
{
	con->timer.expires_after(std::chrono::seconds(1));
	con->timer.async_wait(
	    std::bind(&tcp_server::HandleTimeout, this, con, std::placeholders::_1));
}

void tcp_server::HandleTimeout(const scc &con, const asio::error_code &ec)
{
	if (ec || con->closed)
		return;
	if (con->timeout > 0)
		con->timeout -= 1;
	if (con->timeout <= 0) {
		con->timeout = 0;
		DropConnection(con);
		return;
	}
	StartTimeout(con);
}

void tcp_server::DropConnection(const scc &connection)
{
	// Keep a local owner: callers may pass a reference to the slot we clear.
	const scc con = connection;
	if (!con || con->closed)
		return;
	con->closed = true;
	const plr_t plr = con->plr;
	con->plr = PLR_BROADCAST;
	pending_.erase(con);
	con->outgoing.Clear();
	con->timer.cancel();
	con->socket.close();
	if (plr == PLR_BROADCAST) {
		return;
	}
	if (plr >= playerLimit_ || connections[plr] != con)
		return;
	connections[plr] = nullptr;
	if (closing_)
		return;

	std::expected<std::unique_ptr<packet>, PacketError> pkt
	    = pktfty.make_packet<PT_DISCONNECT>(PLR_MASTER, PLR_BROADCAST,
	        plr, leaveinfo_t::LEAVE_DROP);
	if (pkt.has_value()) {
		if (std::expected<void, PacketError> result = SendPacket(**pkt); !result.has_value())
			LogError("SendPacket<PT_DISCONNECT>: {}", result.error().what());
	} else {
		LogError("make_packet<PT_DISCONNECT>: {}", pkt.error().what());
	}
}

void tcp_server::RaiseIoHandlerError(const PacketError &error)
{
	ioHandlerResult.emplace(error);
}

std::expected<void, PacketError> tcp_server::CheckIoHandlerError()
{
	if (ioHandlerResult == std::nullopt)
		return {};
	std::expected<void, PacketError> packetError = std::unexpected(*ioHandlerResult);
	ioHandlerResult = std::nullopt;
	return packetError;
}

void tcp_server::DisconnectNet(plr_t plr)
{
	if (plr < playerLimit_)
		DropConnection(connections[plr]);
}

void tcp_server::Close()
{
	if (closing_)
		return;
	closing_ = true;
	acceptor->close();
	while (!pending_.empty())
		DropConnection(*pending_.begin());
	for (const auto &con : connections)
		DropConnection(con);
}

tcp_server::~tcp_server()
    = default;

} // namespace devilution::net
