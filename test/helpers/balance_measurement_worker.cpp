#include <algorithm>
#include <array>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <limits>
#include <optional>
#include <queue>
#include <set>
#include <sstream>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

#include <sys/resource.h>

#ifdef USE_SDL3
#include <SDL3/SDL.h>
#else
#include <SDL.h>
#endif

#include "controls/keymapper.hpp"
#include "cursor.h"
#include "diablo.h"
#include "engine/assets.hpp"
#include "game_mode.hpp"
#include "headless_mode.hpp"
#include "inv.h"
#include "items.h"
#include "items/validation.h"
#include "levels/gendung.h"
#include "levels/tile_properties.hpp"
#include "levels/trigs.h"
#include "lighting.h"
#include "lua/lua_global.hpp"
#include "menu.h"
#include "monster.h"
#include "monsters/party_balance.hpp"
#include "msg.h"
#include "multi.h"
#include "objects.h"
#include "options.h"
#include "pack.h"
#include "pfile.h"
#include "player.h"
#include "portal.h"
#include "spells.h"
#include "storm/storm_net.hpp"
#include "tables/itemdat.h"
#include "tables/monstdat.h"
#include "tables/playerdat.hpp"
#include "utils/endian_swap.hpp"
#include "utils/paths.h"
#include "utils/sdl_compat.h"

