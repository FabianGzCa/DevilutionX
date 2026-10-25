/**
 * @file monsters/party_scaling.hpp
 *
 * Deterministic balance revision 1 for extended parties. Lifecycle integration
 * must agree on the level's high-water participant count before using it.
 */
#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>

#include "game_mode.hpp"
#include "utils/fixed_point.hpp"

namespace devilution {

struct Player;

struct PartyLootRoll {
	uint8_t extraRolls;
	uint8_t remainder;
};

class PartyScaling {
public:
	/** Reject unsupported rules/counts. Empty levels require an existing high-water count. */
	static std::optional<PartyScaling> ForSession(SessionRules rules, size_t present, size_t previous = 0);
	uint8_t Participants() const { return participants_; }
	/** Numerator over four; the first four participants retain native balance. */
	uint8_t Numerator() const;
	/** Scale native, difficulty-adjusted health; player minions retain their spell stats. */
	Fixed26_6 HitPoints(Fixed26_6 native, bool playerMinion = false) const;
	/** Scale the native XP pool before contributor division and existing player caps. */
	uint32_t Experience(uint32_t native, bool playerMinion = false) const;
	/** One extra random loot roll per four credits; fixed rewards never earn credits. */
	std::optional<PartyLootRoll> LootRoll(uint8_t remainder, bool eligibleRandomLoot) const;

private:
	explicit PartyScaling(uint8_t participants)
	    : participants_(participants)
	{
	}
	uint8_t participants_;
};

/** Active human slots on a canonical multiplayer level, including dead/loading players. */
size_t CountLevelParticipants(std::span<const Player> players, uint8_t level);
/** Preserve completed damage fraction when a committed scale increases. Never revive a corpse. */
std::optional<Fixed26_6> RebaseMonsterHitPoints(Fixed26_6 current, Fixed26_6 previousMax, Fixed26_6 nextMax);

} // namespace devilution
