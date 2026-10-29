/**
 * @file tmsg.cpp
 *
 * Implementation of delayed network message processing.
 */
#include <cstdint>
#include <list>

#ifdef USE_SDL3
#include <SDL3/SDL_timer.h>
#else
#include <SDL.h>
#endif

#include "diablo.h"
#include "tmsg.h"

namespace devilution {

namespace {

struct TMsg {
	uint32_t time;
	uint8_t playerId;
	std::unique_ptr<std::byte[]> body;
	uint8_t len;

	TMsg(uint32_t time, uint8_t playerId, const std::byte *data, uint8_t len)
	    : time(time)
	    , playerId(playerId)
	    , body(new std::byte[len])
	    , len(len)
	{
		memcpy(body.get(), data, len);
	}
};

std::list<TMsg> TimedMsgList;

} // namespace

uint8_t tmsg_get(uint8_t *playerId, std::unique_ptr<std::byte[]> *msg)
{
	if (TimedMsgList.empty())
		return 0;

	TMsg &head = TimedMsgList.front();
	if ((int)(head.time - SDL_GetTicks()) >= 0)
		return 0;

	const uint8_t len = head.len;
	*playerId = head.playerId;
	*msg = std::move(head.body);
	TimedMsgList.pop_front();
	return len;
}

void tmsg_add(uint8_t playerId, const std::byte *msg, uint8_t len)
{
	const uint32_t time = SDL_GetTicks() + (gnTickDelay * 10);
	TimedMsgList.emplace_back(time, playerId, msg, len);
}

void tmsg_start()
{
	assert(TimedMsgList.empty());
}

void tmsg_cleanup()
{
	TimedMsgList.clear();
}

} // namespace devilution
