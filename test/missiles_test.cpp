#include <gmock/gmock.h>
#include <gtest/gtest.h>

#include <ankerl/unordered_dense.h>

#include "engine/assets.hpp"
#include "engine/random.hpp"
#include "headless_mode.hpp"
#include "levels/dun_tile_data.hpp"
#include "missiles.h"
#include "monster.h"

using namespace devilution;
using ::testing::AllOf;
using ::testing::Gt;
using ::testing::Lt;
using ::testing::Pair;
using ::testing::UnorderedElementsAre;

void TestArrowRotatesUniformly(Missile &missile, int startingFrame, int leftFrame, int rightFrame)
{
	ankerl::unordered_dense::map<int, unsigned> observed {};
	for (auto i = 0; i < 100; i++) {
		missile._miAnimFrame = startingFrame;
		TestRotateBlockedMissile(missile);
		observed[missile._miAnimFrame]++;
	}

	EXPECT_THAT(observed, UnorderedElementsAre(Pair(leftFrame, AllOf(Gt(30U), Lt(70U))), Pair(rightFrame, AllOf(Gt(30U), Lt(70U))))) << "Arrows should rotate either direction roughly 50% of the time";
}

void TestAnimatedMissileRotatesUniformly(Missile &missile, int startingDir, int leftDir, int rightDir)
{
	ankerl::unordered_dense::map<int, unsigned> observed {};
	for (auto i = 0; i < 100; i++) {
		missile.setFrameGroupRaw(startingDir);
		TestRotateBlockedMissile(missile);
		observed[missile.getFrameGroupRaw()]++;
	}

	EXPECT_THAT(observed, UnorderedElementsAre(Pair(leftDir, AllOf(Gt(30U), Lt(70U))), Pair(rightDir, AllOf(Gt(30U), Lt(70U))))) << "Animated missiles should rotate either direction roughly 50% of the time";
}

TEST(Missiles, RotateBlockedMissileArrow)
{
	Players.resize(1);
	MyPlayerId = 0;
	MyPlayer = &Players[MyPlayerId];
	*MyPlayer = {};
	LoadMissileData();

	const devilution::Player &player = Players[0];
	// missile can be a copy or a reference, there's no nullptr check and the functions that use it don't expect the instance to be part of a global structure so it doesn't really matter for this use.
	Missile missile = *AddMissile({ 0, 0 }, { 0, 0 }, Direction::South, MissileID::Arrow, TARGET_MONSTERS, player, 0, 0);

	// Arrows have a hardcoded frame count and use 1-indexed sprites
	EXPECT_EQ(missile._miAnimFrame, 1);

	TestArrowRotatesUniformly(missile, 5, 4, 6);
	TestArrowRotatesUniformly(missile, 1, 16, 2);
	TestArrowRotatesUniformly(missile, 16, 15, 1);

	// All other missiles use the number of 0-indexed sprites defined in MissileSpriteData
	missile = *AddMissile({ 0, 0 }, { 0, 0 }, Direction::South, MissileID::Firebolt, TARGET_MONSTERS, player, 0, 0);
	EXPECT_EQ(missile.getFrameGroupRaw(), 0);
	TestAnimatedMissileRotatesUniformly(missile, 5, 4, 6);
	TestAnimatedMissileRotatesUniformly(missile, 0, 15, 1);
	TestAnimatedMissileRotatesUniformly(missile, 15, 14, 0);
}

