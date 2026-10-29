#pragma once

#include <cstddef>

namespace devilution {

// Level saves serialize the complete ActiveLights array; changing this requires save-format versioning.
#define MAXLIGHTS 32
#define MAXVISION 8
#define NO_LIGHT -1

constexpr bool IsValidLightId(int lightId)
{
	return lightId >= 0 && lightId < MAXLIGHTS;
}

constexpr char LightsMax = 15;

/** @brief A light table maps palette indices, so its size is the same as the palette size. */
constexpr size_t LightTableSize = 256;

/** @brief Number of supported light levels */
constexpr size_t NumLightingLevels = LightsMax + 1;

} // namespace devilution