namespace devilution {
void TestRegisterNetEventHandlers();
namespace {

// One engine process per player. The bot below issues the same network commands
// as a player would, so damage, potions and deaths happen in the owner's engine.
struct Scenario {
	std::filesystem::path directory;
	unsigned role = 0;
	unsigned port = 0;
	unsigned players = 4;
	uint32_t seed = 1;
	_difficulty difficulty = DIFF_NORMAL;
	int level = 2;
	std::string composition = "W";
	int characterLevel = 8;
	unsigned maxTicks = 20 * 60 * 20;
	unsigned tickRate = 20;
	unsigned targets = 32;
	bool friendlyFire = false;
	bool classic = false;
	unsigned leaveAfterKills = 0;
	unsigned leavers = 0;
	/** Endurance mode: keep cycling town and dungeon levels for this many wall-clock seconds. */
	unsigned soakSeconds = 0;
	/** Endurance mode: every this many seconds one of the last three slots leaves in town and a new process rejoins. */
	unsigned leaveEverySeconds = 0;
	/** Zero for the first process of a slot; a rejoining process counts up from one. */
	unsigned incarnation = 0;
};

Scenario Config;
/** objects.cpp stores a closed door as zero in _oVar4. */
constexpr int DoorClosed = 0;

[[noreturn]] void Fail(const std::string &message)
{
	std::fprintf(stderr, "balance peer %u: %s\n", Config.role, message.c_str());
	std::fflush(stderr);
	std::_Exit(1);
}

void Require(bool condition, const char *message)
{
	if (!condition)
		Fail(message);
}

void Mark(const std::string &name, const std::string &contents = "ready")
{
	const auto path = Config.directory / name;
	const auto temporary = path.string() + ".tmp";
	{
		std::ofstream file(temporary);
		file << contents;
	}
	std::filesystem::rename(temporary, path);
}

bool Exists(const std::string &name)
{
	return std::filesystem::exists(Config.directory / name);
}

HeroClass ClassForSlot(unsigned slot)
{
	switch (Config.composition[slot % Config.composition.size()]) {
	case 'R':
		return HeroClass::Rogue;
	case 'S':
		return HeroClass::Sorcerer;
	default:
		return HeroClass::Warrior;
	}
}

_item_indexes FindBaseItem(std::string_view name)
{
	for (size_t i = 0; i < AllItemsList.size(); ++i) {
		if (AllItemsList[i].iName == name)
			return static_cast<_item_indexes>(i);
	}
	Fail("unknown base item " + std::string(name));
}

/**
 * Generate a native magic item. With a bonus target, scan a fixed seed range and keep the first
 * item whose enhanced damage (weapons) or armor (armor) reaches it, so every replica of the same
 * class and level receives the same natively generated item.
 */
void GiveItem(Player &player, _item_indexes index, uint32_t seed, int quality, int bonusTarget = 0)
{
	Item item;
	const bool weapon = AllItemsList[index].iClass == ICLASS_WEAPON;
	int bestBonus = -1;
	uint32_t bestSeed = seed;
	for (uint32_t candidate = seed; candidate < seed + (bonusTarget > 0 ? 20000U : 1U); ++candidate) {
		SetupAllItems(player, item, index, candidate, quality, 1, true, false, 0, true);
		const int bonus = weapon ? item._iPLDam : item._iPLAC;
		if (item._iPLToHit < 0 || item._iPLDam < 0 || item._iPLAC < 0 || bonus <= bestBonus)
			continue;
		bestBonus = bonus;
		bestSeed = candidate;
		if (bonus >= bonusTarget)
			break;
	}
	SetupAllItems(player, item, index, bestSeed, quality, 1, true, false, 0, true);
	item._iIdentified = true;
	item.updateRequiredStatsCacheForPlayer(player);
	Require(AutoEquip(player, item), "generated item cannot be equipped by this build");
}

void GivePotion(Player &player, _item_indexes index, bool belt)
{
	Item item;
	InitializeItem(item, index);
	GenerateNewSeed(item);
	item.updateRequiredStatsCacheForPlayer(player);
	if (belt && AutoPlaceItemInBelt(player, item, true))
		return;
	AutoPlaceItemInInventory(player, item);
}

/**
 * Party members of one class are identical in every party size: the same level,
 * attributes, generated gear (fixed seeds) and potions.
 */
void CreateHero(Player &player, HeroClass heroClass)
{
	CreatePlayer(player, heroClass);
	// Creating a player frees the temporary starting-inventory sprites.
	InitCursor();
	std::snprintf(player._pName, sizeof(player._pName), "Bot%u", Config.role);
	const int level = std::clamp(Config.characterLevel, 1, 50);
	while (player.getCharacterLevel() < level)
		player.addExperience(player.getNextExperienceThreshold() - player._pExperience);
	// Spend the stat points the way each class is usually built.
	CharacterAttribute primary = CharacterAttribute::Magic;
	CharacterAttribute secondary = CharacterAttribute::Vitality;
	if (heroClass == HeroClass::Warrior) {
		primary = CharacterAttribute::Strength;
		secondary = CharacterAttribute::Dexterity;
	} else if (heroClass == HeroClass::Rogue) {
		primary = CharacterAttribute::Dexterity;
		secondary = CharacterAttribute::Strength;
	}
	const auto spend = [&](CharacterAttribute attribute) {
		if (player.GetBaseAttributeValue(attribute) >= player.GetMaximumAttributeValue(attribute))
			return false;
		switch (attribute) {
		case CharacterAttribute::Strength: ModifyPlrStr(player, 1); break;
		case CharacterAttribute::Magic: ModifyPlrMag(player, 1); break;
		case CharacterAttribute::Dexterity: ModifyPlrDex(player, 1); break;
		case CharacterAttribute::Vitality: ModifyPlrVit(player, 1); break;
		}
		player._pStatPts--;
		return true;
	};
	while (player._pStatPts > 0) {
		// Three points to the main attribute, one to the second and one to vitality.
		const unsigned pattern = player._pStatPts % 5;
		CharacterAttribute choice = CharacterAttribute::Vitality;
		if (pattern < 3)
			choice = primary;
		else if (pattern == 3)
			choice = secondary;
		if (!spend(choice) && !spend(CharacterAttribute::Vitality) && !spend(CharacterAttribute::Strength)
		    && !spend(CharacterAttribute::Dexterity) && !spend(CharacterAttribute::Magic))
			player._pStatPts = 0;
	}

	for (auto &item : player.InvBody)
		item.clear();
	for (auto &item : player.InvList)
		item.clear();
	for (auto &item : player.SpdList)
		item.clear();
	std::memset(player.InvGrid, 0, sizeof(player.InvGrid));
	player._pNumInv = 0;
	player._pGold = 0;

	// Dungeon items must carry a level that some monster or dungeon can produce.
	int quality = std::clamp(level + 5, 1, 60);
	while (quality > 1 && !IsDungeonItemValid(static_cast<uint16_t>(quality), 0))
		--quality;
	const int damageTarget = std::min(4 * level, 150);
	const int armorTarget = std::min(3 * level, 100);
	const uint32_t seedBase = 0x20B0'0000U + static_cast<uint32_t>(level) * 64U;
	if (heroClass == HeroClass::Warrior) {
		const char *sword = level < 15 ? "Broad Sword" : "Great Sword";
		if (level >= 15 && level < 30)
			sword = "Bastard Sword";
		GiveItem(player, FindBaseItem(sword), seedBase + 1, quality, damageTarget);
		if (level < 30)
			GiveItem(player, FindBaseItem(level < 15 ? "Small Shield" : "Kite Shield"), seedBase + 2, quality, armorTarget);
	} else if (heroClass == HeroClass::Rogue) {
		const char *bow = level < 15 ? "Hunter's Bow" : "Long War Bow";
		if (level >= 15 && level < 30)
			bow = "Composite Bow";
		GiveItem(player, FindBaseItem(bow), seedBase + 3, quality, damageTarget);
	} else {
		GiveItem(player, FindBaseItem("Short Staff"), seedBase + 4, quality);
		for (const SpellID spell : { SpellID::Lightning, SpellID::ManaShield }) {
			if (player._pMagic < GetSpellData(spell).minInt)
				continue;
			player._pMemSpells |= GetSpellBitmask(spell);
			player._pSplLvl[static_cast<int8_t>(spell)] = 1;
		}
		if (level >= 15 && player._pMagic >= GetSpellData(SpellID::ChainLightning).minInt) {
			player._pMemSpells |= GetSpellBitmask(SpellID::ChainLightning);
			player._pSplLvl[static_cast<int8_t>(SpellID::ChainLightning)] = static_cast<uint8_t>(std::clamp(level / 5, 1, 15));
		}
		player._pMemSpells |= GetSpellBitmask(SpellID::Firebolt);
		player._pSplLvl[static_cast<int8_t>(SpellID::Firebolt)] = static_cast<uint8_t>(std::clamp(level / 3, 1, 15));
		if (level >= 15) {
			player._pMemSpells |= GetSpellBitmask(SpellID::Fireball);
			player._pSplLvl[static_cast<int8_t>(SpellID::Fireball)] = static_cast<uint8_t>(std::clamp(level / 5, 1, 15));
		}
	}
	const char *armor = "Leather Armor";
	if (heroClass == HeroClass::Sorcerer)
		armor = "Robe";
	else if (heroClass == HeroClass::Warrior && level >= 15)
		armor = level < 30 ? "Chain Mail" : "Splint Mail";
	GiveItem(player, FindBaseItem(armor), seedBase + 5, quality, armorTarget);
	GiveItem(player, FindBaseItem(level < 15 || heroClass == HeroClass::Sorcerer ? "Cap" : "Helm"), seedBase + 6, quality, armorTarget);

	const bool caster = heroClass == HeroClass::Sorcerer;
	for (int i = 0; i < MaxBeltItems; ++i)
		GivePotion(player, caster && i >= 4 ? IDI_MANA : IDI_HEAL, true);
	for (int i = 0; i < 16; ++i)
		GivePotion(player, caster && i >= 6 ? IDI_MANA : IDI_HEAL, false);
	if (Config.soakSeconds != 0) {
		// Endurance cycles return to town through each bot's own town portal.
		for (int i = 0; i < 12; ++i)
			GivePotion(player, IDI_PORTAL, false);
	}
	CalcPlrInv(player, false);
	player._pHitPoints = player._pMaxHP;
	player._pHPBase = player._pMaxHPBase;
	player._pMana = player._pMaxMana;
	player._pManaBase = player._pMaxManaBase;
}

void WriteHeroManifest(const Player &player)
{
	std::ostringstream out;
	out << "{\"class\":" << static_cast<int>(player._pClass) << ",\"level\":" << static_cast<unsigned>(player.getCharacterLevel())
	    << ",\"strength\":" << player._pStrength << ",\"magic\":" << player._pMagic
	    << ",\"dexterity\":" << player._pDexterity << ",\"vitality\":" << player._pVitality
	    << ",\"hp\":" << player._pMaxHP.raw() << ",\"mana\":" << player._pMaxMana.raw()
	    << ",\"minDamage\":" << player._pIMinDam << ",\"maxDamage\":" << player._pIMaxDam
	    << ",\"ac\":" << player.GetArmor() << ",\"gear\":[";
	bool first = true;
	for (const Item &item : player.InvBody) {
		out << (first ? "" : ",") << '[' << item.IDidx << ',' << item._iSeed << ',' << item._iCreateInfo << ']';
		first = false;
	}
	out << "],\"spells\":[";
	first = true;
	for (const uint8_t level : player._pSplLvl) {
		out << (first ? "" : ",") << static_cast<unsigned>(level);
		first = false;
	}
	out << "]}\n";
	Mark("hero-" + std::to_string(Config.role) + ".json", out.str());
}

bool SelectGame(GameData *gameData, int *playerId)
{
	static bool used = false;
	if (used)
		return false; // The measurement ended; leave the menu loop.
	used = true;
	gameData->rules = Config.classic ? ClassicSessionRules : ExtendedSessionRules;
	gameData->nDifficulty = Config.difficulty;
	gameData->nTickRate = static_cast<uint8_t>(Config.tickRate);
	gameData->bRunInTown = 1;
	gameData->bFriendlyFire = Config.friendlyFire ? 1 : 0;
	gameData->fullQuests = 0;
	gameData->gameSeed[0] = Config.seed;
	gameData->gameSeed[1] = 0x20;
	gameData->gameSeed[2] = static_cast<uint32_t>(Config.level);
	gameData->gameSeed[3] = static_cast<uint32_t>(Config.difficulty);
	GetOptions().Network.port.SetValue(static_cast<uint16_t>(Config.port));
	std::snprintf(GetOptions().Network.szBindAddress, sizeof(GetOptions().Network.szBindAddress), "%s", "127.0.0.1");
	provider = SELCONN_TCP;
	Require(SNetInitializeProvider(SELCONN_TCP, gameData), "cannot initialize TCP");
	TestRegisterNetEventHandlers();
	if (Config.role == 0) {
		GameData wire = *gameData;
		SwapGameDataLE(wire);
		Require(SNetCreateGame("127.0.0.1", nullptr, reinterpret_cast<char *>(&wire), sizeof(wire), playerId), "cannot create TCP game");
		Mark("created");
	} else {
		const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(60);
		while (!Exists("joined-" + std::to_string(Config.role - 1)) && !(Config.role == 1 && Exists("created"))) {
			Require(std::chrono::steady_clock::now() < deadline, "previous peer did not join");
			std::this_thread::sleep_for(std::chrono::milliseconds(20));
		}
		std::string address = "127.0.0.1:" + std::to_string(Config.port);
		Require(SNetJoinGame(address.data(), nullptr, playerId), "cannot join TCP game");
	}
	if (*playerId != static_cast<int>(Config.role))
		Fail("unexpected player slot: expected " + std::to_string(Config.role) + ", received " + std::to_string(*playerId));
	Mark("joined-" + std::to_string(Config.role));
	return true;
}

enum class Phase : uint8_t {
	Gathering,
	Descending,
	Waiting,
	Fighting,
	Reporting,
	Done,
	Portal,
};

struct MonsterRecord {
	unsigned id;
	unsigned firstDamage = 0;
	unsigned death = 0;
	bool unique;
};

struct Bot {
	Phase phase = Phase::Gathering;
	unsigned tick = 0;
	unsigned fightStart = 0;
	unsigned fightEnd = 0;
	unsigned nextDecision = 0;
	unsigned stuckTicks = 0;
	Point previousTile {};
	int target = -1;
	uint32_t startExperience = 0;
	unsigned healingPotions = 0;
	unsigned manaPotions = 0;
	unsigned deaths = 0;
	bool wasDead = false;
	bool left = false;
	std::vector<MonsterRecord> monsters;
	std::set<int> seenItems;
	unsigned lootItems = 0;
	unsigned eligibleDeaths = 0;
	unsigned paidDeaths = 0;
	unsigned ownContributions = 0;
	unsigned blockedLoot = 0;
	unsigned livingTicks = 0;
	unsigned congestedTicks = 0;
	unsigned maxTurns = 0;
	unsigned settleTicks = 0;
	uint64_t hpAtStart = 0;
	std::vector<std::array<unsigned, 4>> rewards;
	std::vector<std::array<uint64_t, 5>> drops;
	unsigned lootGold = 0;
	uint64_t lootValue = 0;
	unsigned idleTicks = 0;
	unsigned targetUnchangedTicks = 0;
	int observedTarget = -1;
	int32_t observedHp = 0;
	std::array<unsigned, MonsterStorageCapacity> blockedUntil {};
	std::vector<double> tickMs;
	std::vector<double> logicMs;
	std::chrono::steady_clock::time_point logicStart;
	std::chrono::steady_clock::time_point lastTick;
	std::string outcome = "timeout";
	std::array<uint8_t, MAXDUNX * MAXDUNY> reachable {};
	// Endurance mode.
	int level = 0;
	unsigned cycle = 0;
	unsigned townTicks = 0;
	bool rejoinFromTown = false;
	bool portalCast = false;
	bool warpSent = false;
	unsigned portalTicks = 0;
	unsigned portalFallbacks = 0;
	unsigned strayLevels = 0;
	int64_t soakEpoch = 0;
	unsigned leavesHandled = 0;
	std::chrono::steady_clock::time_point soakStart;
	size_t sampledTicks = 0;
	size_t sampledLogic = 0;
};

Bot State;

void ObserveReward(const Monster &monster)
{
	if (State.phase != Phase::Fighting && State.phase != Phase::Reporting)
		return;
	if (monster.isPlayerMinion())
		return;
	++State.paidDeaths;
	if (IsPartyRandomLoot(monster.type().type, static_cast<uint8_t>(monster.uniqueType)))
		++State.eligibleDeaths;
	const unsigned mask = PlayerExperienceMaskToByte(monster.whoHit);
	if ((mask & (1U << MyPlayerId)) != 0 && !MyPlayer->hasNoLife())
		++State.ownContributions;
	State.rewards.push_back({ State.tick - State.fightStart, static_cast<unsigned>(monster.getId()), static_cast<unsigned>(monster.type().type), mask });
}

void ObserveLoot(const Monster &monster, const Item *item)
{
	if (State.phase != Phase::Fighting && State.phase != Phase::Reporting)
		return;
	if (!IsPartyRandomLoot(monster.type().type, static_cast<uint8_t>(monster.uniqueType)))
		return;
	if (item == nullptr) {
		++State.blockedLoot;
		return;
	}
	if (item->_itype == ItemType::Gold)
		State.lootGold += static_cast<unsigned>(item->_ivalue);
	else {
		++State.lootItems;
		State.lootValue += static_cast<uint64_t>(item->_iIvalue);
	}
	State.drops.push_back({ State.tick - State.fightStart, static_cast<uint64_t>(item->IDidx), item->_iSeed, item->_iCreateInfo, static_cast<uint64_t>(item->_iIvalue) });
}

bool Walkable(Point p)
{
	for (int i = 0; i < numtrigs; ++i) {
		if (p == trigs[i].position)
			return false;
	}
	return InDungeonBounds(p) && IsTileWalkable(p, true);
}

void ComputeReachable(Point start)
{
	State.reachable.fill(0);
	std::queue<Point> open;
	open.push(start);
	State.reachable[start.x * MAXDUNY + start.y] = 1;
	while (!open.empty()) {
		const Point p = open.front();
		open.pop();
		for (const Direction d : { Direction::North, Direction::NorthEast, Direction::East, Direction::SouthEast, Direction::South, Direction::SouthWest, Direction::West, Direction::NorthWest }) {
			const Point n = p + d;
			if (!InDungeonBounds(n) || !CanStep(p, n) || State.reachable[n.x * MAXDUNY + n.y] != 0)
				continue;
			// Monsters stand on walkable tiles; treat them as passable for reachability.
			if (!IsTileWalkable(n, true) && dMonster[n.x][n.y] == 0)
				continue;
			State.reachable[n.x * MAXDUNY + n.y] = 1;
			open.push(n);
		}
	}
}

/** Next waypoint about ten steps along a breadth-first path to the goal, opening doors on the way. */
std::optional<Point> Waypoint(Point from, Point goal)
{
	std::vector<int16_t> parent(MAXDUNX * MAXDUNY, -1);
	auto index = [](Point p) { return p.x * MAXDUNY + p.y; };
	std::queue<Point> open;
	open.push(from);
	parent[index(from)] = static_cast<int16_t>(index(from));
	bool found = false;
	while (!open.empty() && !found) {
		const Point p = open.front();
		open.pop();
		for (const Direction d : { Direction::North, Direction::NorthEast, Direction::East, Direction::SouthEast, Direction::South, Direction::SouthWest, Direction::West, Direction::NorthWest }) {
			const Point n = p + d;
			if (!InDungeonBounds(n) || !CanStep(p, n) || parent[index(n)] != -1)
				continue;
			if (n.WalkingDistance(goal) <= 1 && Walkable(n) && dMonster[n.x][n.y] == 0 && dPlayer[n.x][n.y] == 0) {
				parent[index(n)] = static_cast<int16_t>(index(p));
				open.push(n);
				found = true;
				break;
			}
			if (!Walkable(n) || (dPlayer[n.x][n.y] != 0 && std::abs(dPlayer[n.x][n.y]) != MyPlayerId + 1))
				continue;
			parent[index(n)] = static_cast<int16_t>(index(p));
			open.push(n);
		}
	}
	if (!found)
		return std::nullopt;
	Point step = open.back();
	std::vector<Point> path;
	while (step != from) {
		path.push_back(step);
		const int previous = parent[index(step)];
		step = { previous / MAXDUNY, previous % MAXDUNY };
	}
	std::reverse(path.begin(), path.end());
	for (size_t i = 0; i < path.size() && i < 10; ++i) {
		Object *object = FindObjectAtPosition(path[i]);
		if (object != nullptr && object->isDoor() && object->_oVar4 == DoorClosed) {
			return path[i];
		}
	}
	return path.front();
}

bool Alive(const Monster &monster)
{
	return monster.hitPoints.whole() > 0 && monster.mode != MonsterMode::Death;
}

bool IsActiveMonster(unsigned id)
{
	for (size_t i = 0; i < ActiveMonsterCount; ++i) {
		if (ActiveMonsters[i] == id)
			return true;
	}
	return false;
}

bool UsePotion(Player &player, bool mana)
{
	for (int i = 0; i < MaxBeltItems; ++i) {
		const Item &item = player.SpdList[i];
		if (!item.isEmpty() && item._iMiscId == (mana ? IMISC_MANA : IMISC_HEAL)) {
			if (UseInvItem(INVITEM_BELT_FIRST + i))
				return true;
		}
	}
	for (int i = 0; i < player._pNumInv; ++i) {
		const Item &item = player.InvList[i];
		if (!item.isEmpty() && item._iMiscId == (mana ? IMISC_MANA : IMISC_HEAL)) {
			if (UseInvItem(INVITEM_INV_FIRST + i))
				return true;
		}
	}
	return false;
}

std::string TargetsFile()
{
	return State.cycle == 0 ? "targets" : "targets-" + std::to_string(State.cycle);
}

bool PartyOnLevel()
{
	unsigned present = 0;
	unsigned active = 0;
	for (const Player &player : Players) {
		active += player.plractive ? 1 : 0;
		if (player.plractive && player.isOnActiveLevel() && !player._pLvlChanging)
			++present;
	}
	// In endurance mode a bot may be away between leaving and rejoining.
	return present == (Config.soakSeconds != 0 ? active : Config.players);
}

void BeginFight()
{
	State.phase = Phase::Fighting;
	State.fightStart = State.tick;
	State.startExperience = MyPlayer->_pExperience;
	State.hpAtStart = MyPlayer->_pHitPoints.raw();
	const auto manifest = Config.directory / TargetsFile();
	if (!std::filesystem::exists(manifest)) {
		Require(Config.role == 0, "target selection must come from the first peer");
		Point anchor = MyPlayer->position.tile;
		for (int i = 0; i < numtrigs; ++i) {
			if (trigs[i]._tmsg == WM_DIABPREVLVL) {
				anchor = trigs[i].position;
				break;
			}
		}
		ComputeReachable(anchor);
		std::vector<unsigned> ids;
		for (size_t i = 0; i < ActiveMonsterCount; ++i) {
			const unsigned id = ActiveMonsters[i];
			const Monster &monster = Monsters[id];
			if (monster.isPlayerMinion() || !Alive(monster))
				continue;
			if (State.reachable[monster.position.tile.x * MAXDUNY + monster.position.tile.y] != 0)
				ids.push_back(id);
		}
		std::sort(ids.begin(), ids.end(), [&](unsigned a, unsigned b) {
			const int da = anchor.WalkingDistance(Monsters[a].position.tile);
			const int db = anchor.WalkingDistance(Monsters[b].position.tile);
			return da == db ? a < b : da < db;
		});
		if (Config.targets != 0 && ids.size() > Config.targets)
			ids.resize(Config.targets);
		std::ostringstream out;
		for (const unsigned id : ids)
			out << id << ' ';
		Mark(TargetsFile(), out.str());
	}
	std::ifstream in(manifest);
	unsigned id;
	while (in >> id) {
		Require(id < MonsterStorageCapacity && !Monsters[id].isPlayerMinion(), "invalid encounter target");
		State.monsters.push_back({ id, 0, 0, Monsters[id].isUnique() });
	}
	// An endurance cycle may revisit a level whose targets are already dead.
	Require(Config.soakSeconds != 0 || !State.monsters.empty(), "no reachable targets were selected");
}

void Observe()
{
	if (MyPlayer->hasNoLife() && !State.wasDead)
		++State.deaths;
	State.wasDead = MyPlayer->hasNoLife();
	bool remaining = false;
	for (MonsterRecord &record : State.monsters) {
		if (record.death != 0)
			continue;
		const Monster &monster = Monsters[record.id];
		if (record.firstDamage == 0 && monster.hitPoints < monster.maxHitPoints)
			record.firstDamage = State.tick;
		if (!IsActiveMonster(record.id) || !Alive(monster)) {
			record.death = State.tick;
			continue;
		}
		remaining = true;
	}

	bool partyAlive = false;
	for (const Player &player : Players) {
		if (player.plractive && player.isOnActiveLevel() && !player.hasNoLife())
			partyAlive = true;
	}
	if (!remaining) {
		State.outcome = "cleared";
		State.fightEnd = State.tick;
		State.phase = Phase::Reporting;
	} else if (!partyAlive) {
		State.outcome = "wiped";
		State.fightEnd = State.tick;
		State.phase = Phase::Reporting;
	} else if (State.tick - State.fightStart >= Config.maxTicks) {
		State.outcome = "timeout";
		State.fightEnd = State.tick;
		State.phase = Phase::Reporting;
	}
}

int ChooseTarget(const Player &player)
{
	int threat = -1;
	int threatDistance = std::numeric_limits<int>::max();
	for (size_t i = 0; i < ActiveMonsterCount; ++i) {
		const unsigned id = ActiveMonsters[i];
		const Monster &monster = Monsters[id];
		if (monster.isPlayerMinion() || !Alive(monster) || State.blockedUntil[id] > State.tick)
			continue;
		const int distance = player.position.tile.WalkingDistance(monster.position.tile);
		const bool melee = player._pClass == HeroClass::Warrior;
		if (distance < threatDistance && distance <= (melee ? 2 : 10) && LineClearMissile(player.position.tile, monster.position.tile)) {
			threat = static_cast<int>(id);
			threatDistance = distance;
		}
	}
	if (threat >= 0)
		return threat;
	int best = -1;
	int bestDistance = std::numeric_limits<int>::max();
	for (const MonsterRecord &record : State.monsters) {
		if (record.death != 0 || State.blockedUntil[record.id] > State.tick)
			continue;
		const Monster &monster = Monsters[record.id];
		const int distance = player.position.tile.WalkingDistance(monster.position.tile);
		if (distance < bestDistance) {
			bestDistance = distance;
			best = static_cast<int>(record.id);
		}
	}
	return best;
}

void Act()
{
	Player &player = *MyPlayer;
	uint32_t turns = 0;
	Require(SNetGetTurnsInTransit(&turns), "cannot observe turn queue");
	State.maxTurns = std::max(State.maxTurns, turns);
	if (!player.hasNoLife())
		++State.livingTicks;
	if (player.hasNoLife()) {
		State.wasDead = true;
		return;
	}
	State.wasDead = false;
	if (player._pHitPoints.whole() * 100 < player._pMaxHP.whole() * 45 && player._pmode != PM_DEATH) {
		if (UsePotion(player, false)) {
			++State.healingPotions;
			return;
		}
	}
	const bool caster = player._pClass == HeroClass::Sorcerer;
	if (caster && !player.pManaShield && player._pSplLvl[static_cast<int8_t>(SpellID::ManaShield)] != 0
	    && player._pMana >= GetManaAmount(player, SpellID::ManaShield)) {
		NetSendCmdLocParam3(true, CMD_SPELLXY, player.position.tile, static_cast<uint8_t>(SpellID::ManaShield), static_cast<uint8_t>(SpellType::Spell), 0);
		return;
	}
	if (State.target >= 0 && Alive(Monsters[State.target])) {
		const int32_t hp = Monsters[State.target].hitPoints.raw();
		// Only an attack position without progress counts; walking towards a target is progress.
		const Point targetTile = Monsters[State.target].position.tile;
		const int targetDistance = player.position.tile.WalkingDistance(targetTile);
		const bool attacking = player._pClass == HeroClass::Warrior ? targetDistance <= 1
		                                                            : targetDistance <= 10 && LineClearMovingMissile(player.position.tile, targetTile);
		if (attacking && State.observedTarget == State.target && hp >= State.observedHp)
			++State.targetUnchangedTicks;
		else
			State.targetUnchangedTicks = 0;
		State.observedTarget = State.target;
		State.observedHp = hp;
		if (State.targetUnchangedTicks >= 60) {
			// An attack that makes no progress (for example, missiles blocked by the map) must not pin the bot.
			State.blockedUntil[State.target] = State.tick + 120;
			Point best = player.position.tile;
			int bestDistance = std::numeric_limits<int>::max();
			for (unsigned i = 0; i < 8; ++i) {
				const Point next = player.position.tile + static_cast<Direction>((i + Config.role) % 8);
				if (!InDungeonBounds(next) || !CanStep(player.position.tile, next) || !Walkable(next) || IsTileOccupied(next))
					continue;
				const int distance = next.WalkingDistance(Monsters[State.target].position.tile);
				if (distance < bestDistance) {
					best = next;
					bestDistance = distance;
				}
			}
			if (best != player.position.tile)
				NetSendCmdLoc(MyPlayerId, true, CMD_WALKXY, best);
			State.targetUnchangedTicks = 0;
			return;
		}
	}
	if (caster && player._pMana.whole() * 100 < player._pMaxMana.whole() * 25) {
		if (UsePotion(player, true)) {
			++State.manaPotions;
			return;
		}
	}
	if (player._pmode == PM_STAND && (player.destAction != ACTION_NONE || player.walkpath[0] != WALK_NONE) && player.position.tile == State.previousTile)
		++State.stuckTicks;
	else
		State.stuckTicks = 0;
	State.previousTile = player.position.tile;
	if (State.stuckTicks >= 60) {
		NetSendCmdLoc(MyPlayerId, true, CMD_WALKXY, player.position.tile);
		State.stuckTicks = 0;
		return;
	}
	if (player.walkpath[0] != WALK_NONE && player._pmode == PM_STAND)
		++State.congestedTicks;
	const bool idle = player._pmode == PM_STAND && player.destAction == ACTION_NONE && player.walkpath[0] == WALK_NONE;
	if (!idle) {
		State.idleTicks = 0;
		return;
	}
	if (State.tick < State.nextDecision)
		return;
	State.nextDecision = State.tick + 3;
	const int target = ChooseTarget(player);
	if (target < 0)
		return;
	State.target = target;
	const Monster &monster = Monsters[target];
	const Point from = player.position.tile;
	const Point to = monster.position.tile;
	const int distance = from.WalkingDistance(to);
	const bool clear = LineClearMovingMissile(from, to);
	if (player._pClass == HeroClass::Rogue && clear && distance <= 10) {
		NetSendCmdParam1(true, CMD_RATTACKID, static_cast<uint16_t>(target));
		return;
	}
	if (caster && clear && distance <= 10) {
		const bool preferLightning = (monster.resistance & IMMUNE_FIRE) != 0
		    || ((monster.resistance & RESIST_FIRE) != 0 && (monster.resistance & (RESIST_LIGHTNING | IMMUNE_LIGHTNING)) == 0);
		SpellID spell = player._pSplLvl[static_cast<int8_t>(SpellID::Fireball)] > 0 && distance >= 3 ? SpellID::Fireball : SpellID::Firebolt;
		if (preferLightning && player._pSplLvl[static_cast<int8_t>(SpellID::Lightning)] > 0)
			spell = player._pSplLvl[static_cast<int8_t>(SpellID::ChainLightning)] > 0 && distance >= 3 ? SpellID::ChainLightning : SpellID::Lightning;
		if (player._pMana >= GetManaAmount(player, spell)) {
			NetSendCmdParam4(true, CMD_SPELLID, static_cast<uint16_t>(target), static_cast<int8_t>(spell), static_cast<uint8_t>(SpellType::Spell), 0);
			return;
		}
	}
	if (distance <= 1 && CanStep(from, to)) {
		NetSendCmdParam1(true, CMD_ATTACKID, static_cast<uint16_t>(target));
		return;
	}
	const std::optional<Point> waypoint = Waypoint(from, to);
	if (!waypoint) {
		State.blockedUntil[target] = State.tick + 60;
		return;
	}
	Object *door = FindObjectAtPosition(*waypoint);
	if (door != nullptr && door->isDoor() && door->_oVar4 == DoorClosed) {
		NetSendCmdLoc(MyPlayerId, true, CMD_OPOBJXY, *waypoint);
		return;
	}
	NetSendCmdLoc(MyPlayerId, true, CMD_WALKXY, *waypoint);
}

long ResidentKilobytes()
{
	std::ifstream status("/proc/self/status");
	std::string line;
	while (std::getline(status, line)) {
		if (line.rfind("VmRSS:", 0) == 0)
			return std::strtol(line.c_str() + 6, nullptr, 10);
	}
	return -1;
}

void Report()
{
	std::ostringstream out;
	const Player &player = *MyPlayer;
	std::vector<double> sorted = State.logicMs;
	std::sort(sorted.begin(), sorted.end());
	double mean = 0;
	for (const double value : sorted)
		mean += value;
	if (!sorted.empty())
		mean /= static_cast<double>(sorted.size());
	const double p95 = sorted.empty() ? 0 : sorted[std::min(sorted.size() - 1, sorted.size() * 95 / 100)];
	std::vector<double> intervals = State.tickMs;
	std::sort(intervals.begin(), intervals.end());
	double tickMean = 0;
	for (const double value : intervals)
		tickMean += value;
	if (!intervals.empty())
		tickMean /= static_cast<double>(intervals.size());
	const double tickP95 = intervals.empty() ? 0 : intervals[std::min(intervals.size() - 1, intervals.size() * 95 / 100)];
	const double tickMax = intervals.empty() ? 0 : intervals.back();
	unsigned kills = 0;
	out << "{\"role\":" << Config.role << ",\"players\":" << Config.players << ",\"seed\":" << Config.seed
	    << ",\"difficulty\":" << static_cast<int>(Config.difficulty) << ",\"level\":" << Config.level
	    << ",\"class\":" << static_cast<int>(player._pClass) << ",\"characterLevel\":" << static_cast<int>(player.getCharacterLevel())
	    << ",\"classic\":" << Config.classic << ",\"outcome\":\"" << State.outcome << "\""
	    << ",\"ticks\":" << (State.fightEnd - State.fightStart)
	    << ",\"experience\":" << (player._pExperience - State.startExperience)
	    << ",\"eligibleDeaths\":" << State.eligibleDeaths << ",\"paidDeaths\":" << State.paidDeaths
	    << ",\"ownContributions\":" << State.ownContributions << ",\"blockedLoot\":" << State.blockedLoot
	    << ",\"livingTicks\":" << State.livingTicks << ",\"congestedTicks\":" << State.congestedTicks
	    << ",\"maxTurnsInTransit\":" << State.maxTurns << ",\"hpAtStart\":" << State.hpAtStart
	    << ",\"tickRate\":" << Config.tickRate << ",\"friendlyFire\":" << Config.friendlyFire
	    << ",\"deaths\":" << State.deaths << ",\"healingPotions\":" << State.healingPotions << ",\"manaPotions\":" << State.manaPotions
	    << ",\"lootItems\":" << State.lootItems << ",\"lootGold\":" << State.lootGold << ",\"lootValue\":" << State.lootValue
	    << ",\"logicSamples\":" << sorted.size() << ",\"logicMeanMs\":" << mean << ",\"logicP95Ms\":" << p95
	    << ",\"tickMeanMs\":" << tickMean << ",\"tickP95Ms\":" << tickP95 << ",\"tickMaxMs\":" << tickMax << ",\"rssKb\":" << ResidentKilobytes()
	    << ",\"monsters\":[";
	bool first = true;
	for (const MonsterRecord &record : State.monsters) {
		if (record.death != 0)
			++kills;
		out << (first ? "" : ",") << "[" << record.id << "," << (record.firstDamage == 0 ? -1 : static_cast<int>(record.firstDamage - State.fightStart))
		    << "," << (record.death == 0 ? -1 : static_cast<int>(record.death - State.fightStart)) << "," << record.unique << "]";
		first = false;
	}
	Require(State.outcome != "cleared" || kills == State.monsters.size(), "encounter was reported complete with living targets");
	if (State.target >= 0) {
		const Monster &last = Monsters[State.target];
		out << "],\"lastTarget\":[" << State.target << "," << last.hitPoints.whole() << "," << static_cast<int>(last.type().type) << "," << player.position.tile.WalkingDistance(last.position.tile) << "," << static_cast<int>(last.mode) << "," << LineClearMovingMissile(player.position.tile, last.position.tile);
	}
	if (State.outcome == "timeout" && State.target >= 0) {
		// Local map around the unfinished target: # solid, D closed door, d open door, M monster, P player, . walkable.
		const Point centre = Monsters[State.target].position.tile;
		out << "],\"targetMap\":[";
		for (int y = centre.y - 6; y <= centre.y + 6; ++y) {
			out << (y == centre.y - 6 ? "" : ",") << '"';
			for (int x = centre.x - 6; x <= centre.x + 6; ++x) {
				const Point p { x, y };
				char c = '?';
				if (InDungeonBounds(p)) {
					const Object *object = FindObjectAtPosition(p);
					if (p == centre)
						c = 'T';
					else if (dPlayer[x][y] != 0)
						c = 'P';
					else if (dMonster[x][y] != 0)
						c = 'M';
					else if (object != nullptr && object->isDoor())
						c = object->_oVar4 == DoorClosed ? 'D' : 'd';
					else
						c = IsTileWalkable(p, true) ? '.' : '#';
				}
				out << c;
			}
			out << '"';
		}
	}
	out << "],\"kills\":" << kills << ",\"scaleAtEnd\":" << (IsPartyBalanceEnabled() ? static_cast<unsigned>(GetPartyLevelState(currlevel).participants) : 4)
	    << ",\"activeAtEnd\":" << static_cast<unsigned>(gbActivePlayers) << ",\"rewards\":[";
	first = true;
	for (const auto &entry : State.rewards) {
		out << (first ? "" : ",") << '[' << entry[0] << ',' << entry[1] << ',' << entry[2] << ',' << entry[3] << ']';
		first = false;
	}
	out << "],\"drops\":[";
	first = true;
	for (const auto &entry : State.drops) {
		out << (first ? "" : ",") << '[' << entry[0] << ',' << entry[1] << ',' << entry[2] << ',' << entry[3] << ',' << entry[4] << ']';
		first = false;
	}
	out << "]}\n";
	Mark("result-" + std::to_string(Config.role) + ".json", out.str());
}

bool AllGuestsClosed()
{
	for (unsigned role = 1; role < Config.players; ++role) {
		if (!Exists("closed-" + std::to_string(role)))
			return false;
	}
	return true;
}

bool AllReported()
{
	for (unsigned role = 0; role < Config.players; ++role) {
		if (!Exists("result-" + std::to_string(role) + ".json"))
			return false;
	}
	return true;
}

int64_t EpochSeconds()
{
	return std::chrono::duration_cast<std::chrono::seconds>(std::chrono::system_clock::now().time_since_epoch()).count();
}

/** All processes, including rejoining ones, share the first peer's start time. */
bool SoakContinues()
{
	return Config.soakSeconds != 0 && EpochSeconds() - State.soakEpoch < static_cast<int64_t>(Config.soakSeconds);
}

/** The party's current cycle and level, published by the first peer for rejoining or lagging bots. */
std::optional<std::pair<unsigned, int>> PartyPosition()
{
	std::ifstream in(Config.directory / "party");
	unsigned cycle;
	int level;
	if (in >> cycle >> level)
		return std::make_pair(cycle, level);
	return std::nullopt;
}

/** Endurance sample: resources and timing since the previous sample, one JSON line per call. */
void WriteSample(const char *reason)
{
	const auto window = [](const std::vector<double> &values, size_t from) {
		std::vector<double> part(values.begin() + static_cast<std::ptrdiff_t>(std::min(from, values.size())), values.end());
		std::sort(part.begin(), part.end());
		double mean = 0;
		for (const double value : part)
			mean += value;
		if (!part.empty())
			mean /= static_cast<double>(part.size());
		const double p95 = part.empty() ? 0 : part[std::min(part.size() - 1, part.size() * 95 / 100)];
		return std::array<double, 3> { mean, p95, part.empty() ? 0 : part.back() };
	};
	const auto logic = window(State.logicMs, State.sampledLogic);
	const auto ticks = window(State.tickMs, State.sampledTicks);
	// Endurance runs keep only the window since the previous sample; the series lives on disk.
	State.logicMs.clear();
	State.tickMs.clear();
	State.sampledLogic = 0;
	State.sampledTicks = 0;
	rusage usage {};
	getrusage(RUSAGE_SELF, &usage);
	const double cpuSeconds = static_cast<double>(usage.ru_utime.tv_sec + usage.ru_stime.tv_sec) + static_cast<double>(usage.ru_utime.tv_usec + usage.ru_stime.tv_usec) / 1e6;
	uint32_t turns = 0;
	SNetGetTurnsInTransit(&turns);
	std::ofstream out(Config.directory / ("samples-" + std::to_string(Config.role) + ".jsonl"), std::ios::app);
	out << "{\"wallSeconds\":" << std::chrono::duration<double>(std::chrono::steady_clock::now() - State.soakStart).count() << ",\"reason\":\"" << reason << "\""
	    << ",\"tick\":" << State.tick << ",\"cycle\":" << State.cycle << ",\"level\":" << static_cast<int>(currlevel) << ",\"phase\":" << static_cast<int>(State.phase)
	    << ",\"rssKb\":" << ResidentKilobytes() << ",\"cpuSeconds\":" << cpuSeconds
	    << ",\"logicMeanMs\":" << logic[0] << ",\"logicP95Ms\":" << logic[1] << ",\"tickMeanMs\":" << ticks[0] << ",\"tickP95Ms\":" << ticks[1] << ",\"tickMaxMs\":" << ticks[2]
	    << ",\"turnsInTransit\":" << turns << ",\"items\":" << static_cast<unsigned>(ActiveItemCount) << ",\"monsters\":" << ActiveMonsterCount << ",\"lights\":" << ActiveLightCount
	    << ",\"unresolvedTransfers\":" << HasUnresolvedItemTransfers() << ",\"active\":" << static_cast<unsigned>(gbActivePlayers) << ",\"deaths\":" << State.deaths
	    << ",\"incarnation\":" << Config.incarnation << ",\"portalFallbacks\":" << State.portalFallbacks << ",\"strayLevels\":" << State.strayLevels
	    << ",\"kills\":" << std::count_if(State.monsters.begin(), State.monsters.end(), [](const MonsterRecord &m) { return m.death != 0; }) << "}\n";
}

/**
 * Next endurance level: down by stairs from the configured level through five more, then back to
 * town through each bot's own town portal, and down again.
 */
int SoakLevel(unsigned cycle)
{
	const unsigned down = static_cast<unsigned>(std::min(Config.level + 5, 16) - Config.level + 1);
	const unsigned step = cycle % (down + 1);
	return step < down ? Config.level + static_cast<int>(step) : 0;
}

void GoToLevel(int level, int previous)
{
	Player &player = *MyPlayer;
	State.level = level;
	if (level == 0 && currlevel == 0 && player.isOnActiveLevel() && !player._pLvlChanging) {
		State.phase = Phase::Waiting; // already in town
	} else if (player.hasNoLife()) {
		// A dead bot returns to town the way a player would, then walks back down to the party.
		RestartTownLvl(player);
		State.rejoinFromTown = level != 0;
		State.phase = Phase::Descending;
	} else if (level == 0 && previous != 0) {
		State.portalCast = false;
		State.warpSent = false;
		State.portalTicks = 0;
		State.phase = Phase::Portal;
	} else {
		StartNewLvl(player, level > previous ? WM_DIABNEXTLVL : WM_DIABPREVLVL, level);
		State.phase = Phase::Descending;
	}
}

void AdvanceSoak()
{
	++State.cycle;
	State.monsters.clear();
	State.target = -1;
	State.settleTicks = 0;
	State.stuckTicks = 0;
	State.townTicks = 0;
	State.blockedUntil.fill(0);
	State.outcome = "timeout";
	const int level = SoakLevel(State.cycle);
	if (Config.role == 0)
		Mark("party", std::to_string(State.cycle) + " " + std::to_string(level));
	GoToLevel(level, State.level);
}

/** A bot that rejoined, or fell behind, moves straight to where the first peer is. */
bool CatchUpWithParty()
{
	if (Config.soakSeconds == 0 || Config.role == 0)
		return false;
	const auto party = PartyPosition();
	if (!party || party->first <= State.cycle)
		return false;
	State.cycle = party->first;
	State.monsters.clear();
	State.target = -1;
	State.settleTicks = 0;
	State.townTicks = 0;
	State.blockedUntil.fill(0);
	if (party->second == State.level && currlevel == State.level) {
		State.phase = Phase::Waiting;
		return true;
	}
	GoToLevel(party->second, currlevel);
	return true;
}

/** Endurance mode: the designated slot leaves in town every leaveEverySeconds; a new process takes it. */
bool LeaveIfDue()
{
	// One bot away at a time: the previous one must have rejoined first.
	if (Config.leaveEverySeconds == 0 || !SoakContinues() || gbActivePlayers != Config.players)
		return false;
	const int64_t due = (EpochSeconds() - State.soakEpoch) / Config.leaveEverySeconds;
	for (int64_t k = 1; k <= due; ++k) {
		const std::string marker = "left-" + std::to_string(k);
		if (Exists(marker))
			continue;
		// The runner marks each departure's replacement once it is back in the game.
		if (k > 1 && !Exists("rejoined-" + std::to_string(k - 1)))
			return false;
		const unsigned slot = Config.players - 1 - static_cast<unsigned>((k - 1) % std::min(3U, Config.players - 1));
		if (slot != Config.role)
			return false;
		Mark(marker, std::to_string(Config.role));
		State.left = true;
		State.outcome = "left";
		State.phase = Phase::Done;
		return true;
	}
	return false;
}

void Tick(bool before)
{
	if (!before) {
		if (State.phase == Phase::Fighting || Config.soakSeconds != 0)
			State.logicMs.push_back(std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - State.logicStart).count());
		return;
	}
	const auto now = std::chrono::steady_clock::now();
	if ((State.phase == Phase::Fighting && State.tick > State.fightStart) || (Config.soakSeconds != 0 && State.tick > 0))
		State.tickMs.push_back(std::chrono::duration<double, std::milli>(now - State.lastTick).count());
	State.lastTick = now;
	++State.tick;
	Player &player = *MyPlayer;
	if (Config.soakSeconds != 0 && State.phase != Phase::Gathering && State.phase != Phase::Reporting && State.phase != Phase::Done && !SoakContinues()) {
		// Time is up wherever the bot is (town, stairs or a fight): every peer reports and closes.
		State.outcome = "soak-complete";
		State.fightEnd = State.tick;
		State.phase = Phase::Reporting;
	}
	switch (State.phase) {
	case Phase::Gathering:
		if (gbActivePlayers == Config.players && std::all_of(Players.begin(), Players.begin() + Config.players, [](const Player &p) { return p.plractive; })) {
			if (Config.soakSeconds != 0) {
				if (Config.role == 0 && Config.incarnation == 0)
					Mark("soak-epoch", std::to_string(EpochSeconds()));
				std::ifstream epoch(Config.directory / "soak-epoch");
				if (!(epoch >> State.soakEpoch))
					State.soakEpoch = EpochSeconds();
			}
			State.soakStart = std::chrono::steady_clock::now();
			if (Config.incarnation != 0) {
				// A rejoining bot starts in town and goes to wherever the party is now.
				const auto party = PartyPosition();
				Require(party.has_value(), "a rejoining bot needs the party position");
				State.cycle = party->first;
				State.level = 0;
				if (party->second == 0)
					State.phase = Phase::Waiting;
				else
					GoToLevel(party->second, 0);
				break;
			}
			StartNewLvl(player, WM_DIABNEXTLVL, Config.level);
			State.level = Config.level;
			State.phase = Phase::Descending;
		}
		break;
	case Phase::Descending:
		if (State.rejoinFromTown && player.isOnActiveLevel() && currlevel == 0 && !player._pLvlChanging) {
			State.rejoinFromTown = false;
			StartNewLvl(player, WM_DIABNEXTLVL, State.level);
		}
		if (player.isOnActiveLevel() && currlevel == State.level && !player._pLvlChanging)
			State.phase = Phase::Waiting;
		break;
	case Phase::Waiting:
		if (CatchUpWithParty())
			break;
		if (State.level == 0) {
			if (LeaveIfDue())
				break;
			// The first peer decides when the party leaves town, so no bot waits for one that already left.
			if (PartyOnLevel())
				++State.townTicks;
			const std::string leave = "town-" + std::to_string(State.cycle);
			if (Config.role == 0 && State.townTicks >= 200)
				Mark(leave);
			if (Exists(leave))
				AdvanceSoak();
			break;
		}
		// The first peer writes the targets once the whole party is here; the others follow the file,
		// because that peer may already have moved on from a level without living targets.
		if (Config.role == 0 ? PartyOnLevel() : Exists(TargetsFile())) {
			BeginFight();
			if (State.monsters.empty()) {
				WriteSample("no-targets");
				AdvanceSoak();
			}
		}
		break;
	case Phase::Fighting:
		if (Config.soakSeconds != 0 && !player._pLvlChanging && player.isOnActiveLevel() && currlevel != State.level) {
			// Stopping on a stair tile changes level natively; count it and take the stairs back.
			++State.strayLevels;
			StartNewLvl(player, currlevel < State.level ? WM_DIABNEXTLVL : WM_DIABPREVLVL, State.level);
			State.phase = Phase::Descending;
			break;
		}
		Require(currlevel == State.level && player.isOnActiveLevel(), "bot left the encounter level unexpectedly");
		if (Config.leaveAfterKills != 0 && !State.left && Config.role >= Config.players - Config.leavers
		    && std::count_if(State.monsters.begin(), State.monsters.end(), [](const MonsterRecord &m) { return m.death != 0; }) >= Config.leaveAfterKills) {
			State.left = true;
			State.outcome = "left";
			State.fightEnd = State.tick;
			State.phase = Phase::Reporting;
			break;
		}
		Observe();
		if (State.phase == Phase::Fighting)
			Act();
		break;
	case Phase::Reporting:
		// Let native death/reward transactions and earlier commands settle before reading XP/loot.
		if (++State.settleTicks < 128 || HasUnresolvedItemTransfers())
			break;
		if (SoakContinues()) {
			WriteSample("cycle");
			AdvanceSoak();
			break;
		}
		Report();
		State.phase = Phase::Done;
		break;
	case Phase::Portal: {
		// Back to town through the bot's own town portal, as a player would.
		++State.portalTicks;
		if (currlevel == 0 && !player._pLvlChanging) {
			State.phase = Phase::Descending; // already in town; town portals cannot be cast here
			break;
		}
		const Portal &portal = Portals[MyPlayerId];
		const bool opened = portal.open && portal.level == currlevel;
		// Cast when standing; try a second scroll if the first portal has not opened.
		if (player._pmode == PM_STAND && !opened && (!State.portalCast || State.portalTicks == 200)) {
			NetSendCmdLocParam3(true, CMD_SPELLXY, player.position.tile, static_cast<uint16_t>(SpellID::TownPortal), static_cast<uint16_t>(SpellType::Scroll), 0);
			State.portalCast = true;
			break;
		}
		if (!State.warpSent && State.portalTicks > 20 && opened && player._pmode == PM_STAND) {
			NetSendCmdParam1(true, CMD_WARP, static_cast<uint16_t>(MyPlayerId));
			State.warpSent = true;
		}
		if (State.warpSent && player._pLvlChanging) {
			State.phase = Phase::Descending;
		} else if (State.portalTicks > 600) {
			// No scroll or no portal: return the way a dead bot does, and count it.
			++State.portalFallbacks;
			WriteSample(opened ? "portal-fallback-open" : State.portalCast ? "portal-fallback-cast"
			                                                               : "portal-fallback-never-standing");
			RestartTownLvl(player);
			State.phase = Phase::Descending;
		}
		break;
	}
	case Phase::Done:
		if (State.left || (AllReported() && (Config.role != 0 || AllGuestsClosed()))) {
			gbRunGameResult = false;
			gbRunGame = false;
		}
		break;
	}
	if (State.phase == Phase::Fighting && State.tick % 128 == 0) {
		std::ofstream trace(Config.directory / ("trace-" + std::to_string(Config.role) + ".jsonl"), std::ios::app);
		trace << "{\"tick\":" << State.tick - State.fightStart << ",\"x\":" << static_cast<int>(player.position.tile.x) << ",\"y\":" << static_cast<int>(player.position.tile.y)
		      << ",\"level\":" << static_cast<int>(currlevel) << ",\"mode\":" << static_cast<int>(player._pmode) << ",\"action\":" << static_cast<int>(player.destAction) << ",\"path\":" << static_cast<int>(player.walkpath[0]) << ",\"target\":" << State.target
		      << ",\"targetX\":" << (State.target < 0 ? -1 : static_cast<int>(Monsters[State.target].position.tile.x))
		      << ",\"targetY\":" << (State.target < 0 ? -1 : static_cast<int>(Monsters[State.target].position.tile.y)) << "}\n";
	}
	if ((State.phase == Phase::Reporting || State.phase == Phase::Done) && State.tick % 512 == 0) {
		// Shows what a peer that never finishes is still waiting for.
		std::ofstream trace(Config.directory / ("closing-" + std::to_string(Config.role) + ".jsonl"), std::ios::app);
		trace << "{\"tick\":" << State.tick - State.fightStart << ",\"phase\":" << static_cast<int>(State.phase) << ",\"settleTicks\":" << State.settleTicks
		      << ",\"unresolvedTransfers\":" << HasUnresolvedItemTransfers() << ",\"pendingRewards\":" << HasPendingPartyRewards()
		      << ",\"localReward\":" << HasLocalPartyRewardTransfer() << ",\"active\":" << static_cast<unsigned>(gbActivePlayers) << ",\"reported\":" << AllReported() << "}\n";
	}
	if (Config.soakSeconds != 0 && State.tick % 1200 == 0 && State.phase != Phase::Gathering)
		WriteSample("minute");
	State.logicStart = std::chrono::steady_clock::now();
}

void Run()
{
#ifdef USE_SDL3
	Require(SDL_Init(SDL_INIT_EVENTS), "SDL_Init failed");
#else
	Require(SDL_Init(SDL_INIT_EVENTS) >= 0, "SDL_Init failed");
#endif
	HeadlessMode = true;
	if (std::getenv("DEVILUTIONX_BALANCE_VERBOSE") != nullptr)
		SDL_SetLogPriorities(SDL_LOG_PRIORITY_VERBOSE);
	LoadCoreArchives();
	LoadGameArchives();
	Require(HaveMainData() && !gbIsSpawn, "Diablo assets are required");
	const auto profile = Config.directory / ("profile-" + std::to_string(Config.role));
	std::filesystem::create_directories(profile);
	paths::SetPrefPath(profile.string() + "/");
	paths::SetConfigPath(profile.string() + "/");
	InitKeymapActions();
	LoadOptions();
	GetOptions().Graphics.hardwareCursor.SetValue(false);
	LuaInitialize();
	gbIsSpawn = false;
	gbIsHellfire = false;
	gbVanilla = false;
	gbMusicOn = false;
	gbSoundOn = false;
	LoadSpellData();
	LoadPlayerDataFiles();
	LoadMissileData();
	LoadMonsterData();
	LoadItemData();
	LoadObjectData();
	LoadQuestData();
	LoadTextData();

	gbIsMultiplayer = false;
	Players.resize(2);
	MyPlayerId = 0;
	MyPlayer = &Players[0];
	Require(SNetInitializeProvider(SELCONN_LOOPBACK, nullptr), "cannot initialize hero preparation network");
	CreateHero(*MyPlayer, ClassForSlot(Config.role));

	gbIsMultiplayer = true; // The hero is written as a multiplayer save.
	gSaveNumber = 0;
	pfile_write_hero(false);
	pfile_read_player_from_save(gSaveNumber, *MyPlayer);
	PlayerNetPack packed;
	PackNetPlayer(packed, *MyPlayer);
	const auto previousLevel = currlevel;
	currlevel = 1; // Validate the prepared hero off-level, without loading animation/vision state.
	Require(UnPackNetPlayer(packed, Players[1]), "prepared hero does not pass native network validation");
	currlevel = previousLevel;
	WriteHeroManifest(*MyPlayer);
	Players.resize(1);
	FreeCursor();
	SNetDestroy();

	TestSelectMultiplayerGame = SelectGame;
	TestGameLogicHook = Tick;
	TestMonsterLootObserver = ObserveLoot;
	TestMonsterRewardObserver = ObserveReward;
	StartGame(true, false);
	Require(State.phase == Phase::Done, "game ended before the measurement finished");
	Mark("closed-" + std::to_string(Config.role));
}

unsigned Argument(char **argv, int index)
{
	return static_cast<unsigned>(std::strtoul(argv[index], nullptr, 10));
}

} // namespace
} // namespace devilution