TEST(Missiles, GetDirection8)
{
	EXPECT_EQ(Direction::South, GetDirection({ 0, 0 }, { 15, 15 }));
	EXPECT_EQ(Direction::SouthWest, GetDirection({ 0, 0 }, { 0, 15 }));
	EXPECT_EQ(Direction::South, GetDirection({ 0, 0 }, { 8, 15 }));
	EXPECT_EQ(Direction::South, GetDirection({ 0, 0 }, { 8, 8 }));
	EXPECT_EQ(Direction::South, GetDirection({ 0, 0 }, { 15, 8 }));
	EXPECT_EQ(Direction::South, GetDirection({ 0, 0 }, { 15, 7 }));
	EXPECT_EQ(Direction::South, GetDirection({ 0, 0 }, { 11, 7 }));
	EXPECT_EQ(Direction::South, GetDirection({ 0, 0 }, { 8, 11 }));
	EXPECT_EQ(Direction::North, GetDirection({ 15, 15 }, { 0, 0 }));
	EXPECT_EQ(Direction::NorthEast, GetDirection({ 0, 15 }, { 0, 0 }));
	EXPECT_EQ(Direction::North, GetDirection({ 8, 15 }, { 0, 0 }));
	EXPECT_EQ(Direction::North, GetDirection({ 8, 8 }, { 0, 0 }));
	EXPECT_EQ(Direction::North, GetDirection({ 15, 8 }, { 0, 0 }));
	EXPECT_EQ(Direction::North, GetDirection({ 15, 7 }, { 0, 0 }));
	EXPECT_EQ(Direction::North, GetDirection({ 11, 7 }, { 0, 0 }));
	EXPECT_EQ(Direction::North, GetDirection({ 8, 11 }, { 0, 0 }));
	EXPECT_EQ(Direction::East, GetDirection({ 0, 15 }, { 15, 0 }));
	EXPECT_EQ(Direction::SouthEast, GetDirection({ 0, 0 }, { 15, 0 }));
	EXPECT_EQ(Direction::East, GetDirection({ 0, 8 }, { 15, 0 }));
	EXPECT_EQ(Direction::East, GetDirection({ 0, 8 }, { 8, 0 }));
	EXPECT_EQ(Direction::East, GetDirection({ 0, 15 }, { 8, 0 }));
	EXPECT_EQ(Direction::East, GetDirection({ 0, 15 }, { 7, 0 }));
	EXPECT_EQ(Direction::East, GetDirection({ 0, 11 }, { 7, 0 }));
	EXPECT_EQ(Direction::East, GetDirection({ 0, 8 }, { 11, 0 }));

	EXPECT_EQ(Direction::South, GetDirection({ 1, 1 }, { 2, 2 }));
	EXPECT_EQ(Direction::SouthWest, GetDirection({ 1, 1 }, { 1, 2 }));
	EXPECT_EQ(Direction::West, GetDirection({ 1, 1 }, { 0, 2 }));
	EXPECT_EQ(Direction::NorthWest, GetDirection({ 1, 1 }, { 0, 1 }));
	EXPECT_EQ(Direction::North, GetDirection({ 1, 1 }, { 0, 0 }));
	EXPECT_EQ(Direction::NorthEast, GetDirection({ 1, 1 }, { 1, 0 }));
	EXPECT_EQ(Direction::East, GetDirection({ 1, 1 }, { 2, 0 }));
	EXPECT_EQ(Direction::SouthEast, GetDirection({ 1, 1 }, { 2, 1 }));

	EXPECT_EQ(Direction::SouthWest, GetDirection({ 0, 0 }, { 0, 0 })) << "GetDirection is expected to default to Direction::SouthWest when the points occupy the same tile";
}

TEST(Missiles, GetDirection16)
{
	EXPECT_EQ(Direction16::South, GetDirection16({ 0, 0 }, { 15, 15 }));
	EXPECT_EQ(Direction16::SouthWest, GetDirection16({ 0, 0 }, { 0, 15 }));
	EXPECT_EQ(Direction16::South_SouthWest, GetDirection16({ 0, 0 }, { 8, 15 }));
	EXPECT_EQ(Direction16::South, GetDirection16({ 0, 0 }, { 8, 8 }));
	EXPECT_EQ(Direction16::South_SouthEast, GetDirection16({ 0, 0 }, { 15, 8 }));
	EXPECT_EQ(Direction16::South_SouthEast, GetDirection16({ 0, 0 }, { 15, 7 }));
	EXPECT_EQ(Direction16::South_SouthEast, GetDirection16({ 0, 0 }, { 11, 7 }));
	EXPECT_EQ(Direction16::South, GetDirection16({ 0, 0 }, { 8, 11 }));
	EXPECT_EQ(Direction16::North, GetDirection16({ 15, 15 }, { 0, 0 }));
	EXPECT_EQ(Direction16::NorthEast, GetDirection16({ 0, 15 }, { 0, 0 }));
	EXPECT_EQ(Direction16::North_NorthEast, GetDirection16({ 8, 15 }, { 0, 0 }));
	EXPECT_EQ(Direction16::North, GetDirection16({ 8, 8 }, { 0, 0 }));
	EXPECT_EQ(Direction16::North_NorthWest, GetDirection16({ 15, 8 }, { 0, 0 }));
	EXPECT_EQ(Direction16::North_NorthWest, GetDirection16({ 15, 7 }, { 0, 0 }));
	EXPECT_EQ(Direction16::North_NorthWest, GetDirection16({ 11, 7 }, { 0, 0 }));
	EXPECT_EQ(Direction16::North, GetDirection16({ 8, 11 }, { 0, 0 }));
	EXPECT_EQ(Direction16::East, GetDirection16({ 0, 15 }, { 15, 0 }));
	EXPECT_EQ(Direction16::SouthEast, GetDirection16({ 0, 0 }, { 15, 0 }));
	EXPECT_EQ(Direction16::East_SouthEast, GetDirection16({ 0, 8 }, { 15, 0 }));
	EXPECT_EQ(Direction16::East, GetDirection16({ 0, 8 }, { 8, 0 }));
	EXPECT_EQ(Direction16::East_NorthEast, GetDirection16({ 0, 15 }, { 8, 0 }));
	EXPECT_EQ(Direction16::East_NorthEast, GetDirection16({ 0, 15 }, { 7, 0 }));
	EXPECT_EQ(Direction16::East_NorthEast, GetDirection16({ 0, 11 }, { 7, 0 }));
	EXPECT_EQ(Direction16::East, GetDirection16({ 0, 8 }, { 11, 0 }));

	EXPECT_EQ(Direction16::South, GetDirection16({ 2, 2 }, { 3, 3 }));
	EXPECT_EQ(Direction16::South_SouthWest, GetDirection16({ 2, 2 }, { 3, 4 }));
	EXPECT_EQ(Direction16::SouthWest, GetDirection16({ 2, 2 }, { 2, 4 }));
	EXPECT_EQ(Direction16::West_SouthWest, GetDirection16({ 2, 2 }, { 1, 4 }));
	EXPECT_EQ(Direction16::West, GetDirection16({ 2, 2 }, { 1, 3 }));
	EXPECT_EQ(Direction16::West_NorthWest, GetDirection16({ 2, 2 }, { 0, 3 }));
	EXPECT_EQ(Direction16::NorthWest, GetDirection16({ 2, 2 }, { 0, 2 }));
	EXPECT_EQ(Direction16::North_NorthWest, GetDirection16({ 2, 2 }, { 0, 1 }));
	EXPECT_EQ(Direction16::North, GetDirection16({ 2, 2 }, { 1, 1 }));
	EXPECT_EQ(Direction16::North_NorthEast, GetDirection16({ 2, 2 }, { 1, 0 }));
	EXPECT_EQ(Direction16::NorthEast, GetDirection16({ 2, 2 }, { 2, 0 }));
	EXPECT_EQ(Direction16::East_NorthEast, GetDirection16({ 2, 2 }, { 3, 0 }));
	EXPECT_EQ(Direction16::East, GetDirection16({ 2, 2 }, { 3, 1 }));
	EXPECT_EQ(Direction16::East_SouthEast, GetDirection16({ 2, 2 }, { 4, 1 }));
	EXPECT_EQ(Direction16::SouthEast, GetDirection16({ 2, 2 }, { 4, 2 }));
	EXPECT_EQ(Direction16::South_SouthEast, GetDirection16({ 2, 2 }, { 4, 3 }));

	EXPECT_EQ(Direction16::South_SouthWest, GetDirection16({ 0, 0 }, { 0, 0 })) << "GetDirection16 is expected to default to Direction16::South_SouthWest when the points occupy the same tile";
}

