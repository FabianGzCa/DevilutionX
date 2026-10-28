#include "dvlnet/loopback.h"

#include <algorithm>
#include <cstdint>
#include <cstring>

#include "dvlnet/limits.hpp"
#include "dvlnet/session.hpp"
#include "multi.h"
#include "player.h"
#include "utils/language.h"
#include "utils/stubs.h"

namespace devilution::net {

int loopback::create(std::string_view /*addrstr*/)
{
	IsLoopback = true;
	return plr_single;
}

int loopback::join(std::string_view /*addrstr*/)
{
	ABORT();
}

bool loopback::SNetReceiveMessage(uint8_t *sender, void **data, size_t *size)
{
	if (message_queue.empty())
		return false;
	message_last = message_queue.front();
	message_queue.pop();
	*sender = plr_single;
	*size = message_last.size();
	*data = message_last.data();
	return true;
}

bool loopback::SNetSendMessage(uint8_t dest, void *data, size_t size)
{
	if (size > MaxMessageSize || (data == nullptr && size != 0))
		return false;
	// Remote destinations are successful no-ops, as in classic single player.
	if (dest == plr_single) {
		if (message_queue.size() >= MaxQueuedPackets)
			return false;
		buffer_t message(size);
		if (size != 0)
			std::memcpy(message.data(), data, size);
		message_queue.push(message);
	}
	return true;
}

bool loopback::SNetReceiveTurns(char **data, size_t *size, uint32_t * /*status*/)
{
	for (size_t i = 0; i < std::min(Players.size(), static_cast<size_t>(playerLimit_)); ++i) {
		size[i] = 0;
		data[i] = nullptr;
	}
	return true;
}

bool loopback::SNetSendTurn(char * /*data*/, size_t /*size*/)
{
	return true;
}

void loopback::SNetGetProviderCaps(struct _SNETCAPS *caps)
{
	caps->size = 0;                  // engine writes only ?!?
	caps->flags = 0;                 // unused
	caps->maxmessagesize = MaxMessageSize; // capped to 512; underflow if < 24
	caps->maxqueuesize = 0;          // unused
	caps->maxplayers = playerLimit_; // negotiated buffer capacity
	caps->bytessec = 1000000;        // ?
	caps->latencyms = 0;             // unused
	caps->defaultturnssec = 10;      // ?
	caps->defaultturnsintransit = 1; // maximum acceptable number
	                                 // of turns in queue?
}

bool loopback::SNetRegisterEventHandler(event_type /*evtype*/,
    SEVTHANDLER /*func*/)
{
	// not called in real singleplayer mode
	// not needed in pseudo multiplayer mode (?)
	return true;
}

bool loopback::SNetUnregisterEventHandler(event_type /*evtype*/)
{
	// not called in real singleplayer mode
	// not needed in pseudo multiplayer mode (?)
	return true;
}

bool loopback::SNetLeaveGame(net::leaveinfo_t /*type*/)
{
	IsLoopback = false;
	message_queue = {};
	message_last.clear();
	return true;
}

bool loopback::SNetDropPlayer(int /*playerid*/, net::leaveinfo_t /*flags*/)
{
	return true;
}

void loopback::setup_gameinfo(buffer_t info)
{
	playerLimit_ = SessionCapacity(info);
}

bool loopback::SNetGetOwnerTurnsWaiting(uint32_t *turns)
{
	*turns = 0;
	return true;
}

bool loopback::SNetGetTurnsInTransit(uint32_t *turns)
{
	*turns = 0;
	return true;
}

std::string loopback::make_default_gamename()
{
	return std::string(_("loopback"));
}

} // namespace devilution::net
