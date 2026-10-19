#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <utility>
#include <vector>

#include <gtest/gtest.h>

#include "engine/assets.hpp"
#include "engine/load_file.hpp"
#include "game_mode.hpp"
#include "levels/dun_tile_data.hpp"
#include "levels/tile_properties.hpp"
#include "levels/town.h"
#include "levels/trigs.h"
#include "lighting.h"
#include "missiles.h"
#include "multi.h"
#include "player.h"
#include "portal.h"
#include "portals/validation.hpp"
#include "quests.h"
#include "storm/storm_net.hpp"
#include "tables/misdat.h"
#include "tables/playerdat.hpp"
#include "towners.h"
#include "utils/endian_swap.hpp"

namespace devilution {
namespace {

constexpr std::array<Point, 8> TownPortals = { { { 57, 40 }, { 59, 40 }, { 61, 40 }, { 63, 40 }, { 57, 44 }, { 59, 44 }, { 61, 44 }, { 63, 44 } } };
constexpr std::array<Displacement, 8> Neighbours = { { { -1, -1 }, { 0, -1 }, { 1, -1 }, { -1, 0 }, { 1, 0 }, { -1, 1 }, { 0, 1 }, { 1, 1 } } };

class PortalTest : public ::testing::Test {
protected:
	static void SetUpTestSuite()
	{
		LoadCoreArchives();
		LoadPlayerDataFiles();
		LoadQuestData();
		LoadTextData();
		LoadMissileData();
	}

	void SetUp() override
	{
		gbIsMultiplayer = true;
		gbIsHellfire = false;
		gbIsSpawn = false;
		gbBufferMsgs = 0;
		gbActivePlayers = PlayerStorageCapacity;
		gbRunGame = false;
		Players.clear();
		ResizePlayersForSession(PlayerStorageCapacity);
		MyPlayerId = 0;
		MyPlayer = &Players[0];
		InspectPlayer = MyPlayer;
		for (Player &player : Players) {
			player.plractive = true;
			player.setLevel(1);
			player.position.tile = { 75, 68 };
		}
		currlevel = 0;
		leveltype = DTYPE_TOWN;
		setlevel = false;
		InitGameInfo();
		sgGameInitInfo.rules = ExtendedSessionRules;
		InitQuests();
		InitPortals();
		InitNoTriggers();
		InitLighting();
		Missiles.clear();
		std::memset(dPlayer, 0, sizeof(dPlayer));
		std::memset(dMonster, 0, sizeof(dMonster));
		std::memset(dObject, 0, sizeof(dObject));
		std::memset(dFlags, 0, sizeof(dFlags));
		std::fill_n(sgwPackPlrOffsetTbl, PlayerStorageCapacity, 0);
		ASSERT_TRUE(SNetInitializeProvider(SELCONN_LOOPBACK, &sgGameInitInfo));
		delta_init();
	}

	void TearDown() override
	{
		Missiles.clear();
		InitLighting();
		InitPortals();
		delta_init();
		SNetDestroy();
		Players.clear();
		MyPlayer = nullptr;
		InspectPlayer = nullptr;
		MyPlayerId = 0;
		gbIsMultiplayer = false;
		gbIsHellfire = false;
		gbIsSpawn = false;
		setlevel = false;
		leveltype = DTYPE_TOWN;
	}

	template <typename Command>
	static void Receive(uint8_t sender, const Command &command)
	{
		EXPECT_EQ(ParseCmd(sender, reinterpret_cast<const TCmd *>(&command), sizeof(command)), sizeof(command));
	}

