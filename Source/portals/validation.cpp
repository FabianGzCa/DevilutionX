/**
 * @file portals/validation.cpp
 *
 * Implementation of functions for validation of portal data.
 */

#include "portals/validation.hpp"

#include "game_mode.hpp"
#include "levels/gendung.h"
#include "levels/setmaps.h"
#include "quests.h"

namespace devilution {

namespace {

dungeon_type GetQuestLevelType(_setlevels questLevel)
{
	for (const Quest &quest : Quests) {
		if (quest._qslvl == questLevel)
			return quest._qlvltype;
	}
	return DTYPE_NONE;
}

dungeon_type GetSetLevelType(_setlevels setLevel)
{
	const bool isArenaLevel = setLevel >= SL_FIRST_ARENA && setLevel <= SL_LAST;
	return isArenaLevel ? GetArenaLevelType(setLevel) : GetQuestLevelType(setLevel);
}

} // namespace

bool IsPortalDeltaValid(Point location, int level, int ltype, bool isOnSetLevel)
{
	if (!InDungeonBounds(location))
		return false;
	if (ltype <= DTYPE_TOWN || ltype > DTYPE_LAST || level <= 0)
		return false;
	const auto levelType = static_cast<dungeon_type>(ltype);
	if (isOnSetLevel) {
		if (level > SL_LAST)
			return false;
		return levelType == GetSetLevelType(static_cast<_setlevels>(level));
	}
	const int maxLevel = gbIsHellfire ? 24 : 16;
	if (level > maxLevel || (gbIsSpawn && level > 4))
		return false;
	return levelType == GetLevelType(level);
}

} // namespace devilution