namespace {

void CheckHeadlessCharge(_monster_id type, MonsterGraphic graphic, MonsterAIID ai)
{
	ASSERT_TRUE(HeadlessMode);
	LoadCoreArchives();
	LoadMonsterData();
	LoadMissileData();
	Missiles.clear();
	LevelMonsterTypes[0] = { };
	LevelMonsterTypes[0].type = type;
	auto &animation = LevelMonsterTypes[0].anims[static_cast<int>(graphic)];
	animation.frames = MonstersData[type].frames[static_cast<int>(graphic)];
	animation.rate = MonstersData[type].rate[static_cast<int>(graphic)];
	animation.width = 128;
	ASSERT_FALSE(animation.sprites.has_value());
	Monster &monster = Monsters[4];
	monster = { };
	monster.levelType = 0;
	monster.uniqueType = UniqueMonsterType::None;
	monster.mode = MonsterMode::Charge;
	monster.ai = ai;
	monster.position.tile = { 40, 40 };
	monster.position.future = monster.position.tile;
	SOLData[1] = TileProperties::None;
	for (int x = 36; x < 52; ++x) {
		for (int y = 36; y < 52; ++y) {
			dPiece[x][y] = 1;
			dPlayer[x][y] = 0;
			dMonster[x][y] = 0;
			dObject[x][y] = 0;
		}
	}
	Missile *missile = AddMissile({ 40, 40 }, { 46, 40 }, Direction::SouthEast, MissileID::Rhino, TARGET_PLAYERS, monster, 0, 0);
	ASSERT_NE(missile, nullptr);
	EXPECT_FALSE(missile->_miAnimData.has_value());
	EXPECT_EQ(missile->_miAnimLen, animation.frames);
	EXPECT_EQ(missile->_miAnimDelay, animation.rate);
	const auto before = missile->position.traveled;
	ProcessRhino(*missile);
	EXPECT_NE(missile->position.traveled, before);
	EXPECT_FALSE(missile->_miDelFlag);
	Missiles.clear();
	LevelMonsterTypes[0] = { };
	monster = { };
}

} // namespace

TEST(Missiles, SnakeChargeAdvancesWithoutGraphics)
{
	CheckHeadlessCharge(MT_NSNAKE, MonsterGraphic::Attack, MonsterAIID::Snake);
}

TEST(Missiles, HornedChargeAdvancesWithoutGraphics)
{
	CheckHeadlessCharge(MT_HORNED, MonsterGraphic::Special, MonsterAIID::Rhino);
}