	static TCmdLocParam3 OpenCommand(Point position, uint16_t level = 1, uint16_t type = DTYPE_CATHEDRAL, uint16_t special = 0)
	{
		return { CMD_ACTIVATEPORTAL, static_cast<uint8_t>(position.x), static_cast<uint8_t>(position.y), Swap16LE(level), Swap16LE(type), Swap16LE(special) };
	}
};

TEST_F(PortalTest, AllOwnersAndVisitorsUseTheCorrectDestinationAndTownArrival)
{
	RecordProperty("owner_visitor_level_combinations", 2560);
	for (const bool hellfire : { false, true }) {
		gbIsHellfire = hellfire;
		for (int level = 1; level <= (hellfire ? 24 : 16); ++level) {
			SCOPED_TRACE(level);
			for (size_t owner = 0; owner < Players.size(); ++owner) {
				SCOPED_TRACE(owner);
				const Point destination { 40 + static_cast<int>(owner), 50 };
				for (uint8_t visitor = 0; visitor < Players.size(); ++visitor) {
					SCOPED_TRACE(visitor);
					MyPlayerId = visitor;
					MyPlayer = &Players[visitor];
					leveltype = DTYPE_TOWN;
					SetPortalStats(static_cast<int>(owner), true, destination, level, GetLevelType(level), false);
					SetCurrentPortal(owner);
					GetPortalLevel();
					EXPECT_EQ(currlevel, level);
					EXPECT_EQ(MyPlayer->plrlevel, level);
					EXPECT_FALSE(setlevel);
					EXPECT_EQ(Portals[owner].open, visitor != owner);
					GetPortalLvlPos();
					EXPECT_EQ(ViewPosition, visitor == owner ? destination : destination + Displacement(1, 1));
					GetPortalLevel();
					EXPECT_EQ(currlevel, 0);
					EXPECT_EQ(MyPlayer->plrlevel, 0);
					GetPortalLvlPos();
					EXPECT_EQ(ViewPosition, TownPortals[owner] + Displacement(1, 1));
				}
			}
		}
	}
}

TEST_F(PortalTest, QuestAndArenaDestinationsKeepTheirLevelIdentity)
{
	const std::array<std::pair<_setlevels, dungeon_type>, 7> destinations = { { { SL_SKELKING, DTYPE_CATHEDRAL }, { SL_BONECHAMB, DTYPE_CATACOMBS }, { SL_POISONWATER, DTYPE_CAVES }, { SL_VILEBETRAYER, DTYPE_CATHEDRAL }, { SL_ARENA_CHURCH, DTYPE_CATHEDRAL }, { SL_ARENA_HELL, DTYPE_HELL }, { SL_ARENA_CIRCLE_OF_LIFE, DTYPE_HELL } } };
	for (size_t owner = 0; owner < Players.size(); ++owner) {
		SCOPED_TRACE(owner);
		for (const auto &[level, type] : destinations) {
			SCOPED_TRACE(level);
			leveltype = DTYPE_TOWN;
			SetPortalStats(static_cast<int>(owner), true, { 40, 50 }, level, type, true);
			ASSERT_TRUE(Portals[owner].open);
			SetCurrentPortal(owner);
			GetPortalLevel();
			EXPECT_TRUE(setlevel);
			EXPECT_TRUE(MyPlayer->plrIsOnSetLevel);
			EXPECT_EQ(setlvlnum, level);
			EXPECT_EQ(leveltype, type);
			EXPECT_EQ(setlvltype, type);
			GetPortalLevel();
			EXPECT_FALSE(setlevel);
			EXPECT_EQ(MyPlayer->plrlevel, 0);
		}
	}
}

TEST_F(PortalTest, SyncDoesNotReadPlayerSlotsAbsentFromTheSession)
{
	for (int owner = 0; owner < MAXPORTAL; ++owner)
		SetPortalStats(owner, true, { 40 + owner, 50 }, 1, DTYPE_CATHEDRAL, false);
	for (const size_t count : { size_t { 1 }, size_t { 4 }, size_t { 8 } }) {
		SCOPED_TRACE(count);
		Missiles.clear();
		ResizePlayersForSession(count);
		MyPlayer = &Players[0];
		SyncPortals();
		EXPECT_EQ(Missiles.size(), count);
	}
}

TEST_F(PortalTest, EightVisiblePortalsReplaceAndCloseIndependently)
{
	for (size_t owner = 0; owner < Players.size(); ++owner)
		SetPortalStats(static_cast<int>(owner), true, { 40 + static_cast<int>(owner), 50 }, 1, DTYPE_CATHEDRAL, false);
	SyncPortals();
	ASSERT_EQ(Missiles.size(), 8U);
	for (uint8_t owner = 0; owner < Players.size(); ++owner) {
		SCOPED_TRACE(owner);
		MyPlayerId = (owner + 1) % Players.size();
		MyPlayer = &Players[MyPlayerId];
		Receive(owner, OpenCommand({ 45 + owner, 55 }));
		EXPECT_EQ(Portals[owner].position, Point(45 + owner, 55));
		EXPECT_EQ(std::count_if(Missiles.begin(), Missiles.end(), [owner](const Missile &missile) { return missile._misource == owner && missile.duration > 0; }), 1);
		for (const Missile &missile : Missiles) {
			if (missile.duration > 0) {
				EXPECT_EQ(missile.position.tile, TownPortals[missile._misource]);
			}
		}
		Receive(owner, TCmd { CMD_DEACTIVATEPORTAL });
		EXPECT_FALSE(Portals[owner].open);
		EXPECT_EQ(std::count_if(Missiles.begin(), Missiles.end(), [owner](const Missile &missile) { return missile._misource == owner; }), 0);
	}
	EXPECT_TRUE(Missiles.empty());
}

TEST_F(PortalTest, EightDungeonPortalLightsAreReplacedAndReleasedWithTheirOwners)
{
	currlevel = 1;
	leveltype = DTYPE_CATHEDRAL;
	std::memset(dPiece, 0, sizeof(dPiece));
	const TileProperties savedProperties = SOLData[0];
	SOLData[0] = TileProperties::None;
	for (size_t owner = 0; owner < Players.size(); ++owner)
		SetPortalStats(static_cast<int>(owner), true, { 30 + static_cast<int>(owner) * 4, 40 }, 1, DTYPE_CATHEDRAL, false);
	SyncPortals();
	ASSERT_EQ(Missiles.size(), 8U);
	EXPECT_EQ(ActiveLightCount, 8);
	for (uint8_t owner = 0; owner < Players.size(); ++owner) {
		SCOPED_TRACE(owner);
		MyPlayerId = (owner + 1) % Players.size();
		MyPlayer = &Players[MyPlayerId];
		const Point replacement { 30 + owner * 4, 50 };
		Receive(owner, OpenCommand(replacement));
		ProcessLightList();
		ProcessMissiles();
		ProcessLightList();
		EXPECT_EQ(ActiveLightCount, 8 - owner);
		const auto missile = std::find_if(Missiles.begin(), Missiles.end(), [owner](const Missile &candidate) { return candidate._misource == owner; });
		ASSERT_NE(missile, Missiles.end());
		EXPECT_EQ(missile->position.tile, replacement);
		EXPECT_EQ(Missiles.size(), 8U - owner);
		Receive(owner, OpenCommand({ 40, 50 }, 5, DTYPE_CATACOMBS));
		ProcessLightList();
		EXPECT_EQ(ActiveLightCount, 7 - owner);
		EXPECT_EQ(Missiles.size(), 7U - owner);
		EXPECT_TRUE(Portals[owner].open);
		EXPECT_EQ(Portals[owner].level, 5);
		Receive(owner, TCmd { CMD_DEACTIVATEPORTAL });
	}
	SOLData[0] = savedProperties;
}

TEST_F(PortalTest, DepartureClosesEveryOwnerAndSlotReuseStartsWithoutAPortal)
{
	for (uint8_t owner = 0; owner < Players.size(); ++owner) {
		SCOPED_TRACE(owner);
		MyPlayerId = (owner + 1) % Players.size();
		MyPlayer = &Players[MyPlayerId];
		Receive(owner, OpenCommand({ 40 + owner, 50 }));
		ASSERT_TRUE(Portals[owner].open);
		multi_player_left(owner, leaveinfo_t::LEAVE_DROP);
		EXPECT_FALSE(Portals[owner].open);
		EXPECT_TRUE(Missiles.empty());
		Players[owner].plractive = true;
		Players[owner].setLevel(1);
		Receive(owner, OpenCommand({ 50 + owner, 60 }));
		EXPECT_TRUE(Portals[owner].open);
		EXPECT_EQ(Portals[owner].position, Point(50 + owner, 60));
		Receive(owner, TCmd { CMD_DEACTIVATEPORTAL });
	}
}

TEST_F(PortalTest, InactiveOwnerDepartureClearsImportedPortalAndMissile)
{
	SetPortalStats(7, true, { 40, 50 }, 1, DTYPE_CATHEDRAL, false);
	SyncPortals();
	ASSERT_EQ(Missiles.size(), 1U);
	Players[7].plractive = false;
	multi_player_left(7, leaveinfo_t::LEAVE_DROP);
	EXPECT_FALSE(Portals[7].open);
	EXPECT_TRUE(Missiles.empty());
}

TEST_F(PortalTest, MalformedActivationCannotReplaceAnExistingPortal)
{
	const std::array<TCmdLocParam3, 10> invalid = {
		OpenCommand({ 255, 50 }), OpenCommand({ 40, 255 }), OpenCommand({ 40, 50 }, 0),
		OpenCommand({ 40, 50 }, 257), OpenCommand({ 40, 50 }, 1, DTYPE_TOWN),
		OpenCommand({ 40, 50 }, 1, DTYPE_CATACOMBS), OpenCommand({ 40, 50 }, 1, 256),
		OpenCommand({ 40, 50 }, 1, DTYPE_CATHEDRAL, 2), OpenCommand({ 40, 50 }, 0, DTYPE_CATHEDRAL, 1),
		OpenCommand({ 40, 50 }, 257, DTYPE_CATHEDRAL, 1)
	};
	for (uint8_t owner = 0; owner < Players.size(); ++owner) {
		SCOPED_TRACE(owner);
		SetPortalStats(owner, true, { 30, 40 }, 5, DTYPE_CATACOMBS, false);
		for (const auto &command : invalid) {
			Receive(owner, command);
			EXPECT_TRUE(Portals[owner].open);
			EXPECT_EQ(Portals[owner].position, Point(30, 40));
			EXPECT_EQ(Portals[owner].level, 5);
		}
	}
	EXPECT_FALSE(IsPortalDeltaValid({ -1, 50 }, 1, DTYPE_CATHEDRAL, false));
	EXPECT_FALSE(IsPortalDeltaValid({ 40, 50 }, -255, DTYPE_CATHEDRAL, false));
	EXPECT_FALSE(IsPortalDeltaValid({ 40, 50 }, 17, DTYPE_NEST, false));
	gbIsHellfire = true;
	EXPECT_TRUE(IsPortalDeltaValid({ 40, 50 }, 17, DTYPE_NEST, false));
	EXPECT_TRUE(IsPortalDeltaValid({ 40, 50 }, 24, DTYPE_CRYPT, false));
	gbIsSpawn = true;
	EXPECT_FALSE(IsPortalDeltaValid({ 40, 50 }, 5, DTYPE_CATACOMBS, false));
}

TEST_F(PortalTest, WarpRejectsUnavailableAndInvalidPortalOwners)
{
	Player &visitor = Players[1];
	visitor.setLevel(0);
	for (const uint16_t owner : { uint16_t { 7 }, uint16_t { 8 }, uint16_t { 65535 } }) {
		Receive(1, TCmdParam1 { CMD_WARP, Swap16LE(owner) });
		EXPECT_EQ(visitor.plrlevel, 0);
		EXPECT_FALSE(visitor._pLvlChanging);
	}
	SetPortalStats(7, true, { 40, 50 }, 5, DTYPE_CATACOMBS, false);
	sgGameInitInfo.rules = ClassicSessionRules;
	Receive(1, TCmdParam1 { CMD_WARP, Swap16LE(7) });
	EXPECT_EQ(visitor.plrlevel, 0);
	EXPECT_FALSE(visitor._pLvlChanging);
	sgGameInitInfo.rules = ExtendedSessionRules;
	Receive(1, TCmdParam1 { CMD_WARP, Swap16LE(7) });
	EXPECT_EQ(visitor.plrlevel, 5);
	EXPECT_TRUE(visitor._pLvlChanging);
}

TEST_F(PortalTest, ClosingPortalPreservesTheDestinationOfAnAlreadyQueuedWarp)
{
	SetPortalStats(7, true, { 40, 50 }, 5, DTYPE_CATACOMBS, false);
	Players[1].setLevel(0);
	// Portal processing queues CMD_WARP before the network command is replayed.
	Players[1]._pmode = PM_NEWLVL;
	Receive(7, TCmd { CMD_DEACTIVATEPORTAL });
	ASSERT_FALSE(Portals[7].open);
	Receive(1, TCmdParam1 { CMD_WARP, Swap16LE(7) });
	EXPECT_EQ(Players[1].plrlevel, 5);
	EXPECT_TRUE(Players[1]._pLvlChanging);
}

class TownPortalTest : public PortalTest {
protected:
	void SetUp() override
	{
		PortalTest::SetUp();
		LoadGameArchives();
		if (!HaveMainData())
			GTEST_SKIP() << "MPQ assets (spawn.mpq or DIABDAT.MPQ) not found - skipping town map test";
		pMegaTiles = LoadFileInMem<MegaTile>("levels\\towndata\\town.til");
		ASSERT_TRUE(LoadLevelSOLData().has_value());
		CreateTown(ENTRY_MAIN);
		InitTownTriggers();
		InitTowners();
	}