int main(int argc, char **argv)
{
	using namespace devilution;
	if (argc < 12) {
		std::fprintf(stderr, "usage: %s directory role port players seed difficulty level composition characterLevel maxTicks tickRate [classic] [leaveAfterKills leavers targets friendlyFire [soakSeconds [leaveEverySeconds [incarnation]]]]\n", argv[0]);
		return 2;
	}
	Config.directory = argv[1];
	Config.role = Argument(argv, 2);
	Config.port = Argument(argv, 3);
	Config.players = Argument(argv, 4);
	Config.seed = Argument(argv, 5);
	Config.difficulty = static_cast<_difficulty>(Argument(argv, 6));
	Config.level = static_cast<int>(Argument(argv, 7));
	Config.composition = argv[8];
	Config.characterLevel = static_cast<int>(Argument(argv, 9));
	Config.maxTicks = Argument(argv, 10);
	Config.tickRate = Argument(argv, 11);
	if (argc > 12)
		Config.classic = std::string_view(argv[12]) == "classic";
	if (argc > 14) {
		Config.leaveAfterKills = Argument(argv, 13);
		Config.leavers = Argument(argv, 14);
	}
	if (argc > 15)
		Config.targets = Argument(argv, 15);
	if (argc > 16)
		Config.friendlyFire = Argument(argv, 16) != 0;
	if (argc > 17)
		Config.soakSeconds = Argument(argv, 17);
	if (argc > 18)
		Config.leaveEverySeconds = Argument(argv, 18);
	if (argc > 19)
		Config.incarnation = Argument(argv, 19);
	Require(Config.players >= 1 && Config.players <= (Config.classic ? 4U : 8U), "invalid player count");
	Require(Config.role < Config.players && Config.port > 0 && Config.port <= 65535, "invalid role or port");
	Require(Config.level >= 1 && Config.level <= 16 && static_cast<unsigned>(Config.difficulty) <= DIFF_HELL, "invalid level or difficulty");
	Require(Config.tickRate >= 1 && Config.tickRate <= 255 && Config.characterLevel >= 1 && Config.characterLevel <= 50, "invalid tick rate or character level");
	Require(!Config.composition.empty() && Config.composition.find_first_not_of("WRS") == std::string::npos, "invalid party composition");
	Require(Config.leavers < Config.players && Config.maxTicks > 0, "invalid departure count or duration");
	Require(Config.leaveEverySeconds == 0 || (Config.soakSeconds != 0 && Config.players >= 2), "periodic departures need endurance mode and a party");
	Run();
	std::fflush(stdout);
	std::_Exit(0);
}
