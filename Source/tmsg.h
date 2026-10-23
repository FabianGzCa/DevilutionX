/**
 * @file tmsg.h
 *
 * Interface of delayed network message processing.
 */
#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>

namespace devilution {

uint8_t tmsg_get(uint8_t *playerId, std::unique_ptr<std::byte[]> *msg);
/** Queue a retry with the identity of the original message sender. */
void tmsg_add(uint8_t playerId, const std::byte *msg, uint8_t bLen);
void tmsg_start();
void tmsg_cleanup();

} // namespace devilution