	void TearDown() override
	{
		FreeTownerGFX();
		Towners.clear();
		pMegaTiles.reset();
		PortalTest::TearDown();
	}

	static std::array<bool, MAXDUNX * MAXDUNY> Reachable(Point start, bool avoidPortals)
	{
		std::array<bool, MAXDUNX * MAXDUNY> reached { };
		std::vector<Point> queue = { start };
		reached[start.y * MAXDUNX + start.x] = true;
		for (size_t i = 0; i < queue.size(); ++i) {
			for (const Displacement offset : Neighbours) {
				const Point next = queue[i] + offset;
				if (IsTileOccupied(next) || !CanStep(queue[i], next))
					continue;
				if (avoidPortals && std::find(TownPortals.begin(), TownPortals.end(), next) != TownPortals.end())
					continue;
				const size_t index = next.y * MAXDUNX + next.x;
				if (!reached[index]) {
					reached[index] = true;
					queue.push_back(next);
				}
			}
		}
		return reached;
	}
};

TEST_F(TownPortalTest, EightPortalTilesAndArrivalsAreWalkableAndConnectedToTownServices)
{
	RecordProperty("town_asset", "levels/towndata/town.til + town.sol + sector1s..4s.dun");
	RecordProperty("portal_tiles", "57,40;59,40;61,40;63,40;57,44;59,44;61,44;63,44");
	RecordProperty("town_arrival_offset", "1,1");
	const auto reached = Reachable({ 75, 68 }, false);
	const auto services = Reachable({ 75, 68 }, true);
	for (size_t owner = 0; owner < TownPortals.size(); ++owner) {
		SCOPED_TRACE(owner);
		const Point portal = TownPortals[owner];
		const Point arrival = portal + Displacement { 1, 1 };
		EXPECT_FALSE(IsTileOccupied(portal)) << portal;
		EXPECT_FALSE(IsTileOccupied(arrival)) << arrival;
		EXPECT_TRUE(reached[portal.y * MAXDUNX + portal.x]) << portal;
		EXPECT_TRUE(services[arrival.y * MAXDUNX + arrival.x]) << arrival;
		EXPECT_TRUE(CanStep(arrival, portal));
		for (int i = 0; i < numtrigs; ++i) {
			EXPECT_FALSE(EntranceBoundaryContains(trigs[i].position, portal));
			EXPECT_FALSE(EntranceBoundaryContains(trigs[i].position, arrival));
		}
	}
	for (const Towner &towner : Towners) {
		SCOPED_TRACE(towner.name);
		const bool reachable = std::any_of(Neighbours.begin(), Neighbours.end(), [&](Displacement offset) {
			const Point neighbour = towner.position + offset;
			return InDungeonBounds(neighbour) && services[neighbour.y * MAXDUNX + neighbour.x];
		});
		EXPECT_TRUE(reachable) << towner.position;
	}
}

} // namespace
} // namespace devilution
