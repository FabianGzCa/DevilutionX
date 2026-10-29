#include <array>
#include <cstdint>
#include <limits>
#include <vector>

#include <gtest/gtest.h>

#include "monsters/party_scaling.hpp"
#include "msg.h"
#include "player.h"

namespace devilution {
namespace {

class PartyScalingTest : public ::testing::TestWithParam<unsigned> { };

TEST_P(PartyScalingTest, MatchesGoldenHealthAndExperienceTablesAfterNativeDifficulty)
{
	const auto scale = PartyScaling::ForSession(ExtendedSessionRules, GetParam());
	ASSERT_TRUE(scale);
	const unsigned index = GetParam() - 1;
	constexpr int32_t Health[3][8] = {
		{ 6464, 9696, 12928, 16160, 19392, 22624, 25856, 29088 },
		{ 25792, 38688, 51584, 64480, 77376, 90272, 103168, 116064 },
		{ 38656, 57984, 77312, 96640, 115968, 135296, 154624, 173952 },
	};
	constexpr uint32_t Experience[3][8] = {
		{ 1003, 1504, 2006, 2507, 3009, 3510, 4012, 4513 },
		{ 4006, 6009, 8012, 10015, 12018, 14021, 16024, 18027 },
		{ 8012, 12018, 16024, 20030, 24036, 28042, 32048, 36054 },
	};
	for (unsigned difficulty = 0; difficulty < 3; ++difficulty) {
		SCOPED_TRACE(difficulty);
		EXPECT_EQ(scale->HitPoints(Fixed26_6::fromRaw(Health[difficulty][0])).raw(), Health[difficulty][index]);
		EXPECT_EQ(scale->Experience(Experience[difficulty][0]), Experience[difficulty][index]);
		EXPECT_EQ(scale->HitPoints(Fixed26_6::fromRaw(Health[difficulty][0]), true).raw(), Health[difficulty][0]);
		EXPECT_EQ(scale->Experience(Experience[difficulty][0], true), Experience[difficulty][0]);
	}
}

TEST_P(PartyScalingTest, RoundsHealthUpExperienceDownAndSaturatesWithoutWrapping)
{
	const auto scale = PartyScaling::ForSession(ExtendedSessionRules, GetParam());
	ASSERT_TRUE(scale);
	constexpr int32_t Health[] = { 5, 8, 10, 13, 15, 18, 20, 23 };
	constexpr uint32_t Experience[] = { 5, 7, 10, 12, 15, 17, 20, 22 };
	const unsigned index = GetParam() - 1;
	EXPECT_EQ(scale->HitPoints(Fixed26_6::fromRaw(5)).raw(), Health[index]);
	EXPECT_EQ(scale->Experience(5), Experience[index]);
	for (int32_t hp : { -64, -1, 0 })
		EXPECT_EQ(scale->HitPoints(Fixed26_6::fromRaw(hp)).raw(), hp);
	EXPECT_EQ(scale->Experience(0), 0U);
	EXPECT_EQ(scale->HitPoints(Fixed26_6::fromRaw(std::numeric_limits<int32_t>::max())).raw(), std::numeric_limits<int32_t>::max());
	EXPECT_EQ(scale->Experience(std::numeric_limits<uint32_t>::max()), std::numeric_limits<uint32_t>::max());
	EXPECT_EQ(scale->HitPoints(Fixed26_6::fromRaw(1), true).raw(), 1);
}

TEST_P(PartyScalingTest, LootCreditsConserveExpectedRollsAndExcludeFixedRewards)
{
	const auto scale = PartyScaling::ForSession(ExtendedSessionRules, GetParam());
	ASSERT_TRUE(scale);
	uint8_t remainder = 0;
	unsigned extra = 0;
	for (unsigned kill = 0; kill < 1000; ++kill) {
		const auto fixedReward = scale->LootRoll(remainder, false);
		ASSERT_TRUE(fixedReward);
		EXPECT_EQ(fixedReward->extraRolls, 0);
		EXPECT_EQ(fixedReward->remainder, remainder);
		const auto randomLoot = scale->LootRoll(remainder, true);
		ASSERT_TRUE(randomLoot);
		extra += randomLoot->extraRolls;
		remainder = randomLoot->remainder;
		EXPECT_LE(randomLoot->extraRolls, GetParam() / 2);
		EXPECT_LT(remainder, 2);
	}
	// (N + 1) / 2 rolls per kill: the native one plus (N - 1) / 2 extra.
	EXPECT_EQ(extra, 500 * (GetParam() - 1));
	EXPECT_EQ(remainder, 0);
	EXPECT_FALSE(scale->LootRoll(2, true));
	EXPECT_FALSE(scale->LootRoll(255, false));
}

TEST_P(PartyScalingTest, DiabloScalesWithEveryPlayerSoTheFinalFightLastsAsLongAsAlone)
{
	const auto scale = PartyScaling::ForSession(ExtendedSessionRules, GetParam());
	ASSERT_TRUE(scale);
	EXPECT_EQ(scale->HitPoints(Fixed26_6::fromRaw(1000), false, true).raw(), 1000 * static_cast<int32_t>(GetParam()));
	EXPECT_EQ(scale->Experience(1001, false, true), 1001 * GetParam());
	EXPECT_EQ(scale->HitPoints(Fixed26_6::fromRaw(1000), true, true).raw(), 1000);
	if (GetParam() <= 4) {
		const auto classic = PartyScaling::ForSession(ClassicSessionRules, GetParam());
		ASSERT_TRUE(classic);
		EXPECT_EQ(classic->HitPoints(Fixed26_6::fromRaw(1000), false, true).raw(), 1000);
		EXPECT_EQ(classic->Experience(1001, false, true), 1001U);
	}
}

TEST(PartyScalingRules, ClassicSessionsAndLonePlayersKeepNativeBalanceAtEveryRemainder)
{
	for (unsigned count = 1; count <= 4; ++count) {
		for (SessionRules rules : { ClassicSessionRules, ExtendedSessionRules }) {
			if (rules == ExtendedSessionRules && count != 1)
				continue;
			const auto scale = PartyScaling::ForSession(rules, count);
			ASSERT_TRUE(scale);
			for (int32_t hp : { 1, 5, 64, 867, std::numeric_limits<int32_t>::max() })
				EXPECT_EQ(scale->HitPoints(Fixed26_6::fromRaw(hp)).raw(), hp);
			for (uint32_t xp : { 0U, 1U, 1003U, std::numeric_limits<uint32_t>::max() })
				EXPECT_EQ(scale->Experience(xp), xp);
			for (uint8_t credit = 0; credit < 2; ++credit) {
				const auto loot = scale->LootRoll(credit, true);
				ASSERT_TRUE(loot);
				EXPECT_EQ(loot->extraRolls, 0);
				EXPECT_EQ(loot->remainder, credit);
			}
		}
	}
}

INSTANTIATE_TEST_SUITE_P(OneThroughEight, PartyScalingTest, ::testing::Range(1U, 9U));

TEST(PartyScalingRules, RejectsInvalidCountsAndUnsupportedNegotiatedRules)
{
	for (size_t count : { size_t { 0 }, size_t { 9 }, std::numeric_limits<size_t>::max() })
		EXPECT_FALSE(PartyScaling::ForSession(ExtendedSessionRules, count));
	for (unsigned count = 5; count <= 8; ++count)
		EXPECT_FALSE(PartyScaling::ForSession(ClassicSessionRules, count));
	EXPECT_FALSE(PartyScaling::ForSession(ClassicSessionRules, 0));
	auto unsupported = ExtendedSessionRules;
	++unsupported.balanceRevision;
	EXPECT_FALSE(PartyScaling::ForSession(unsupported, 8));
	unsupported = ExtendedSessionRules;
	--unsupported.balanceRevision;
	EXPECT_FALSE(PartyScaling::ForSession(unsupported, 8));
	unsupported = ExtendedSessionRules;
	++unsupported.protocolRevision;
	EXPECT_FALSE(PartyScaling::ForSession(unsupported, 8));
}

TEST(PartyScalingRules, ScaleFollowsThePlayersPresentWhenThePartyShrinks)
{
	constexpr unsigned Present[] = { 1, 4, 8, 5, 2, 1 };
	constexpr int32_t Health[] = { 1000, 2500, 4500, 3000, 1500, 1000 };
	for (unsigned i = 0; i < std::size(Present); ++i) {
		const auto scale = PartyScaling::ForSession(ExtendedSessionRules, Present[i]);
		ASSERT_TRUE(scale);
		EXPECT_EQ(scale->Participants(), Present[i]);
		EXPECT_EQ(scale->HitPoints(Fixed26_6::fromRaw(1000)).raw(), Health[i]);
	}
}

TEST(PartyScalingRules, CountsHumanSlotsByLevelRegardlessOfClassLifeOrLoadingState)
{
	std::vector<Player> players(8);
	for (unsigned mask = 0; mask < 256; ++mask) {
		size_t expected = 0;
		for (unsigned id = 0; id < players.size(); ++id) {
			auto &player = players[id];
			player.plractive = (mask & (1U << id)) != 0;
			player.setLevel(static_cast<uint8_t>(id < 6 ? 1 : 2));
			player._pLvlChanging = (id % 2) != 0;
			player.pLvlLoad = id % 3;
			player._pHitPoints = Fixed26_6::fromInt(id % 2 == 0 ? 0 : 100);
			player._pClass = static_cast<HeroClass>(id % 3);
			expected += player.plractive && id < 6;
		}
		EXPECT_EQ(CountLevelParticipants(players, 1), expected);
		EXPECT_EQ(CountLevelParticipants(players, 0), 0U);
	}
	players[0].plractive = true;
	players[0].setLevel(SL_SKELKING);
	EXPECT_EQ(CountLevelParticipants(players, GetLevelForMultiplayer(players[0])), 1U);
	EXPECT_EQ(CountLevelParticipants(players, 255), 0U);
	EXPECT_EQ(CountLevelParticipants({}, 1), 0U);
}

TEST(PartyScalingRules, RescalingPreservesFractionalProgressAndNeverRevivesOrKills)
{
	const auto oldMax = Fixed26_6::fromInt(100);
	const auto nextMax = Fixed26_6::fromInt(125);
	const auto rebased = RebaseMonsterHitPoints(Fixed26_6::fromInt(30), oldMax, nextMax);
	ASSERT_TRUE(rebased);
	EXPECT_EQ(rebased->raw(), 2400); // 37.5 / 125 == 30 / 100.
	const auto shrunk = RebaseMonsterHitPoints(Fixed26_6::fromInt(75), nextMax, oldMax);
	ASSERT_TRUE(shrunk);
	EXPECT_EQ(shrunk->raw(), Fixed26_6::fromInt(60).raw());
	const auto lastPoint = RebaseMonsterHitPoints(Fixed26_6::fromInt(1), Fixed26_6::fromInt(450), oldMax);
	ASSERT_TRUE(lastPoint);
	EXPECT_EQ(lastPoint->raw(), Fixed26_6::fromInt(1).raw());
	for (int32_t hp : { -64, -1, 0, 1, 32, 63 }) {
		const auto corpse = RebaseMonsterHitPoints(Fixed26_6::fromRaw(hp), oldMax, nextMax);
		ASSERT_TRUE(corpse);
		EXPECT_EQ(corpse->raw(), hp);
	}
	for (int32_t hp = 64; hp <= 1000; ++hp) {
		const auto result = RebaseMonsterHitPoints(Fixed26_6::fromRaw(hp), Fixed26_6::fromRaw(1000), Fixed26_6::fromRaw(1750));
		ASSERT_TRUE(result);
		EXPECT_GE(static_cast<int64_t>(result->raw()) * 1000, static_cast<int64_t>(hp) * 1750);
		EXPECT_LT(static_cast<int64_t>(result->raw()) * 1000 - static_cast<int64_t>(hp) * 1750, 1000);
	}
}

TEST(PartyScalingRules, RebaseRejectsInvalidInputsAndHandlesTheLargestHealthValues)
{
	const auto maximum = Fixed26_6::fromRaw(std::numeric_limits<int32_t>::max());
	const auto full = RebaseMonsterHitPoints(maximum, maximum, maximum);
	ASSERT_TRUE(full);
	EXPECT_EQ(full->raw(), maximum.raw());
	EXPECT_FALSE(RebaseMonsterHitPoints(Fixed26_6::fromInt(1), Fixed26_6::fromInt(0), maximum));
	EXPECT_FALSE(RebaseMonsterHitPoints(Fixed26_6::fromInt(2), Fixed26_6::fromInt(1), maximum));
	EXPECT_FALSE(RebaseMonsterHitPoints(Fixed26_6::fromInt(1), Fixed26_6::fromInt(2), Fixed26_6::fromInt(0)));
}

} // namespace
} // namespace devilution
