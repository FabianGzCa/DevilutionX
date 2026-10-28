#include "dvlnet/base.h"

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <expected>
#include <memory>

#ifdef USE_SDL3
#include <SDL3/SDL_timer.h>
#else
#include <SDL.h>
#endif

#include "dvlnet/limits.hpp"
#include "dvlnet/session.hpp"
#include "player.h"

namespace devilution {
namespace net {

void base::process_network_packets()
{
	std::expected<void, PacketError> result = poll();
	if (!result.has_value()) {
		LogVerbose("Error polling network: {}", result.error().what());
	}
}

void base::setup_gameinfo(buffer_t info)
{
	player_limit_ = SessionCapacity(info);
	game_init_info = std::move(info);
}

void base::setup_password(std::string pw)
{
	pktfty = std::make_unique<packet_factory>(pw);
}

void base::clear_password()
{
	pktfty = std::make_unique<packet_factory>();
}

DvlNetLatencies base::get_latencies(uint8_t playerid)
{
	DvlNetLatencies latencies = abstract_net::get_latencies(playerid);
	if (playerid >= player_limit_)
		return latencies;
	const PlayerState &playerState = playerStateTable_[playerid];
	latencies.echoLatency = playerState.roundTripLatency;
	return latencies;
}

std::optional<GameDataError> base::get_join_rejection()
{
	return joinRejection_;
}

void base::RunEventHandler(_SNETEVENT &ev)
{
	auto f = registered_handlers[static_cast<event_type>(ev.eventid)];
	if (f != nullptr) {
		f(&ev);
	}
}

void base::DisconnectNet(plr_t plr)
{
}

std::expected<void, PacketError> base::SendEchoRequest(plr_t player)
{
	if (plr_self == PLR_BROADCAST)
		return {};
	if (player == plr_self)
		return {};

	const timestamp_t now = SDL_GetTicks();
	std::expected<std::unique_ptr<packet>, PacketError> pkt
	    = pktfty->make_packet<PT_ECHO_REQUEST>(plr_self, player, now);
	if (!pkt.has_value()) {
		return std::unexpected(pkt.error());
	}
	return send(**pkt);
}

std::expected<void, PacketError> base::HandleAccept(packet &pkt)
{
	if (plr_self != PLR_BROADCAST)
		return {}; // already have player id
	const auto cookie = pkt.Cookie();
	if (!cookie.has_value())
		return std::unexpected(cookie.error());
	if (*cookie != cookie_self)
		return {}; // acceptance for a different joining client
	const auto info = pkt.Info();
	if (!info.has_value())
		return std::unexpected(info.error());
	const auto host = DecodeGameData(**info);
	if (!host.has_value())
		return std::unexpected(GameDataErrorMessage(host.error()));
	const auto local = DecodeGameData(game_init_info);
	if (!local.has_value())
		return std::unexpected(GameDataErrorMessage(local.error()));
	if (auto error = CheckGameCompatibility(*host, *local))
		return std::unexpected(GameDataErrorMessage(*error));
	if (host->isExtended() != (pkt.Type() == PT_JOIN_ACCEPT_EXTENDED))
		return std::unexpected(GameDataErrorMessage(GameDataError::SessionMismatch));
	const auto newPlayer = pkt.NewPlayer();
	if (!newPlayer.has_value())
		return std::unexpected(newPlayer.error());
	if (*newPlayer >= player_limit_)
		return std::unexpected("Invalid player ID in join acceptance");
	plr_self = *newPlayer;
	if (auto result = Connect(plr_self); !result.has_value()) {
		DisconnectNet(plr_self);
		plr_self = PLR_BROADCAST;
		return result;
	}
	if (game_init_info == **info)
		return {};
	// Publish game state only after the full identity and assigned ID are validated.
	game_init_info = **info;
	_SNETEVENT ev;
	ev.eventid = EVENT_TYPE_PLAYER_CREATE_GAME;
	ev.playerid = plr_self;
	ev.data = game_init_info.data();
	ev.databytes = game_init_info.size();
	RunEventHandler(ev);
	return {};
}

std::expected<void, PacketError> base::HandleReject(packet &pkt)
{
	const auto cookie = pkt.Cookie();
	if (!cookie.has_value())
		return std::unexpected(cookie.error());
	if (plr_self != PLR_BROADCAST || *cookie != cookie_self)
		return {};
	const auto info = pkt.Info();
	if (!info.has_value())
		return std::unexpected(info.error());
	if ((*info)->size() != 1 || (**info)[0] > static_cast<uint8_t>(GameDataError::SessionFull))
		return std::unexpected("Invalid join rejection");
	joinRejection_ = static_cast<GameDataError>((**info)[0]);
	joinError_.emplace(GameDataErrorMessage(*joinRejection_));
	return std::unexpected(*joinError_);
}

std::expected<void, PacketError> base::HandleConnect(packet &pkt)
{
	return pkt.NewPlayer().and_then([this](plr_t &&newPlayer) {
		return Connect(newPlayer);
	});
}

std::expected<void, PacketError> base::HandleTurn(packet &pkt)
{
	const plr_t src = pkt.Source();
	PlayerState &playerState = playerStateTable_[src];
	std::deque<turn_t> &turnQueue = playerState.turnQueue;
	if (turnQueue.size() >= MaxQueuedTurns)
		return std::unexpected("Network turn queue is full");
	return pkt.Turn().and_then([&](turn_t &&turn) {
		turnQueue.push_back(turn);
		return MakeReady(turn.SequenceNumber);
	});
}

std::expected<void, PacketError> base::HandleDisconnect(packet &pkt)
{
	std::expected<plr_t, PacketError> newPlayer = pkt.NewPlayer();
	if (!newPlayer.has_value())
		return std::unexpected(newPlayer.error());
	if (*newPlayer == plr_self)
		return std::unexpected("We were dropped by the owner?");
	if (IsConnected(*newPlayer)) {
		std::expected<leaveinfo_t, PacketError> leaveinfo = pkt.LeaveInfo();
		if (!leaveinfo.has_value())
			return std::unexpected(leaveinfo.error());
		_SNETEVENT ev;
		ev.eventid = EVENT_TYPE_PLAYER_LEAVE_GAME;
		ev.playerid = *newPlayer;
		ev.data = reinterpret_cast<unsigned char *>(&*leaveinfo);
		ev.databytes = sizeof(leaveinfo_t);
		RunEventHandler(ev);
		DisconnectNet(*newPlayer);
		ResetPlayerState(*newPlayer);
	}
	return {};
}

std::expected<void, PacketError> base::HandleEchoRequest(packet &pkt)
{
	// If we have already left the game,
	// there is no need to respond to echoes
	if (plr_self == PLR_BROADCAST) return {};

	return pkt.Time()
	    .and_then([&](cookie_t &&pktTime) {
		    return pktfty->make_packet<PT_ECHO_REPLY>(plr_self, pkt.Source(), pktTime);
	    })
	    .and_then([&](std::unique_ptr<packet> &&pkt) {
		    return send(*pkt);
	    });
}

std::expected<void, PacketError> base::HandleEchoReply(packet &pkt)
{
	const uint32_t now = SDL_GetTicks();
	plr_t src = pkt.Source();
	if (src >= player_limit_) return {};
	return pkt.Time().transform([&](cookie_t &&pktTime) {
		PlayerState &playerState = playerStateTable_[src];
		playerState.roundTripLatency = now - pktTime;
	});
}

void base::ClearMsg(plr_t plr)
{
	std::erase_if(message_queue, [&](const message_t &msg) {
		if (msg.sender != plr)
			return false;
		messageQueueBytes_ -= msg.payload.size();
		return true;
	});
}

void base::ResetPlayerState(plr_t player)
{
	if (player >= player_limit_)
		return;
	ClearMsg(player);
	playerStateTable_[player] = {};
}

std::expected<void, PacketError> base::EnqueueMessage(plr_t sender, const buffer_t &message)
{
	if (message.size() > MaxMessageSize || message_queue.size() >= MaxQueuedPackets || message.size() > MaxQueuedBytes - messageQueueBytes_)
		return std::unexpected("Network message queue is full or message is too large");
	message_queue.emplace_back(sender, message);
	messageQueueBytes_ += message.size();
	return {};
}

std::expected<void, PacketError> base::Connect(plr_t player)
{
	if (player >= player_limit_)
		return std::unexpected("Invalid player ID");
	PlayerState &playerState = playerStateTable_[player];
	const bool wasConnected = playerState.isConnected;
	playerState.isConnected = true;

	if (!wasConnected) {
		if (std::expected<void, PacketError> result = SendFirstTurnIfReady(player); !result.has_value()) {
			playerState.isConnected = false;
			return result;
		}
	}
	return {};
}

bool base::IsConnected(plr_t player) const
{
	return player < player_limit_ && playerStateTable_[player].isConnected;
}

std::expected<void, PacketError> base::RecvLocal(packet &pkt)
{
	if (auto result = ValidatePacketAddresses(pkt, player_limit_); !result.has_value())
		return result;
	// Join messages must not mark their sender connected before validation.
	if (pkt.Type() == PT_JOIN_ACCEPT || pkt.Type() == PT_JOIN_ACCEPT_EXTENDED)
		return HandleAccept(pkt);
	if (pkt.Type() == PT_JOIN_REJECT)
		return HandleReject(pkt);
	if ((pkt.Type() == PT_TURN || pkt.Type() == PT_MESSAGE)
	    && (plr_self == PLR_BROADCAST || pkt.Source() >= player_limit_))
		return std::unexpected("Gameplay packet received before a valid join");
	if (pkt.Source() < player_limit_) {
		if (std::expected<void, PacketError> result = Connect(pkt.Source());
		    !result.has_value()) {
			return result;
		}
	}
	switch (pkt.Type()) {
	case PT_MESSAGE:
		return pkt.Message().and_then([&](const buffer_t *message) {
			return EnqueueMessage(pkt.Source(), *message);
		});
	case PT_TURN:
		return HandleTurn(pkt);
	case PT_CONNECT:
		return HandleConnect(pkt);
	case PT_DISCONNECT:
		return HandleDisconnect(pkt);
	case PT_ECHO_REQUEST:
		return HandleEchoRequest(pkt);
	case PT_ECHO_REPLY:
		return HandleEchoReply(pkt);
	default:
		return {};
		// otherwise drop
	}
}

bool base::SNetReceiveMessage(uint8_t *sender, void **data, size_t *size)
{
	uint32_t now = SDL_GetTicks();
	if (now == 0) now++;
	if (lastEchoTime == 0 || now - lastEchoTime > 5000) {
		for (plr_t i = 0; i < player_limit_; i++) {
			if (std::expected<void, PacketError> result = SendEchoRequest(i); !result.has_value())
				LogError("SendEchoRequest: {}", result.error().what());
		}
		lastEchoTime = now;
	}
	process_network_packets();
	if (message_queue.empty())
		return false;
	messageQueueBytes_ -= message_queue.front().payload.size();
	message_last = std::move(message_queue.front());
	message_queue.pop_front();
	*sender = message_last.sender;
	*size = message_last.payload.size();
	*data = message_last.payload.data();
	return true;
}

bool base::SNetSendMessage(uint8_t playerId, void *data, size_t size)
{
	if ((playerId != SNPLAYER_OTHERS && playerId >= player_limit_) || plr_self >= player_limit_
	    || size > MaxMessageSize || (data == nullptr && size != 0))
		return false;
	buffer_t message(size);
	if (size != 0)
		std::memcpy(message.data(), data, size);
	if (playerId == plr_self)
		return EnqueueMessage(plr_self, message).has_value();
	plr_t dest;
	if (playerId == SNPLAYER_OTHERS)
		dest = PLR_BROADCAST;
	else
		dest = playerId;
	if (dest != plr_self) {
		std::expected<std::unique_ptr<packet>, PacketError> pkt
		    = pktfty->make_packet<PT_MESSAGE>(plr_self, dest, message);
		if (!pkt.has_value()) {
			LogError("make_packet: {}", pkt.error().what());
			return false;
		}
		std::expected<void, PacketError> result = send(**pkt);
		if (!result.has_value()) {
			LogError("send: {}", result.error().what());
			return false;
		}
	}
	return true;
}

bool base::AllTurnsArrived()
{
	for (size_t i = 0; i < player_limit_; ++i) {
		const PlayerState &playerState = playerStateTable_[i];
		if (!playerState.isConnected)
			continue;

		const std::deque<turn_t> &turnQueue = playerState.turnQueue;
		if (turnQueue.empty())
			return false;
	}

	return true;
}

bool base::SNetReceiveTurns(char **data, size_t *size, uint32_t *status)
{
	process_network_packets();

	for (size_t i = 0; i < player_limit_; ++i) {
		status[i] = 0;
		size[i] = 0;
		data[i] = nullptr;

		PlayerState &playerState = playerStateTable_[i];
		if (!playerState.isConnected)
			continue;

		status[i] |= PS_CONNECTED;

		std::deque<turn_t> &turnQueue = playerState.turnQueue;
		while (!turnQueue.empty()) {
			const turn_t &turn = turnQueue.front();
			const seq_t diff = turn.SequenceNumber - current_turn;
			if (diff <= 0x7F)
				break;
			turnQueue.pop_front();
		}
	}

	if (AllTurnsArrived()) {
		for (size_t i = 0; i < player_limit_; ++i) {
			PlayerState &playerState = playerStateTable_[i];
			if (!playerState.isConnected)
				continue;

			std::deque<turn_t> &turnQueue = playerState.turnQueue;
			if (turnQueue.empty())
				continue;

			const turn_t &turn = turnQueue.front();
			if (turn.SequenceNumber != current_turn)
				continue;

			playerState.lastTurnValue = turn.Value;
			turnQueue.pop_front();

			status[i] |= PS_ACTIVE;
			status[i] |= PS_TURN_ARRIVED;
			size[i] = sizeof(int32_t);
			data[i] = reinterpret_cast<char *>(&playerState.lastTurnValue);
		}

		current_turn++;

		return true;
	}

	for (size_t i = 0; i < player_limit_; ++i) {
		const PlayerState &playerState = playerStateTable_[i];
		if (!playerState.isConnected)
			continue;

		const std::deque<turn_t> &turnQueue = playerState.turnQueue;
		if (turnQueue.empty())
			continue;

		status[i] |= PS_ACTIVE;
	}

	return false;
}

bool base::SNetSendTurn(char *data, size_t size)
{
	if (size != sizeof(int32_t) || data == nullptr || plr_self >= player_limit_
	    || playerStateTable_[plr_self].turnQueue.size() >= MaxQueuedTurns)
		return false;

	turn_t turn;
	turn.SequenceNumber = next_turn;
	std::memcpy(&turn.Value, data, size);
	next_turn++;

	PlayerState &playerState = playerStateTable_[plr_self];
	std::deque<turn_t> &turnQueue = playerState.turnQueue;
	turnQueue.push_back(turn);
	if (std::expected<void, PacketError> result = SendTurnIfReady(turn); !result.has_value()) {
		turnQueue.pop_back();
		next_turn = turn.SequenceNumber;
		LogError("SendTurnIfReady: {}", result.error().what());
		return false;
	}
	return true;
}

std::expected<void, PacketError> base::SendTurnIfReady(turn_t turn)
{
	if (awaitingSequenceNumber_)
		awaitingSequenceNumber_ = !IsGameHost();

	if (!awaitingSequenceNumber_) {
		std::expected<std::unique_ptr<packet>, PacketError> pkt
		    = pktfty->make_packet<PT_TURN>(plr_self, PLR_BROADCAST, turn);
		if (!pkt.has_value()) {
			return std::unexpected(pkt.error());
		}
		return send(**pkt);
	}
	return {};
}

std::expected<void, PacketError> base::SendFirstTurnIfReady(plr_t player)
{
	if (awaitingSequenceNumber_ || plr_self >= player_limit_)
		return {};

	const PlayerState &playerState = playerStateTable_[plr_self];
	const std::deque<turn_t> &turnQueue = playerState.turnQueue;
	if (turnQueue.empty())
		return {};

	for (const turn_t turn : turnQueue) {
		std::expected<std::unique_ptr<packet>, PacketError> pkt
		    = pktfty->make_packet<PT_TURN>(plr_self, player, turn);
		if (!pkt.has_value()) {
			return std::unexpected(pkt.error());
		}
		std::expected<void, PacketError> result = send(**pkt);
		if (!result.has_value()) {
			return result;
		}
	}
	return {};
}

std::expected<void, PacketError> base::MakeReady(seq_t sequenceNumber)
{
	if (!awaitingSequenceNumber_)
		return {};

	current_turn = sequenceNumber;
	next_turn = sequenceNumber;
	awaitingSequenceNumber_ = false;

	PlayerState &playerState = playerStateTable_[plr_self];
	std::deque<turn_t> &turnQueue = playerState.turnQueue;
	for (turn_t &turn : turnQueue) {
		turn.SequenceNumber = next_turn;
		next_turn++;
		if (std::expected<void, PacketError> result = SendTurnIfReady(turn);
		    !result.has_value()) {
			return result;
		}
	}
	return {};
}

void base::SNetGetProviderCaps(struct _SNETCAPS *caps)
{
	caps->size = 0;                  // engine writes only ?!?
	caps->flags = 0;                 // unused
	caps->maxmessagesize = MaxMessageSize; // capped to 512; underflow if < 24
	caps->maxqueuesize = 0;          // unused
	caps->maxplayers = player_limit_; // negotiated session capacity
	caps->bytessec = 1000000;        // ?
	caps->latencyms = 0;             // unused
	caps->defaultturnssec = 10;      // ?
	caps->defaultturnsintransit = 2; // maximum acceptable number
	                                 // of turns in queue?
}

bool base::SNetUnregisterEventHandler(event_type evtype)
{
	registered_handlers.erase(evtype);
	return true;
}

bool base::SNetRegisterEventHandler(event_type evtype, SEVTHANDLER func)
{
	/*
  engine registers handler for:
  EVENT_TYPE_PLAYER_LEAVE_GAME
  EVENT_TYPE_PLAYER_CREATE_GAME (should be raised during SNetCreateGame
  for non-creating player)
  EVENT_TYPE_PLAYER_MESSAGE (for bnet? not implemented)
  (engine uses same function for all three)
*/
	registered_handlers[evtype] = func;
	return true;
}

bool base::SNetLeaveGame(net::leaveinfo_t type)
{
	if (plr_self >= player_limit_)
		return true;
	std::expected<std::unique_ptr<packet>, PacketError> pkt
	    = pktfty->make_packet<PT_DISCONNECT>(
	        plr_self, PLR_BROADCAST, plr_self, type);
	if (!pkt.has_value()) {
		LogError("make_packet: {}", pkt.error().what());
		return false;
	}
	std::expected<void, PacketError> result = send(**pkt);
	if (!result.has_value()) {
		LogError("send: {}", result.error().what());
		return false;
	}
	plr_self = PLR_BROADCAST;
	for (plr_t player = 0; player < player_limit_; ++player)
		ResetPlayerState(player);
	return true;
}

bool base::SNetDropPlayer(int playerid, net::leaveinfo_t flags)
{
	if (playerid < 0 || playerid >= player_limit_ || plr_self >= player_limit_)
		return false;
	const auto plr = static_cast<plr_t>(playerid);
	std::expected<std::unique_ptr<packet>, PacketError> pkt
	    = pktfty->make_packet<PT_DISCONNECT>(
	        plr_self,
	        PLR_BROADCAST,
	        plr,
	        flags);
	if (!pkt.has_value()) {
		LogError("make_packet: {}", pkt.error().what());
		return false;
	}
	// Disconnect at the network layer first so we
	// don't send players their own disconnect packet
	DisconnectNet(plr);
	std::expected<void, PacketError> sendResult = send(**pkt);
	if (!sendResult.has_value()) {
		LogError("send: {}", sendResult.error().what());
		return false;
	}
	std::expected<void, PacketError> receiveResult = RecvLocal(**pkt);
	if (!receiveResult.has_value()) {
		LogError("SNetDropPlayer: {}", receiveResult.error().what());
		return false;
	}
	return true;
}

plr_t base::GetOwner()
{
	for (plr_t i = 0; i < player_limit_; ++i) {
		if (IsConnected(i)) {
			return i;
		}
	}
	return PLR_BROADCAST; // should be unreachable
}

bool base::SNetGetOwnerTurnsWaiting(uint32_t *turns)
{
	process_network_packets();

	const plr_t owner = GetOwner();
	if (owner >= player_limit_) {
		*turns = 0;
		return false;
	}
	const PlayerState &playerState = playerStateTable_[owner];
	const std::deque<turn_t> &turnQueue = playerState.turnQueue;
	*turns = static_cast<uint32_t>(turnQueue.size());

	return true;
}

bool base::SNetGetTurnsInTransit(uint32_t *turns)
{
	if (plr_self >= player_limit_) {
		*turns = 0;
		return false;
	}
	const PlayerState &playerState = playerStateTable_[plr_self];
	const std::deque<turn_t> &turnQueue = playerState.turnQueue;
	*turns = static_cast<uint32_t>(turnQueue.size());
	return true;
}

} // namespace net
} // namespace devilution
