#include <array>
#include <bit>
#include <cstddef>
#include <cstdint>
#include <cstring>

#include <gtest/gtest.h>

#include "engine/assets.hpp"
#include "game_mode.hpp"
#include "levels/dun_tile_data.hpp"
#include "monster.h"
#include "multi.h"
#include "player.h"
#include "player_mask.hpp"
#include "portal.h"
#include "storm/storm_net.hpp"
#include "tables/playerdat.hpp"
#include "utils/fixed_point.hpp"

namespace devilution {
namespace {

class ClassicMultiplayerTest : public ::testing::Test {
protected:
	static void SetUpTestSuite()
	{
		LoadCoreArchives();
		LoadPlayerDataFiles();
	}

	void SetUp() override
	{
		gbIsMultiplayer = true;
		gbIsHellfire = false;
		gbIsSpawn = false;
		leveltype = DTYPE_CATHEDRAL;
		Players.clear();
		ResizePlayersForSession(SessionPlayerLimit);
		MyPlayerId = 0;
		MyPlayer = &Players[MyPlayerId];
		InspectPlayer = MyPlayer;
		for (Player &player : Players) {
			player.setCharacterLevel(1);
			player._pHitPoints = Fixed26_6::fromInt(100);
			player._pMaxHP = player._pHitPoints;
		}
		InitPortals();
		ASSERT_TRUE(SNetInitializeProvider(SELCONN_LOOPBACK, nullptr));
	}

