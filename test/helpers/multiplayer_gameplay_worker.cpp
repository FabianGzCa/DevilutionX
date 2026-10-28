#include <algorithm>
#include <array>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <numeric>
#include <optional>
#include <span>
#include <sstream>
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
#include "monsters/party_balance.hpp"
#include "msg.h"
#include "multi.h"
#include "nthread.h"
#include "objects.h"
#include "options.h"
#include "player.h"
#include "players/level_entry.hpp"
#include "portal.h"
#include "storm/storm_net.hpp"
#include "sync.h"
#include "tables/playerdat.hpp"
#include "utils/endian_swap.hpp"
#include "utils/paths.h"

namespace devilution {
bool TestDeadItem(Player &player, Item &item, Displacement direction);
bool TestPlayerMeleeHit(Player &player, Monster &monster);
void TestDropHalfPlayersGold(Player &player);
void TestRegisterNetEventHandlers();
namespace {
std::filesystem::path Directory;
unsigned Role;
unsigned PeerCount = 8;
uint32_t ScenarioSeed = 19001;
bool Classic = false;
bool SessionScenario = false;
bool PrivateGame = false;
uint32_t ConfirmedTurn = 0;
uint32_t GenerationRng = 0;
unsigned BossId = MonsterStorageCapacity;
bool Captured = false;
bool ResumeSnapshot = false;
bool PartyLifecycle = false;
uint32_t LastEntryPump = 0;
std::optional<PartyDamageMessage> DelayedPartyDamage;

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
	if (SDL_GetTicks() - LastEntryPump >= 50) {
		LastEntryPump = SDL_GetTicks();
		ProcessLevelEntries();
	}
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
void ResetWorld(unsigned count, uint8_t humans = 0)
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
		Quests[i] = {};
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
	GetPartyLevelState(CurrentPartyLevel()).participants = humans != 0 ? humans : PeerCount;
	CapturePartyMonsters();
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
	CommitPartyLevel(CurrentPartyLevel(), 8, 0, MultiplayerSlotEpoch(0), 16);
	DeltaLoadLevel();
}

void LoadPartyWorld(uint8_t destination)
{
	CapturePartyMonsters();
	MyPlayer->setLevel(destination);
	currlevel = destination;
	setlevel = false;
	leveltype = destination == 0 ? DTYPE_TOWN : DTYPE_CATHEDRAL;
	gbBufferMsgs = 1;
	ActiveItemCount = 0;
	std::iota(std::begin(ActiveItems), std::end(ActiveItems), 0);
	std::memset(dItem, 0, sizeof(dItem));
	std::memset(dMonster, 0, sizeof(dMonster));
	std::memset(dPlayer, 0, sizeof(dPlayer));
	Missiles.clear();
	InitLighting();
	InitLevelMonsters();
	if (destination != 0) {
		Require(AddMonsterType(MT_GOLEM, PLACE_SPECIAL).has_value(), "cannot load party golem type");
		Require(AddMonsterType(MT_SKING, PLACE_SPECIAL).has_value(), "cannot load party boss type");
		Require(AddMonsterType(MT_DIABLO, PLACE_SPECIAL).has_value(), "cannot load party Diablo type");
		SetRndSeed(SessionScenario ? ScenarioSeed : 17001);
		InitGolems();
	}
	DeltaLoadLevel();
	gbBufferMsgs = 0;
	for (size_t i = 0; i < ActiveMonsterCount; ++i) {
		const auto id = ActiveMonsters[i];
		if (Monsters[id].type().type != MT_GOLEM)
			BossId = id;
	}
}
void PublishPartyState()
{
	if (!PartyLifecycle || currlevel == 0)
		return;
	std::array<std::byte, 512> bytes;
	const auto remaining = sync_all_monsters(bytes.data(), bytes.size());
	if (remaining != bytes.size())
		multi_send_msg_packet(PlayerRecipientMask { 255 }, bytes.data(), bytes.size() - remaining);
}

std::string Describe()
{
	std::vector<std::string> records;
	int worldGold = 0;
	for (int i = 0; i < ActiveItemCount; ++i) {
		const Item &item = Items[ActiveItems[i]];
		if (item._itype == ItemType::Gold)
			worldGold += item._ivalue;
		TCmdPItem wire {};
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
	if (PartyLifecycle) {
		const auto &level = GetPartyLevelState(1);
		size_t index = BossId;
		if (index >= MonsterStorageCapacity) {
			for (size_t i = 0; i < level.monsters.size(); ++i) {
				if ((level.monsters[i].flags & PartyMonsterKnown) != 0 && level.monsters[i].type == MT_SKING) {
					index = i;
					break;
				}
			}
		}
		const auto state = index < MonsterStorageCapacity ? level.monsters[index] : PartyMonsterState {};
		result += "\n" + std::to_string(level.participants) + " " + std::to_string(level.lootRemainder)
		    + " " + std::to_string(state.nativeMax) + " " + std::to_string(state.damage)
		    + " " + std::to_string(PartyMonsterHitPoints(state, level.participants).raw())
		    + " " + std::to_string(state.contributors) + " " + std::to_string(state.flags)
		    + " " + std::to_string(state.generation);
	}
	return result;
}
/** Native transport turns fence the checkpoint; wall-clock quiet is not its identity. */
void ConfirmTurn(unsigned stage)
{
	std::byte empty {};
	multi_send_msg_packet(PlayerRecipientMask { (1U << PeerCount) - 1 }, &empty, 0);
	FlushNetMessageBuffers();
	uint32_t turn = Swap32LE(stage + 1);
	Require(SNetSendTurn(reinterpret_cast<char *>(&turn), sizeof(turn)), "cannot send checkpoint turn");
	std::array<char *, PlayerStorageCapacity> data {};
	std::array<size_t, PlayerStorageCapacity> sizes {};
	std::array<uint32_t, PlayerStorageCapacity> status {};
	const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(15);
	while (!SNetReceiveTurns(Players.size(), data.data(), sizes.data(), status.data())) {
		Require(std::chrono::steady_clock::now() < deadline, "checkpoint turn did not arrive");
		Pump();
	}
	for (unsigned peer = 0; peer < PeerCount; ++peer) {
		Require((status[peer] & PS_TURN_ARRIVED) != 0 && sizes[peer] == sizeof(turn), "checkpoint omitted an active peer");
		uint32_t received;
		std::memcpy(&received, data[peer], sizeof(received));
		Require(Swap32LE(received) == stage + 1, "checkpoint mixed different turns");
	}
	Pump();
	ConfirmedTurn = stage + 1;
}

/** Field-wise serialization of replicated state, never pointers, padding or local inventory. */
std::string CanonicalState()
{
	std::ostringstream state;
	state << "turn " << ConfirmedTurn << " generation-rng " << GenerationRng << '\n';
	for (const Player &player : Players) {
		state << "player " << static_cast<unsigned>(player.getId()) << ' ' << player.plractive << " epoch " << MultiplayerSlotEpoch(static_cast<uint8_t>(player.getId()));
		if (player.plractive)
			state << ' ' << static_cast<unsigned>(player.plrlevel) << ' ' << player.plrIsOnSetLevel
			      << ' ' << +player.position.tile.x << ' ' << +player.position.tile.y
			      << ' ' << player._pLvlChanging << ' ' << player._pHitPoints.raw();
		state << '\n';
	}
	for (unsigned owner = 0; owner < Players.size(); ++owner) {
		const auto &portal = Portals[owner];
		state << "portal " << owner << ' ' << portal.open;
		if (portal.open)
			state << ' ' << portal.position.x << ' ' << portal.position.y << ' ' << portal.level << ' ' << static_cast<int>(portal.ltype) << ' ' << portal.setlvl;
		state << '\n';
	}
	for (const auto &quest : Quests)
		state << "quest " << static_cast<int>(quest._qidx) << ' ' << static_cast<int>(quest._qactive) << ' ' << static_cast<int>(quest._qvar1) << ' ' << static_cast<int>(quest._qvar2) << '\n';
	state << "end-shared\nlevel " << static_cast<unsigned>(currlevel) << ' ' << setlevel << '\n';
	std::vector<unsigned> monsters(ActiveMonsters, ActiveMonsters + ActiveMonsterCount);
	std::sort(monsters.begin(), monsters.end());
	for (unsigned id : monsters) {
		const auto &monster = Monsters[id];
		// Disconnect cleanup kills a summon locally on each peer. Its dead attribution awards no XP or loot.
		state << "monster " << id << ' ' << static_cast<int>(monster.type().type) << ' ' << +monster.position.tile.x << ' ' << +monster.position.tile.y
		      << ' ' << monster.hitPoints.raw() << ' ' << monster.maxHitPoints.raw() << ' ' << monster.aiSeed
		      << ' ' << (monster.isPlayerMinion() && monster.hitPoints == 0 ? 0U : static_cast<unsigned>(monster.whoHit.bits)) << ' ' << +monster.goalVar3 << '\n';
	}
	// Describe already canonicalizes floor items, including their generation seeds and properties.
	const auto items = Describe();
	const auto start = items.find('\n') + 1;
	state << "items " << ActiveItemCount << ' ' << items.substr(start, items.find('\n', start) - start) << '\n';
	state << "pools " << ActiveObjectCount << ' ' << Missiles.size() << '\n';
	return state.str();
}
std::string Checkpoint()
{
	const std::string state = CanonicalState();
	uint64_t digest = 14695981039346656037ULL;
	for (const unsigned char byte : state)
		digest = (digest ^ byte) * 1099511628211ULL;
	// Retain the canonical payload as well: failures cannot hide behind a hash collision.
	return Describe() + "\ncheckpoint " + std::to_string(ConfirmedTurn) + " " + std::to_string(digest) + "\n" + state;
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
	ResizePlayersForSession(Classic ? ClassicPlayerLimit : PlayerStorageCapacity);
	for (unsigned id = 0; id < Players.size(); ++id) {
		CreatePlayer(Players[id], HeroClass::Warrior);
		std::snprintf(Players[id]._pName, sizeof(Players[id]._pName), "Peer%u", id);
		Players[id].plractive = id < PeerCount;
		Players[id].setLevel(currlevel);
		Players[id].position.tile = { static_cast<uint8_t>(35 + id), 40 };
		Players[id].position.future = Players[id].position.tile;
	}
	MyPlayerId = Role;
	MyPlayer = &Players[Role];
	InspectPlayer = MyPlayer;
	gbActivePlayers = PeerCount;
	FreeCursor();
	InitCursor();
	InitGameInfo();
	sgGameInitInfo.rules = Classic ? ClassicSessionRules : ExtendedSessionRules;
	sgGameInitInfo.fullQuests = 0;
	if (SessionScenario) {
		sgGameInitInfo.gameSeed[0] = ScenarioSeed;
		sgGameInitInfo.gameSeed[1] = 19;
		sgGameInitInfo.gameSeed[2] = 1;
		sgGameInitInfo.gameSeed[3] = 8;
	}
	GetOptions().Network.port.SetValue(static_cast<uint16_t>(port));
	std::snprintf(GetOptions().Network.szBindAddress, sizeof(GetOptions().Network.szBindAddress), "%s", "127.0.0.1");
	provider = SELCONN_TCP;
	Require(SNetInitializeProvider(SELCONN_TCP, &sgGameInitInfo), "cannot initialize TCP");
	if (SessionScenario)
		TestRegisterNetEventHandlers();
	GameData wire = sgGameInitInfo;
	SwapGameDataLE(wire);
	int player;
	std::string address = "127.0.0.1:" + std::to_string(port);
	// Exercise the same classic-first negotiation as direct joins from the real menu.
	if (SessionScenario && Role != 0)
		sgGameInitInfo.rules = ClassicSessionRules;
	char password[] = "eight-engine-session";
	char *sessionPassword = PrivateGame ? password : nullptr;
	Require(Role == 0 ? SNetCreateGame("127.0.0.1", sessionPassword, reinterpret_cast<char *>(&wire), sizeof(wire), &player) : SNetJoinGame(address.data(), sessionPassword, &player), "cannot join TCP session");
	Require(player == static_cast<int>(Role), "unexpected assigned slot");
	gdwNormalMsgSize = gdwLargestMsgSize = 512;
	SetRndSeed(ScenarioSeed);
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
		const uint32_t rngBefore = GetLCGEngineState();
		if (action == "quit")
			break;
		if (action == "session-init") {
			ResetWorld(2);
			InitPortals();
			SetRndSeed(ScenarioSeed);
			Require(AddMonsterType(MT_GOLEM, PLACE_SPECIAL).has_value(), "cannot load session golems");
			Require(AddMonsterType(MT_SKING, PLACE_SPECIAL).has_value(), "cannot load session boss");
			InitGolems();
			GenerationRng = GetLCGEngineState();
		} else if (action == "session-entry" && !Classic) {
			for (Player &player : Players) {
				if (player.plractive)
					player._pLvlChanging = true;
			}
			BeginLevelEntry();
		} else if (action == "session-portal") {
			NetSendCmdLocParam3(true, CMD_ACTIVATEPORTAL, { 50 + static_cast<int>(Role) * 2, 50 }, currlevel, DTYPE_CATHEDRAL, 0);
		} else if (action == "session-golem" && actor == Role) {
			NetSendCmdLocParam1(true, CMD_REQUESTSPAWNGOLEM, { 45 + static_cast<int>(Role) * 2, 45 }, 1);
		} else if (action == "session-diverge" && actor == Role) {
			Quests[Q_GARBUD]._qvar1 = value == 1 ? 1 : 0;
			if (value >= 2)
				Monsters[0].aiSeed ^= 1;
			if (value == 3)
				LeaveItemTransfers(MyPlayerId);
		} else if (action == "party-init") {
			PartyLifecycle = true;
			ResetWorld(0, 4);
			for (unsigned id = 0; id < Players.size(); ++id)
				Players[id].setLevel(static_cast<uint8_t>(id < 4 ? 1 : 0));
			LoadPartyWorld(Role < 4 ? 1 : 0);
		} else if (action == "party-spawn" && Role == 0) {
			SpawnMonster({ 45, 40 }, Direction::South, 1);
			BossId = 4;
		} else if (action == "party-move" && Role == actor) {
			LoadPartyWorld(static_cast<uint8_t>(value));
			MyPlayer->_pLvlChanging = true;
			MyPlayer->_pmode = PM_NEWLVL;
			MyPlayer->_pInvincible = true;
			BeginLevelEntry();
		} else if (action == "party-stage-hit" && Role == actor) {
			BossId = 4;
			auto &monster = Monsters[BossId];
			ReconcilePartyMonster(monster);
			PartyDamageMessage message { CMD_PARTYMONSTDAMAGE, 1, GetPartyLevelState(1).participants, MultiplayerSlotEpoch(Role), 1, static_cast<uint16_t>(BossId), 10 * 64,
				monster.partyGeneration, monster.partyNativeMax.raw(), MT_SKING, static_cast<uint8_t>(UniqueMonsterType::None), 45, 40 };
			ReceivePartyMonsterDamage(Role, message);
			DelayedPartyDamage = message;
		} else if ((action == "party-release-hit" || action == "party-replay-hit") && Role == actor) {
			Require(DelayedPartyDamage.has_value(), "late damage is missing");
			NetSendPartyDamage(*DelayedPartyDamage);
		} else if (action == "party-dead") {
			Players[actor]._pmode = PM_DEATH;
			SetPlayerHitPoints(Players[actor], Fixed26_6::fromInt(0));
		} else if (action == "party-kill" && Role == actor) {
			BossId = 4;
			ApplyMonsterDamage(DamageType::Physical, Monsters[BossId], Monsters[BossId].hitPoints);
			M_StartKill(Monsters[BossId], *MyPlayer);
		} else if (action == "party-buffer" && Role == actor) {
			GetPartyLevelState(1) = {};
			msg_start_resync();
			gbDeltaSender = 0;
			ResumeSnapshot = true;
		} else if (action == "party-export" && Role == 0) {
			DeltaExportData(actor);
		} else if (action == "reset") {
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
			Monsters[BossId].maxHitPoints = Monsters[BossId].hitPoints = Fixed26_6::fromInt(50);
			InitializePartyMonster(Monsters[BossId]);
			Monsters[BossId].whoHit = {};
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
		PublishPartyState();
		Mark("sent-" + std::to_string(stage));
		Wait("settle-" + std::to_string(stage));
		if (action == "session-fault") {
			const auto timeout = std::chrono::steady_clock::now() + std::chrono::seconds(10);
			bool lost = false;
			while (std::chrono::steady_clock::now() < timeout) {
				ProcessGameMessagePackets();
				std::array<char *, PlayerStorageCapacity> turns {};
				std::array<size_t, PlayerStorageCapacity> sizes {};
				std::array<uint32_t, PlayerStorageCapacity> status {};
				SNetReceiveTurns(Players.size(), turns.data(), sizes.data(), status.data());
				if (actor == 0 || actor == Role) {
					lost = std::all_of(status.begin(), status.end(), [](uint32_t flags) { return (flags & PS_CONNECTED) == 0; });
				} else {
					lost = !Players[actor].plractive && !Portals[actor].open && FindGolemForPlayer(Players[actor]) == nullptr;
				}
				if (lost)
					break;
				std::this_thread::sleep_for(std::chrono::milliseconds(1));
			}
			Require(lost, "the native transport did not close or clean the departed slot");
			if (actor == 0 || actor == Role) {
				uint32_t turn = 0;
				Require(!SNetSendTurn(reinterpret_cast<char *>(&turn), sizeof(turn)), "disconnected TCP transport still accepts turns");
				Mark("result-" + std::to_string(stage), "closed");
				break;
			}
			Require(!RestoreLevelEntryPlayerInfo(Players[actor]), "departed slot kept its old level admission");
			--PeerCount;
			Require(gbActivePlayers == PeerCount, "departed peer stayed active");
			Mark("result-" + std::to_string(stage), "departed");
			continue;
		}
		// The parent waits for every source before asking for a stable result.
		// Keep draining the real network; the barrier does not process commands.
		const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(15);
		auto quiet = std::chrono::steady_clock::now();
		int received = dwRecCount;
		while (std::chrono::steady_clock::now() - quiet < std::chrono::milliseconds(300)) {
			if (std::chrono::steady_clock::now() >= deadline) {
				std::fprintf(stderr, "unresolved=%d pendingReward=%d localReward=%d level=%u\n", HasUnresolvedItemTransfers(), HasPendingPartyRewards(), HasLocalPartyRewardTransfer(), CurrentPartyLevel());
				for (size_t i = 0; i < GetPartyLevelState(CurrentPartyLevel()).monsters.size(); ++i) {
					const auto &state = GetPartyLevelState(CurrentPartyLevel()).monsters[i];
					if ((state.flags & PartyMonsterDead) != 0)
						std::fprintf(stderr, "monster=%zu flags=%u killer=%u native=%d generation=%llu\n", i, state.flags, state.killer, state.nativeMax, static_cast<unsigned long long>(state.generation));
				}
				Fail("transfer did not finish\n" + Describe());
			}
			Pump();
			if (HasUnresolvedItemTransfers() || ((PartyLifecycle || SessionScenario) && std::any_of(Players.begin(), Players.end(), [](const auto &player) { return player.plractive && player._pLvlChanging; })) || dwRecCount != received)
				quiet = std::chrono::steady_clock::now();
			received = dwRecCount;
		}
		if (SessionScenario) {
			ConfirmTurn(stage * 2);
			// Drain effects emitted while the first fence was in flight, then confirm again.
			const auto settled = std::chrono::steady_clock::now() + std::chrono::milliseconds(300);
			while (std::chrono::steady_clock::now() < settled)
				Pump();
			ConfirmTurn(stage * 2 + 1);
		}
		if (SessionScenario) {
			if (action == "session-entry" && !Classic) {
				Require(GetLCGEngineState() == rngBefore, "level admission consumed the world RNG");
				for (unsigned peer = 0; peer < PeerCount; ++peer) {
					const auto position = Players[peer].position.tile;
					Require(!Players[peer]._pLvlChanging && dPlayer[position.x][position.y] == static_cast<int>(peer + 1), "entry did not reserve a unique tile");
				}
			} else if (action == "session-portal") {
				for (unsigned peer = 0; peer < PeerCount; ++peer)
					Require(Portals[peer].open && Portals[peer].level == currlevel, "a participant lost its portal");
			} else if (action == "session-golem") {
				Require(FindGolemForPlayer(Players[actor]) != nullptr, "a participant lost its summon");
			} else if (action == "boss-hit") {
				Require(Monsters[BossId].whoHit.bits == (1U << PeerCount) - 1, "boss damage lost a contributor");
			} else if (action == "boss-kill") {
				Require(Monsters[BossId].hitPoints == 0 && Quests[Q_SKELKING]._qactive == QUEST_DONE, "boss death or quest completion did not arrive");
			}
		}
		Mark("result-" + std::to_string(stage), SessionScenario ? Checkpoint() : Describe());
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
	if (argc != 4 && argc != 8)
		return 2;
	devilution::Directory = argv[1];
	devilution::Role = static_cast<unsigned>(std::strtoul(argv[2], nullptr, 10));
	if (argc == 8) {
		devilution::PeerCount = static_cast<unsigned>(std::strtoul(argv[4], nullptr, 10));
		devilution::Classic = std::strcmp(argv[5], "classic") == 0;
		devilution::ScenarioSeed = static_cast<uint32_t>(std::strtoul(argv[6], nullptr, 10));
		devilution::SessionScenario = true;
		devilution::PrivateGame = std::strcmp(argv[7], "private") == 0;
		if (devilution::PeerCount == 0 || devilution::PeerCount > (devilution::Classic ? 4U : 8U) || devilution::Role >= devilution::PeerCount)
			return 2;
	}
	devilution::Run(static_cast<unsigned>(std::strtoul(argv[3], nullptr, 10)));
	return 0;
}
