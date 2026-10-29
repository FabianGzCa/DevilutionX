#pragma once

#include <array>
#include <cstdint>
#include <string_view>

#include "engine/clx_sprite.hpp"
#include "engine/palette.h"
#include "engine/surface.hpp"
#include "multi.h"

namespace devilution {

/** Visual cues shared by all player-facing renderers. */
struct PlayerVisualIdentity {
	uint8_t automapColor;
	uint8_t outlineColor;
	char number;
	std::string_view nameLabel;
	std::array<uint8_t, 5> automapNumber;
};

/**
 * Player number glyphs use three bits per row, from left to right.
 * Optional tags use the same number in a three-by-five pixel font.
 */
inline constexpr std::array<PlayerVisualIdentity, PlayerStorageCapacity> PlayerVisualIdentities = { {
	{ PAL8_ORANGE + 1, PAL16_ORANGE + 7, '1', "1.", { 0b010, 0b110, 0b010, 0b010, 0b111 } },
	{ PAL8_YELLOW + 1, PAL16_YELLOW + 7, '2', "2.", { 0b111, 0b001, 0b111, 0b100, 0b111 } },
	{ PAL8_RED + 1, PAL16_RED + 7, '3', "3.", { 0b111, 0b001, 0b111, 0b001, 0b111 } },
	{ PAL8_BLUE + 1, PAL16_BLUE + 7, '4', "4.", { 0b101, 0b101, 0b111, 0b001, 0b001 } },
	{ PAL8_ORANGE + 4, PAL16_ORANGE + 10, '5', "5.", { 0b111, 0b100, 0b111, 0b001, 0b111 } },
	{ PAL8_YELLOW + 4, PAL16_YELLOW + 10, '6', "6.", { 0b111, 0b100, 0b111, 0b101, 0b111 } },
	{ PAL8_RED + 4, PAL16_RED + 10, '7', "7.", { 0b111, 0b001, 0b010, 0b010, 0b010 } },
	{ PAL8_BLUE + 4, PAL16_BLUE + 10, '8', "8.", { 0b111, 0b101, 0b111, 0b101, 0b111 } },
} };

static_assert(PlayerVisualIdentities.size() == PlayerStorageCapacity);

/** Returns the requested identity, falling back to player one for invalid IDs. */
[[nodiscard]] constexpr const PlayerVisualIdentity &GetPlayerVisualIdentity(int playerId)
{
	if (playerId < 0 || playerId >= PlayerStorageCapacity)
		return PlayerVisualIdentities[0];
	return PlayerVisualIdentities[static_cast<size_t>(playerId)];
}

/** Returns local display colors, preserving the four classic colors by default. */
DVL_API_FOR_TEST const PlayerVisualIdentity &GetConfiguredPlayerVisualIdentity(int playerId);

/** Returns an optional number prefix for a party member's name. */
DVL_API_FOR_TEST std::string_view GetPlayerNameLabel(int playerId);

/** Draws a small number only when player identifiers are enabled locally. */
DVL_API_FOR_TEST void DrawPlayerNumber(const Surface &out, Point center, int playerId);

/** Draws a player tag using the original five-sprite asset, with an optional number. */
DVL_API_FOR_TEST void DrawPlayerTag(const Surface &out, ClxSpriteList tags, Point position, int playerId);

} // namespace devilution
