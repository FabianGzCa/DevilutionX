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
#include "items.h"
#include "levels/dun_tile_data.hpp"
#include "levels/tile_properties.hpp"
#include "levels/trigs.h"
#include "lighting.h"
#include "missiles.h"
#include "monster.h"
#include "monsters/party_balance.hpp"
#include "monsters/party_scaling.hpp"
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
		sgGameInitInfo.nDifficulty = DIFF_NORMAL;
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
	if (!HaveMainData() || gbIsSpawn)
		GTEST_SKIP() << "DIABDAT.MPQ not found - the shareware data has no catacomb, cave or hell levels";
	MyPlayer->pOriginalCathedral = true;
	for (const int level : { 1, 5, 9, 13 }) {
		SCOPED_TRACE(level);
		std::vector<EnemyState> expected;
		size_t expectedCount = 0;
		uint32_t expectedSeed = 0;
		for (const unsigned count : { 0, 1, 2, 3, 4, 8 }) {
			const bool extended = count != 0;
			gbActivePlayers = extended ? count : 4;
			for (unsigned id = 0; id < Players.size(); ++id)
				Players[id].plractive = id < gbActivePlayers;
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
			RecordProperty("level" + std::to_string(level) + "_case" + std::to_string(count) + "_active_monsters", ActiveMonsterCount);
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

TEST_F(MonsterSummonsTest, NativeMultiplayerStatsAreIdenticalForOneThroughFourParticipants)
{
	ASSERT_TRUE(AddMonsterType(MT_DIABLO, PLACE_SPECIAL));
	for (bool hellfire : { false, true }) {
		gbIsHellfire = hellfire;
		for (auto difficulty : { DIFF_NORMAL, DIFF_NIGHTMARE, DIFF_HELL }) {
			sgGameInitInfo.nDifficulty = difficulty;
			using Stats = std::tuple<int32_t, int, int, int, int, unsigned, uint32_t, uint32_t>;
			std::array<Stats, 2> expected;
			for (unsigned count = 1; count <= 4; ++count) {
				gbActivePlayers = count;
				for (unsigned id = 0; id < Players.size(); ++id)
					Players[id].plractive = id < count;
				for (unsigned type = 1; type <= 2; ++type) {
					SetRndSeed(16001);
					Monster *monster = AddMonster({ 40, 40 }, Direction::South, type, false);
					ASSERT_NE(monster, nullptr);
					const Stats stats { monster->maxHitPoints.raw(), monster->minDamage, monster->maxDamage, monster->minDamageSpecial, monster->maxDamageSpecial, monster->exp(difficulty), monster->aiSeed, monster->rndItemSeed };
					if (count == 1)
						expected[type - 1] = stats;
					else
						EXPECT_EQ(stats, expected[type - 1]);
					const std::string key = std::string(hellfire ? "hf" : "diablo") + "_difficulty" + std::to_string(difficulty) + "_party" + std::to_string(count) + "_type" + std::to_string(type);
					RecordProperty(key + "_hp_raw", monster->maxHitPoints.raw());
					RecordProperty(key + "_damage_min", static_cast<int>(monster->minDamage));
					RecordProperty(key + "_damage_max", static_cast<int>(monster->maxDamage));
					RecordProperty(key + "_xp_pool", monster->exp(difficulty));
				}
			}
		}
	}
	gbIsHellfire = false;
}

TEST_F(MonsterSummonsTest, NativeSinglePlayerHealthUsesItsOwnDifficultyRules)
{
	ASSERT_TRUE(AddMonsterType(MT_DIABLO, PLACE_SPECIAL));
	constexpr int Expected[2][3] = { { 833, 2599, 3532 }, { 833, 2549, 3432 } };
	gbIsMultiplayer = false;
	for (unsigned edition = 0; edition < 2; ++edition) {
		gbIsHellfire = edition != 0;
		for (auto difficulty : { DIFF_NORMAL, DIFF_NIGHTMARE, DIFF_HELL }) {
			sgGameInitInfo.nDifficulty = difficulty;
			Monster *monster = AddMonster({ 40, 40 }, Direction::South, 2, false);
			ASSERT_NE(monster, nullptr);
			EXPECT_EQ(monster->maxHitPoints, Fixed26_6::fromInt(Expected[edition][difficulty]));
		}
	}
	gbIsHellfire = false;
}

TEST_F(MonsterSummonsTest, NativeLootRollsDoNotIncreaseWithMultiplayerParticipantCount)
{
	for (auto difficulty : { DIFF_NORMAL, DIFF_NIGHTMARE, DIFF_HELL }) {
		sgGameInitInfo.nDifficulty = difficulty;
		Monster *monster = AddMonster({ 40, 40 }, Direction::South, 1, false);
		ASSERT_NE(monster, nullptr);
		using Drop = std::tuple<int, int, uint32_t, uint16_t, int, uint32_t>;
		std::vector<Drop> expected;
		for (unsigned count = 1; count <= 4; ++count) {
			gbActivePlayers = count;
			unsigned drops = 0;
			for (unsigned seed = 0; seed < 256; ++seed) {
				ActiveItemCount = 0;
				std::memset(dItem, 0, sizeof(dItem));
				std::iota(std::begin(ActiveItems), std::end(ActiveItems), 0);
				SetRndSeed(16000 + seed);
				SpawnItem(*monster, { 40, 40 }, false);
				ASSERT_LE(ActiveItemCount, 1);
				drops += ActiveItemCount;
				const Item item = ActiveItemCount == 0 ? Item {} : Items[ActiveItems[0]];
				const Drop result { ActiveItemCount, static_cast<int>(item.IDidx), item._iSeed, item._iCreateInfo, item._ivalue, GetLCGEngineState() };
				if (count == 1)
					expected.push_back(result);
				else
					EXPECT_EQ(result, expected[seed]);
			}
			RecordProperty("difficulty" + std::to_string(difficulty) + "_party" + std::to_string(count) + "_drops_per_256", drops);
		}
	}
	ActiveItemCount = 0;
	std::memset(dItem, 0, sizeof(dItem));
}

TEST_F(MonsterSummonsTest, FixedDamageBenchmarkKeepsEachPlayersSoloExperiencePaceAndDiablosSoloKillTime)
{
	ASSERT_TRUE(AddMonsterType(MT_DIABLO, PLACE_SPECIAL));
	for (auto difficulty : { DIFF_NORMAL, DIFF_NIGHTMARE, DIFF_HELL }) {
		sgGameInitInfo.nDifficulty = difficulty;
		for (unsigned type = 1; type <= 2; ++type) {
			unsigned soloTurns = 0;
			uint32_t soloShare = 0;
			Fixed26_6 soloNative;
			for (unsigned count = 1; count <= 8; ++count) {
				gbActivePlayers = count;
				const auto scale = PartyScaling::ForSession(ExtendedSessionRules, count);
				ASSERT_TRUE(scale);
				Monster *monster = AddMonster({ 40, 40 }, Direction::South, type, false);
				ASSERT_NE(monster, nullptr);
				const bool diablo = monster->type().type == MT_DIABLO;
				ASSERT_EQ(diablo, type == 2);
				// Spawns roll their health; compare every party against the solo spawn's roll, raised to
				// 200 points so that whole-point hits and the death threshold do not dominate the pace.
				if (count == 1)
					soloNative = Fixed26_6::fromInt(std::max(200, monster->maxHitPoints.whole()));
				const auto native = soloNative;
				monster->maxHitPoints = scale->HitPoints(native, false, diablo);
				monster->hitPoints = monster->maxHitPoints;
				// Every player deals the same fixed damage per turn; alone, a kill takes about 200 turns.
				const auto hit = Fixed26_6::fromInt(native.whole() / 200);
				unsigned turns = 0;
				while (!monster->hasNoLife() && turns < 100000) {
					++turns;
					for (unsigned attacker = 0; attacker < count && !monster->hasNoLife(); ++attacker)
						ApplyMonsterDamage(DamageType::Physical, *monster, hit);
					FlushNetMessageBuffers();
					// Damage was already applied locally; consume its loopback echoes
					// at each turn rather than accumulating an unbounded test backlog.
					uint8_t sender;
					void *data;
					size_t size;
					while (SNetReceiveMessage(&sender, &data, &size)) { }
				}
				ASSERT_TRUE(monster->hasNoLife());
				const uint32_t share = scale->Experience(monster->exp(difficulty), false, diablo) / count;
				if (count == 1) {
					soloTurns = turns;
					soloShare = share;
				} else if (diablo) {
					// The whole party needs as long as one player alone, and each still earns a solo kill.
					EXPECT_LE(turns, soloTurns);
					EXPECT_GE(turns + 1, soloTurns);
					EXPECT_EQ(share, soloShare);
				} else {
					// The party needs (N + 1) / (2N) of the solo time, and each player's experience per
					// turn stays within 2% of the solo pace before the native per-contributor rounding.
					EXPECT_LT(turns, soloTurns);
					const double pool = scale->Experience(monster->exp(difficulty));
					const double pace = pool / count * soloTurns / (static_cast<double>(soloShare) * turns);
					EXPECT_NEAR(pace, 1.0, 0.02) << count;
				}
				const std::string key = "difficulty" + std::to_string(difficulty) + (diablo ? "_diablo" : "_zombie") + "_party" + std::to_string(count);
				RecordProperty(key + "_native_hp_raw", native.raw());
				RecordProperty(key + "_scaled_hp_raw", monster->maxHitPoints.raw());
				RecordProperty(key + "_turns_fixed_damage_per_player", turns);
				RecordProperty(key + "_xp_per_contributor_before_caps", share);
			}
		}
	}
}

TEST_F(MonsterSummonsTest, PartyDamageAndScaleChangesCommuteAtFractionalBoundaries)
{
	InitItemTransfers();
	CommitPartyLevel(1, 4, 0, 0, 8);
	Monster *monster = AddMonster({ 40, 40 }, Direction::South, 1, true);
	ASSERT_NE(monster, nullptr);
	monster->maxHitPoints = monster->hitPoints = Fixed26_6::fromInt(100);
	InitializePartyMonster(*monster);
	CapturePartyMonsters();
	ASSERT_EQ(monster->maxHitPoints, Fixed26_6::fromInt(250));
	const auto initial = GetPartyLevelState(1);
	// Senders weigh their hits at the scale they see, which may differ from the receiver's.
	const auto damage = [&](uint8_t actor, uint8_t participants, uint32_t amount) {
		PartyDamageMessage message { CMD_PARTYMONSTDAMAGE, 1, participants, 0, 1, static_cast<uint16_t>(monster->getId()), amount,
			monster->partyGeneration, monster->partyNativeMax.raw(), MT_NZOMBIE, static_cast<uint8_t>(UniqueMonsterType::None), 40, 40 };
		ReceivePartyMonsterDamage(actor, message);
	};
	damage(1, 4, 1);
	damage(2, 5, 3);
	EXPECT_EQ(GetPartyLevelState(1).monsters[monster->getId()].damage, 1 * 1008 + 3 * 840);
	CommitPartyLevel(1, 5, 0, 0, 16);
	const int32_t expected = monster->hitPoints.raw();
	const int64_t debt = GetPartyLevelState(1).monsters[monster->getId()].damage;
	EXPECT_EQ(monster->maxHitPoints, Fixed26_6::fromInt(300));
	EXPECT_EQ(expected, 300 * 64 - 4);
	GetPartyLevelState(1) = initial;
	monster->hitPoints = monster->maxHitPoints = Fixed26_6::fromInt(250);
	monster->partyLastHitPoints = monster->hitPoints;
	monster->partyParticipants = 4;
	monster->whoHit = {};
	CommitPartyLevel(1, 5, 0, 0, 16);
	damage(2, 5, 3);
	damage(1, 4, 1);
	EXPECT_EQ(monster->hitPoints.raw(), expected);
	EXPECT_EQ(GetPartyLevelState(1).monsters[monster->getId()].damage, debt);
	EXPECT_EQ(monster->whoHit.bits, 6);
	// A smaller party lowers health at once, keeping the same fraction, and growing back restores it.
	CommitPartyLevel(1, 2, 0, 0, 24);
	EXPECT_EQ(GetPartyLevelState(1).participants, 2);
	EXPECT_EQ(monster->maxHitPoints, Fixed26_6::fromInt(150));
	EXPECT_EQ(monster->hitPoints.raw(), 150 * 64 - 2);
	EXPECT_EQ(GetPartyLevelState(1).monsters[monster->getId()].damage, debt);
	CommitPartyLevel(1, 5, 0, 0, 32);
	EXPECT_EQ(monster->hitPoints.raw(), expected);
}

TEST_F(MonsterSummonsTest, PartySnapshotsRetainFutureDamageAndReceiptWatermarks)
{
	InitItemTransfers();
	CommitPartyLevel(1, 4, 0, 0, 8);
	Monster *monster = AddMonster({ 40, 40 }, Direction::South, 1, true);
	ASSERT_NE(monster, nullptr);
	monster->maxHitPoints = monster->hitPoints = Fixed26_6::fromInt(100);
	InitializePartyMonster(*monster);
	CapturePartyMonsters();
	// Player 7 hits a monster it spawned in this slot before this peer saw the spawn.
	const uint64_t spawn = (static_cast<uint64_t>(MultiplayerSlotEpoch(7)) << 32) | (1U << 3) | 7U;
	PartyDamageMessage damage { CMD_PARTYMONSTDAMAGE, 1, 5, MultiplayerSlotEpoch(7), 1, static_cast<uint16_t>(monster->getId()), 128,
		spawn, 100 * 64, MT_NZOMBIE, static_cast<uint8_t>(UniqueMonsterType::None), 40, 40 };
	ReceivePartyMonsterDamage(7, damage);
	ASSERT_EQ(GetPartyLevelState(1).pending.size(), 1U);
	const auto bytes = EncodePartyLevel(GetPartyLevelState(1));
	const auto restored = DecodePartyLevel(bytes);
	ASSERT_TRUE(restored);
	GetPartyLevelState(1) = *restored;
	ASSERT_EQ(GetPartyLevelState(1).pending.size(), 1U);
	ASSERT_TRUE(ResetPartyMonster(1, monster->getId(), spawn));
	DrainPartyDamage(1);
	EXPECT_TRUE(GetPartyLevelState(1).pending.empty());
	const auto &state = GetPartyLevelState(1).monsters[monster->getId()];
	EXPECT_EQ(state.generation, spawn);
	EXPECT_EQ(state.damage, 128 * 2 * PartyDamageUnits / 6);
	EXPECT_EQ(PartyMonsterHitPoints(state, 5).raw(), 300 * 64 - 128);
	EXPECT_EQ(state.receipts[7], 1U);
	ReceivePartyMonsterDamage(7, damage);
	EXPECT_EQ(state.damage, 128 * 2 * PartyDamageUnits / 6);
	ASSERT_TRUE(ResetPartyMonster(1, monster->getId(), spawn + 8));
	ReceivePartyMonsterDamage(7, damage);
	EXPECT_EQ(GetPartyLevelState(1).monsters[monster->getId()].flags, PartyMonsterExpected);
}

TEST_F(MonsterSummonsTest, PartyCodecRejectsEveryTruncationAndMalformedHealth)
{
	InitItemTransfers();
	CommitPartyLevel(1, 4, 0, 0, 8);
	Monster *monster = AddMonster({ 40, 40 }, Direction::South, 1, true);
	ASSERT_NE(monster, nullptr);
	CapturePartyMonsters();
	const auto bytes = EncodePartyLevel(GetPartyLevelState(1));
	ASSERT_TRUE(DecodePartyLevel(bytes));
	for (size_t size = 0; size < bytes.size(); ++size)
		EXPECT_FALSE(DecodePartyLevel(std::span(bytes).first(size))) << size;
	auto trailing = bytes;
	trailing.push_back(std::byte { 0 });
	EXPECT_FALSE(DecodePartyLevel(trailing));
	auto state = GetPartyLevelState(1).monsters[monster->getId()];
	state.nativeMax = -1;
	EXPECT_FALSE(DecodePartyMonster(EncodePartyMonster(state)));
	state = GetPartyLevelState(1).monsters[monster->getId()];
	state.damage = static_cast<int64_t>(state.nativeMax) * PartyDamageUnits + 1;
	EXPECT_FALSE(DecodePartyMonster(EncodePartyMonster(state)));
}

TEST_F(MonsterSummonsTest, PartyHealthPreservesDeathAndNativeGolemStats)
{
	InitItemTransfers();
	CommitPartyLevel(1, 4, 0, 0, 8);
	Monster *monster = AddMonster({ 40, 40 }, Direction::South, 1, true);
	ASSERT_NE(monster, nullptr);
	CapturePartyMonsters();
	auto &state = GetPartyLevelState(1).monsters[monster->getId()];
	state.flags |= PartyMonsterDead;
	state.deathParticipants = 4;
	RestorePartyMonsters();
	ASSERT_TRUE(monster->hasNoLife());
	CommitPartyLevel(1, 8, 0, 0, 16);
	EXPECT_TRUE(monster->hasNoLife());
	EXPECT_TRUE(IsPartyMonsterRewardPending(*monster));
	SpawnGolem(Players[0], { 60, 70 }, 3);
	Monster *golem = FindGolemForPlayer(Players[0]);
	ASSERT_NE(golem, nullptr);
	const auto native = golem->maxHitPoints;
	CapturePartyMonsters();
	EXPECT_EQ(golem->maxHitPoints, native);
	EXPECT_FALSE(IsPartyMonsterRewardPending(*golem));
}

TEST_F(MonsterSummonsTest, PartyScaleCommitCannotReviveFractionalNativeDeath)
{
	InitItemTransfers();
	CommitPartyLevel(1, 4, 0, 0, 8);
	Monster *monster = AddMonster({ 40, 40 }, Direction::South, 1, true);
	ASSERT_NE(monster, nullptr);
	monster->hitPoints = Fixed26_6::fromRaw(63);
	CommitPartyLevel(1, 8, 0, 0, 16);
	EXPECT_TRUE(monster->hasNoLife());
	EXPECT_EQ(monster->hitPoints.raw(), 0);
	EXPECT_TRUE(IsPartyMonsterRewardPending(*monster));
}

TEST_F(MonsterSummonsTest, PartyHealthFollowsDeparturesAndDisconnectionsWithoutLosingProgress)
{
	InitItemTransfers();
	CommitPartyLevel(1, 8, 0, 0, 8);
	Monster *monster = AddMonster({ 40, 40 }, Direction::South, 1, true);
	ASSERT_NE(monster, nullptr);
	monster->maxHitPoints = monster->hitPoints = Fixed26_6::fromInt(100);
	InitializePartyMonster(*monster);
	CapturePartyMonsters();
	ASSERT_EQ(monster->maxHitPoints, Fixed26_6::fromInt(450));
	PartyDamageMessage damage { CMD_PARTYMONSTDAMAGE, 1, 8, 0, 1, static_cast<uint16_t>(monster->getId()), 90 * 64,
		monster->partyGeneration, monster->partyNativeMax.raw(), MT_NZOMBIE, static_cast<uint8_t>(UniqueMonsterType::None), 40, 40 };
	ReceivePartyMonsterDamage(1, damage);
	ASSERT_EQ(monster->hitPoints, Fixed26_6::fromInt(360));
	const int64_t debt = GetPartyLevelState(1).monsters[monster->getId()].damage;
	// Two players walk to another level and two disconnect in the middle of the fight.
	Players[4].plractive = false;
	Players[5].plractive = false;
	Players[6].setLevel(2);
	Players[7].setLevel(2);
	gbBufferMsgs = 1;
	UpdatePartyParticipants();
	EXPECT_EQ(GetPartyLevelState(1).participants, 8);
	EXPECT_EQ(monster->hitPoints, Fixed26_6::fromInt(360));
	gbBufferMsgs = 0;
	UpdatePartyParticipants();
	EXPECT_EQ(GetPartyLevelState(1).participants, 4);
	EXPECT_EQ(monster->maxHitPoints, Fixed26_6::fromInt(250));
	EXPECT_EQ(monster->hitPoints, Fixed26_6::fromInt(200));
	EXPECT_EQ(GetPartyLevelState(1).monsters[monster->getId()].damage, debt);
	// A level nobody committed an entry to does not start counting on its own.
	const auto *other = FindPartyLevelState(2);
	EXPECT_TRUE(other == nullptr || other->participants == 0);
	for (uint8_t id = 1; id < 4; ++id)
		Players[id].setLevel(2);
	UpdatePartyParticipants();
	EXPECT_EQ(GetPartyLevelState(1).participants, 1);
	EXPECT_EQ(monster->maxHitPoints, Fixed26_6::fromInt(100));
	EXPECT_EQ(monster->hitPoints, Fixed26_6::fromInt(80));
	for (auto &player : Players) {
		player.plractive = true;
		player.setLevel(1);
	}
	UpdatePartyParticipants();
	EXPECT_EQ(GetPartyLevelState(1).participants, 8);
	EXPECT_EQ(monster->hitPoints, Fixed26_6::fromInt(360));
	EXPECT_EQ(GetPartyLevelState(1).monsters[monster->getId()].damage, debt);
}

TEST_F(MonsterSummonsTest, PartyDeathDependsOnlyOnTheLedgerSoPeersCountingDifferentPartiesAgree)
{
	InitItemTransfers();
	CommitPartyLevel(1, 8, 0, 0, 8);
	Monster *monster = AddMonster({ 40, 40 }, Direction::South, 1, true);
	ASSERT_NE(monster, nullptr);
	monster->maxHitPoints = monster->hitPoints = Fixed26_6::fromInt(100);
	InitializePartyMonster(*monster);
	CapturePartyMonsters();
	const auto damage = [&](uint8_t actor, uint8_t participants, uint32_t amount) {
		PartyDamageMessage message { CMD_PARTYMONSTDAMAGE, 1, participants, 0, 1, static_cast<uint16_t>(monster->getId()), amount,
			monster->partyGeneration, monster->partyNativeMax.raw(), MT_NZOMBIE, static_cast<uint8_t>(UniqueMonsterType::None), 40, 40 };
		ReceivePartyMonsterDamage(actor, message);
	};
	const auto &state = GetPartyLevelState(1).monsters[monster->getId()];
	// A peer that already sees itself alone leaves exactly one whole native point.
	damage(1, 1, 99 * 64);
	EXPECT_FALSE(IsPartyMonsterDepleted(state));
	EXPECT_FALSE(monster->hasNoLife());
	EXPECT_EQ(monster->hitPoints.raw(), 288);
	// Five raw points at the scale of eight are worth less than one whole point here,
	// but they take the native remainder below one point, which every peer sees as death.
	damage(2, 8, 5);
	EXPECT_TRUE(IsPartyMonsterDepleted(state));
	EXPECT_NE(state.flags & PartyMonsterDead, 0);
	EXPECT_EQ(state.killer, 2);
	EXPECT_EQ(state.deathParticipants, 8);
	EXPECT_TRUE(monster->hasNoLife());
}

TEST_F(MonsterSummonsTest, ExtraLootRollsKeepTheNativeDropAndAddIndependentRolls)
{
	struct Drop {
		uint32_t seed;
		int index;
	};
	static std::vector<Drop> drops;
	TestMonsterLootObserver = [](const Monster &, const Item *item) {
		if (item != nullptr)
			drops.push_back({ item->_iSeed, static_cast<int>(item->IDidx) });
	};
	Monster *monster = AddMonster({ 40, 40 }, Direction::South, 1, false);
	ASSERT_NE(monster, nullptr);
	ASSERT_TRUE(IsPartyRandomLoot(monster->type().type, static_cast<uint8_t>(monster->uniqueType)));
	unsigned native = 0;
	unsigned party = 0;
	for (uint32_t seed = 0; seed < 400; ++seed) {
		std::array<std::vector<Drop>, 2> results;
		for (unsigned extended = 0; extended < 2; ++extended) {
			drops.clear();
			ActiveItemCount = 0;
			std::memset(dItem, 0, sizeof(dItem));
			std::iota(std::begin(ActiveItems), std::end(ActiveItems), 0);
			monster->rndItemSeed = 7000 + seed;
			SetRndSeed(123);
			FinalizePartyMonsterReward(*monster, false, extended != 0 ? 8 : 1, extended != 0 ? 4 : 0);
			EXPECT_EQ(GetLCGEngineState(), 123U);
			results[extended] = drops;
		}
		native += results[0].size();
		party += results[1].size();
		ASSERT_LE(results[0].size(), 1U);
		if (!results[0].empty()) {
			ASSERT_FALSE(results[1].empty());
			EXPECT_EQ(results[1][0].seed, results[0][0].seed);
			EXPECT_EQ(results[1][0].index, results[0][0].index);
		}
	}
	TestMonsterLootObserver = nullptr;
	ActiveItemCount = 0;
	std::memset(dItem, 0, sizeof(dItem));
	// Five independent rolls per kill instead of one.
	ASSERT_GT(native, 0U);
	EXPECT_GE(party, native * 4);
	EXPECT_LE(party, native * 6);
	RecordProperty("native_drops_per_400", native);
	RecordProperty("eight_player_drops_per_400", party);
}

TEST_F(MonsterSummonsTest, PartyRewardCreditsAdvanceOnceAndPreserveUnrelatedRandomState)
{
	InitItemTransfers();
	CommitPartyLevel(1, 2, 0, 0, 8);
	for (unsigned death = 1; death <= 4; ++death) {
		Monster *monster = AddMonster({ static_cast<WorldTileCoord>(40 + death), 40 }, Direction::South, 1, true);
		ASSERT_NE(monster, nullptr);
		monster->tag(*MyPlayer);
		CapturePartyMonsters();
		const auto &state = GetPartyLevelState(1).monsters[monster->getId()];
		PartyMonsterReward reward { CMD_PARTYREWARD, static_cast<uint16_t>(monster->getId()), state.generation,
			state.nativeMax, state.type, state.unique, 2, static_cast<uint8_t>(40 + death), 40 };
		ASSERT_TRUE(ReservePartyReward(1, reward));
		const auto seed = GetLCGEngineState();
		ASSERT_TRUE(CommitPartyReward(1, reward, MyPlayerId));
		EXPECT_EQ(GetPartyLevelState(1).lootRemainder, death % 2);
		EXPECT_EQ(GetLCGEngineState(), seed);
		const auto experience = MyPlayer->_pExperience;
		ASSERT_TRUE(CommitPartyReward(1, reward, MyPlayerId));
		EXPECT_EQ(GetPartyLevelState(1).lootRemainder, death % 2);
		EXPECT_EQ(MyPlayer->_pExperience, experience);
		EXPECT_EQ(GetLCGEngineState(), seed);
	}
}

TEST_F(MonsterSummonsTest, PartyRewardRejectionDoesNotKillAnotherIncarnation)
{
	InitItemTransfers();
	CommitPartyLevel(1, 8, 0, 0, 8);
	Monster *monster = AddMonster({ 40, 40 }, Direction::South, 1, true);
	ASSERT_NE(monster, nullptr);
	CapturePartyMonsters();
	const auto health = monster->hitPoints;
	const auto &state = GetPartyLevelState(1).monsters[monster->getId()];
	PartyMonsterReward reward { CMD_PARTYREWARD, static_cast<uint16_t>(monster->getId()), state.generation + 1,
		state.nativeMax, state.type, state.unique, 8, 40, 40 };
	EXPECT_FALSE(ReservePartyReward(1, reward));
	EXPECT_EQ(monster->hitPoints, health);
	EXPECT_EQ(state.flags & PartyMonsterDead, 0);
	reward.generation = state.generation;
	++reward.nativeMax;
	EXPECT_FALSE(ReservePartyReward(1, reward));
	EXPECT_EQ(monster->hitPoints, health);
	EXPECT_EQ(state.flags & PartyMonsterDead, 0);
}

TEST_F(MonsterSummonsTest, PartySpawnReceiptsAndDepartedContributorsCannotBeReused)
{
	InitItemTransfers();
	CommitPartyLevel(1, 8, 0, 0, 8);
	Monster *monster = AddMonster({ 40, 40 }, Direction::South, 1, true);
	ASSERT_NE(monster, nullptr);
	const auto first = AssignPartySpawnGeneration(*monster);
	PartyDamageMessage damage { CMD_PARTYMONSTDAMAGE, 1, 8, 0, 1, static_cast<uint16_t>(monster->getId()), 64,
		first, monster->partyNativeMax.raw(), MT_NZOMBIE, static_cast<uint8_t>(UniqueMonsterType::None), 40, 40 };
	ReceivePartyMonsterDamage(7, damage);
	auto &state = GetPartyLevelState(1).monsters[monster->getId()];
	EXPECT_EQ(state.contributors, 128);
	const auto debt = state.damage;
	LeavePartyBalance(7);
	EXPECT_EQ(state.contributors, 0);
	EXPECT_EQ(state.receipts[7], 0U);
	EXPECT_EQ(state.damage, debt);
	const auto second = AssignPartySpawnGeneration(*monster);
	ASSERT_NE(first, second);
	ReceivePartyMonsterDamage(7, damage);
	EXPECT_EQ(state.receipts[7], 0U);
	EXPECT_EQ(state.damage, debt);
}

TEST_F(MonsterSummonsTest, PartyDamageWaitsForItsSpawnAcrossDifferentConnections)
{
	InitItemTransfers();
	CommitPartyLevel(1, 8, 0, 0, 8);
	Monster *monster = AddMonster({ 40, 40 }, Direction::South, 1, true);
	ASSERT_NE(monster, nullptr);
	const auto oldGeneration = monster->partyGeneration;
	PartyDamageMessage damage { CMD_PARTYMONSTDAMAGE, 1, 8, 0, 1, static_cast<uint16_t>(monster->getId()), 128,
		8, monster->partyNativeMax.raw(), MT_NZOMBIE, static_cast<uint8_t>(UniqueMonsterType::None), 40, 40 };
	ReceivePartyMonsterDamage(7, damage);
	EXPECT_EQ(monster->partyGeneration, oldGeneration);
	ASSERT_EQ(GetPartyLevelState(1).pending.size(), 1U);
	ASSERT_TRUE(ResetPartyMonster(1, monster->getId(), 8));
	monster->maxHitPoints = monster->hitPoints = monster->partyNativeMax;
	InitializePartyMonster(*monster);
	DrainPartyDamage(1);
	EXPECT_TRUE(GetPartyLevelState(1).pending.empty());
	EXPECT_EQ(monster->partyGeneration, 8U);
	EXPECT_EQ(monster->hitPoints, monster->maxHitPoints - Fixed26_6::fromRaw(128));
	const auto after = monster->hitPoints;
	ReceivePartyMonsterDamage(7, damage);
	EXPECT_EQ(monster->hitPoints, after);
	EXPECT_FALSE(ResetPartyMonster(1, monster->getId(), 0));
}

TEST_F(MonsterSummonsTest, PartyEarlySyncDoesNotSuppressSpawnOrReviveItsReplayedGeneration)
{
	InitItemTransfers();
	CommitPartyLevel(1, 8, 0, 0, 8);
	MyPlayerId = 7;
	MyPlayer = &Players[7];
	PartyMonsterState incoming;
	incoming.generation = 8;
	incoming.nativeMax = 6400;
	incoming.type = MT_NZOMBIE;
	incoming.flags = PartyMonsterKnown;
	incoming.x = incoming.y = 40;
	std::array<std::byte, sizeof(TSyncPartyHeader) + sizeof(TSyncPartyMonster)> bytes {};
	auto &header = *reinterpret_cast<TSyncPartyHeader *>(bytes.data());
	header.native.bCmd = CMD_PARTYSYNCDATA;
	header.native.bLevel = 1;
	header.native.wLen = Swap16LE(sizeof(TSyncPartyMonster));
	header.participants = 8;
	header.membership = Swap32LE(MultiplayerMembershipDigest());
	auto &record = *reinterpret_cast<TSyncPartyMonster *>(bytes.data() + sizeof(header));
	record.native._mndx = 4;
	record.native._mx = record.native._my = 40;
	record.native._menemy = 0;
	record.party = EncodePartyMonster(incoming);
	EXPECT_EQ(OnPartySyncData(header, bytes.size(), Players[0]), bytes.size());
	EXPECT_EQ(GetPartyLevelState(1).monsters[4].generation, 8U);
	EXPECT_EQ(GetPartyLevelState(1).spawns[0], 0U);
	TCmdPartySpawn spawn {};
	spawn.native.bCmd = CMD_PARTYSPAWN;
	spawn.native.monsterId = Swap16LE(4);
	spawn.native.typeIndex = Swap16LE(1);
	spawn.native.dir = Direction::South;
	spawn.native.x = spawn.native.y = 40;
	spawn.native.seed = Swap32LE(17001);
	spawn.generation = Swap64LE(8);
	EXPECT_EQ(ParseCmd(0, reinterpret_cast<const TCmd *>(&spawn), sizeof(spawn)), sizeof(spawn));
	EXPECT_EQ(Monsters[4].partyGeneration, 8U);
	const auto health = Monsters[4].hitPoints;
	ApplyMonsterDamage(DamageType::Physical, Monsters[4], Fixed26_6::fromInt(1));
	EXPECT_EQ(ParseCmd(0, reinterpret_cast<const TCmd *>(&spawn), sizeof(spawn)), sizeof(spawn));
	EXPECT_EQ(Monsters[4].hitPoints, health - Fixed26_6::fromInt(1));
}

TEST_F(MonsterSummonsTest, PartyEnvironmentalDeathCannotGrantThePublisherExperience)
{
	InitItemTransfers();
	CommitPartyLevel(1, 8, 0, 0, 8);
	Monster *monster = AddMonster({ 40, 40 }, Direction::South, 1, true);
	ASSERT_NE(monster, nullptr);
	PartyDamageMessage damage { CMD_PARTYMONSTDAMAGE, 1, 8, 0, 1, static_cast<uint16_t>(monster->getId()), static_cast<uint32_t>(monster->hitPoints.raw()),
		monster->partyGeneration, monster->partyNativeMax.raw(), MT_NZOMBIE, static_cast<uint8_t>(UniqueMonsterType::None), 40, 40, 0 };
	ReceivePartyMonsterDamage(7, damage);
	CapturePartyMonsters();
	EXPECT_TRUE(monster->hasNoLife());
	EXPECT_EQ(monster->whoHit.bits, 0);
	const auto &state = GetPartyLevelState(1).monsters[monster->getId()];
	EXPECT_EQ(state.contributors, 0);
	PartyMonsterReward reward { CMD_PARTYREWARD, static_cast<uint16_t>(monster->getId()), state.generation,
		state.nativeMax, state.type, state.unique, 8, 40, 40 };
	const auto experience = MyPlayer->_pExperience;
	ASSERT_TRUE(ReservePartyReward(1, reward));
	ASSERT_TRUE(CommitPartyReward(1, reward, 7));
	EXPECT_EQ(MyPlayer->_pExperience, experience);
}

TEST_F(MonsterSummonsTest, PartyRestoredRewardReceiptsDoNotReplayExperienceLootOrCredits)
{
	InitItemTransfers();
	CommitPartyLevel(1, 8, 0, 0, 8);
	Monster *monster = AddMonster({ 40, 40 }, Direction::South, 1, true);
	ASSERT_NE(monster, nullptr);
	monster->tag(*MyPlayer);
	CapturePartyMonsters();
	const auto &state = GetPartyLevelState(1).monsters[monster->getId()];
	PartyMonsterReward reward { CMD_PARTYREWARD, static_cast<uint16_t>(monster->getId()), state.generation,
		state.nativeMax, state.type, state.unique, 8, 40, 40 };
	ASSERT_TRUE(ReservePartyReward(1, reward));
	ASSERT_TRUE(CommitPartyReward(1, reward, MyPlayerId));
	const auto experience = MyPlayer->_pExperience;
	const auto credits = GetPartyLevelState(1).lootRemainder;
	const auto seed = GetLCGEngineState();
	EXPECT_GT(experience, 0U);
	ASSERT_TRUE(RestorePartyRewardReceipt(1, reward));
	ASSERT_TRUE(CommitPartyReward(1, reward, MyPlayerId));
	EXPECT_EQ(MyPlayer->_pExperience, experience);
	EXPECT_EQ(GetPartyLevelState(1).lootRemainder, credits);
	EXPECT_EQ(GetLCGEngineState(), seed);
	// A fresh replica imported the paid world but missed its private XP callback.
	InitPartyBalance();
	MyPlayer->_pExperience = 0;
	ASSERT_TRUE(RestorePartyRewardReceipt(1, reward));
	EXPECT_EQ(MyPlayer->_pExperience, experience);
	ASSERT_TRUE(RestorePartyRewardReceipt(1, reward));
	EXPECT_EQ(MyPlayer->_pExperience, experience);
	EXPECT_EQ(GetPartyLevelState(1).lootRemainder, credits);
	EXPECT_EQ(GetLCGEngineState(), seed);
}

TEST_F(MonsterSummonsTest, RemoteExtendedDamageKeepsNativeRegenerationWithFourPlayers)
{
	InitItemTransfers();
	CommitPartyLevel(1, 4, 0, 0, 4);
	Monster *monster = AddMonster({ 40, 40 }, Direction::South, 1, true);
	ASSERT_NE(monster, nullptr);
	ApplyMonsterDamage(DamageType::Physical, *monster, Fixed26_6::fromInt(2));
	const auto wounded = monster->hitPoints;
	ASSERT_LT(wounded, monster->maxHitPoints);
	for (int tick = 0; tick < 32; ++tick)
		ProcessMonsters();
	const auto regenerated = monster->hitPoints;
	ASSERT_GT(regenerated, wounded);
	// Another player's hit, as received from the network. A classic engine subtracts it from the
	// local, regenerated health; extended sessions with four players must do the same.
	const auto &state = GetPartyLevelState(1).monsters[monster->getId()];
	PartyDamageMessage remote {};
	remote.command = CMD_PARTYMONSTDAMAGE;
	remote.level = 1;
	remote.participants = 4;
	remote.epoch = MultiplayerSlotEpoch(1);
	remote.receipt = 1;
	remote.monster = static_cast<uint16_t>(monster->getId());
	remote.damage = static_cast<uint32_t>(Fixed26_6::fromInt(1).raw());
	remote.generation = state.generation;
	remote.nativeMax = state.nativeMax;
	remote.type = state.type;
	remote.unique = state.unique;
	remote.x = state.x;
	remote.y = state.y;
	ReceivePartyMonsterDamage(1, remote);
	EXPECT_EQ(monster->hitPoints, regenerated - Fixed26_6::fromInt(1)) << "native regeneration was discarded by a remote hit";
}

TEST_F(MonsterSummonsTest, LethalRemoteHitWaitsForTheKillerInClassicButKillsInExtended)
{
	// Classic: a remote hit can only lower health to one; the attacker's own engine reports the kill.
	sgGameInitInfo.rules = ClassicSessionRules;
	Monster *monster = AddMonster({ 40, 40 }, Direction::South, 1, true);
	ASSERT_NE(monster, nullptr);
	TCmdMonDamage hit {};
	hit.bCmd = CMD_MONSTDAMAGE;
	hit.wMon = Swap16LE(static_cast<uint16_t>(monster->getId()));
	hit.dwDam = Swap32LE(static_cast<uint32_t>((monster->hitPoints + Fixed26_6::fromInt(5)).raw()));
	ASSERT_EQ(ParseCmd(1, reinterpret_cast<const TCmd *>(&hit), sizeof(hit)), sizeof(hit));
	EXPECT_EQ(monster->hitPoints, Fixed26_6::fromInt(1));
	EXPECT_FALSE(monster->hasNoLife());

	// Extended: the receipt that crosses zero is the single agreed death, whoever sent it.
	sgGameInitInfo.rules = ExtendedSessionRules;
	InitItemTransfers();
	CommitPartyLevel(1, 4, 0, 0, 4);
	Monster *partyMonster = AddMonster({ 44, 40 }, Direction::South, 1, true);
	ASSERT_NE(partyMonster, nullptr);
	CapturePartyMonsters();
	const auto &state = GetPartyLevelState(1).monsters[partyMonster->getId()];
	PartyDamageMessage remote {};
	remote.command = CMD_PARTYMONSTDAMAGE;
	remote.level = 1;
	remote.participants = 4;
	remote.epoch = MultiplayerSlotEpoch(1);
	remote.receipt = 1;
	remote.monster = static_cast<uint16_t>(partyMonster->getId());
	remote.damage = static_cast<uint32_t>((partyMonster->hitPoints + Fixed26_6::fromInt(5)).raw());
	remote.generation = state.generation;
	remote.nativeMax = state.nativeMax;
	remote.type = state.type;
	remote.unique = state.unique;
	remote.x = state.x;
	remote.y = state.y;
	ReceivePartyMonsterDamage(1, remote);
	EXPECT_TRUE(partyMonster->hasNoLife());
	EXPECT_NE(state.flags & PartyMonsterDead, 0);
	EXPECT_EQ(state.killer, 1);
}

TEST_F(MonsterSummonsTest, PartySharedDamageHasOnePublisherAndPreservesEnvironmentalContributions)
{
	InitItemTransfers();
	CommitPartyLevel(1, 8, 0, 0, 8);
	Monster *monster = AddMonster({ 40, 40 }, Direction::South, 1, true);
	ASSERT_NE(monster, nullptr);
	unsigned publishers = 0;
	for (uint8_t peer = 0; peer < 8; ++peer) {
		MyPlayerId = peer;
		MyPlayer = &Players[peer];
		publishers += IsPartyDamageAuthority();
	}
	EXPECT_EQ(publishers, 1U);
	MyPlayerId = 0;
	MyPlayer = &Players[0];
	const auto before = monster->hitPoints;
	ApplyMonsterDamage(DamageType::Fire, *monster, Fixed26_6::fromInt(1), false);
	EXPECT_EQ(monster->hitPoints, before - Fixed26_6::fromInt(1));
	EXPECT_EQ(monster->whoHit.bits, 0);
	SpawnGolem(Players[7], { 70, 70 }, 3);
	const Monster *golem = FindGolemForPlayer(Players[7]);
	ASSERT_NE(golem, nullptr);
	EXPECT_FALSE(IsPartyDamageAuthority(golem));
	MyPlayerId = 7;
	MyPlayer = &Players[7];
	EXPECT_TRUE(IsPartyDamageAuthority(golem));
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
		std::array<std::byte, 512> buffer {};
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
	std::array<std::byte, sizeof(TSyncHeader) + sizeof(TSyncMonster)> bytes {};
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
