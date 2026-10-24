#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <span>
#include <string>
#include <thread>
#include <vector>

#include "cursor.h"
#include "diablo.h"
#include "engine/assets.hpp"
#include "engine/random.hpp"
#include "game_mode.hpp"
#include "headless_mode.hpp"
#include "inv.h"
#include "levels/dun_tile_data.hpp"
#include "lighting.h"
#include "loadsave.h"
#include "missiles.h"
#include "monster.h"
#include "msg.h"
#include "multi.h"
#include "nthread.h"
#include "objects.h"
#include "options.h"
#include "player.h"
#include "storm/storm_net.hpp"
#include "tables/playerdat.hpp"
#include "utils/endian_swap.hpp"
#include "utils/paths.h"

namespace devilution {
bool TestDeadItem(Player &player, Item &item, Displacement direction);
bool TestPlayerMeleeHit(Player &player, Monster &monster);
void TestDropHalfPlayersGold(Player &player);
namespace {
std::filesystem::path Directory;
unsigned Role;
unsigned BossId = MonsterStorageCapacity;
bool Captured = false;
bool ResumeSnapshot = false;

[[noreturn]] void Fail(const std::string &message)
{
	std::fprintf(stderr, "peer %u: %s\n", Role, message.c_str());
	std::exit(1);
}
void Require(bool condition, const char *message)
{
	if (!condition)
		Fail(message);
}
void Mark(const std::string &name, const std::string &contents = "ready")
{
	const auto path = Directory / (name + "." + std::to_string(Role));
	const auto temporary = path.string() + ".tmp";
	{
		std::ofstream file(temporary);
		file << contents;
	}
	std::filesystem::rename(temporary, path);
}
void Pump()
{
	FlushNetMessageBuffers();
	ProcessGameMessagePackets();
	if (Role == 0 && !Captured && std::filesystem::exists(Directory / "capture-request") && IsItemTileReserved({ 41, 45 })) {
		Captured = true;
		DeltaExportData(7);
		Mark("captured");
	}
	if (ResumeSnapshot && TestHasCompleteDelta()) {
		ResumeSnapshot = false;
		DeltaLoadLevel();
		run_delta_info();
		Mark("restored");
	}
	Require(!gbGameDestroyed, "session terminated unexpectedly");
	std::this_thread::sleep_for(std::chrono::milliseconds(1));
}
void Wait(const std::string &name)
{
	const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(30);
	while (!std::filesystem::exists(Directory / name)) {
		Require(std::chrono::steady_clock::now() < deadline, "command timed out");
		Pump();
	}
}
void ResetWorld(unsigned count)
{
	delta_init();
	gbBufferMsgs = 0;
	for (int x = 0; x < MAXDUNX; ++x) {
		for (int y = 0; y < MAXDUNY; ++y) {
			dItem[x][y] = 0;
			dPlayer[x][y] = 0;
			dMonster[x][y] = 0;
			dObject[x][y] = 0;
			dPiece[x][y] = 1;
			dFlags[x][y] = DungeonFlag::None;
		}
	}
	SOLData[1] = TileProperties::None;
	ActiveItemCount = 0;
	Missiles.clear();
	InitLighting();
	ActiveObjectCount = 0;
	for (int i = 0; i < MAXOBJECTS; ++i)
		AvailableObjects[i] = i;
	BossId = MonsterStorageCapacity;
	InitLevelMonsters();
	for (int i = 0; i < MAXQUESTS; ++i) {
		Quests[i] = { };
		Quests[i]._qidx = static_cast<quest_id>(i);
		Quests[i]._qactive = QUEST_NOTAVAIL;
	}
	for (int i = 0; i < MAXITEMS; ++i)
		ActiveItems[i] = i;
	for (Player &player : Players) {
		player.HoldItem.clear();
		for (Item &item : player.InvBody)
			item.clear();
		for (Item &item : player.InvList)
			item.clear();
		for (Item &item : player.SpdList)
			item.clear();
		std::fill(std::begin(player.InvGrid), std::end(player.InvGrid), 0);
		player._pNumInv = 0;
		player._pGold = 0;
		player._pmode = PM_STAND;
		player._pInvincible = false;
		player._pBlockFlag = false;
		player._pMaxHPBase = player._pMaxHP = Fixed26_6::fromInt(100);
		SetPlayerHitPoints(player, Fixed26_6::fromInt(100));
	}
	for (unsigned i = 0; i < count; ++i) {
		Item item;
		MakeGoldStack(item, 75);
		item._iSeed = 1000 + i;
		item._iCreateInfo = CF_PREGEN;
		const int index = PlaceItemInWorld(std::move(item), { static_cast<uint8_t>(35 + i % 20), static_cast<uint8_t>(55 + i / 20) });
		DeltaAddItem(index);
	}
	InitItemTransfers();
	NewCursor(CURSOR_HAND);
}
void SwitchLevel(unsigned destination, unsigned count)
{
	currlevel = destination;
	for (Player &player : Players)
		player.setLevel(currlevel);
	ActiveItemCount = 0;
	for (int i = 0; i < MAXITEMS; ++i)
		ActiveItems[i] = i;
	for (auto &column : dItem)
		std::fill(std::begin(column), std::end(column), 0);
	for (unsigned i = 0; i < count; ++i) {
		Item item;
		MakeGoldStack(item, 75);
		item._iSeed = 1000 + i;
		item._iCreateInfo = CF_PREGEN;
		const int index = PlaceItemInWorld(std::move(item), { static_cast<uint8_t>(35 + i % 20), static_cast<uint8_t>(55 + i / 20) });
		DeltaAddItem(index);
	}
	DeltaLoadLevel();
}

std::string Describe()
{
	std::vector<std::string> records;
	int worldGold = 0;
	for (int i = 0; i < ActiveItemCount; ++i) {
		const Item &item = Items[ActiveItems[i]];
		if (item._itype == ItemType::Gold)
			worldGold += item._ivalue;
		TCmdPItem wire { };
		wire.bCmd = CMD_DROPITEM;
		wire.x = item.position.x;
		wire.y = item.position.y;
		wire.def.wIndx = static_cast<_item_indexes>(Swap16LE(item.IDidx));
		wire.def.wCI = Swap16LE(item._iCreateInfo);
		wire.def.dwSeed = Swap32LE(item._iSeed);
		if (item.IDidx == IDI_EAR)
			PrepareEarForNetwork(item, wire.ear);
		else
			PrepareItemForNetwork(item, wire.item);
		std::string record;
		constexpr char Digits[] = "0123456789abcdef";
		for (const std::byte byte : std::as_bytes(std::span(&wire, 1))) {
			const auto value = static_cast<uint8_t>(byte);
			record += Digits[value >> 4];
			record += Digits[value & 15];
		}
		for (const int property : { static_cast<int>(item._iAC), static_cast<int>(item._iMinDam), static_cast<int>(item._iMaxDam), item._iUid,
		         static_cast<int>(item._iPLDam), static_cast<int>(item._iPLToHit), static_cast<int>(item._iPLAC), static_cast<int>(item._iPLStr), static_cast<int>(item._iPLMag),
		         static_cast<int>(item._iPLDex), static_cast<int>(item._iPLVit), static_cast<int>(item._iPLFR), static_cast<int>(item._iPLLR), static_cast<int>(item._iPLMR),
		         static_cast<int>(item._iMinStr), static_cast<int>(item._iMinMag), static_cast<int>(item._iMinDex) })
			record += ":" + std::to_string(property);
		record += ":" + std::to_string(static_cast<uint32_t>(item._iFlags));
		records.push_back(std::move(record));
	}
	std::sort(records.begin(), records.end());
	std::string result = std::to_string(ActiveItemCount) + " " + std::to_string(MyPlayer->HoldItem.isEmpty()) + " " + std::to_string(CalculateGold(*MyPlayer)) + " " + std::to_string(MyPlayer->_pHitPoints.whole()) + " " + std::to_string(MyPlayer->_pExperience)
	    + " " + std::to_string(worldGold) + " " + std::to_string(MyPlayer->HoldItem.isEmpty() ? 0 : MyPlayer->HoldItem._ivalue) + " " + std::to_string(MyPlayer->HoldItem.isEmpty() ? 0 : MyPlayer->HoldItem._iSeed) + "\n";
	for (const auto &record : records)
		result += record + " ";
	result += "\n" + std::to_string(Quests[Q_SKELKING]._qactive) + " " + std::to_string(Quests[Q_DIABLO]._qactive) + " " + std::to_string(Missiles.size()) + " " + std::to_string(ActiveObjectCount);
	if (BossId < MonsterStorageCapacity)
		result += " " + std::to_string(Monsters[BossId].hitPoints.whole()) + " " + std::to_string(Monsters[BossId].whoHit.bits);
	return result;
}
void Run(unsigned port)
{
	HeadlessMode = true;
	LoadCoreArchives();
	LoadGameArchives();
	Require(HaveMainData() && !gbIsSpawn, "Diablo assets are required");
	const auto profile = Directory / ("profile-" + std::to_string(Role));
	std::filesystem::create_directory(profile);
	paths::SetPrefPath(profile.string() + "/");
	paths::SetConfigPath(profile.string() + "/");
	LoadOptions();
	GetOptions().Graphics.hardwareCursor.SetValue(false);
	LoadPlayerDataFiles();
	LoadItemData();
	LoadSpellData();
	LoadQuestData();
	LoadTextData();
	LoadMissileData();
	LoadMonsterData();
	LoadObjectData();
	gbIsMultiplayer = true;
	gbIsHellfire = false;
	gbIsSpawn = false;
	gbVanilla = false;
	currlevel = 1;
	setlevel = false;
	leveltype = DTYPE_CATHEDRAL;
	ResizePlayersForSession(PlayerStorageCapacity);
	for (unsigned id = 0; id < Players.size(); ++id) {
		CreatePlayer(Players[id], HeroClass::Warrior);
		std::snprintf(Players[id]._pName, sizeof(Players[id]._pName), "Peer%u", id);
		Players[id].plractive = true;
		Players[id].setLevel(currlevel);
		Players[id].position.tile = { static_cast<uint8_t>(35 + id), 40 };
		Players[id].position.future = Players[id].position.tile;
	}
	MyPlayerId = Role;
	MyPlayer = &Players[Role];
	InspectPlayer = MyPlayer;
	gbActivePlayers = 8;
	FreeCursor();
	InitCursor();
	InitGameInfo();
	sgGameInitInfo.rules = ExtendedSessionRules;
	sgGameInitInfo.fullQuests = 0;
	GetOptions().Network.port.SetValue(static_cast<uint16_t>(port));
	std::snprintf(GetOptions().Network.szBindAddress, sizeof(GetOptions().Network.szBindAddress), "%s", "127.0.0.1");
	provider = SELCONN_TCP;
	Require(SNetInitializeProvider(SELCONN_TCP, &sgGameInitInfo), "cannot initialize TCP");
	GameData wire = sgGameInitInfo;
	SwapGameDataLE(wire);
	int player;
	std::string address = "127.0.0.1:" + std::to_string(port);
	Require(Role == 0 ? SNetCreateGame("127.0.0.1", nullptr, reinterpret_cast<char *>(&wire), sizeof(wire), &player) : SNetJoinGame(address.data(), nullptr, &player), "cannot join TCP session");
	Require(player == static_cast<int>(Role), "unexpected assigned slot");
	gdwNormalMsgSize = gdwLargestMsgSize = 512;
	ResetWorld(2);
	Mark("ready");
	for (unsigned stage = 0;; ++stage) {
		const auto command = "command-" + std::to_string(stage);
		Wait(command);
		std::ifstream input(Directory / command);
		std::string action;
		unsigned actor = 0;
		unsigned value = 0;
		input >> action >> actor >> value;
		if (action == "quit")
			break;
		if (action == "reset") {
			ResetWorld(value);
		} else if (action == "burst" && actor == Role) {
			for (unsigned i = 0; i < MAXITEMS; ++i)
				CreateRndUseful({ 35 + static_cast<int>(i % 20), 55 + static_cast<int>(i / 20) }, true);
			ProcessItemTransfers();
			Require(ActiveItemCount == 0, "generated candidates were not staged");
			for (unsigned i = 0; i < MAXITEMS; ++i)
				CreateRndUseful({ 35 + static_cast<int>(i % 20), 55 + static_cast<int>(i / 20) }, true);
			Require(ActiveItemCount == 0, "unoffered candidates did not reserve their generation capacity");
		} else if (action == "block-reward-position") {
			Item item;
			MakeGoldStack(item, 75);
			item._iSeed = 2000;
			item._iCreateInfo = CF_PREGEN;
			DeltaAddItem(PlaceItemInWorld(std::move(item), { 35, 48 }));
		} else if (action == "level") {
			SwitchLevel(actor, value);
		} else if (action == "contend" || ((action == "drop" || action == "drop-sync" || action == "drop-finish") && actor == Role)) {
			if (action == "drop-sync") {
				std::ofstream capture(Directory / "capture-request");
			}
			MakeGoldStack(MyPlayer->HoldItem, 123);
			MyPlayer->HoldItem._iSeed = 9000 + Role;
			NewCursor(MyPlayer->HoldItem);
			Require(NetSendCmdPItem(true, CMD_PUTITEM, { 35 + static_cast<int>(Role), 45 }, MyPlayer->HoldItem), "drop request was not queued");
			if (action == "drop-finish") {
				FinishItemTransfers();
				const Item owned = MyPlayer->HoldItem;
				pfile_write_hero(false);
				MyPlayer->HoldItem.clear();
				Require(LoadHeldItem(*MyPlayer) == !owned.isEmpty(), "exit did not persist the cursor item");
				if (!owned.isEmpty())
					Require(MyPlayer->HoldItem.keyAttributesMatch(owned._iSeed, owned.IDidx, owned._iCreateInfo) && MyPlayer->HoldItem._ivalue == owned._ivalue, "exit changed private ownership");
			}
		} else if (action == "buffer" && actor == Role) {
			msg_start_resync();
			gbDeltaSender = 0;
			ResumeSnapshot = true;
		} else if (action == "redrop" && actor == Role) {
			Require(!MyPlayer->HoldItem.isEmpty(), "redrop source missing");
			Require(NetSendCmdPItem(true, CMD_PUTITEM, { 35 + static_cast<int>(Role), 45 }, MyPlayer->HoldItem), "redrop request was not queued");
		} else if ((action == "pickup" || action == "pickup-race" || action == "pickup-auto" || action == "pickup-dropped") && actor == Role) {
			const int index = FindGetItem(1000 + value, IDI_GOLD, action == "pickup-dropped" ? 0 : CF_PREGEN);
			Require(index != -1, "pickup source missing");
			const bool queued = NetSendCmdGItem(false, action == "pickup-auto" ? CMD_REQUESTAGITEM : CMD_REQUESTGITEM, *MyPlayer, ActiveItems[index]);
			Require(queued || !MyPlayer->HoldItem.isEmpty(), "pickup request was not queued");
			if (action == "pickup-race") {
				Require(queued, "race must begin with a queued request");
				InitializeItem(MyPlayer->HoldItem, IDI_HEAL);
				MyPlayer->HoldItem._iSeed = 8000;
			}
		} else if ((action == "fill" || action == "fill-inventory") && actor == Role) {
			for (int i = 0; i < InventoryGridCells; ++i) {
				InitializeItem(MyPlayer->InvList[i], IDI_HEAL);
				MyPlayer->InvGrid[i] = i + 1;
			}
			MyPlayer->_pNumInv = InventoryGridCells;
			if (action == "fill") {
				InitializeItem(MyPlayer->HoldItem, IDI_HEAL);
				MyPlayer->HoldItem._iSeed = 8000;
			}
		} else if (action == "release" && actor == Role) {
			MyPlayer->HoldItem.clear();
		} else if (action == "death" && actor == Role) {
			RecreateItem(*MyPlayer, MyPlayer->InvBody[INVLOC_HEAD], IDI_SKCROWN, 0, 7100 + Role, 0, 0);
			MakeGoldStack(MyPlayer->InvList[0], 4000);
			MyPlayer->InvGrid[0] = 1;
			MyPlayer->_pNumInv = 1;
			MyPlayer->_pGold = 4000;
			MyPlayer->_pmode = PM_DEATH;
			SetPlayerHitPoints(*MyPlayer, Fixed26_6::fromInt(0));
			Require(!TestDeadItem(*MyPlayer, MyPlayer->InvBody[INVLOC_HEAD], { 1, 0 }), "death removed gear before confirmation");
			TestDropHalfPlayersGold(*MyPlayer);
		} else if (action == "friendly-off" || action == "friendly-on") {
			sgGameInitInfo.bFriendlyFire = action == "friendly-on";
			MyPlayer->friendlyMode = true;
			Player &target = Players[(Role + value) % 8];
			if (action == "friendly-off") {
				Require(!TestPlayerMissileHit(*MyPlayer, target), "friendly fire bypassed protection");
			} else {
				bool hit = false;
				for (unsigned seed = 0; seed < 100 && !hit; ++seed) {
					SetRndSeed(seed);
					hit = TestPlayerMissileHit(*MyPlayer, target);
				}
				Require(hit, "missile never hit its target");
			}
		} else if (action == "resources") {
			Missiles.clear();
			for (unsigned id = 0; id < 8; ++id) {
				for (unsigned missile = 0; missile < 32; ++missile) {
					const auto *created = AddMissile({ 30, 30 }, { 50, 50 }, Direction::South, MissileID::Arrow, TARGET_MONSTERS, Players[id], 1, 0);
					Require(created != nullptr && created->_misource == static_cast<int>(id), "projectile lost its source");
				}
			}
			Require(Missiles.size() == 256, "projectile burst was truncated");
			for (int i = 0; i < MAXOBJECTS; ++i)
				Require(AddObject(OBJ_BARREL, { 75 + i % 15, 70 + i / 15 }) != nullptr, "object capacity shrank");
			Require(AddObject(OBJ_BARREL, { 100, 100 }) == nullptr && dObject[100][100] == 0, "object overflow overwrote a tile");
		} else if (action == "boss-types") {
			Require(AddMonsterType(MT_GOLEM, PLACE_SPECIAL).has_value(), "cannot load golem type");
			Require(AddMonsterType(MT_SKING, PLACE_SPECIAL).has_value(), "cannot load Leoric type");
			Require(AddMonsterType(MT_DIABLO, PLACE_SPECIAL).has_value(), "cannot load Diablo type");
			InitGolems();
		} else if (action == "boss-spawn" && Role == 0) {
			SpawnMonster({ 45, 40 }, Direction::South, value);
		} else if (action == "boss-health") {
			for (unsigned i = 0; i < ActiveMonsterCount; ++i) {
				const auto id = ActiveMonsters[i];
				if (!Monsters[id].isPlayerMinion())
					BossId = id;
			}
			Require(BossId < MonsterStorageCapacity, "boss spawn did not arrive");
			Monsters[BossId].hitPoints = Fixed26_6::fromInt(100);
			Monsters[BossId].whoHit = { };
		} else if (action == "boss-hit") {
			MyPlayer->_pIMinDam = MyPlayer->_pIMaxDam = 1;
			MyPlayer->_pDamageMod = MyPlayer->_pIBonusDamMod = MyPlayer->_pIBonusDam = 0;
			bool hit = false;
			for (unsigned seed = 0; seed < 100 && !hit; ++seed) {
				SetRndSeed(seed);
				hit = TestPlayerMeleeHit(*MyPlayer, Monsters[BossId]);
			}
			Require(hit, "melee attack never hit the boss");
		} else if (action == "boss-kill" && actor == Role) {
			ApplyMonsterDamage(DamageType::Physical, Monsters[BossId], Monsters[BossId].hitPoints);
			M_StartKill(Monsters[BossId], *MyPlayer);
		} else if (action == "resurrect" && (Role + 1) % 8 == actor) {
			NetSendCmdParam1(true, CMD_RESURRECT, static_cast<uint16_t>(actor));
		} else if (action == "damage") {
			NetSendCmdDamage(true, Players[(Role + 1) % 8], 64, DamageType::Physical);
		} else if (action == "quest-unique" && actor == Role) {
			SpawnUnique(UITEM_HARCREST, { 36, 48 });
		} else if (action == "quest-book" && actor == Role) {
			CreateTypeItem({ 35, 48 }, false, ItemType::Misc, IMISC_BOOK, false, false, true);
		} else if (action == "quest" && actor == Role) {
			SpawnQuestItem(IDI_BRAIN, { 35, 48 }, 0, SelectionRegion::Bottom, true);
		}
		Mark("sent-" + std::to_string(stage));
		Wait("settle-" + std::to_string(stage));
		// The parent waits for every source before asking for a stable result.
		// Keep draining the real network; the barrier does not process commands.
		const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(15);
		auto quiet = std::chrono::steady_clock::now();
		int received = dwRecCount;
		while (std::chrono::steady_clock::now() - quiet < std::chrono::milliseconds(300)) {
			Require(std::chrono::steady_clock::now() < deadline, "transfer did not finish");
			Pump();
			if (HasUnresolvedItemTransfers() || dwRecCount != received)
				quiet = std::chrono::steady_clock::now();
			received = dwRecCount;
		}
		Mark("result-" + std::to_string(stage), Describe());
	}
	delta_init();
	SNetDestroy();
	FreeCursor();
	Players.clear();
	MyPlayer = InspectPlayer = nullptr;
}
} // namespace
} // namespace devilution

int main(int argc, char **argv)
{
	if (argc != 4)
		return 2;
	devilution::Directory = argv[1];
	devilution::Role = static_cast<unsigned>(std::strtoul(argv[2], nullptr, 10));
	devilution::Run(static_cast<unsigned>(std::strtoul(argv[3], nullptr, 10)));
	return 0;
}
