#include <algorithm>
#include <array>
#include <cstring>
#include <numeric>
#include <random>
#include <set>
#include <tuple>
#include <vector>

#include <gtest/gtest.h>

#include "automap.h"
#include "engine/assets.hpp"
#include "engine/load_file.hpp"
#include "engine/random.hpp"
#include "engine/surface.hpp"
#include "game_mode.hpp"
#include "levels/dun_tile_data.hpp"
#include "levels/tile_properties.hpp"
#include "levels/trigs.h"
#include "lighting.h"
#include "missiles.h"
#include "monster.h"
#include "monsters/validation.hpp"
#include "multi.h"
#include "nthread.h"
#include "options.h"
#include "panels/partypanel.hpp"
#include "panels/partypanel_layout.hpp"
#include "player.h"
#include "storm/storm_net.hpp"
#include "sync.h"
#include "tables/misdat.h"
#include "tables/playerdat.hpp"
#include "utils/endian_swap.hpp"
#include "utils/ui_fwd.h"

namespace devilution {
namespace {

class MonsterSummonsTest : public ::testing::Test {
protected:
	static void SetUpTestSuite()
	{
		LoadCoreArchives();
		LoadPlayerDataFiles();
		LoadItemData();
		LoadSpellData();
		LoadQuestData();
		LoadTextData();
		LoadMonsterData();
		LoadMissileData();
	}
	void SetUp() override
	{
		gbIsMultiplayer = true;
		gbIsHellfire = false;
		gbIsSpawn = false;
		gbBufferMsgs = 0;
		gbRunGame = false;
		gbGameDestroyed = false;
		Players.clear();
		ResizePlayersForSession(8);
		MyPlayerId = 0;
		MyPlayer = &Players[0];
		InspectPlayer = MyPlayer;
		currlevel = 1;
		leveltype = DTYPE_CATHEDRAL;
		setlevel = false;
		InitGameInfo();
		sgGameInitInfo.rules = ExtendedSessionRules;
		for (uint8_t i = 0; i < 8; ++i) {
			Player &player = Players[i];
			player.plractive = true;
			player.setLevel(1);
			player.setCharacterLevel(10);
			std::strcpy(player._pName, "Summoner");
			player.position.tile = { static_cast<WorldTileCoord>(80 + i), 80 };
			player.position.future = player.position.tile;
			player._pMaxMana = Fixed26_6::fromInt(100);
			player._pMaxHP = Fixed26_6::fromInt(100);
			player._pHitPoints = player._pMaxHP;
		}
		InitLighting();
		Missiles.clear();
		std::memset(dMonster, 0, sizeof(dMonster));
		std::memset(dPlayer, 0, sizeof(dPlayer));
		std::memset(dObject, 0, sizeof(dObject));
		std::memset(dPiece, 0, sizeof(dPiece));
		std::memset(dFlags, 0, sizeof(dFlags));
		SOLData[0] = TileProperties::None;
		InitLevelMonsters();
		ASSERT_TRUE(AddMonsterType(MT_GOLEM, PLACE_SPECIAL));
		ASSERT_TRUE(AddMonsterType(MT_NZOMBIE, PLACE_SCATTER));
		InitGolems();
		ASSERT_EQ(ActiveMonsterCount, 4U);
		ASSERT_TRUE(SNetInitializeProvider(SELCONN_LOOPBACK, &sgGameInitInfo));
		GameData wire = sgGameInitInfo;
		SwapGameDataLE(wire);
		int playerId;
		ASSERT_TRUE(SNetCreateGame("summons-test", nullptr, reinterpret_cast<char *>(&wire), sizeof(wire), &playerId));
		gdwLargestMsgSize = 512;
		gdwNormalMsgSize = 512;
		delta_init();
	}
	void TearDown() override
	{
		delta_init();
		SNetDestroy();
		Missiles.clear();
		Players.clear();
		MyPlayer = nullptr;
		InspectPlayer = nullptr;
		gbIsMultiplayer = false;
		sgGameInitInfo.rules = ClassicSessionRules;
		ActiveMonsterCount = 0;
	}
	void FillEnemyPool()
	{
		while (AvailableMonsterSlots() != 0) {
			const unsigned n = static_cast<unsigned>(MaxMonsters - AvailableMonsterSlots());
			ASSERT_NE(AddMonster({ static_cast<WorldTileCoord>(10 + (n % 18) * 3), static_cast<WorldTileCoord>(10 + (n / 18) * 3) }, Direction::South, 1, true), nullptr);
		}
	}
	void SummonAll()
	{
		for (uint8_t owner = 0; owner < 8; ++owner) {
			SpawnGolem(Players[owner], { static_cast<WorldTileCoord>(60 + owner * 3), 70 }, 3);
			ASSERT_NE(FindGolemForPlayer(Players[owner]), nullptr) << unsigned(owner);
		}
	}
	using EnemyState = std::tuple<size_t, Point, int32_t, uint32_t, UniqueMonsterType>;
	static std::vector<EnemyState> Enemies()
	{
		std::vector<EnemyState> result;
		for (size_t i = 4; i < MaxMonsters; ++i) {
			const Monster &monster = Monsters[i];
			result.emplace_back(monster.levelType, monster.position.tile, monster.hitPoints.raw(), monster.aiSeed, monster.uniqueType);
		}
		return result;
	}
};

TEST_F(MonsterSummonsTest, EightSummonsDoNotDisplaceAFullClassicEnemyPoolOrZhar)
{
	FillEnemyPool();
	Monsters[ZharMonsterIndex].uniqueType = UniqueMonsterType::Zhar;
	const auto enemies = Enemies();
	SummonAll();
	EXPECT_EQ(ActiveMonsterCount, MonsterStorageCapacity);
	EXPECT_EQ(AvailableMonsterSlots(), 0U);
	EXPECT_EQ(Enemies(), enemies);
	std::set<size_t> ids;
	for (const Player &player : Players) {
		const Monster *golem = FindGolemForPlayer(player);
		ASSERT_NE(golem, nullptr);
		ids.insert(golem->getId());
		EXPECT_TRUE(golem->isPlayerMinion());
		EXPECT_EQ(golem->goalVar3, player.getId());
	}
	EXPECT_EQ(ids.size(), 8U);
	EXPECT_EQ(*ids.rbegin(), MonsterStorageCapacity - 1);
}

TEST_F(MonsterSummonsTest, SummonsNeverConsumeFreeEnemySlots)
{
	SummonAll();
	EXPECT_EQ(AvailableMonsterSlots(), MaxMonsters - 4);
	FillEnemyPool();
	EXPECT_EQ(ActiveMonsterCount, MonsterStorageCapacity);
	EXPECT_EQ(AddMonster({ 50, 50 }, Direction::South, 1, true), nullptr);
	for (size_t i = 4; i < MaxMonsters; ++i)
		EXPECT_EQ(Monsters[i].type().type, MT_NZOMBIE);
}

TEST_F(MonsterSummonsTest, DuplicateRequestsDoNotCreateAnotherLivingGolem)
{
	SummonAll();
	std::array<size_t, 8> ids;
	for (uint8_t owner = 0; owner < 8; ++owner)
		ids[owner] = FindGolemForPlayer(Players[owner])->getId();
	const size_t count = ActiveMonsterCount;
	SummonAll();
	EXPECT_EQ(ActiveMonsterCount, count);
	for (uint8_t owner = 0; owner < 8; ++owner)
		EXPECT_EQ(FindGolemForPlayer(Players[owner])->getId(), ids[owner]);
}

TEST_F(MonsterSummonsTest, LocalGolemRendersInPartyPanel)
{
	struct RestorePanelState {
		PartyPortraits partyOption;
		bool showFps;
		bool automapActive;
		bool partyPanelOpen;
		uint16_t viewportHeight;
		bool friendlyMode;
		int portraitIdUnderCursor;

		~RestorePanelState()
		{
			FreePartyPanel();
			GetOptions().Gameplay.showMultiplayerPartyInfo.SetValue(partyOption);
			GetOptions().Graphics.showFPS.SetValue(showFps);
			AutomapActive = automapActive;
			PartySidePanelOpen = partyPanelOpen;
			gnViewportHeight = viewportHeight;
			Players[0].friendlyMode = friendlyMode;
			PortraitIdUnderCursor = portraitIdUnderCursor;
		}
	} restore {
		*GetOptions().Gameplay.showMultiplayerPartyInfo,
		*GetOptions().Graphics.showFPS,
		AutomapActive,
		PartySidePanelOpen,
		gnViewportHeight,
		Players[0].friendlyMode,
		PortraitIdUnderCursor,
	};

	if (!HaveMainData())
		LoadGameArchives();
	if (!HaveMainData())
		GTEST_SKIP() << "DIABDAT.MPQ or spawn.mpq not found - skipping party panel summon rendering";
	ASSERT_TRUE(LoadPartyPanel());
	Players[0].friendlyMode = true;
	GetOptions().Gameplay.showMultiplayerPartyInfo.SetValue(PartyPortraits::Summons);
	GetOptions().Graphics.showFPS.SetValue(false);
	AutomapActive = false;
	PartySidePanelOpen = true;
	gnViewportHeight = 352;

	SpawnGolem(Players[0], { 60, 70 }, 3);
	Monster *golem = FindGolemForPlayer(Players[0]);
	ASSERT_NE(golem, nullptr);

	OwnedSurface out(640, 480);
	DrawPartyMemberInfoPanel(out);
	const Point healthBarPosition { PartyPanelColumnMargin, PartyPanelColumnMargin };
	EXPECT_NE(out[healthBarPosition], 0);
	EXPECT_FALSE(DidRightClickPartyPortrait());
}

TEST_F(MonsterSummonsTest, EveryOwnerCanReplaceItsGolemWhileThePoolIsFull)
{
	FillEnemyPool();
	SummonAll();
	for (uint8_t owner = 0; owner < 8; ++owner) {
		Monster *golem = FindGolemForPlayer(Players[owner]);
		const size_t id = golem->getId();
		KillGolem(*golem);
		EXPECT_EQ(FindGolemForPlayer(Players[owner]), nullptr);
		SpawnGolem(Players[owner], { static_cast<WorldTileCoord>(60 + owner * 3), 72 }, 4);
		golem = FindGolemForPlayer(Players[owner]);
		ASSERT_NE(golem, nullptr);
		EXPECT_EQ(golem->getId(), id);
		EXPECT_EQ(golem->goalVar3, owner);
		EXPECT_EQ(ActiveMonsterCount, MonsterStorageCapacity);
	}
}

TEST_F(MonsterSummonsTest, OwnerDepartureAndSlotReuseRemoveOnlyItsSummon)
{
	SummonAll();
	for (uint8_t owner = 0; owner < 8; ++owner) {
		RemovePlrMissiles(Players[owner]);
		EXPECT_EQ(FindGolemForPlayer(Players[owner]), nullptr);
		Players[owner].plractive = false;
		for (uint8_t other = owner + 1; other < 8; ++other)
			ASSERT_NE(FindGolemForPlayer(Players[other]), nullptr);
	}
	for (Player &player : Players)
		player.plractive = true;
	SummonAll();
}

TEST_F(MonsterSummonsTest, SaturatedMapRejectsSummoningWithoutChangingThePool)
{
	SOLData[0] = TileProperties::Solid;
	const uint32_t seed = GetLCGEngineState();
	SpawnGolem(*MyPlayer, { 60, 70 }, 3);
	EXPECT_EQ(FindGolemForPlayer(*MyPlayer), nullptr);
	EXPECT_EQ(ActiveMonsterCount, 4U);
	EXPECT_EQ(GetLCGEngineState(), seed);
	ProcessGameMessagePackets();
	EXPECT_FALSE(gbGameDestroyed);
}

TEST_F(MonsterSummonsTest, ExhaustedSummonPoolDoesNotBorrowAnEnemySlot)
{
	SummonAll();
	// Occupied summon entries awaiting reclamation must not consume enemy capacity.
	for (size_t i = 0; i < ActiveMonsterCount; ++i)
		Monsters[ActiveMonsters[i]].goalVar3 = -1;
	const size_t count = ActiveMonsterCount;
	SpawnGolem(Players[7], { 60, 75 }, 3);
	EXPECT_EQ(FindGolemForPlayer(Players[7]), nullptr);
	EXPECT_EQ(ActiveMonsterCount, count);
	EXPECT_EQ(AvailableMonsterSlots(), MaxMonsters - 4);
}

TEST_F(MonsterSummonsTest, AllGolemsSelectHostileMonstersWithoutTargetingEachOther)
{
	FillEnemyPool();
	SummonAll();
	for (const Player &player : Players) {
		const Monster &golem = *FindGolemForPlayer(player);
		EXPECT_TRUE((golem.flags & MFLAG_TARGETS_MONSTER) != 0);
		EXPECT_GE(golem.enemy, 4);
		EXPECT_LT(golem.enemy, MaxMonsters);
		EXPECT_FALSE(Monsters[golem.enemy].isPlayerMinion());
	}
}

TEST_F(MonsterSummonsTest, OwnerDeathKeepsTheClassicSummonUntilTheOwnerLeavesTheLevel)
{
	SummonAll();
	for (uint8_t owner = 0; owner < 8; ++owner) {
		Monster *golem = FindGolemForPlayer(Players[owner]);
		Players[owner]._pHitPoints = Fixed26_6::fromInt(0);
		GolumAi(*golem);
		EXPECT_EQ(FindGolemForPlayer(Players[owner]), golem);
		StartNewLvl(Players[owner], WM_DIABNEXTLVL, 2);
		EXPECT_EQ(FindGolemForPlayer(Players[owner]), nullptr);
	}
}

TEST_F(MonsterSummonsTest, ExtraMonsterDeltasRestoreAllEightSummonsWithTheirOwners)
{
	SummonAll();
	ProcessGameMessagePackets();
	DeltaSaveLevel();
	const std::vector<unsigned> active(ActiveMonsters, ActiveMonsters + ActiveMonsterCount);
	std::array<size_t, 8> ids;
	std::array<Point, 8> positions;
	for (uint8_t owner = 0; owner < 8; ++owner) {
		const Monster &golem = *FindGolemForPlayer(Players[owner]);
		ids[owner] = golem.getId();
		positions[owner] = golem.position.tile;
	}
	InitLevelMonsters();
	ASSERT_TRUE(AddMonsterType(MT_GOLEM, PLACE_SPECIAL));
	ASSERT_TRUE(AddMonsterType(MT_NZOMBIE, PLACE_SCATTER));
	InitGolems();
	std::memset(dMonster, 0, sizeof(dMonster));
	DeltaLoadLevel();
	EXPECT_EQ(std::vector<unsigned>(ActiveMonsters, ActiveMonsters + ActiveMonsterCount), active);
	for (uint8_t owner = 0; owner < 8; ++owner) {
		const Monster *golem = FindGolemForPlayer(Players[owner]);
		ASSERT_NE(golem, nullptr) << unsigned(owner);
		EXPECT_EQ(golem->getId(), ids[owner]);
		EXPECT_EQ(golem->position.tile, positions[owner]);
		EXPECT_EQ(dMonster[positions[owner].x][positions[owner].y], ids[owner] + 1);
	}
}

TEST_F(MonsterSummonsTest, FailureIsReportedToTheSummonerThroughTheNetworkCommand)
{
	SOLData[0] = TileProperties::Solid;
	SpawnGolem(Players[7], { 60, 70 }, 3);
	uint8_t sender;
	void *data;
	size_t length;
	bool reported = false;
	while (SNetReceiveMessage(&sender, &data, &length)) {
		ASSERT_GE(length, sizeof(TPktHdr) + sizeof(TCmdParam1));
		const auto *body = reinterpret_cast<const std::byte *>(data) + sizeof(TPktHdr);
		const auto &command = *reinterpret_cast<const TCmdParam1 *>(body);
		if (command.bCmd == CMD_GOLEMFAILED) {
			reported = true;
			EXPECT_EQ(Swap16LE(command.wParam1), 7);
		}
	}
	EXPECT_TRUE(reported);
}

TEST_F(MonsterSummonsTest, NormalAndExtendedSessionsGenerateIdenticalEnemyPopulations)
{
	if (!HaveMainData())
		LoadGameArchives();
	if (!HaveMainData())
		GTEST_SKIP() << "DIABDAT.MPQ not found - skipping actual monster populations";
	MyPlayer->pOriginalCathedral = true;
	for (const int level : { 1, 5, 9, 13 }) {
		SCOPED_TRACE(level);
		std::vector<EnemyState> expected;
		size_t expectedCount = 0;
		uint32_t expectedSeed = 0;
		for (const bool extended : { false, true }) {
			sgGameInitInfo.rules = extended ? ExtendedSessionRules : ClassicSessionRules;
			currlevel = level;
			leveltype = GetLevelType(level);
			for (Player &player : Players)
				player.setLevel(level);
			InitLighting();
			InitQuests();
			const char *tiles = level == 1 ? "levels\\l1data\\l1.til" : level == 5 ? "levels\\l2data\\l2.til"
			    : level == 9                                                       ? "levels\\l3data\\l3.til"
			                                                                       : "levels\\l4data\\l4.til";
			pMegaTiles = LoadFileInMem<MegaTile>(tiles);
			ASSERT_TRUE(LoadLevelSOLData());
			CreateDungeon(2588, ENTRY_MAIN);
			SetRndSeed(10011);
			InitLevelMonsters();
			ASSERT_TRUE(GetLevelMTypes());
			InitGolems();
			InitNoTriggers();
			ASSERT_TRUE(InitMonsters());
			if (!extended) {
				expected = Enemies();
				expectedCount = ActiveMonsterCount;
				expectedSeed = GetLCGEngineState();
			} else {
				EXPECT_EQ(ActiveMonsterCount, expectedCount);
				EXPECT_EQ(Enemies(), expected);
				EXPECT_EQ(GetLCGEngineState(), expectedSeed);
			}
		}
	}
	pMegaTiles.reset();
}

TEST_F(MonsterSummonsTest, AuthoritativeSpawnsReplayOnEveryPeerInRequestOrder)
{
	std::array<uint8_t, 8> order { 0, 1, 2, 3, 4, 5, 6, 7 };
	std::mt19937 random(11021);
	std::shuffle(order.begin(), order.end(), random);
	for (const uint8_t owner : order)
		SpawnGolem(Players[owner], { static_cast<WorldTileCoord>(60 + owner * 3), 70 }, 3);
	std::array<size_t, 8> expected;
	for (uint8_t owner = 0; owner < 8; ++owner) {
		ASSERT_NE(FindGolemForPlayer(Players[owner]), nullptr);
		expected[owner] = FindGolemForPlayer(Players[owner])->getId();
	}
	std::vector<TCmdSpawnMonster> commands;
	uint8_t sender;
	void *data;
	size_t length;
	while (SNetReceiveMessage(&sender, &data, &length)) {
		ASSERT_GE(length, sizeof(TPktHdr) + sizeof(TCmdSpawnMonster));
		TCmdSpawnMonster command;
		std::memcpy(&command, reinterpret_cast<const std::byte *>(data) + sizeof(TPktHdr), sizeof(command));
		if (command.bCmd == CMD_SPAWNMONSTER)
			commands.push_back(command);
	}
	ASSERT_EQ(commands.size(), 8U);
	for (uint8_t peer = 1; peer < 8; ++peer) {
		SCOPED_TRACE(peer);
		MyPlayerId = peer;
		MyPlayer = &Players[peer];
		InitLevelMonsters();
		ASSERT_TRUE(AddMonsterType(MT_GOLEM, PLACE_SPECIAL));
		InitGolems();
		std::memset(dMonster, 0, sizeof(dMonster));
		delta_init();
		for (const auto &command : commands)
			EXPECT_EQ(ParseCmd(0, reinterpret_cast<const TCmd *>(&command), sizeof(command)), sizeof(command));
		for (uint8_t owner = 0; owner < 8; ++owner) {
			const Monster *golem = FindGolemForPlayer(Players[owner]);
			ASSERT_NE(golem, nullptr) << unsigned(owner);
			EXPECT_EQ(golem->getId(), expected[owner]);
			EXPECT_EQ(golem->position.tile, Point(60 + owner * 3, 70));
		}
	}
}

TEST_F(MonsterSummonsTest, SyncPacketsCarryAndApplyTheHighestSummonIndex)
{
	ASSERT_NE(AddMonster({ 50, 50 }, Direction::South, 1, true), nullptr);
	SummonAll();
	for (size_t i = 0; i < ActiveMonsterCount; ++i) {
		Monster &monster = Monsters[ActiveMonsters[i]];
		monster.activeForTicks = 255;
		monster.flags |= MFLAG_TARGETS_MONSTER;
		monster.enemy = 4;
	}
	sync_init();
	std::set<uint8_t> indices;
	for (int round = 0; round < 3; ++round) {
		std::array<std::byte, 512> buffer { };
		const size_t remaining = sync_all_monsters(buffer.data(), buffer.size());
		if (remaining == buffer.size())
			continue;
		const auto &header = *reinterpret_cast<const TSyncHeader *>(buffer.data());
		const size_t count = Swap16LE(header.wLen) / sizeof(TSyncMonster);
		for (size_t i = 0; i < count; ++i) {
			const auto &record = *reinterpret_cast<const TSyncMonster *>(buffer.data() + sizeof(header) + i * sizeof(TSyncMonster));
			indices.insert(record._mndx);
		}
	}
	EXPECT_TRUE(indices.contains(MonsterStorageCapacity - 1));
	std::array<std::byte, sizeof(TSyncHeader) + sizeof(TSyncMonster)> bytes { };
	auto &header = *reinterpret_cast<TSyncHeader *>(bytes.data());
	header.bCmd = CMD_SYNCDATA;
	header.bLevel = 1;
	header.wLen = Swap16LE(sizeof(TSyncMonster));
	header.bItemI = 255;
	auto &record = *reinterpret_cast<TSyncMonster *>(bytes.data() + sizeof(header));
	Monster &golem = Monsters[MonsterStorageCapacity - 1];
	record._mndx = golem.getId();
	record._mx = golem.position.tile.x + 1;
	record._my = golem.position.tile.y;
	record._menemy = 4;
	record._mdelta = 0;
	record._mhitpoints = Swap32LE(golem.hitPoints.raw());
	record.mWhoHit = PlayerExperienceMaskFromByte(0x80);
	EXPECT_EQ(OnSyncData(header, bytes.size(), Players[1]), bytes.size());
	EXPECT_EQ(PlayerExperienceMaskToByte(golem.whoHit), 0x80);
}

TEST_F(MonsterSummonsTest, DeletedExtraSummonIsReclaimedWithoutReducingEnemyCapacity)
{
	SummonAll();
	Monster &golem = *FindGolemForPlayer(Players[7]);
	const size_t id = golem.getId();
	golem.hitPoints = Fixed26_6::fromInt(0);
	golem.isInvalid = true;
	ProcessMonsters();
	EXPECT_EQ(ActiveMonsterCount, 7U);
	EXPECT_EQ(AvailableMonsterSlots(), MaxMonsters - 4);
	SpawnGolem(Players[7], { 81, 72 }, 3);
	ASSERT_NE(FindGolemForPlayer(Players[7]), nullptr);
	EXPECT_EQ(FindGolemForPlayer(Players[7])->getId(), id);
	EXPECT_TRUE(std::is_sorted(ActiveMonsters, ActiveMonsters + ActiveMonsterCount));
}

TEST_F(MonsterSummonsTest, EveryEncodedEnemyIdFitsAndRoundTripsInEachSessionMode)
{
	for (const bool extended : { false, true }) {
		sgGameInitInfo.rules = extended ? ExtendedSessionRules : ClassicSessionRules;
		Monster &source = Monsters[10];
		for (size_t id = 0; id < MonsterLimit(); ++id) {
			Monsters[id].hitPoints = Fixed26_6::fromInt(1);
			source.flags = MFLAG_TARGETS_MONSTER;
			source.enemy = id;
			EXPECT_EQ(encode_enemy(source), id);
			decode_enemy(source, encode_enemy(source));
			EXPECT_EQ(source.enemy, id);
			EXPECT_TRUE((source.flags & MFLAG_TARGETS_MONSTER) != 0);
		}
		for (uint8_t player = 0; player < (extended ? 8 : 4); ++player) {
			source.flags = 0;
			source.enemy = player;
			const uint8_t encoded = encode_enemy(source);
			EXPECT_EQ(encoded, MonsterLimit() + player);
			EXPECT_TRUE(IsEnemyIdValid(encoded));
			decode_enemy(source, encoded);
			EXPECT_EQ(source.enemy, player);
			EXPECT_EQ(source.enemyPosition, Players[player].position.future);
		}
		EXPECT_FALSE(IsEnemyIdValid(MonsterLimit() + Players.size()));
	}
}

TEST_F(MonsterSummonsTest, InitialMonsterGenerationConsumesTheSameRandomSequence)
{
	for (const bool extended : { false, true }) {
		sgGameInitInfo.rules = extended ? ExtendedSessionRules : ClassicSessionRules;
		SetRndSeed(1234);
		for (size_t i = 0; i < MaxMonsters; ++i) {
			GenerateRnd(8);
			DiscardRandomValues(1);
		}
		const uint32_t expected = GetLCGEngineState();
		SetRndSeed(1234);
		InitLevelMonsters();
		EXPECT_EQ(GetLCGEngineState(), expected);
		EXPECT_EQ(ActiveMonsterCount, 0U);
		for (size_t i = 0; i < MonsterStorageCapacity; ++i)
			EXPECT_EQ(ActiveMonsters[i], i);
	}
}

TEST_F(MonsterSummonsTest, ClassicSessionsKeepTheirOriginalFourReservations)
{
	sgGameInitInfo.rules = ClassicSessionRules;
	FillEnemyPool();
	for (uint8_t owner = 0; owner < 4; ++owner)
		SpawnGolem(Players[owner], { static_cast<WorldTileCoord>(60 + owner * 3), 70 }, 3);
	EXPECT_EQ(ActiveMonsterCount, MaxMonsters);
	EXPECT_EQ(MonsterLimit(), MaxMonsters);
	for (uint8_t owner = 0; owner < 4; ++owner)
		EXPECT_EQ(FindGolemForPlayer(Players[owner])->getId(), owner);
	for (size_t id = MaxMonsters; id < MonsterStorageCapacity; ++id)
		EXPECT_EQ(Monsters[id].hitPoints.raw(), 0);
}

} // namespace
} // namespace devilution
