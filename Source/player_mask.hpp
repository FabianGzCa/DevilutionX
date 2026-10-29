/**
 * @file player_mask.hpp
 *
 * Types and helpers for masks indexed by player ID.
 */
#pragma once

#include <cstddef>
#include <cstdint>
#include <limits>
#include <type_traits>

namespace devilution {

/** Bits identifying players who contributed damage to a monster. */
struct PlayerExperienceMask {
	uint8_t bits;
};

/** Bits identifying recipients of a multiplayer message. */
struct PlayerRecipientMask {
	uint32_t bits;
};

constexpr size_t PlayerExperienceMaskBitCount = std::numeric_limits<uint8_t>::digits;
constexpr size_t PlayerRecipientMaskBitCount = std::numeric_limits<uint32_t>::digits;

static_assert(sizeof(PlayerExperienceMask) == sizeof(uint8_t));
static_assert(sizeof(PlayerRecipientMask) == sizeof(uint32_t));
static_assert(std::is_trivially_copyable_v<PlayerExperienceMask>);
static_assert(std::is_trivially_copyable_v<PlayerRecipientMask>);

constexpr PlayerExperienceMask PlayerExperienceMaskFromByte(uint8_t bits)
{
	return { bits };
}

constexpr uint8_t PlayerExperienceMaskToByte(PlayerExperienceMask mask)
{
	return mask.bits;
}

constexpr PlayerExperienceMask PlayerExperienceBit(size_t playerId)
{
	if (playerId >= PlayerExperienceMaskBitCount)
		return { 0 };
	return { static_cast<uint8_t>(uint32_t { 1 } << playerId) };
}

constexpr PlayerExperienceMask AddPlayerToExperienceMask(PlayerExperienceMask mask, size_t playerId)
{
	return { static_cast<uint8_t>(mask.bits | PlayerExperienceBit(playerId).bits) };
}

constexpr bool IsPlayerInExperienceMask(PlayerExperienceMask mask, size_t playerId)
{
	return (mask.bits & PlayerExperienceBit(playerId).bits) != 0;
}

constexpr PlayerExperienceMask MergePlayerExperienceMasks(PlayerExperienceMask lhs, PlayerExperienceMask rhs)
{
	return { static_cast<uint8_t>(lhs.bits | rhs.bits) };
}

constexpr PlayerRecipientMask PlayerRecipientBit(size_t playerId)
{
	if (playerId >= PlayerRecipientMaskBitCount)
		return { 0 };
	return { uint32_t { 1 } << playerId };
}

constexpr PlayerRecipientMask AddPlayerToRecipientMask(PlayerRecipientMask mask, size_t playerId)
{
	return { mask.bits | PlayerRecipientBit(playerId).bits };
}

constexpr bool IsPlayerInRecipientMask(PlayerRecipientMask mask, size_t playerId)
{
	return (mask.bits & PlayerRecipientBit(playerId).bits) != 0;
}

constexpr PlayerRecipientMask AllPlayerRecipients()
{
	return { std::numeric_limits<uint32_t>::max() };
}

} // namespace devilution