	void TearDown() override
	{
		SNetDestroy();
		InitPortals();
		Players.clear();
		MyPlayer = nullptr;
		InspectPlayer = nullptr;
		MyPlayerId = 0;
		gbIsMultiplayer = false;
		leveltype = DTYPE_TOWN;
	}
};

TEST_F(ClassicMultiplayerTest, AdvertisesFourPlayerSlots)
{
	_SNETCAPS caps { };
	SNetGetProviderCaps(&caps);
	EXPECT_EQ(caps.maxplayers, 4U);
}

TEST_F(ClassicMultiplayerTest, PlayerLimitsAndReservedMonsterSlotRemainDistinct)
{
	EXPECT_EQ(ClassicPlayerLimit, 4);
	EXPECT_EQ(SessionPlayerLimit, 4);
	EXPECT_EQ(NoDeltaSender, SessionPlayerLimit);
	EXPECT_EQ(PlayerStorageCapacity, 8);
	EXPECT_EQ(ZharMonsterIndex, 4U);
	EXPECT_EQ(Players.size(), static_cast<size_t>(SessionPlayerLimit));
	EXPECT_GE(Players.capacity(), static_cast<size_t>(PlayerStorageCapacity));
}

TEST_F(ClassicMultiplayerTest, GrowingPlayerStorageDoesNotMoveSessionPointers)
{
	ResizePlayersForSession(1);
	MyPlayer = &Players[0];
	InspectPlayer = MyPlayer;
	Player *const localPlayer = MyPlayer;

	Players.resize(PlayerStorageCapacity);

	EXPECT_EQ(Players.size(), static_cast<size_t>(PlayerStorageCapacity));
	EXPECT_EQ(MyPlayer, localPlayer);
	EXPECT_EQ(InspectPlayer, localPlayer);
}

TEST_F(ClassicMultiplayerTest, TurnBuffersRespectTheSessionPlayerCount)
{
	for (const size_t count : { size_t { 1 }, static_cast<size_t>(SessionPlayerLimit) }) {
		SCOPED_TRACE(count);
		Players.resize(count);
		MyPlayer = &Players[0];
		char sentinel = 0;
		std::array<char *, ClassicPlayerLimit + 1> messages { };
		messages.fill(&sentinel);
		std::array<size_t, ClassicPlayerLimit + 1> sizes { };
		sizes.fill(99);
		std::array<uint32_t, ClassicPlayerLimit + 1> status { };

		ASSERT_TRUE(SNetReceiveTurns(ClassicPlayerLimit, messages.data(), sizes.data(), status.data()));

		for (size_t i = 0; i < count; i++) {
			EXPECT_EQ(messages[i], nullptr);
			EXPECT_EQ(sizes[i], 0U);
		}
		for (size_t i = count; i < messages.size(); i++) {
			EXPECT_EQ(messages[i], &sentinel);
			EXPECT_EQ(sizes[i], 99U);
		}
	}
}

TEST_F(ClassicMultiplayerTest, ExperienceIsSharedOnlyAmongContributors)
{
	for (unsigned mask = 0; mask < (1U << ClassicPlayerLimit); mask++) {
		SCOPED_TRACE(mask);
		const unsigned contributors = std::popcount(mask);
		for (uint8_t localPlayer = 0; localPlayer < ClassicPlayerLimit; localPlayer++) {
			SCOPED_TRACE(localPlayer);
			MyPlayerId = localPlayer;
			MyPlayer = &Players[localPlayer];
			for (Player &player : Players)
				player._pExperience = 0;

			const PlayerExperienceMask contributorsMask = PlayerExperienceMaskFromByte(static_cast<uint8_t>(mask));
			AddPlrMonstExper(1, 37, contributorsMask);

			const bool contributed = IsPlayerInExperienceMask(contributorsMask, localPlayer);
			const unsigned expected = contributed ? 37 / contributors : 0;
			EXPECT_EQ(MyPlayer->_pExperience, expected);
			for (const Player &player : Players) {
				if (&player != MyPlayer) {
					EXPECT_EQ(player._pExperience, 0U);
				}
			}
		}
	}
}

TEST_F(ClassicMultiplayerTest, ExperienceMasksDistributeAcrossAllEightPlayers)
{
	Players.resize(PlayerStorageCapacity);
	for (Player &player : Players) {
		player.setCharacterLevel(1);
		player._pHitPoints = Fixed26_6::fromInt(100);
	}
	for (unsigned rawMask = 0; rawMask < (1U << PlayerExperienceMaskBitCount); rawMask++) {
		SCOPED_TRACE(rawMask);
		const PlayerExperienceMask contributorsMask = PlayerExperienceMaskFromByte(static_cast<uint8_t>(rawMask));
		const unsigned contributorCount = std::popcount(rawMask);
		for (uint8_t localPlayer = 0; localPlayer < PlayerStorageCapacity; localPlayer++) {
			SCOPED_TRACE(localPlayer);
			MyPlayerId = localPlayer;
			MyPlayer = &Players[localPlayer];
			for (Player &player : Players)
				player._pExperience = 0;

			AddPlrMonstExper(1, 37, contributorsMask);

			const bool contributed = IsPlayerInExperienceMask(contributorsMask, localPlayer);
			const unsigned expected = contributed ? 37 / contributorCount : 0;
			EXPECT_EQ(MyPlayer->_pExperience, expected);
			for (const Player &player : Players) {
				if (&player != MyPlayer) {
					EXPECT_EQ(player._pExperience, 0U);
				}
			}
		}
	}
}

TEST_F(ClassicMultiplayerTest, ExperienceUsesCurrentParticipantCountNotStorageCapacity)
{
	ASSERT_GE(Players.capacity(), static_cast<size_t>(PlayerStorageCapacity));
	const PlayerExperienceMask contributorsMask = PlayerExperienceMaskFromByte(0xFF);
	for (const size_t participantCount : { size_t { 1 }, static_cast<size_t>(ClassicPlayerLimit), static_cast<size_t>(PlayerStorageCapacity) }) {
		SCOPED_TRACE(participantCount);
		Players.resize(participantCount);
		for (Player &player : Players) {
			player.setCharacterLevel(1);
			player._pHitPoints = Fixed26_6::fromInt(100);
			player._pExperience = 0;
		}
		MyPlayerId = static_cast<uint8_t>(participantCount - 1);
		MyPlayer = &Players[MyPlayerId];
		MyPlayer->_pExperience = 0;

		AddPlrMonstExper(1, 37, contributorsMask);

		EXPECT_EQ(MyPlayer->_pExperience, 37 / participantCount);
	}
}

TEST_F(ClassicMultiplayerTest, MonsterDamageMaskTracksAllEightPlayers)
{
	Players.resize(PlayerStorageCapacity);
	Monster monster { };
	for (size_t playerId = 0; playerId < Players.size(); playerId++) {
		monster.tag(Players[playerId]);
		EXPECT_EQ(PlayerExperienceMaskToByte(monster.whoHit), (1U << (playerId + 1)) - 1);
	}
}

TEST_F(ClassicMultiplayerTest, ExperienceMaskByteAndSyncPacketRoundTripAllValues)
{
	for (unsigned rawMask = 0; rawMask < (1U << PlayerExperienceMaskBitCount); rawMask++) {
		SCOPED_TRACE(rawMask);
		const uint8_t rawByte = static_cast<uint8_t>(rawMask);
		const PlayerExperienceMask mask = PlayerExperienceMaskFromByte(rawByte);
		EXPECT_EQ(PlayerExperienceMaskToByte(mask), rawByte);
		const PlayerExperienceMask complement = PlayerExperienceMaskFromByte(static_cast<uint8_t>(0xFFU ^ rawMask));
		EXPECT_EQ(PlayerExperienceMaskToByte(MergePlayerExperienceMasks(mask, complement)), 0xFFU);

		TSyncMonster sent { };
		sent.mWhoHit = mask;
		std::array<std::byte, sizeof(TSyncMonster)> packet { };
		std::memcpy(packet.data(), &sent, sizeof(sent));
		EXPECT_EQ(packet[offsetof(TSyncMonster, mWhoHit)], std::byte { rawByte });

		TSyncMonster received { };
		std::memcpy(&received, packet.data(), sizeof(received));
		EXPECT_EQ(PlayerExperienceMaskToByte(received.mWhoHit), rawByte);
	}
}

TEST_F(ClassicMultiplayerTest, RecipientMasksCoverAllEightPlayerBits)
{
	PlayerRecipientMask allRecipients { 0 };
	for (size_t playerId = 0; playerId < PlayerStorageCapacity; playerId++)
		allRecipients = AddPlayerToRecipientMask(allRecipients, playerId);

	EXPECT_EQ(allRecipients.bits, 0xFFU);
	EXPECT_EQ(AllPlayerRecipients().bits, UINT32_MAX);
	EXPECT_FALSE(IsPlayerInRecipientMask(allRecipients, PlayerStorageCapacity));

	for (unsigned rawMask = 0; rawMask < (1U << PlayerStorageCapacity); rawMask++) {
		SCOPED_TRACE(rawMask);
		const PlayerRecipientMask recipients { rawMask };
		for (size_t playerId = 0; playerId < PlayerStorageCapacity; playerId++) {
			const bool expected = (rawMask & (1U << playerId)) != 0;
			EXPECT_EQ(IsPlayerInRecipientMask(recipients, playerId), expected);
		}
	}
}

TEST_F(ClassicMultiplayerTest, MultiplayerExperienceRetainsThePowerLevelingCap)
{
	AddPlrMonstExper(1, 10000, PlayerExperienceMaskFromByte(1));
	EXPECT_EQ(MyPlayer->_pExperience, 100U);
}

TEST_F(ClassicMultiplayerTest, SinglePlayerIgnoresUnusedContributorSlots)
{
	Players.resize(1);
	MyPlayer = &Players[0];
	gbIsMultiplayer = false;

	AddPlrMonstExper(1, 150, PlayerExperienceMaskFromByte(15));

	EXPECT_EQ(MyPlayer->_pExperience, 150U);
}

TEST_F(ClassicMultiplayerTest, DeadPlayersDoNotGainExperienceInTheDungeon)
{
	MyPlayer->_pHitPoints = Fixed26_6::fromInt(0);
	AddPlrMonstExper(1, 37, PlayerExperienceMaskFromByte(1));
	EXPECT_EQ(MyPlayer->_pExperience, 0U);
}

TEST_F(ClassicMultiplayerTest, PortalTownArrivalPositionsRemainClassic)
{
	leveltype = DTYPE_TOWN;
	const std::array<Point, ClassicPlayerLimit> expected = { { { 58, 41 }, { 60, 41 }, { 62, 41 }, { 64, 41 } } };
	for (size_t owner = 0; owner < expected.size(); owner++) {
		SCOPED_TRACE(owner);
		SetCurrentPortal(owner);
		GetPortalLvlPos();
		EXPECT_EQ(ViewPosition, expected[owner]);
	}
}

TEST_F(ClassicMultiplayerTest, PortalArrivalDistinguishesTheOwnerFromOtherPlayers)
{
	for (size_t owner = 0; owner < ClassicPlayerLimit; owner++) {
		SCOPED_TRACE(owner);
		const Point destination { 40, 45 };
		SetPortalStats(static_cast<int>(owner), true, destination, 5, DTYPE_CATACOMBS, false);
		SetCurrentPortal(owner);
		for (uint8_t visitor = 0; visitor < ClassicPlayerLimit; visitor++) {
			SCOPED_TRACE(visitor);
			MyPlayerId = visitor;
			MyPlayer = &Players[visitor];
			GetPortalLvlPos();
			const Point expected = visitor == owner ? destination : destination + Displacement { 1, 1 };
			EXPECT_EQ(ViewPosition, expected);
		}
	}
}

TEST_F(ClassicMultiplayerTest, ClosingAPortalDoesNotCloseOtherPlayersPortals)
{
	for (int owner = 0; owner < ClassicPlayerLimit; owner++)
		SetPortalStats(owner, true, { 40 + owner * 4, 45 }, 5, DTYPE_CATACOMBS, false);

	DeactivatePortal(Players[2]);

	for (int owner = 0; owner < ClassicPlayerLimit; owner++) {
		SCOPED_TRACE(owner);
		EXPECT_EQ(PosOkPortal(5, { 40 + owner * 4, 45 }), owner != 2);
		EXPECT_FALSE(PosOkPortal(6, { 40 + owner * 4, 45 }));
	}
}

} // namespace
} // namespace devilution
