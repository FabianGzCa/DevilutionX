/**
 * @file monsters/party_scaling.hpp
 *
 * Deterministic balance revision 2 for extended parties: every player present on a
 * level from the second one on adds half of the native health, experience and random
 * loot, so each player keeps the solo pace of experience and loot per minute.
 * Diablo instead scales with every player, so the final fight lasts as long as alone.
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
	/** Reject unsupported rules and counts. Classic sessions always keep native balance. */
	static std::optional<PartyScaling> ForSession(SessionRules rules, size_t present);
	uint8_t Participants() const { return participants_; }
	/**
	 * Numerator over two: participants + 1 in extended sessions (twice the participants
	 * for Diablo) and 2, the native balance, in classic ones.
	 */
	uint8_t Numerator(bool diablo = false) const;
	/** Scale native, difficulty-adjusted health; player minions retain their spell stats. */
	Fixed26_6 HitPoints(Fixed26_6 native, bool playerMinion = false, bool diablo = false) const;
	/** Scale the native XP pool before contributor division and existing player caps. */
	uint32_t Experience(uint32_t native, bool playerMinion = false, bool diablo = false) const;
	/** One extra random loot roll per two credits; fixed rewards never earn credits. */
	std::optional<PartyLootRoll> LootRoll(uint8_t remainder, bool eligibleRandomLoot) const;

private:
	PartyScaling(uint8_t participants, bool extended)
	    : participants_(participants)
	    , extended_(extended)
	{
	}
	uint8_t participants_;
	bool extended_;
};

/** Active human slots on a canonical multiplayer level, including dead/loading players. */
size_t CountLevelParticipants(std::span<const Player> players, uint8_t level);
/** Preserve the completed damage fraction when the scale changes. Never revive or kill a monster. */
std::optional<Fixed26_6> RebaseMonsterHitPoints(Fixed26_6 current, Fixed26_6 previousMax, Fixed26_6 nextMax);

} // namespace devilution
