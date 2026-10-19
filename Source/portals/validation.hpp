/**
 * @file portals/validation.hpp
 *
 * Interface of functions for validation of portal data.
 */
#pragma once

#include "engine/point.hpp"

namespace devilution {

bool IsPortalDeltaValid(Point location, int level, int levelType, bool isOnSetLevel);

} // namespace devilution
