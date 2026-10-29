#include "monsters/party_scaling.hpp"

#include <algorithm>
#include <limits>

#include "msg.h"
#include "player.h"

namespace devilution {

std::optional<PartyScaling> PartyScaling::ForSession(SessionRules rules, size_t present)
{
	const size_t capacity = rules == ClassicSessionRules ? 4 : rules == ExtendedSessionRules ? ExtendedSessionRules.maxPlayers
	                                                                                         : 0;
	if (capacity == 0 || present > capacity || present == 0)
		return std::nullopt;
	return PartyScaling(static_cast<uint8_t>(present), rules == ExtendedSessionRules);
}

uint8_t PartyScaling::Numerator(bool diablo) const
{
	if (!extended_)
		return 2;
	return diablo ? 2 * participants_ : participants_ + 1;
}

Fixed26_6 PartyScaling::HitPoints(Fixed26_6 native, bool playerMinion, bool diablo) const
{
	if (playerMinion || native.raw() <= 0)
		return native;
	const uint64_t scaled = (static_cast<uint64_t>(native.raw()) * Numerator(diablo) + 1) / 2;
	return Fixed26_6::fromRaw(static_cast<int32_t>(std::min<uint64_t>(scaled, std::numeric_limits<int32_t>::max())));
}

uint32_t PartyScaling::Experience(uint32_t native, bool playerMinion, bool diablo) const
{
	if (playerMinion)
		return native;
	const uint64_t scaled = static_cast<uint64_t>(native) * Numerator(diablo) / 2;
	return static_cast<uint32_t>(std::min<uint64_t>(scaled, std::numeric_limits<uint32_t>::max()));
}

std::optional<PartyLootRoll> PartyScaling::LootRoll(uint8_t remainder, bool eligibleRandomLoot) const
{
	if (remainder >= 2)
		return std::nullopt;
	const unsigned credits = remainder + (eligibleRandomLoot ? Numerator() - 2 : 0);
	return PartyLootRoll { static_cast<uint8_t>(credits / 2), static_cast<uint8_t>(credits % 2) };
}

size_t CountLevelParticipants(std::span<const Player> players, uint8_t level)
{
	if (level == 0 || !IsValidLevelForMultiplayer(level))
		return 0;
	return std::count_if(players.begin(), players.end(), [=](const Player &player) {
		return player.plractive && GetLevelForMultiplayer(player) == level;
	});
}

std::optional<Fixed26_6> RebaseMonsterHitPoints(Fixed26_6 current, Fixed26_6 previousMax, Fixed26_6 nextMax)
{
	if (previousMax.raw() <= 0 || nextMax.raw() <= 0 || current > previousMax)
		return std::nullopt;
	if (current.whole() <= 0)
		return current;
	const uint64_t numerator = static_cast<uint64_t>(current.raw()) * nextMax.raw() + previousMax.raw() - 1;
	return Fixed26_6::fromRaw(std::max<int32_t>(Fixed26_6::fromInt(1).raw(), static_cast<int32_t>(numerator / previousMax.raw())));
}

} // namespace devilution
