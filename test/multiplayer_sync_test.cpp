#include <algorithm>
#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <iterator>
#include <map>
#include <memory>
#include <span>
#include <thread>
#include <vector>

#ifdef USE_SDL3
#include <SDL3/SDL_timer.h>
#else
#include <SDL_timer.h>
#endif

#include <gtest/gtest.h>

#include "controls/plrctrls.h"
#include "cursor.h"
#include "diablo.h"
#if !defined(NONET) && !defined(DISABLE_TCP)
#include "dvlnet/tcp_client.h"
#include "dvlnet/tcp_server.h"
#endif
#include "encrypt.h"
#include "engine/assets.hpp"
#include "game_mode.hpp"
#include "inv.h"
#include "items/item_transfer.hpp"
#include "levels/dun_tile_data.hpp"
#include "lighting.h"
#include "loadsave.h"
#include "missiles.h"
#include "monsters/party_balance.hpp"
#include "msg.h"
#include "multi.h"
#include "nthread.h"
#include "pack.h"
#include "player.h"
#include "players/validation.hpp"
#include "qol/stash.h"
#include "storm/storm_net.hpp"
#include "sync.h"
#include "tables/playerdat.hpp"
#include "tmsg.h"
#include "utils/endian_swap.hpp"

namespace devilution {
bool TestDeadItem(Player &player, Item &item, Displacement direction);
void TestDropHalfPlayersGold(Player &player);
namespace {

using Message = std::vector<std::byte>;
using Snapshot = std::vector<Message>;

class MultiplayerSyncTest : public ::testing::Test {
protected:
	static void SetUpTestSuite()
	{
		LoadCoreArchives();
		LoadPlayerDataFiles();
		LoadItemData();
		LoadSpellData();
		LoadQuestData();
		LoadTextData();
	}

	void SetUp() override
	{
		tmsg_cleanup();
		gbIsMultiplayer = true;
		gbIsHellfire = false;
		gbIsSpawn = false;
		gbRunGame = false;
		gbBufferMsgs = 0;
		gbGameDestroyed = false;
		currlevel = 1;
		setlevel = false;
		leveltype = DTYPE_CATHEDRAL;
		Players.clear();
		ResizePlayersForSession(PlayerStorageCapacity);
		MyPlayerId = 0;
		MyPlayer = &Players[0];
		InspectPlayer = MyPlayer;
		gbActivePlayers = 1;
		InitPortals();
		for (int i = 0; i < MAXQUESTS; ++i) {
			Quests[i] = {};
			Quests[i]._qidx = static_cast<quest_id>(i);
		}
		InitGameInfo();
		sgGameInitInfo.rules = ExtendedSessionRules;
		ASSERT_TRUE(SNetInitializeProvider(SELCONN_LOOPBACK, &sgGameInitInfo));
		GameData wire = sgGameInitInfo;
		SwapGameDataLE(wire);
		int playerId;
		ASSERT_TRUE(SNetCreateGame("sync-test", nullptr, reinterpret_cast<char *>(&wire), sizeof(wire), &playerId));
		gdwLargestMsgSize = 512;
		gdwNormalMsgSize = 512;
		std::fill_n(player_state, PlayerStorageCapacity, 0);
		std::fill_n(sgwPackPlrOffsetTbl, PlayerStorageCapacity, 0);
		delta_init();
		delta_reset_transfer();
		ActiveMonsterCount = 0;
		ActiveItemCount = 0;
	}

	void TearDown() override
	{
		run_delta_info();
		tmsg_cleanup();
		delta_init();
		SNetDestroy();
		Players.clear();
		MyPlayer = nullptr;
		InspectPlayer = nullptr;
		MyPlayerId = 0;
		gbIsMultiplayer = false;
		gbRunGame = false;
		leveltype = DTYPE_TOWN;
		InitPortals();
	}

	static Message Fragment(_cmd_id command, uint16_t offset, std::span<const std::byte> data)
	{
		Message result(sizeof(TCmdPlrInfoHdr) + data.size());
		const TCmdPlrInfoHdr header { command, Swap16LE(offset), Swap16LE(static_cast<uint16_t>(data.size())) };
		std::memcpy(result.data(), &header, sizeof(header));
		std::copy(data.begin(), data.end(), result.begin() + sizeof(header));
		return result;
	}

	static void Receive(uint8_t sender, const Message &message)
	{
		EXPECT_EQ(ParseCmd(sender, reinterpret_cast<const TCmd *>(message.data()), message.size()), message.size());
	}

	static Snapshot Drain()
	{
		Snapshot result;
		uint8_t sender;
		void *data;
		size_t size;
		while (SNetReceiveMessage(&sender, &data, &size)) {
			EXPECT_LE(size, gdwLargestMsgSize);
			EXPECT_GT(size, sizeof(TPktHdr));
			const auto *bytes = static_cast<const std::byte *>(data);
			result.emplace_back(bytes + sizeof(TPktHdr), bytes + size);
		}
		return result;
	}

	static Snapshot Export()
	{
		// Loopback delivers destination 0. The production fragmenter therefore runs
		// as slot 7 while the captured bytes are later received by another slot.
		const uint8_t savedId = MyPlayerId;
		MyPlayerId = 7;
		DeltaExportData(0);
		MyPlayerId = savedId;
		return Drain();
	}

	static std::map<unsigned, Message> Canonical(const Snapshot &snapshot)
	{
		std::map<unsigned, Message> result;
		Message chunk;
		_cmd_id command = CMD_DLEVEL_END;
		const auto flush = [&]() {
			if (chunk.empty())
				return;
			Message unpacked(65535);
			std::copy(chunk.begin() + 1, chunk.end(), unpacked.begin());
			size_t size = chunk.size() - 1;
			if (chunk[0] != std::byte { 0 })
				size = PkwareDecompress(unpacked.data(), static_cast<uint32_t>(size), unpacked.size());
			EXPECT_GT(size, 0U);
			unpacked.resize(size);
			const unsigned key = command == CMD_DLEVEL ? static_cast<unsigned>(unpacked[0]) : 256;
			result[key] = std::move(unpacked);
			chunk.clear();
		};
		for (const auto &message : snapshot) {
			const auto &header = *reinterpret_cast<const TCmdPlrInfoHdr *>(message.data());
			if (header.wOffset == 0) {
				flush();
				command = header.bCmd;
			}
			if (command != CMD_DLEVEL_END)
				chunk.insert(chunk.end(), message.begin() + sizeof(header), message.end());
		}
		flush();
		return result;
	}

	static void SeedLevel(uint8_t level, unsigned count = 1)
	{
		for (unsigned i = 0; i < count; ++i) {
			TSyncMonster monster {};
			monster._mndx = static_cast<uint16_t>(i);
			monster._mx = static_cast<uint8_t>(20 + i % 70);
			monster._my = static_cast<uint8_t>(20 + (i * 7) % 70);
			monster._mhitpoints = Swap32LE(64 * (i + 1));
			monster.mWhoHit = PlayerExperienceMaskFromByte(static_cast<uint8_t>(i + level));
			delta_sync_monster(monster, level);
		}
	}

	static PlayerNetPack MakePlayer(uint8_t slot, const char *name = "New arrival")
	{
		Player &player = Players[slot];
		FreeCursor();
		CreatePlayer(player, HeroClass::Warrior);
		InitCursor();
		std::strcpy(player._pName, name);
		player.plrlevel = static_cast<uint8_t>(2 + slot);
		player.position.tile = { static_cast<WorldTileCoord>(40 + slot), 50 };
		player.position.future = player.position.tile;
		player._pdir = Direction::South;
		PlayerNetPack pack {};
		PackNetPlayer(pack, player);
		player = {};
		return pack;
	}

	static void Join(uint8_t slot, const PlayerNetPack &pack, size_t fragmentSize = 193)
	{
		const auto bytes = std::as_bytes(std::span(&pack, 1));
		for (size_t offset = 0; offset < bytes.size(); offset += fragmentSize) {
			Receive(slot, Fragment(CMD_ACK_PLRINFO, static_cast<uint16_t>(offset), bytes.subspan(offset, std::min(fragmentSize, bytes.size() - offset))));
		}
	}
};

class PlayerSynchronizationTest : public MultiplayerSyncTest {
protected:
	static void SetUpTestSuite()
	{
		MultiplayerSyncTest::SetUpTestSuite();
		LoadGameArchives();
	}

	void SetUp() override
	{
		MultiplayerSyncTest::SetUp();
		if (!HaveMainData())
			GTEST_SKIP() << "MPQ assets (spawn.mpq or DIABDAT.MPQ) not found - skipping player inventory tests";
	}
	void TearDown() override
	{
		FreeCursor();
		MultiplayerSyncTest::TearDown();
	}
};

void PrepareOpenItemDropArea()
{
	for (int x = 0; x < MAXDUNX; ++x) {
		for (int y = 0; y < MAXDUNY; ++y) {
			dItem[x][y] = 0;
			dPlayer[x][y] = 0;
			dMonster[x][y] = 0;
			dObject[x][y] = 0;
			dFlags[x][y] = DungeonFlag::None;
			dPiece[x][y] = 0;
		}
	}
	SOLData[0] = TileProperties::Solid;
	SOLData[1] = TileProperties::None;
	for (int x = 30; x < 60; ++x) {
		for (int y = 30; y < 60; ++y)
			dPiece[x][y] = 1;
	}
}

void PrepareGoldPickupArea(uint8_t localPlayerId)
{
	PrepareOpenItemDropArea();
	MyPlayerId = localPlayerId;
	MyPlayer = &Players[localPlayerId];
	InspectPlayer = MyPlayer;
	FreeCursor();
	CreatePlayer(*MyPlayer, HeroClass::Warrior);
	InitCursor();
	MyPlayer->setLevel(currlevel);
	MyPlayer->plractive = true;
	MyPlayer->position.tile = { 40, 49 };
	MyPlayer->position.future = MyPlayer->position.tile;
	for (uint8_t i = 0; i < MAXITEMS; ++i)
		ActiveItems[i] = i;
	std::swap(ActiveItems[0], ActiveItems[9]);
	std::swap(ActiveItems[1], ActiveItems[5]);
	ActiveItemCount = 2;
	MakeGoldStack(Items[5], 75);
	Items[5]._iSeed = 31001;
	Items[5]._iCreateInfo = CF_PREGEN;
	Items[5].position = { 40, 50 };
	dItem[40][50] = 6;
	MakeGoldStack(Items[9], 700);
	Items[9]._iSeed = 31002;
	Items[9]._iCreateInfo = CF_PREGEN;
	Items[9].position = { 43, 50 };
	dItem[43][50] = 10;
}

TCmdGItem MakePickupCommand(_cmd_id command, const Item &item, uint8_t master, uint8_t cursorItem)
{
	TCmdGItem message {};
	message.bCmd = command;
	message.x = item.position.x;
	message.y = item.position.y;
	message.def.wIndx = static_cast<_item_indexes>(Swap16LE(item.IDidx));
	message.def.wCI = Swap16LE(item._iCreateInfo);
	message.def.dwSeed = Swap32LE(item._iSeed);
	PrepareItemForNetwork(item, message.item);
	message.bMaster = master;
	message.bPnum = MyPlayerId;
	message.bCursitem = cursorItem;
	message.bLevel = currlevel;
	return message;
}

void PrepareFullPickupStorage(uint8_t localPlayerId, bool heldGold, bool fullDelta = true)
{
	delta_init();
	for (Player &player : Players)
		player.plractive = false;
	PrepareGoldPickupArea(localPlayerId);
	InitializeItem(Items[5], IDI_HEAL);
	Items[5]._iSeed = 63001;
	Items[5]._iCreateInfo = CF_PREGEN;
	Items[5].position = { 40, 50 };
	if (heldGold)
		MakeGoldStack(MyPlayer->HoldItem, 321);
	else
		InitializeItem(MyPlayer->HoldItem, IDI_HEAL);
	MyPlayer->HoldItem._iSeed = 63002;
	NewCursor(MyPlayer->HoldItem);
	for (int i = 0; i < InventoryGridCells; ++i) {
		InitializeItem(MyPlayer->InvList[i], IDI_HEAL);
		MyPlayer->InvGrid[i] = static_cast<int8_t>(i + 1);
	}
	MyPlayer->_pNumInv = InventoryGridCells;
	for (Item &item : MyPlayer->SpdList)
		InitializeItem(item, IDI_HEAL);
	MyPlayer->_pGold = CalculateGold(*MyPlayer);
	if (fullDelta) {
		for (uint32_t i = 0; i < ItemDeltaCapacity() - 1; ++i) {
			TCmdGItem entry = MakePickupCommand(CMD_GETITEM, Items[5], MyPlayerId, 5);
			entry.def.dwSeed = Swap32LE(65000 + i);
			EXPECT_EQ(ParseCmd(MyPlayerId, reinterpret_cast<const TCmd *>(&entry), sizeof(entry)), sizeof(entry));
		}
	}
}

TEST_F(PlayerSynchronizationTest, FullStorageOwnerRejectsPickupUntilHeldItemCanBeStored)
{
	for (const uint8_t owner : { 0, 7 }) {
		for (const bool heldGold : { false, true }) {
			SCOPED_TRACE(owner);
			SCOPED_TRACE(heldGold);
			PrepareFullPickupStorage(owner, heldGold);
			DeltaAddItem(5);
			PutItemRecord(Items[5]._iSeed, Items[5]._iCreateInfo, Items[5].IDidx);
			const Item heldItem = MyPlayer->HoldItem;
			ASSERT_FALSE(CanDeltaPutItem(heldItem, *MyPlayer));
			const auto before = Canonical(Export());
			const TCmdGItem pickup = MakePickupCommand(CMD_REQUESTGITEM, Items[5], owner, 5);
			Items[5]._iRequest = true;

			ASSERT_EQ(ParseCmd(owner, reinterpret_cast<const TCmd *>(&pickup), sizeof(pickup)), sizeof(pickup));

			EXPECT_EQ(ActiveItemCount, 2);
			EXPECT_EQ(dItem[40][50], 6);
			EXPECT_TRUE(Items[5].keyAttributesMatch(63001, IDI_HEAL, CF_PREGEN));
			EXPECT_TRUE(MyPlayer->HoldItem.keyAttributesMatch(heldItem._iSeed, heldItem.IDidx, heldItem._iCreateInfo));
			EXPECT_FALSE(Items[5]._iRequest);
			EXPECT_TRUE(Drain().empty());
			EXPECT_EQ(Canonical(Export()), before);

			if (heldGold) {
				MyPlayer->InvList[InventoryGridCells - 1].clear();
				MyPlayer->InvGrid[InventoryGridCells - 1] = 0;
				--MyPlayer->_pNumInv;
			} else {
				MyPlayer->SpdList[0].clear();
			}
			ASSERT_EQ(ParseCmd(owner, reinterpret_cast<const TCmd *>(&pickup), sizeof(pickup)), sizeof(pickup));

			EXPECT_EQ(ActiveItemCount, 1);
			EXPECT_TRUE(MyPlayer->HoldItem.keyAttributesMatch(63001, IDI_HEAL, 0));
			const Item &stored = heldGold ? MyPlayer->InvList[InventoryGridCells - 1] : MyPlayer->SpdList[0];
			EXPECT_TRUE(stored.keyAttributesMatch(heldItem._iSeed, heldItem.IDidx, heldItem._iCreateInfo));
			EXPECT_EQ(MyPlayer->_pGold, CalculateGold(*MyPlayer));
			Drain();
		}
	}
}

TEST_F(PlayerSynchronizationTest, FullStorageOwnerRejectsPartialGoldPickupBeforeChangingGold)
{
	PrepareFullPickupStorage(7, false);
	MakeGoldStack(Items[5], 75);
	Items[5]._iSeed = 63003;
	Items[5]._iCreateInfo = CF_PREGEN;
	Items[5].position = { 40, 50 };
	DeltaAddItem(5);
	PutItemRecord(Items[5]._iSeed, Items[5]._iCreateInfo, Items[5].IDidx);
	MakeGoldStack(MyPlayer->InvList[0], MaxGold - 25);
	MyPlayer->_pGold = CalculateGold(*MyPlayer);
	const int goldBefore = MyPlayer->_pGold;
	const TCmdGItem pickup = MakePickupCommand(CMD_REQUESTGITEM, Items[5], MyPlayerId, 5);
	const auto before = Canonical(Export());

	ASSERT_EQ(ParseCmd(MyPlayerId, reinterpret_cast<const TCmd *>(&pickup), sizeof(pickup)), sizeof(pickup));

	EXPECT_EQ(MyPlayer->_pGold, goldBefore);
	EXPECT_EQ(CalculateGold(*MyPlayer), goldBefore);
	EXPECT_EQ(MyPlayer->InvList[0]._ivalue, MaxGold - 25);
	EXPECT_TRUE(MyPlayer->HoldItem.keyAttributesMatch(63002, IDI_HEAL, 0));
	EXPECT_EQ(Items[5]._ivalue, 75);
	EXPECT_EQ(ActiveItemCount, 2);
	EXPECT_TRUE(Drain().empty());
	EXPECT_EQ(Canonical(Export()), before);

	MyPlayer->SpdList[0].clear();
	ASSERT_EQ(ParseCmd(MyPlayerId, reinterpret_cast<const TCmd *>(&pickup), sizeof(pickup)), sizeof(pickup));
	EXPECT_EQ(MyPlayer->_pGold, goldBefore + 25);
	EXPECT_TRUE(MyPlayer->HoldItem.keyAttributesMatch(63003, IDI_GOLD, 0));
	EXPECT_EQ(MyPlayer->HoldItem._ivalue, 50);
	EXPECT_TRUE(MyPlayer->SpdList[0].keyAttributesMatch(63002, IDI_HEAL, 0));
	EXPECT_EQ(ActiveItemCount, 1);
}

TEST_F(PlayerSynchronizationTest, FullStorageOwnerCanCollectGoldThatFitsExistingStack)
{
	PrepareFullPickupStorage(7, false);
	MakeGoldStack(Items[5], 75);
	Items[5]._iSeed = 63004;
	Items[5]._iCreateInfo = CF_PREGEN;
	Items[5].position = { 40, 50 };
	DeltaAddItem(5);
	PutItemRecord(Items[5]._iSeed, Items[5]._iCreateInfo, Items[5].IDidx);
	MakeGoldStack(MyPlayer->InvList[0], MaxGold - 75);
	MyPlayer->_pGold = CalculateGold(*MyPlayer);
	const TCmdGItem pickup = MakePickupCommand(CMD_REQUESTGITEM, Items[5], MyPlayerId, 5);
	Drain();

	ASSERT_EQ(ParseCmd(MyPlayerId, reinterpret_cast<const TCmd *>(&pickup), sizeof(pickup)), sizeof(pickup));

	EXPECT_EQ(MyPlayer->_pGold, MaxGold);
	EXPECT_TRUE(MyPlayer->HoldItem.keyAttributesMatch(63002, IDI_HEAL, 0));
	EXPECT_EQ(ActiveItemCount, 1);
	for (const Message &message : Drain())
		EXPECT_NE(static_cast<_cmd_id>(message.front()), CMD_SYNCPUTITEM);
}

TEST_F(PlayerSynchronizationTest, OwnerCanSwapHeldItemWithFullPersonalStorageWhenDeltaHasRoom)
{
	PrepareFullPickupStorage(0, true, false);
	PutItemRecord(Items[5]._iSeed, Items[5]._iCreateInfo, Items[5].IDidx);
	const Item heldItem = MyPlayer->HoldItem;
	const TCmdGItem pickup = MakePickupCommand(CMD_REQUESTGITEM, Items[5], MyPlayerId, 5);
	ASSERT_TRUE(CanDeltaPutItem(heldItem, *MyPlayer));
	Drain();

	ASSERT_EQ(ParseCmd(MyPlayerId, reinterpret_cast<const TCmd *>(&pickup), sizeof(pickup)), sizeof(pickup));

	EXPECT_TRUE(MyPlayer->HoldItem.keyAttributesMatch(63001, IDI_HEAL, 0));
	EXPECT_EQ(ActiveItemCount, 1);
	unsigned dropped = 0;
	for (const Message &message : Drain()) {
		if (static_cast<_cmd_id>(message.front()) == CMD_SYNCPUTITEM) {
			++dropped;
			Receive(MyPlayerId, message);
		}
	}
	EXPECT_EQ(dropped, 1U);
	EXPECT_EQ(ActiveItemCount, 2);
	EXPECT_NE(FindGetItem(heldItem._iSeed, heldItem.IDidx, heldItem._iCreateInfo), -1);
}

class TemporaryHeldItemSave {
public:
	TemporaryHeldItemSave()
	    : directory_(std::filesystem::temp_directory_path() / ("devilutionx-held-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count())))
	{
		std::filesystem::create_directory(directory_);
#ifdef UNPACKED_SAVES
		path_ = directory_.string() + "/";
#else
		path_ = (directory_ / "hero.sv").string();
#endif
	}
	~TemporaryHeldItemSave()
	{
		std::error_code error;
		std::filesystem::remove_all(directory_, error);
	}
	void Save(const Player &player)
	{
		SaveWriter writer { std::string(path_) };
		SaveHeldItem(writer, player);
	}
	void SaveCorruptRecord()
	{
		SaveWriter writer { std::string(path_) };
		const std::array<std::byte, 3> bytes { std::byte { 1 }, std::byte { 2 }, std::byte { 3 } };
		ASSERT_TRUE(writer.WriteFile("helditem", bytes.data(), bytes.size()));
	}
	bool Load(Player &player)
	{
#ifdef UNPACKED_SAVES
		return LoadHeldItem(SaveReader { std::string(path_) }, player);
#else
		auto archive = SaveReader::Open(path_.c_str());
		if (!archive)
			return false;
		return LoadHeldItem(std::move(*archive), player);
#endif
	}

private:
	std::filesystem::path directory_;
	std::string path_;
};

TEST_F(PlayerSynchronizationTest, LevelLoadingCanStartANewBufferAfterThePreviousReplayWasFreed)
{
	Join(7, MakePlayer(7));
	run_delta_info();
	for (uint16_t value : { 71, 72 }) {
		const auto before = Players[7]._pBaseDex;
		gbBufferMsgs = 1;
		TCmdParam1 command { CMD_SETDEX, Swap16LE(value) };
		EXPECT_EQ(ParseCmd(7, reinterpret_cast<const TCmd *>(&command), sizeof(command)), sizeof(command));
		EXPECT_EQ(Players[7]._pBaseDex, before);
		run_delta_info();
		EXPECT_EQ(Players[7]._pBaseDex, value);
		EXPECT_FALSE(gbGameDestroyed);
	}
}

TEST_F(PlayerSynchronizationTest, HeldItemsSurviveAProfileRoundTripWithoutChangingInventory)
{
	PrepareGoldPickupArea(0);
	for (const bool gold : { false, true }) {
		SCOPED_TRACE(gold);
		TemporaryHeldItemSave archive;
		if (gold)
			MakeGoldStack(MyPlayer->HoldItem, 321);
		else
			InitializeItem(MyPlayer->HoldItem, IDI_HEAL);
		MyPlayer->HoldItem._iSeed = 91000 + gold;
		const Item before = MyPlayer->HoldItem;
		const int inventoryGold = CalculateGold(*MyPlayer);
		const int inventoryCount = MyPlayer->_pNumInv;
		archive.Save(*MyPlayer);
		MyPlayer->HoldItem.clear();

		ASSERT_TRUE(archive.Load(*MyPlayer));

		EXPECT_TRUE(MyPlayer->HoldItem.keyAttributesMatch(before._iSeed, before.IDidx, before._iCreateInfo));
		EXPECT_EQ(MyPlayer->HoldItem._ivalue, before._ivalue);
		EXPECT_EQ(CalculateGold(*MyPlayer), inventoryGold);
		EXPECT_EQ(MyPlayer->_pNumInv, inventoryCount);
	}
}

TEST_F(PlayerSynchronizationTest, MissingAndCorruptHeldRecordsLeaveTheProfileUnchanged)
{
	PrepareGoldPickupArea(0);
	MyPlayer->HoldItem.clear();
	EXPECT_FALSE(LoadHeldItem(std::nullopt, *MyPlayer));
	TemporaryHeldItemSave archive;
	archive.SaveCorruptRecord();
	EXPECT_FALSE(archive.Load(*MyPlayer));
	EXPECT_TRUE(MyPlayer->HoldItem.isEmpty());
}

TEST_F(PlayerSynchronizationTest, HellfireHeldRecordsRestoreTheirEditionWithoutChangingGlobalState)
{
	PrepareGoldPickupArea(0);
	TemporaryHeldItemSave archive;
	gbIsHellfire = true;
	MakeGoldStack(MyPlayer->HoldItem, 7000);
	MyPlayer->HoldItem._iSeed = 99101;
	archive.Save(*MyPlayer);
	MyPlayer->HoldItem.clear();
	gbIsHellfireSaveGame = false;
	EXPECT_TRUE(archive.Load(*MyPlayer));
	EXPECT_FALSE(gbIsHellfireSaveGame);
	EXPECT_EQ(MyPlayer->HoldItem._ivalue, 7000);
	gbIsHellfire = false;
}

TEST_F(PlayerSynchronizationTest, AnInvalidHellfireItemIndexIsRejectedBeforeLookingUpItsDefinition)
{
	PrepareGoldPickupArea(0);
	TCmdPItem item {};
	item.bCmd = CMD_SPAWNITEM;
	item.x = 40;
	item.y = 48;
	item.def.wIndx = static_cast<_item_indexes>(Swap16LE(0x7FFF));
	item.item.dwBuff = Swap32LE(CF_HELLFIRE);
	EXPECT_EQ(ParseCmd(MyPlayerId, reinterpret_cast<const TCmd *>(&item), sizeof(item)), sizeof(item));
	EXPECT_EQ(ActiveItemCount, 2);
	EXPECT_FALSE(gbGameDestroyed);
}

TEST_F(PlayerSynchronizationTest, SavingAnEmptyHandCannotRestoreAnOlderHeldItem)
{
	PrepareGoldPickupArea(0);
	TemporaryHeldItemSave archive;
	MakeGoldStack(MyPlayer->HoldItem, 321);
	archive.Save(*MyPlayer);
	MyPlayer->HoldItem.clear();
	archive.Save(*MyPlayer);

	EXPECT_FALSE(archive.Load(*MyPlayer));
	EXPECT_TRUE(MyPlayer->HoldItem.isEmpty());
}

TEST_F(PlayerSynchronizationTest, RestoringAHeldItemCannotOverwriteAnOccupiedHand)
{
	PrepareGoldPickupArea(0);
	TemporaryHeldItemSave archive;
	MakeGoldStack(MyPlayer->HoldItem, 321);
	archive.Save(*MyPlayer);
	InitializeItem(MyPlayer->HoldItem, IDI_HEAL);
	MyPlayer->HoldItem._iSeed = 91002;

	EXPECT_FALSE(archive.Load(*MyPlayer));
	EXPECT_TRUE(MyPlayer->HoldItem.keyAttributesMatch(91002, IDI_HEAL, 0));
}

TEST_F(PlayerSynchronizationTest, ExtendedDeltaKeepsPregenHistoryAlongsideAFullFloorOfDrops)
{
	PrepareGoldPickupArea(0);
	for (uint32_t i = 0; i < MAXITEMS; ++i) {
		TCmdGItem pickup = MakePickupCommand(CMD_GETITEM, Items[5], MyPlayerId, 5);
		pickup.def.dwSeed = Swap32LE(81000 + i);
		ASSERT_EQ(ParseCmd(MyPlayerId, reinterpret_cast<const TCmd *>(&pickup), sizeof(pickup)), sizeof(pickup));
	}
	for (uint32_t i = 0; i < MAXITEMS; ++i) {
		TCmdPItem drop {};
		drop.bCmd = CMD_DROPITEM;
		drop.x = 40 + i % 15;
		drop.y = 40 + i / 15;
		drop.item = MakePickupCommand(CMD_GETITEM, Items[5], MyPlayerId, 5).item;
		drop.def.wCI = 0;
		drop.def.dwSeed = Swap32LE(82000 + i);
		ASSERT_TRUE(DeltaPutItem(drop, { drop.x, drop.y }, *MyPlayer)) << i;
	}
	const Snapshot snapshot = Export();
	const auto before = Canonical(snapshot);
	delta_init();
	gbDeltaSender = 7;
	msg_start_resync();
	for (const Message &message : snapshot)
		Receive(7, message);
	ASSERT_EQ(gbDeltaSender, 7);
	gbBufferMsgs = 0;
	EXPECT_EQ(Canonical(Export()), before);
	for (uint32_t i = 0; i < MAXITEMS; ++i) {
		TCmdGItem pickup = MakePickupCommand(CMD_GETITEM, Items[5], MyPlayerId, 5);
		pickup.def.dwSeed = Swap32LE(82000 + i);
		pickup.def.wCI = 0;
		ASSERT_EQ(ParseCmd(MyPlayerId, reinterpret_cast<const TCmd *>(&pickup), sizeof(pickup)), sizeof(pickup));
	}
	Item next = Items[5];
	next._iSeed = 83000;
	next._iCreateInfo = 0;
	EXPECT_TRUE(CanDeltaPutItem(next, *MyPlayer));
}

TEST_F(PlayerSynchronizationTest, DeltaReloadRemovesAllTakenPregenItemsBeforeRestoringDrops)
{
	PrepareGoldPickupArea(0);
	TCmdPItem drop {};
	drop.bCmd = CMD_DROPITEM;
	drop.item = MakePickupCommand(CMD_GETITEM, Items[5], MyPlayerId, 5).item;
	drop.def.dwSeed = Swap32LE(84000);
	drop.def.wCI = 0;
	drop.x = 40;
	drop.y = 50;
	ASSERT_TRUE(DeltaPutItem(drop, { drop.x, drop.y }, *MyPlayer));
	for (uint32_t i = 0; i < MAXITEMS - 1; ++i) {
		TCmdGItem pickup = MakePickupCommand(CMD_GETITEM, Items[5], MyPlayerId, 5);
		pickup.def.dwSeed = Swap32LE(85000 + i);
		ASSERT_EQ(ParseCmd(MyPlayerId, reinterpret_cast<const TCmd *>(&pickup), sizeof(pickup)), sizeof(pickup));
	}
	PrepareOpenItemDropArea();
	for (uint8_t i = 0; i < MAXITEMS; ++i) {
		ActiveItems[i] = i;
		MakeGoldStack(Items[i], 75);
		Items[i]._iSeed = 85000 + i;
		Items[i]._iCreateInfo = CF_PREGEN;
		Items[i].position = { static_cast<WorldTileCoord>(40 + i % 15), static_cast<WorldTileCoord>(40 + i / 15) };
		dItem[Items[i].position.x][Items[i].position.y] = i + 1;
	}
	ActiveItemCount = MAXITEMS;
	const auto savedType = leveltype;
	leveltype = DTYPE_TOWN;
	DeltaLoadLevel();
	leveltype = savedType;
	EXPECT_EQ(ActiveItemCount, 2);
	EXPECT_NE(FindGetItem(84000, IDI_GOLD, 0), -1);
	EXPECT_NE(FindGetItem(85000 + MAXITEMS - 1, IDI_GOLD, CF_PREGEN), -1);
}

TEST_F(PlayerSynchronizationTest, PickupRequestCannotImpersonateAnotherRecipient)
{
	for (const SessionRules rules : { ClassicSessionRules, ExtendedSessionRules }) {
		for (const _cmd_id command : { CMD_REQUESTGITEM, CMD_REQUESTAGITEM }) {
			SCOPED_TRACE(rules.maxPlayers);
			SCOPED_TRACE(static_cast<int>(command));
			delta_init();
			sgGameInitInfo.rules = rules;
			ResizePlayersForSession(rules == ClassicSessionRules ? ClassicPlayerLimit : PlayerStorageCapacity);
			PrepareGoldPickupArea(0);
			const uint8_t sender = rules == ClassicSessionRules ? 3 : 7;
			Players[sender].setLevel(currlevel);
			Players[sender].plractive = true;
			PutItemRecord(Items[5]._iSeed, Items[5]._iCreateInfo, Items[5].IDidx);
			TCmdGItem request = MakePickupCommand(command, Items[5], sender, 5);
			const int goldBefore = CalculateGold(*MyPlayer);
			const auto before = Canonical(Export());

			ASSERT_EQ(ParseCmd(sender, reinterpret_cast<const TCmd *>(&request), sizeof(request)), sizeof(request));

			EXPECT_EQ(CalculateGold(*MyPlayer), goldBefore);
			EXPECT_EQ(ActiveItemCount, 2);
			EXPECT_EQ(dItem[40][50], 6);
			EXPECT_TRUE(Drain().empty());
			EXPECT_EQ(Canonical(Export()), before);

			request.bPnum = sender;
			ASSERT_EQ(ParseCmd(sender, reinterpret_cast<const TCmd *>(&request), sizeof(request)), sizeof(request));
			EXPECT_EQ(ActiveItemCount, 1);
			EXPECT_EQ(dItem[40][50], 0);
			EXPECT_FALSE(Drain().empty());
		}
	}
}

TEST_F(PlayerSynchronizationTest, PickupRequestMustMatchRequestersLevel)
{
	for (const SessionRules rules : { ClassicSessionRules, ExtendedSessionRules }) {
		for (const _cmd_id command : { CMD_REQUESTGITEM, CMD_REQUESTAGITEM }) {
			SCOPED_TRACE(rules.maxPlayers);
			SCOPED_TRACE(static_cast<int>(command));
			delta_init();
			sgGameInitInfo.rules = rules;
			ResizePlayersForSession(rules == ClassicSessionRules ? ClassicPlayerLimit : PlayerStorageCapacity);
			PrepareGoldPickupArea(0);
			const uint8_t sender = rules == ClassicSessionRules ? 3 : 7;
			Players[sender].setLevel(currlevel);
			Players[sender].plractive = true;
			PutItemRecord(Items[5]._iSeed, Items[5]._iCreateInfo, Items[5].IDidx);
			TCmdGItem request = MakePickupCommand(command, Items[5], sender, 5);
			request.bPnum = sender;
			request.bLevel = currlevel + 1;
			const auto before = Canonical(Export());

			ASSERT_EQ(ParseCmd(sender, reinterpret_cast<const TCmd *>(&request), sizeof(request)), sizeof(request));

			EXPECT_EQ(ActiveItemCount, 2);
			EXPECT_EQ(dItem[40][50], 6);
			EXPECT_TRUE(Drain().empty());
			EXPECT_EQ(Canonical(Export()), before);

			request.bLevel = currlevel;
			ASSERT_EQ(ParseCmd(sender, reinterpret_cast<const TCmd *>(&request), sizeof(request)), sizeof(request));
			EXPECT_EQ(ActiveItemCount, 1);
			Drain();
		}
	}
}

TEST_F(PlayerSynchronizationTest, PickupGrantCannotImpersonateAnotherMaster)
{
	for (const SessionRules rules : { ClassicSessionRules, ExtendedSessionRules }) {
		for (const _cmd_id command : { CMD_GETITEM, CMD_AGETITEM, CMD_ITEMEXTRA }) {
			SCOPED_TRACE(rules.maxPlayers);
			SCOPED_TRACE(static_cast<int>(command));
			delta_init();
			sgGameInitInfo.rules = rules;
			ResizePlayersForSession(rules == ClassicSessionRules ? ClassicPlayerLimit : PlayerStorageCapacity);
			PrepareGoldPickupArea(rules == ClassicSessionRules ? 3 : 7);
			Players[0].setLevel(currlevel);
			Players[1].setLevel(currlevel);
			const TCmdGItem grant = MakePickupCommand(command, Items[5], 0, 5);
			const int goldBefore = CalculateGold(*MyPlayer);
			const auto before = Canonical(Export());

			ASSERT_EQ(ParseCmd(1, reinterpret_cast<const TCmd *>(&grant), sizeof(grant)), sizeof(grant));

			EXPECT_EQ(CalculateGold(*MyPlayer), goldBefore);
			EXPECT_EQ(ActiveItemCount, 2);
			EXPECT_EQ(dItem[40][50], 6);
			EXPECT_EQ(Canonical(Export()), before);

			ASSERT_EQ(ParseCmd(0, reinterpret_cast<const TCmd *>(&grant), sizeof(grant)), sizeof(grant));
			EXPECT_EQ(ActiveItemCount, 1);
			EXPECT_EQ(CalculateGold(*MyPlayer), goldBefore + (command == CMD_ITEMEXTRA ? 0 : 75));
		}
	}
}

TEST_F(PlayerSynchronizationTest, BufferedPickupGrantStillValidatesItsOriginalSender)
{
	PrepareGoldPickupArea(7);
	const TCmdGItem grant = MakePickupCommand(CMD_GETITEM, Items[5], 0, 5);
	const int goldBefore = CalculateGold(*MyPlayer);
	const auto before = Canonical(Export());
	Players[1].setLevel(currlevel);
	Players[1].position.tile = { 41, 49 };
	std::strcpy(Players[1]._pName, "Peer");
	ASSERT_TRUE(IsNetPlayerValid(1));
	msg_start_resync();
	ASSERT_EQ(ParseCmd(1, reinterpret_cast<const TCmd *>(&grant), sizeof(grant)), sizeof(grant));
	EXPECT_EQ(ActiveItemCount, 2);

	run_delta_info();

	EXPECT_EQ(ActiveItemCount, 2);
	EXPECT_EQ(CalculateGold(*MyPlayer), goldBefore);
	EXPECT_EQ(Canonical(Export()), before);
	ASSERT_EQ(ParseCmd(0, reinterpret_cast<const TCmd *>(&grant), sizeof(grant)), sizeof(grant));
	EXPECT_EQ(ActiveItemCount, 1);
	EXPECT_EQ(CalculateGold(*MyPlayer), goldBefore + 75);
}

TEST_F(PlayerSynchronizationTest, DeferredPickupGrantKeepsItsOriginalMaster)
{
	for (const _cmd_id command : { CMD_GETITEM, CMD_AGETITEM }) {
		SCOPED_TRACE(static_cast<int>(command));
		delta_init();
		tmsg_cleanup();
		PrepareGoldPickupArea(7);
		Items[5]._iCreateInfo = 0;
		const TCmdGItem grant = MakePickupCommand(command, Items[5], 0, 5);
		const int goldBefore = CalculateGold(*MyPlayer);
		const uint16_t savedTickDelay = gnTickDelay;
		gnTickDelay = 0;
		const size_t parsed = ParseCmd(0, reinterpret_cast<const TCmd *>(&grant), sizeof(grant));
		gnTickDelay = savedTickDelay;
		ASSERT_EQ(parsed, sizeof(grant));
		EXPECT_EQ(ActiveItemCount, 2);
		EXPECT_EQ(CalculateGold(*MyPlayer), goldBefore);
		ASSERT_TRUE(DeltaStoreDroppedItem(Items[5], Items[5].position, *MyPlayer));
		Drain();
		std::this_thread::sleep_for(std::chrono::milliseconds(2));

		ProcessGameMessagePackets();

		EXPECT_EQ(ActiveItemCount, 1);
		EXPECT_EQ(CalculateGold(*MyPlayer), goldBefore + 75);
	}
}

TEST_F(PlayerSynchronizationTest, DeferredPickupRequestUsesOriginalRequestersLevel)
{
	for (const bool changesLevel : { false, true }) {
		for (const _cmd_id command : { CMD_REQUESTGITEM, CMD_REQUESTAGITEM }) {
			SCOPED_TRACE(changesLevel);
			SCOPED_TRACE(static_cast<int>(command));
			delta_init();
			tmsg_cleanup();
			PrepareGoldPickupArea(0);
			Players[7].setLevel(currlevel);
			Players[7].plractive = true;
			TCmdGItem request = MakePickupCommand(command, Items[5], 7, 5);
			request.bPnum = 7;
			request.def.dwSeed = Swap32LE(75000);
			PutItemRecord(75000, CF_PREGEN, IDI_GOLD);
			const uint16_t savedTickDelay = gnTickDelay;
			gnTickDelay = 0;
			const size_t parsed = ParseCmd(7, reinterpret_cast<const TCmd *>(&request), sizeof(request));
			gnTickDelay = savedTickDelay;
			ASSERT_EQ(parsed, sizeof(request));
			EXPECT_EQ(ActiveItemCount, 2);
			Items[5]._iSeed = 75000;
			if (changesLevel)
				Players[7].setLevel(currlevel + 1);
			Drain();
			std::this_thread::sleep_for(std::chrono::milliseconds(2));

			ProcessGameMessagePackets();

			EXPECT_EQ(ActiveItemCount, changesLevel ? 2 : 1);
			EXPECT_EQ(dItem[40][50], changesLevel ? 6 : 0);
		}
	}
}

TEST_F(PlayerSynchronizationTest, AutomaticPickupGrantUsesIdentityInsteadOfRemoteCursorIndex)
{
	PrepareGoldPickupArea(7);
	const int goldBefore = CalculateGold(*MyPlayer);
	const TCmdGItem pickup = MakePickupCommand(CMD_AGETITEM, Items[5], 0, 9);

	ASSERT_EQ(ParseCmd(0, reinterpret_cast<const TCmd *>(&pickup), sizeof(pickup)), sizeof(pickup));

	EXPECT_EQ(CalculateGold(*MyPlayer), goldBefore + 75);
	EXPECT_EQ(ActiveItemCount, 1);
	EXPECT_EQ(dItem[40][50], 0);
	EXPECT_EQ(dItem[43][50], 10);
}

TEST_F(PlayerSynchronizationTest, AutomaticPickupOwnerUsesIdentityInsteadOfStaleCursorIndex)
{
	PrepareGoldPickupArea(0);
	ASSERT_TRUE(MyPlayer->isLevelOwnedByLocalClient());
	PutItemRecord(31001, CF_PREGEN, IDI_GOLD);
	const int goldBefore = CalculateGold(*MyPlayer);
	const TCmdGItem pickup = MakePickupCommand(CMD_REQUESTAGITEM, Items[5], 0, 9);

	ASSERT_EQ(ParseCmd(0, reinterpret_cast<const TCmd *>(&pickup), sizeof(pickup)), sizeof(pickup));

	EXPECT_EQ(CalculateGold(*MyPlayer), goldBefore + 75);
	EXPECT_EQ(ActiveItemCount, 1);
	EXPECT_EQ(dItem[40][50], 0);
	EXPECT_EQ(dItem[43][50], 10);
}

TEST_F(PlayerSynchronizationTest, ReplayedPickupGrantCannotCollectAReusedFloorSlot)
{
	for (const _cmd_id command : { CMD_GETITEM, CMD_AGETITEM }) {
		SCOPED_TRACE(static_cast<int>(command));
		delta_init();
		PrepareGoldPickupArea(7);
		const int goldBefore = CalculateGold(*MyPlayer);
		const TCmdGItem pickup = MakePickupCommand(command, Items[5], 0, 5);
		ASSERT_EQ(ParseCmd(0, reinterpret_cast<const TCmd *>(&pickup), sizeof(pickup)), sizeof(pickup));
		ASSERT_EQ(CalculateGold(*MyPlayer), goldBefore + 75);
		Item replacement;
		MakeGoldStack(replacement, 30);
		replacement._iSeed = 31003;
		ASSERT_EQ(PlaceItemInWorld(std::move(replacement), { 40, 50 }), 5);

		ASSERT_EQ(ParseCmd(0, reinterpret_cast<const TCmd *>(&pickup), sizeof(pickup)), sizeof(pickup));

		EXPECT_EQ(CalculateGold(*MyPlayer), goldBefore + 75);
		EXPECT_EQ(ActiveItemCount, 2);
		EXPECT_EQ(dItem[40][50], 6);
		EXPECT_TRUE(Items[5].keyAttributesMatch(31003, IDI_GOLD, 0));
	}
}

TEST_F(PlayerSynchronizationTest, FullDeltaStoresPreviousHeldItemBeforeAcceptingAnother)
{
	for (const bool fullDelta : { false, true }) {
		for (const bool heldGold : { false, true }) {
			SCOPED_TRACE(fullDelta);
			SCOPED_TRACE(heldGold);
			delta_init();
			PrepareOpenItemDropArea();
			FreeCursor();
			CreatePlayer(*MyPlayer, HeroClass::Warrior);
			InitCursor();
			MyPlayer->setLevel(currlevel);
			MyPlayer->position.tile = { 40, 49 };
			MyPlayer->position.future = MyPlayer->position.tile;
			for (uint8_t i = 0; i < MAXITEMS; ++i)
				ActiveItems[i] = i;
			if (heldGold)
				MakeGoldStack(MyPlayer->HoldItem, 321);
			else
				InitializeItem(MyPlayer->HoldItem, IDI_HEAL);
			MyPlayer->HoldItem._iSeed = 50000;
			const Item previous = MyPlayer->HoldItem;
			NewCursor(MyPlayer->HoldItem);
			if (fullDelta) {
				for (uint32_t i = 0; i < ItemDeltaCapacity(); ++i) {
					TCmdGItem entry {};
					entry.bCmd = CMD_GETITEM;
					entry.x = 40;
					entry.y = 50;
					entry.def.wIndx = static_cast<_item_indexes>(Swap16LE(IDI_HEAL));
					entry.def.wCI = Swap16LE(CF_PREGEN);
					entry.def.dwSeed = Swap32LE(41000 + i);
					entry.bMaster = MyPlayerId;
					entry.bPnum = MyPlayerId;
					entry.bLevel = currlevel;
					ASSERT_EQ(ParseCmd(MyPlayerId, reinterpret_cast<const TCmd *>(&entry), sizeof(entry)), sizeof(entry));
				}
			}
			InitializeItem(Items[0], IDI_HEAL);
			Items[0]._iSeed = 41000;
			Items[0]._iCreateInfo = CF_PREGEN;
			Items[0].position = { 40, 50 };
			ActiveItemCount = 1;
			dItem[40][50] = 1;
			const TCmdGItem pickup = MakePickupCommand(CMD_GETITEM, Items[0], 7, 0);
			Drain();

			ASSERT_EQ(ParseCmd(7, reinterpret_cast<const TCmd *>(&pickup), sizeof(pickup)), sizeof(pickup));

			EXPECT_TRUE(MyPlayer->HoldItem.keyAttributesMatch(41000, IDI_HEAL, 0));
			const auto matchesPrevious = [&](const Item &item) {
				return !item.isEmpty() && item.keyAttributesMatch(previous._iSeed, previous.IDidx, previous._iCreateInfo);
			};
			const auto stored = std::count_if(std::begin(MyPlayer->InvList), std::end(MyPlayer->InvList), matchesPrevious)
			    + std::count_if(std::begin(MyPlayer->SpdList), std::end(MyPlayer->SpdList), matchesPrevious);
			EXPECT_EQ(stored, fullDelta ? 1 : 0);
			EXPECT_EQ(MyPlayer->_pGold, CalculateGold(*MyPlayer));
			unsigned dropped = 0;
			for (const Message &message : Drain()) {
				if (static_cast<_cmd_id>(message.front()) == CMD_SYNCPUTITEM) {
					++dropped;
					Receive(MyPlayerId, message);
				}
			}
			EXPECT_EQ(dropped, fullDelta ? 0U : 1U);
			EXPECT_EQ(ActiveItemCount, fullDelta ? 0 : 1);
		}
	}
}

TEST_F(PlayerSynchronizationTest, LocalPutUsesItsMessageInsteadOfTheLastSentItem)
{
	PrepareOpenItemDropArea();
	FreeCursor();
	CreatePlayer(*MyPlayer, HeroClass::Warrior);
	InitCursor();
	MyPlayer->setLevel(currlevel);
	MyPlayer->position.tile = { 40, 50 };
	const Item droppedItem = MyPlayer->InvBody[INVLOC_HAND_LEFT];
	ASSERT_FALSE(droppedItem.isEmpty());
	TCmdPItem drop {};
	drop.bCmd = CMD_PUTITEM;
	drop.x = 41;
	drop.y = 50;
	drop.def.wIndx = static_cast<_item_indexes>(Swap16LE(droppedItem.IDidx));
	drop.def.wCI = Swap16LE(droppedItem._iCreateInfo);
	drop.def.dwSeed = Swap32LE(droppedItem._iSeed);
	PrepareItemForNetwork(droppedItem, drop.item);
	for (uint8_t i = 0; i < MAXITEMS; ++i)
		ActiveItems[i] = i;

	NetSendCmdPItem(true, CMD_PUTITEM, { drop.x, drop.y }, droppedItem);
	Item unrelatedItem;
	MakeGoldStack(unrelatedItem, 321);
	NetSendCmdPItem(false, CMD_SPAWNITEM, { 45, 50 }, unrelatedItem);
	ASSERT_EQ(ParseCmd(MyPlayerId, reinterpret_cast<const TCmd *>(&drop), sizeof(drop)), sizeof(drop));

	ASSERT_EQ(ActiveItemCount, 1);
	const Item &placedItem = Items[ActiveItems[0]];
	EXPECT_TRUE(placedItem.keyAttributesMatch(droppedItem._iSeed, droppedItem.IDidx, droppedItem._iCreateInfo));
	EXPECT_EQ(placedItem._iDurability, droppedItem._iDurability);
	EXPECT_EQ(placedItem._iCharges, droppedItem._iCharges);
}

TEST_F(PlayerSynchronizationTest, ReplayedPutMessagesDoNotDuplicateFloorItems)
{
	for (const _cmd_id command : { CMD_PUTITEM, CMD_SYNCPUTITEM }) {
		SCOPED_TRACE(static_cast<int>(command));
		delta_init();
		PrepareOpenItemDropArea();
		ActiveItemCount = 0;
		for (uint8_t i = 0; i < MAXITEMS; ++i)
			ActiveItems[i] = i;
		Players[7].setLevel(currlevel);
		TCmdPItem drop {};
		drop.bCmd = command;
		drop.x = 41;
		drop.y = 50;
		drop.def.wIndx = static_cast<_item_indexes>(Swap16LE(IDI_GOLD));
		drop.def.dwSeed = Swap32LE(21000 + static_cast<uint32_t>(command));
		drop.item.wValue = Swap16LE(123);

		ASSERT_EQ(ParseCmd(7, reinterpret_cast<const TCmd *>(&drop), sizeof(drop)), sizeof(drop));
		ASSERT_EQ(ActiveItemCount, 1);
		const auto before = Canonical(Export());
		ASSERT_EQ(ParseCmd(7, reinterpret_cast<const TCmd *>(&drop), sizeof(drop)), sizeof(drop));
		EXPECT_EQ(ActiveItemCount, 1);
		EXPECT_EQ(Canonical(Export()), before);
	}
}

TEST_F(PlayerSynchronizationTest, FullDeltaRejectsPutBeforeCreatingAnUntrackedFloorItem)
{
	PrepareOpenItemDropArea();
	MyPlayer->setLevel(currlevel);
	Players[7].setLevel(currlevel);
	for (uint32_t i = 0; i < ItemDeltaCapacity(); ++i) {
		TCmdGItem pickup {};
		pickup.bCmd = CMD_GETITEM;
		pickup.x = 40;
		pickup.y = 50;
		pickup.def.wIndx = static_cast<_item_indexes>(Swap16LE(IDI_GOLD));
		pickup.def.wCI = Swap16LE(CF_PREGEN);
		pickup.def.dwSeed = Swap32LE(22000 + i);
		pickup.bMaster = MyPlayerId;
		pickup.bPnum = MyPlayerId;
		pickup.bLevel = currlevel;
		ASSERT_EQ(ParseCmd(MyPlayerId, reinterpret_cast<const TCmd *>(&pickup), sizeof(pickup)), sizeof(pickup));
	}
	const auto before = Canonical(Export());
	TCmdPItem drop {};
	drop.bCmd = CMD_PUTITEM;
	drop.x = 41;
	drop.y = 50;
	drop.def.wIndx = static_cast<_item_indexes>(Swap16LE(IDI_GOLD));
	drop.def.dwSeed = Swap32LE(23000);
	drop.item.wValue = Swap16LE(123);

	ASSERT_EQ(ParseCmd(7, reinterpret_cast<const TCmd *>(&drop), sizeof(drop)), sizeof(drop));
	EXPECT_EQ(ActiveItemCount, 0);
	EXPECT_EQ(Canonical(Export()), before);
}

TEST_F(PlayerSynchronizationTest, ManualDropPreservesHeldItemWhenDeltaIsFull)
{
	PrepareOpenItemDropArea();
	FreeCursor();
	CreatePlayer(*MyPlayer, HeroClass::Warrior);
	InitCursor();
	MyPlayer->setLevel(currlevel);
	MyPlayer->position.tile = { 40, 50 };
	MyPlayer->position.future = MyPlayer->position.tile;
	MyPlayer->_pdir = Direction::East;
	InitializeItem(MyPlayer->HoldItem, IDI_HEAL);
	MyPlayer->HoldItem._iSeed = 24000;
	NewCursor(MyPlayer->HoldItem);
	for (uint32_t i = 0; i < ItemDeltaCapacity(); ++i) {
		TCmdGItem pickup {};
		pickup.bCmd = CMD_GETITEM;
		pickup.x = 40;
		pickup.y = 50;
		pickup.def.wIndx = static_cast<_item_indexes>(Swap16LE(IDI_GOLD));
		pickup.def.wCI = Swap16LE(CF_PREGEN);
		pickup.def.dwSeed = Swap32LE(25000 + i);
		pickup.bMaster = MyPlayerId;
		pickup.bPnum = MyPlayerId;
		pickup.bLevel = currlevel;
		ASSERT_EQ(ParseCmd(MyPlayerId, reinterpret_cast<const TCmd *>(&pickup), sizeof(pickup)), sizeof(pickup));
	}
	const int cursorBefore = pcurs;

	EXPECT_FALSE(TryDropItem());
	EXPECT_FALSE(MyPlayer->HoldItem.isEmpty());
	EXPECT_EQ(MyPlayer->HoldItem._iSeed, 24000U);
	EXPECT_EQ(pcurs, cursorBefore);
	EXPECT_EQ(ActiveItemCount, 0);
}

TEST_F(PlayerSynchronizationTest, ManualDropRoundTripPreservesItemInClassicAndExtendedSessions)
{
	for (const SessionRules rules : { ClassicSessionRules, ExtendedSessionRules }) {
		SCOPED_TRACE(rules.maxPlayers);
		sgGameInitInfo.rules = rules;
		ResizePlayersForSession(rules == ClassicSessionRules ? ClassicPlayerLimit : PlayerStorageCapacity);
		MyPlayer = &Players[MyPlayerId];
		for (const HeroClass heroClass : { HeroClass::Warrior, HeroClass::Sorcerer }) {
			SCOPED_TRACE(static_cast<int>(heroClass));
			delta_init();
			PrepareOpenItemDropArea();
			ActiveItemCount = 0;
			for (uint8_t i = 0; i < MAXITEMS; ++i)
				ActiveItems[i] = i;
			FreeCursor();
			CreatePlayer(*MyPlayer, heroClass);
			InitCursor();
			MyPlayer->setLevel(currlevel);
			MyPlayer->position.tile = { 40, 50 };
			MyPlayer->position.future = MyPlayer->position.tile;
			MyPlayer->_pdir = Direction::East;
			MyPlayer->HoldItem = MyPlayer->InvBody[INVLOC_HAND_LEFT].pop();
			ASSERT_FALSE(MyPlayer->HoldItem.isEmpty());
			const Item source = MyPlayer->HoldItem;
			NewCursor(MyPlayer->HoldItem);
			Drain();

			ASSERT_TRUE(TryDropItem());
			EXPECT_TRUE(MyPlayer->HoldItem.isEmpty());
			const auto messages = Drain();
			ASSERT_EQ(messages.size(), 1U);
			Receive(MyPlayerId, messages.front());
			ASSERT_EQ(ActiveItemCount, 1);
			const Item &placed = Items[ActiveItems[0]];
			EXPECT_TRUE(placed.keyAttributesMatch(source._iSeed, source.IDidx, source._iCreateInfo));
			EXPECT_EQ(placed._iDurability, source._iDurability);
			EXPECT_EQ(placed._iCharges, source._iCharges);
			EXPECT_EQ(placed._iMinDam, source._iMinDam);
			EXPECT_EQ(placed._iMaxDam, source._iMaxDam);
			EXPECT_EQ(placed._iStatFlag, source._iStatFlag);
			const auto beforeReplay = Canonical(Export());
			Receive(MyPlayerId, messages.front());
			EXPECT_EQ(ActiveItemCount, 1);
			EXPECT_EQ(Canonical(Export()), beforeReplay);
		}
	}
}

TEST_F(PlayerSynchronizationTest, ClosingStashReturnsHeldItemToBeltWhenDropDeltaIsFull)
{
	PrepareOpenItemDropArea();
	FreeCursor();
	CreatePlayer(*MyPlayer, HeroClass::Warrior);
	InitCursor();
	MyPlayer->setLevel(currlevel);
	MyPlayer->position.tile = { 40, 50 };
	MyPlayer->position.future = MyPlayer->position.tile;
	InitializeItem(MyPlayer->HoldItem, IDI_HEAL);
	MyPlayer->HoldItem._iSeed = 26000;
	NewCursor(MyPlayer->HoldItem);
	for (uint32_t i = 0; i < ItemDeltaCapacity(); ++i) {
		TCmdGItem pickup {};
		pickup.bCmd = CMD_GETITEM;
		pickup.x = 40;
		pickup.y = 50;
		pickup.def.wIndx = static_cast<_item_indexes>(Swap16LE(IDI_GOLD));
		pickup.def.wCI = Swap16LE(CF_PREGEN);
		pickup.def.dwSeed = Swap32LE(27000 + i);
		pickup.bMaster = MyPlayerId;
		pickup.bPnum = MyPlayerId;
		pickup.bLevel = currlevel;
		ASSERT_EQ(ParseCmd(MyPlayerId, reinterpret_cast<const TCmd *>(&pickup), sizeof(pickup)), sizeof(pickup));
	}
	IsStashOpen = true;

	CloseStash();

	EXPECT_FALSE(IsStashOpen);
	EXPECT_TRUE(MyPlayer->HoldItem.isEmpty());
	EXPECT_EQ(ActiveItemCount, 0);
	EXPECT_EQ(std::count_if(std::begin(MyPlayer->SpdList), std::end(MyPlayer->SpdList), [](const Item &item) {
		return !item.isEmpty() && item.keyAttributesMatch(26000, IDI_HEAL, 0);
	}),
	    1);
}

TEST_F(PlayerSynchronizationTest, DeathDropPreservesItemUntilFloorAndDeltaAcceptIt)
{
	PrepareOpenItemDropArea();
	FreeCursor();
	CreatePlayer(*MyPlayer, HeroClass::Warrior);
	InitCursor();
	MyPlayer->setLevel(currlevel);
	MyPlayer->position.tile = { 40, 50 };
	MyPlayer->position.future = MyPlayer->position.tile;
	const auto equippedItem = std::find_if(std::begin(MyPlayer->InvBody), std::end(MyPlayer->InvBody), [](const Item &item) {
		return !item.isEmpty();
	});
	ASSERT_NE(equippedItem, std::end(MyPlayer->InvBody));
	ActiveItemCount = MAXITEMS;

	EXPECT_FALSE(TestDeadItem(*MyPlayer, *equippedItem, { 0, 0 }));
	EXPECT_FALSE(equippedItem->isEmpty());

	ActiveItemCount = 0;
	for (uint8_t i = 0; i < MAXITEMS; ++i)
		ActiveItems[i] = i;
	EXPECT_TRUE(TestDeadItem(*MyPlayer, *equippedItem, { 0, 0 }));
	EXPECT_TRUE(equippedItem->isEmpty());
	EXPECT_EQ(ActiveItemCount, 1);
}

TEST_F(PlayerSynchronizationTest, FailedGoldDeathDropPreservesAllGold)
{
	PrepareOpenItemDropArea();
	FreeCursor();
	CreatePlayer(*MyPlayer, HeroClass::Warrior);
	InitCursor();
	const int goldBeforeDeath = CalculateGold(*MyPlayer);
	ASSERT_GT(goldBeforeDeath, 0);
	ActiveItemCount = MAXITEMS;

	TestDropHalfPlayersGold(*MyPlayer);

	EXPECT_EQ(CalculateGold(*MyPlayer), goldBeforeDeath);
	EXPECT_EQ(MyPlayer->_pGold, goldBeforeDeath);

	ActiveItemCount = 0;
	for (uint8_t i = 0; i < MAXITEMS; ++i)
		ActiveItems[i] = i;
	TestDropHalfPlayersGold(*MyPlayer);
	EXPECT_EQ(CalculateGold(*MyPlayer), goldBeforeDeath / 2);
	EXPECT_EQ(MyPlayer->_pGold, goldBeforeDeath / 2);
	EXPECT_EQ(ActiveItemCount, 1);
}

TEST_F(PlayerSynchronizationTest, RemoteDeathEquipmentClearsOnlyAfterDropIsStored)
{
	PrepareOpenItemDropArea();
	Player &player = Players[7];
	FreeCursor();
	CreatePlayer(player, HeroClass::Warrior);
	InitCursor();
	player.setLevel(currlevel);
	player.position.tile = { 47, 50 };
	player.position.future = player.position.tile;
	player._pdir = Direction::South;
	const auto equippedItem = std::find_if(std::begin(player.InvBody), std::end(player.InvBody), [](const Item &item) {
		return !item.isEmpty();
	});
	ASSERT_NE(equippedItem, std::end(player.InvBody));
	const Item droppedItem = *equippedItem;

	StartPlayerKill(player, DeathReason::MonsterOrTrap);
	ASSERT_FALSE(equippedItem->isEmpty());

	TCmdPItem spawn {};
	spawn.bCmd = CMD_SPAWNITEM;
	spawn.x = 48;
	spawn.y = 50;
	spawn.item.wIndx = static_cast<_item_indexes>(Swap16LE(droppedItem.IDidx));
	spawn.item.wCI = Swap16LE(droppedItem._iCreateInfo);
	spawn.item.dwSeed = Swap32LE(droppedItem._iSeed);
	PrepareItemForNetwork(droppedItem, spawn.item);
	ActiveItemCount = MAXITEMS;
	EXPECT_EQ(ParseCmd(7, reinterpret_cast<const TCmd *>(&spawn), sizeof(spawn)), sizeof(spawn));
	EXPECT_FALSE(equippedItem->isEmpty());

	ActiveItemCount = 0;
	for (uint8_t i = 0; i < MAXITEMS; ++i)
		ActiveItems[i] = i;
	EXPECT_EQ(ParseCmd(7, reinterpret_cast<const TCmd *>(&spawn), sizeof(spawn)), sizeof(spawn));
	EXPECT_TRUE(equippedItem->isEmpty());
	ASSERT_EQ(ActiveItemCount, 1);

	EXPECT_EQ(ParseCmd(7, reinterpret_cast<const TCmd *>(&spawn), sizeof(spawn)), sizeof(spawn));
	EXPECT_EQ(ActiveItemCount, 1);
}

TEST_F(PlayerSynchronizationTest, RemoteDeathDropKeepsZeroLifeAfterRemovingItemBonuses)
{
	for (const int lifeBonus : { -10, 10 }) {
		SCOPED_TRACE(lifeBonus);
		delta_init();
		PrepareOpenItemDropArea();
		Player &player = Players[7];
		FreeCursor();
		CreatePlayer(player, HeroClass::Warrior);
		InitCursor();
		player.setLevel(currlevel);
		player.position.tile = { 47, 50 };
		player.position.future = player.position.tile;
		player._pdir = Direction::South;
		Item &equippedItem = player.InvBody[INVLOC_HAND_LEFT];
		ASSERT_FALSE(equippedItem.isEmpty());
		equippedItem._iPLHP = Fixed10_6::fromInt(lifeBonus);
		CalcPlrInv(player, false);
		const Item droppedItem = equippedItem;
		StartPlayerKill(player, DeathReason::MonsterOrTrap);
		ASSERT_EQ(player._pHitPoints.raw(), 0);

		TCmdPItem spawn {};
		spawn.bCmd = CMD_SPAWNITEM;
		spawn.x = 48;
		spawn.y = 50;
		spawn.def.wIndx = static_cast<_item_indexes>(Swap16LE(droppedItem.IDidx));
		spawn.def.wCI = Swap16LE(droppedItem._iCreateInfo);
		spawn.def.dwSeed = Swap32LE(droppedItem._iSeed);
		PrepareItemForNetwork(droppedItem, spawn.item);
		for (uint8_t i = 0; i < MAXITEMS; ++i)
			ActiveItems[i] = i;

		ASSERT_EQ(ParseCmd(7, reinterpret_cast<const TCmd *>(&spawn), sizeof(spawn)), sizeof(spawn));
		EXPECT_TRUE(equippedItem.isEmpty());
		EXPECT_EQ(player._pmode, PM_DEATH);
		EXPECT_EQ(player._pHitPoints.raw(), 0);
	}
}

TEST_F(PlayerSynchronizationTest, PregenPickupCompletesOnFirstMessage)
{
	for (const SessionRules rules : { ClassicSessionRules, ExtendedSessionRules }) {
		SCOPED_TRACE(rules.maxPlayers);
		sgGameInitInfo.rules = rules;
		ResizePlayersForSession(rules == ClassicSessionRules ? ClassicPlayerLimit : PlayerStorageCapacity);
		MyPlayer = &Players[MyPlayerId];
		for (const _cmd_id command : { CMD_GETITEM, CMD_AGETITEM, CMD_ITEMEXTRA }) {
			SCOPED_TRACE(static_cast<int>(command));
			delta_init();
			PrepareOpenItemDropArea();
			FreeCursor();
			CreatePlayer(*MyPlayer, HeroClass::Warrior);
			InitCursor();
			MyPlayer->setLevel(currlevel);
			Players[1].setLevel(currlevel);
			const int goldBefore = CalculateGold(*MyPlayer);
			const Point position { 40, 50 };
			MakeGoldStack(Items[0], 100);
			Items[0]._iCreateInfo = CF_PREGEN;
			Items[0].position = position;
			ActiveItems[0] = 0;
			ActiveItemCount = 1;
			dItem[position.x][position.y] = 1;

			TCmdGItem pickup {};
			pickup.bCmd = command;
			pickup.x = position.x;
			pickup.y = position.y;
			pickup.def.wIndx = static_cast<_item_indexes>(Swap16LE(IDI_GOLD));
			pickup.def.wCI = Swap16LE(CF_PREGEN);
			pickup.def.dwSeed = Swap32LE(Items[0]._iSeed);
			PrepareItemForNetwork(Items[0], pickup.item);
			pickup.bMaster = 1;
			pickup.bPnum = MyPlayerId;
			pickup.bLevel = currlevel;

			ASSERT_EQ(ParseCmd(1, reinterpret_cast<const TCmd *>(&pickup), sizeof(pickup)), sizeof(pickup));
			EXPECT_EQ(ActiveItemCount, 0);
			EXPECT_EQ(dItem[position.x][position.y], 0);
			EXPECT_EQ(CalculateGold(*MyPlayer), goldBefore + (command == CMD_ITEMEXTRA ? 0 : 100));
		}
	}
}

TEST_F(PlayerSynchronizationTest, ItemOwnerPreservesPregenPickupWhenDeltaIsFull)
{
	MyPlayer->setLevel(currlevel);
	MyPlayer->plractive = true;
	Player &sender = Players[7];
	sender.setLevel(currlevel);
	sender.plractive = true;
	ASSERT_TRUE(sender.isLevelOwnedByLocalClient());
	for (uint32_t i = 0; i < ItemDeltaCapacity(); ++i) {
		TCmdGItem pickup {};
		pickup.bCmd = CMD_GETITEM;
		pickup.x = 40;
		pickup.y = 50;
		pickup.def.wIndx = static_cast<_item_indexes>(Swap16LE(IDI_GOLD));
		pickup.def.wCI = Swap16LE(CF_PREGEN);
		pickup.def.dwSeed = Swap32LE(10000 + i);
		pickup.bMaster = MyPlayerId;
		pickup.bPnum = MyPlayerId;
		pickup.bLevel = currlevel;
		ASSERT_EQ(ParseCmd(MyPlayerId, reinterpret_cast<const TCmd *>(&pickup), sizeof(pickup)), sizeof(pickup));
	}

	for (const _cmd_id command : { CMD_REQUESTGITEM, CMD_REQUESTAGITEM }) {
		SCOPED_TRACE(static_cast<int>(command));
		PrepareOpenItemDropArea();
		const Point position { 40, 50 };
		MakeGoldStack(Items[0], 100);
		Items[0]._iSeed = 11000 + static_cast<uint32_t>(command);
		Items[0]._iCreateInfo = CF_PREGEN;
		Items[0].position = position;
		ActiveItems[0] = 0;
		ActiveItemCount = 1;
		dItem[position.x][position.y] = 1;

		TCmdGItem pickup {};
		pickup.bCmd = command;
		pickup.x = position.x;
		pickup.y = position.y;
		pickup.def.wIndx = static_cast<_item_indexes>(Swap16LE(IDI_GOLD));
		pickup.def.wCI = Swap16LE(CF_PREGEN);
		pickup.def.dwSeed = Swap32LE(Items[0]._iSeed);
		PrepareItemForNetwork(Items[0], pickup.item);
		pickup.bPnum = 7;
		pickup.bLevel = currlevel;

		ASSERT_EQ(ParseCmd(7, reinterpret_cast<const TCmd *>(&pickup), sizeof(pickup)), sizeof(pickup));
		EXPECT_EQ(ActiveItemCount, 1);
		EXPECT_EQ(dItem[position.x][position.y], 1);
	}
}

TEST_F(PlayerSynchronizationTest, FailedGoldOverflowDropDoesNotDuplicatePartiallyPlacedGold)
{
	for (const bool heldItem : { false, true }) {
		SCOPED_TRACE(heldItem);
		PrepareOpenItemDropArea();
		FreeCursor();
		CreatePlayer(*MyPlayer, HeroClass::Warrior);
		InitCursor();
		for (int i = 0; i < InventoryGridCells; ++i) {
			InitializeItem(MyPlayer->InvList[i], IDI_HEAL);
			MyPlayer->InvGrid[i] = static_cast<int8_t>(i + 1);
		}
		MyPlayer->_pNumInv = InventoryGridCells;
		MakeGoldStack(MyPlayer->InvList[0], MaxGold + 1000);
		MakeGoldStack(MyPlayer->InvList[1], MaxGold - 500);
		if (heldItem) {
			InitializeItem(MyPlayer->HoldItem, IDI_HEAL);
			for (Item &item : MyPlayer->SpdList)
				InitializeItem(item, IDI_HEAL);
		}
		const int goldBefore = CalculateGold(*MyPlayer);
		ActiveItemCount = heldItem ? MAXITEMS - 1 : MAXITEMS;

		StripTopGold(*MyPlayer);

		EXPECT_EQ(CalculateGold(*MyPlayer), goldBefore);
		EXPECT_EQ(MyPlayer->_pGold, goldBefore);
		EXPECT_EQ(MyPlayer->InvList[0]._ivalue, MaxGold + 500);
		EXPECT_EQ(MyPlayer->InvList[1]._ivalue, MaxGold);
	}
}

TEST_F(PlayerSynchronizationTest, PickedUpPregenRecordCannotBeOverwrittenBySpawn)
{
	MyPlayer->setLevel(currlevel);
	TCmdGItem pickup {};
	pickup.bCmd = CMD_GETITEM;
	pickup.x = 40;
	pickup.y = 50;
	pickup.def.wIndx = static_cast<_item_indexes>(Swap16LE(IDI_GOLD));
	pickup.def.wCI = Swap16LE(CF_PREGEN);
	pickup.def.dwSeed = Swap32LE(12000);
	pickup.bMaster = MyPlayerId;
	pickup.bPnum = MyPlayerId;
	pickup.bLevel = currlevel;
	ASSERT_EQ(ParseCmd(MyPlayerId, reinterpret_cast<const TCmd *>(&pickup), sizeof(pickup)), sizeof(pickup));
	const auto before = Canonical(Export());

	TCmdPItem spawn {};
	spawn.bCmd = CMD_SPAWNITEM;
	spawn.x = 42;
	spawn.y = 50;
	spawn.def = pickup.def;
	EXPECT_FALSE(CanDeltaPutItem(spawn, *MyPlayer));
	EXPECT_FALSE(DeltaPutItem(spawn, { spawn.x, spawn.y }, *MyPlayer));
	EXPECT_EQ(Canonical(Export()), before);
}

TEST_F(PlayerSynchronizationTest, FullItemDeltaRejectsAdditionalPregenPickup)
{
	MyPlayer->setLevel(1);
	for (uint32_t i = 0; i < ItemDeltaCapacity(); ++i) {
		TCmdGItem pickup {};
		pickup.bCmd = CMD_GETITEM;
		pickup.x = 40;
		pickup.y = 50;
		pickup.def.wIndx = static_cast<_item_indexes>(Swap16LE(IDI_GOLD));
		pickup.def.wCI = Swap16LE(CF_PREGEN);
		pickup.def.dwSeed = Swap32LE(1000 + i);
		pickup.bMaster = MyPlayerId;
		pickup.bPnum = MyPlayerId;
		pickup.bLevel = 2;
		ASSERT_EQ(ParseCmd(MyPlayerId, reinterpret_cast<const TCmd *>(&pickup), sizeof(pickup)), sizeof(pickup));
	}

	TCmdGItem pickup {};
	pickup.bCmd = CMD_GETITEM;
	pickup.x = 40;
	pickup.y = 50;
	pickup.def.wIndx = static_cast<_item_indexes>(Swap16LE(IDI_GOLD));
	pickup.def.wCI = Swap16LE(CF_PREGEN);
	pickup.def.dwSeed = Swap32LE(2000);
	pickup.item.bId = 1;
	pickup.item.wValue = Swap16LE(100);
	pickup.bMaster = 1;
	pickup.bPnum = MyPlayerId;
	pickup.bLevel = 2;

	EXPECT_EQ(ParseCmd(1, reinterpret_cast<const TCmd *>(&pickup), sizeof(pickup)), sizeof(pickup));
	EXPECT_EQ(ActiveItemCount, 0);
	EXPECT_EQ(MyPlayer->_pNumInv, 0);

	Player &player = Players[7];
	FreeCursor();
	CreatePlayer(player, HeroClass::Warrior);
	InitCursor();
	player.setLevel(2);
	const auto equippedItem = std::find_if(std::begin(player.InvBody), std::end(player.InvBody), [](const Item &item) {
		return !item.isEmpty();
	});
	ASSERT_NE(equippedItem, std::end(player.InvBody));
	TCmdPItem spawn {};
	spawn.bCmd = CMD_SPAWNITEM;
	spawn.x = 48;
	spawn.y = 50;
	spawn.item.wIndx = static_cast<_item_indexes>(Swap16LE(equippedItem->IDidx));
	spawn.item.wCI = Swap16LE(equippedItem->_iCreateInfo);
	spawn.item.dwSeed = Swap32LE(equippedItem->_iSeed);
	PrepareItemForNetwork(*equippedItem, spawn.item);

	EXPECT_EQ(ParseCmd(7, reinterpret_cast<const TCmd *>(&spawn), sizeof(spawn)), sizeof(spawn));
	EXPECT_FALSE(equippedItem->isEmpty());
	EXPECT_EQ(ActiveItemCount, 0);
}

TEST_F(PlayerSynchronizationTest, FullItemDeltaPreservesPregenItemOnExtraMessage)
{
	PrepareOpenItemDropArea();
	MyPlayer->setLevel(currlevel);
	Player &sender = Players[1];
	sender.setLevel(currlevel);
	for (uint32_t i = 0; i < ItemDeltaCapacity(); ++i) {
		TCmdGItem pickup {};
		pickup.bCmd = CMD_GETITEM;
		pickup.x = 40;
		pickup.y = 50;
		pickup.def.wIndx = static_cast<_item_indexes>(Swap16LE(IDI_GOLD));
		pickup.def.wCI = Swap16LE(CF_PREGEN);
		pickup.def.dwSeed = Swap32LE(3000 + i);
		pickup.bMaster = MyPlayerId;
		pickup.bPnum = MyPlayerId;
		pickup.bLevel = currlevel;
		ASSERT_EQ(ParseCmd(MyPlayerId, reinterpret_cast<const TCmd *>(&pickup), sizeof(pickup)), sizeof(pickup));
	}

	const Point position { 40, 50 };
	InitializeItem(Items[0], IDI_GOLD);
	Items[0]._iSeed = 5000;
	Items[0]._iCreateInfo = CF_PREGEN;
	Items[0].position = position;
	ActiveItems[0] = 0;
	ActiveItemCount = 1;
	dItem[position.x][position.y] = 1;

	TCmdGItem extra {};
	extra.bCmd = CMD_ITEMEXTRA;
	extra.x = position.x;
	extra.y = position.y;
	extra.def.wIndx = static_cast<_item_indexes>(Swap16LE(IDI_GOLD));
	extra.def.wCI = Swap16LE(CF_PREGEN);
	extra.def.dwSeed = Swap32LE(5000);
	extra.bMaster = 1;
	extra.bPnum = MyPlayerId;
	extra.bLevel = currlevel;

	EXPECT_EQ(ParseCmd(1, reinterpret_cast<const TCmd *>(&extra), sizeof(extra)), sizeof(extra));
	EXPECT_EQ(ActiveItemCount, 1);
	EXPECT_FALSE(Items[0].isEmpty());
	EXPECT_EQ(dItem[position.x][position.y], 1);
}

TEST_F(PlayerSynchronizationTest, FullItemDeltaPreservesSourceAndPregenPickupRecord)
{
	PrepareOpenItemDropArea();
	FreeCursor();
	CreatePlayer(*MyPlayer, HeroClass::Warrior);
	InitCursor();
	MyPlayer->setLevel(currlevel);
	for (uint32_t i = 0; i < ItemDeltaCapacity(); ++i) {
		TCmdGItem pickup {};
		pickup.bCmd = CMD_GETITEM;
		pickup.x = 40;
		pickup.y = 50;
		pickup.def.wIndx = static_cast<_item_indexes>(Swap16LE(IDI_HEAL));
		pickup.def.wCI = Swap16LE(CF_PREGEN);
		pickup.def.dwSeed = Swap32LE(6000 + i);
		pickup.bMaster = MyPlayerId;
		pickup.bPnum = MyPlayerId;
		pickup.bLevel = currlevel;
		ASSERT_EQ(ParseCmd(MyPlayerId, reinterpret_cast<const TCmd *>(&pickup), sizeof(pickup)), sizeof(pickup));
	}

	const auto before = Canonical(Export());
	const Point position { 40, 50 };
	InitializeItem(Items[0], IDI_HEAL);
	Items[0]._iSeed = 6000;
	Items[0]._iCreateInfo = CF_PREGEN;
	Items[0].position = position;
	ActiveItems[0] = 0;
	ActiveItemCount = 1;
	dItem[position.x][position.y] = 1;

	TCmdGItem repick {};
	repick.bCmd = CMD_GETITEM;
	repick.x = position.x;
	repick.y = position.y;
	repick.def.wIndx = static_cast<_item_indexes>(Swap16LE(IDI_HEAL));
	repick.def.wCI = Swap16LE(CF_PREGEN);
	repick.def.dwSeed = Swap32LE(6000);
	PrepareItemForNetwork(Items[0], repick.item);
	repick.bMaster = 1;
	repick.bPnum = MyPlayerId;
	repick.bLevel = currlevel;
	EXPECT_EQ(ParseCmd(1, reinterpret_cast<const TCmd *>(&repick), sizeof(repick)), sizeof(repick));
	ASSERT_FALSE(MyPlayer->HoldItem.isEmpty());
	EXPECT_EQ(MyPlayer->HoldItem._iCreateInfo & CF_PREGEN, 0);
	MyPlayer->position.tile = position;
	EXPECT_FALSE(TestDeadItem(*MyPlayer, MyPlayer->HoldItem, { 0, 0 }));
	EXPECT_FALSE(MyPlayer->HoldItem.isEmpty());
	EXPECT_EQ(ActiveItemCount, 0);
	EXPECT_EQ(Canonical(Export()), before);
}

TEST_F(MultiplayerSyncTest, ReceivesTurnsWithoutTruncatingTheNegotiatedCapacity)
{
	// Prevent the background worker: this test only exercises initialization and
	// the engine's receive path; loopback still advertises eight storage slots.
	gbIsMultiplayer = false;
	nthread_start(false);
	EXPECT_EQ(gdwLargestMsgSize, 512U);
	EXPECT_EQ(gdwNormalMsgSize, 512U);
	EXPECT_EQ(sgbNetUpdateRate, 2);
	EXPECT_TRUE(nthread_recv_turns());
	for (unsigned i = 0; i < PlayerStorageCapacity; ++i) {
		EXPECT_EQ(gdwMsgLenTbl[i], 0U);
		EXPECT_EQ(glpMsgTbl[i], 0U);
	}
	nthread_cleanup();
}

TEST_F(MultiplayerSyncTest, VisionTracksEightPlayersAcrossLevelsDeathAndDeparture)
{
	InitLighting();
	std::array<DungeonFlag, MAXDUNX * MAXDUNY> savedFlags;
	std::array<int8_t, MAXDUNX * MAXDUNY> savedTransparency;
	std::copy(&dFlags[0][0], &dFlags[0][0] + savedFlags.size(), savedFlags.begin());
	std::copy(&dTransVal[0][0], &dTransVal[0][0] + savedTransparency.size(), savedTransparency.begin());
	std::fill(&dFlags[0][0], &dFlags[0][0] + MAXDUNX * MAXDUNY, DungeonFlag::None);
	std::fill(&dTransVal[0][0], &dTransVal[0][0] + MAXDUNX * MAXDUNY, 0);
	std::array<Point, PlayerStorageCapacity> positions;
	for (uint8_t playerId = 0; playerId < PlayerStorageCapacity; playerId++) {
		Player &player = Players[playerId];
		player.plractive = true;
		player.setLevel(currlevel);
		player.position.tile = { static_cast<WorldTileCoord>(10 + 12 * playerId), 50 };
		player._pLightRad = 1;
		player.friendlyMode = true;
		positions[playerId] = player.position.tile;
		ActivateVision(player.position.tile, player._pLightRad, playerId);
	}
	ActivateVision({ 50, 50 }, 1, PlayerStorageCapacity);
	ChangeVisionXY(PlayerStorageCapacity, { 51, 50 });
	DeactivateVision(PlayerStorageCapacity);
	Missile infravision {};
	infravision._misource = 7;
	infravision.duration = 2;
	ProcessInfravision(infravision);
	EXPECT_TRUE(Players[7]._pInfraFlag);
	EXPECT_FALSE(Players[MyPlayerId]._pInfraFlag);
	ProcessVisionList();

	for (uint8_t playerId = 0; playerId < PlayerStorageCapacity; playerId++) {
		SCOPED_TRACE(playerId);
		EXPECT_TRUE(VisionActive[playerId]);
		EXPECT_TRUE(HasAnyOf(dFlags[positions[playerId].x][positions[playerId].y], DungeonFlag::Visible));
		EXPECT_TRUE(HasAnyOf(dFlags[positions[playerId].x][positions[playerId].y], DungeonFlag::Explored));
		EXPECT_EQ(HasAnyOf(dFlags[positions[playerId].x][positions[playerId].y], DungeonFlag::Lit), playerId == MyPlayerId);
	}

	Players[6].setLevel(2);
	ChangeVisionXY(6, { 81, 50 });
	ProcessVisionList();
	EXPECT_FALSE(VisionActive[6]);
	EXPECT_FALSE(HasAnyOf(dFlags[positions[6].x][positions[6].y], DungeonFlag::Visible));
	EXPECT_FALSE(HasAnyOf(dFlags[81][50], DungeonFlag::Visible));
	EXPECT_TRUE(HasAnyOf(dFlags[positions[5].x][positions[5].y], DungeonFlag::Visible));

	Players[6].setLevel(currlevel);
	ActivateVision(positions[6], Players[6]._pLightRad, 6);
	Players[5]._pHitPoints = Fixed26_6::fromInt(0);
	ProcessVisionList();
	EXPECT_TRUE(Players[5].hasNoLife());
	EXPECT_TRUE(VisionActive[5]);
	EXPECT_TRUE(VisionActive[6]);

	multi_player_left(7, leaveinfo_t::LEAVE_DROP);
	EXPECT_FALSE(VisionActive[7]);
	ProcessVisionList();
	EXPECT_FALSE(HasAnyOf(dFlags[positions[7].x][positions[7].y], DungeonFlag::Visible));
	EXPECT_TRUE(HasAnyOf(dFlags[positions[4].x][positions[4].y], DungeonFlag::Visible));
	std::copy(savedFlags.begin(), savedFlags.end(), &dFlags[0][0]);
	std::copy(savedTransparency.begin(), savedTransparency.end(), &dTransVal[0][0]);
	InitLighting();
}

TEST_F(MultiplayerSyncTest, LightingPoolRejectsExhaustionAndReclaimsReleasedEntries)
{
	InitLighting();
	std::array<uint8_t, MAXDUNX * MAXDUNY> savedLight;
	std::copy(&dLight[0][0], &dLight[0][0] + savedLight.size(), savedLight.begin());
	std::array<int, MAXLIGHTS> lightIds;
	// Model eight portal lights and 24 concurrent effects sharing the legacy pool.
	for (int i = 0; i < MAXLIGHTS; i++) {
		lightIds[i] = AddLight({ static_cast<WorldTileCoord>(20 + i), 40 }, 1);
		EXPECT_EQ(lightIds[i], i);
	}
	EXPECT_EQ(ActiveLightCount, MAXLIGHTS);
	EXPECT_EQ(AddLight({ 50, 50 }, 1), NO_LIGHT);

	ChangeLightXY(NO_LIGHT, { 50, 50 });
	ChangeLightRadius(NO_LIGHT, 2);
	ChangeLightOffset(NO_LIGHT, { 1, 1 });
	ChangeLight(NO_LIGHT, { 50, 50 }, 2);
	AddUnLight(NO_LIGHT);
	ChangeLightXY(MAXLIGHTS, { 50, 50 });
	ChangeLightRadius(MAXLIGHTS, 2);
	ChangeLightOffset(MAXLIGHTS, { 1, 1 });
	ChangeLight(MAXLIGHTS, { 50, 50 }, 2);
	AddUnLight(MAXLIGHTS);
	EXPECT_EQ(ActiveLightCount, MAXLIGHTS);

	AddUnLight(lightIds[0]);
	ProcessLightList();
	EXPECT_EQ(ActiveLightCount, MAXLIGHTS - 1);
	EXPECT_EQ(AddLight({ 51, 50 }, 1), lightIds[0]);
	EXPECT_EQ(ActiveLightCount, MAXLIGHTS);
	std::copy(savedLight.begin(), savedLight.end(), &dLight[0][0]);
	InitLighting();
}

TEST_F(MultiplayerSyncTest, DeltaSenderCanBeEverySlotAndTheSentinelCannotCollide)
{
	std::array<uint32_t, PlayerStorageCapacity> turns {};
	for (uint8_t donor = 0; donor < PlayerStorageCapacity; ++donor) {
		SCOPED_TRACE(donor);
		MyPlayerId = donor == 0 ? 7 : 0;
		MyPlayer = &Players[MyPlayerId];
		std::fill_n(player_state, PlayerStorageCapacity, 0);
		player_state[donor] = PS_CONNECTED;
		player_state[MyPlayerId] = PS_CONNECTED | PS_TURN_ARRIVED;
		turns[MyPlayerId] = 0x80000000U | 37U;
		glpMsgTbl[MyPlayerId] = reinterpret_cast<uintptr_t>(&turns[MyPlayerId]);
		gdwMsgLenTbl[MyPlayerId] = sizeof(uint32_t);
		gbDeltaSender = NoDeltaSender;
		multi_msg_countdown();
		EXPECT_EQ(gbDeltaSender, donor);
	}
	std::fill_n(player_state, PlayerStorageCapacity, 0);
	player_state[MyPlayerId] = PS_CONNECTED | PS_TURN_ARRIVED;
	multi_msg_countdown();
	EXPECT_EQ(gbDeltaSender, NoDeltaSender);
}

TEST_F(MultiplayerSyncTest, LateJoinImportsTheSameLevelStateAtACommonTurn)
{
	SeedLevel(2, MaxMonsters);
	SeedLevel(9, 17);
	const auto expected = Canonical(Export());
	const auto snapshot = Export();
	ASSERT_GT(snapshot.size(), 3U);
	for (uint8_t newcomer = 4; newcomer < 8; ++newcomer) {
		SCOPED_TRACE(newcomer);
		delta_init();
		MyPlayerId = newcomer;
		MyPlayer = &Players[newcomer];
		gbDeltaSender = 1;
		msg_start_resync();
		for (const auto &message : snapshot)
			Receive(1, message);
		gbBufferMsgs = 0;
		EXPECT_EQ(Canonical(Export()), expected);
	}
}

TEST_F(MultiplayerSyncTest, JoinProgressClosesAfterACompleteSnapshot)
{
	SeedLevel(2, 17);
	const auto snapshot = Export();
	delta_init();
	MyPlayerId = 4;
	MyPlayer = &Players[4];
	gbDeltaSender = 1;
	msg_start_resync();
	for (const auto &message : snapshot)
		Receive(1, message);
	ASSERT_TRUE(TestHasCompleteDelta());
	// The dialog shows completion and then closes, as msg_wait_resync requires.
	EXPECT_EQ(TestWaitForTurns(), 99);
	EXPECT_EQ(TestWaitForTurns(), 100);
	EXPECT_TRUE(TestHasCompleteDelta());
	gbBufferMsgs = 0;
}

TEST_F(MultiplayerSyncTest, DamageSendersEncodeTheirPayloadsInLittleEndian)
{
	NetSendCmdMonDmg(true, 0x1234, 0x11223344);
	const auto monsterPackets = Drain();
	ASSERT_EQ(monsterPackets.size(), 1U);
	const Message monster { static_cast<std::byte>(CMD_MONSTDAMAGE), std::byte { 0x34 }, std::byte { 0x12 }, std::byte { 0x44 }, std::byte { 0x33 }, std::byte { 0x22 }, std::byte { 0x11 } };
	EXPECT_EQ(monsterPackets.front(), monster);
	NetSendCmdDamage(true, Players[7], 0x11223344, DamageType::Fire);
	const auto playerPackets = Drain();
	ASSERT_EQ(playerPackets.size(), 1U);
	const Message player { static_cast<std::byte>(CMD_PLRDAMAGE), std::byte { 7 }, std::byte { 0x44 }, std::byte { 0x33 }, std::byte { 0x22 }, std::byte { 0x11 }, static_cast<std::byte>(DamageType::Fire) };
	EXPECT_EQ(playerPackets.front(), player);
}

TEST_F(MultiplayerSyncTest, ImportsJunkEvenWhenNoLevelHasChanged)
{
	const auto snapshot = Export();
	ASSERT_EQ(snapshot.front()[0], static_cast<std::byte>(CMD_DLEVEL_JUNK));
	const auto expected = Canonical(snapshot);
	delta_init();
	gbDeltaSender = 7;
	msg_start_resync();
	for (const auto &message : snapshot)
		Receive(7, message);
	EXPECT_EQ(gbDeltaSender, 7);
	gbBufferMsgs = 0;
	EXPECT_EQ(Canonical(Export()), expected);
}

TEST_F(MultiplayerSyncTest, PortalSnapshotsPreserveClassicAndExtendedQuestOffsets)
{
	for (const SessionRules rules : { ClassicSessionRules, ExtendedSessionRules }) {
		SCOPED_TRACE(rules.maxPlayers);
		sgGameInitInfo.rules = rules;
		delta_init();
		InitPortals();
		// Keep the receiving map separate from all portal destinations.
		currlevel = 16;
		leveltype = DTYPE_HELL;
		const size_t count = rules == ClassicSessionRules ? ClassicPortalCount : MAXPORTAL;
		for (uint8_t owner = 0; owner < Players.size(); ++owner) {
			const TCmdLocParam3 command { CMD_ACTIVATEPORTAL, static_cast<uint8_t>(40 + owner), 50, Swap16LE(5), Swap16LE(DTYPE_CATACOMBS), 0 };
			ASSERT_EQ(ParseCmd(owner, reinterpret_cast<const TCmd *>(&command), sizeof(command)), sizeof(command));
		}
		Quests[Q_SKELKING]._qactive = QUEST_ACTIVE;
		Quests[Q_SKELKING]._qvar1 = 37;
		const auto snapshot = Export();
		const auto expected = Canonical(snapshot);
		const Message &junk = expected.at(256);
		for (size_t owner = 0; owner < count; ++owner) {
			const Message portal { static_cast<std::byte>(40 + owner), std::byte { 50 }, std::byte { 5 }, static_cast<std::byte>(DTYPE_CATACOMBS), std::byte { 0 } };
			EXPECT_TRUE(std::equal(portal.begin(), portal.end(), junk.begin() + owner * portal.size()));
		}
		const size_t questCount = std::count_if(std::begin(Quests), std::end(Quests), [](const Quest &quest) { return !QuestsData[quest._qidx].isSinglePlayerOnly || !UseMultiplayerQuests(); });
		const size_t gameplayBytes = count * 5 + questCount * 6;
		if (rules == ExtendedSessionRules) {
			const auto transfers = DecodeItemTransfers(std::span(junk).subspan(gameplayBytes));
			ASSERT_TRUE(transfers);
			EXPECT_EQ(transfers->issued, 0U);
			EXPECT_TRUE(transfers->queued.empty());
		} else {
			EXPECT_EQ(junk.size(), gameplayBytes);
		}
		delta_init();
		InitPortals();
		Quests[Q_SKELKING]._qvar1 = 0;
		gbDeltaSender = 7;
		msg_start_resync();
		for (const auto &message : snapshot)
			Receive(7, message);
		ASSERT_EQ(gbDeltaSender, 7);
		gbBufferMsgs = 0;
		DeltaSyncJunk();
		for (size_t owner = 0; owner < Players.size(); ++owner) {
			SCOPED_TRACE(owner);
			EXPECT_EQ(Portals[owner].open, owner < count);
			if (owner < count) {
				EXPECT_EQ(Portals[owner].position, Point(40 + static_cast<int>(owner), 50));
				EXPECT_EQ(Portals[owner].level, 5);
			}
		}
		EXPECT_EQ(Quests[Q_SKELKING]._qvar1, 37);
		EXPECT_EQ(Canonical(Export()), expected);
	}
}

TEST_F(MultiplayerSyncTest, InvalidExtendedPortalRecordsDoNotShiftQuestsOrOtherOwners)
{
	currlevel = 16;
	leveltype = DTYPE_HELL;
	for (uint8_t owner = 0; owner < Players.size(); ++owner) {
		const TCmdLocParam3 command { CMD_ACTIVATEPORTAL, static_cast<uint8_t>(40 + owner), 50, Swap16LE(5), Swap16LE(DTYPE_CATACOMBS), 0 };
		ASSERT_EQ(ParseCmd(owner, reinterpret_cast<const TCmd *>(&command), sizeof(command)), sizeof(command));
	}
	Quests[Q_SKELKING]._qactive = QUEST_ACTIVE;
	Quests[Q_SKELKING]._qvar1 = 73;
	const Message valid = Canonical(Export()).at(256);
	for (const size_t field : { size_t { 0 }, size_t { 2 }, size_t { 3 }, size_t { 4 } }) {
		SCOPED_TRACE(field);
		Message junk = valid;
		// 0xFF in x is a compressed closed record, not a five-byte invalid record.
		junk[7 * 5 + field] = field == 0 ? std::byte { 200 } : std::byte { 254 };
		junk.insert(junk.begin(), std::byte { 0 });
		delta_init();
		InitPortals();
		Quests[Q_SKELKING]._qvar1 = 0;
		gbDeltaSender = 7;
		msg_start_resync();
		Receive(7, Fragment(CMD_DLEVEL_JUNK, 0, junk));
		Receive(7, Fragment(CMD_DLEVEL_END, 0, Message { std::byte { 0 } }));
		ASSERT_EQ(gbDeltaSender, 7);
		gbBufferMsgs = 0;
		DeltaSyncJunk();
		EXPECT_FALSE(Portals[7].open);
		for (size_t owner = 0; owner < 7; ++owner)
			EXPECT_TRUE(Portals[owner].open);
		EXPECT_EQ(Quests[Q_SKELKING]._qvar1, 73);
	}
}

TEST_F(MultiplayerSyncTest, ClosedAndDepartedHighPortalSlotsStayClosedAfterSnapshotImport)
{
	currlevel = 16;
	leveltype = DTYPE_HELL;
	for (uint8_t owner = 0; owner < Players.size(); ++owner) {
		Players[owner].plractive = true;
		Players[owner].setLevel(5);
		const TCmdLocParam3 command { CMD_ACTIVATEPORTAL, static_cast<uint8_t>(40 + owner), 50, Swap16LE(5), Swap16LE(DTYPE_CATACOMBS), 0 };
		ASSERT_EQ(ParseCmd(owner, reinterpret_cast<const TCmd *>(&command), sizeof(command)), sizeof(command));
	}
	const TCmd close { CMD_DEACTIVATEPORTAL };
	ASSERT_EQ(ParseCmd(5, &close, sizeof(close)), sizeof(close));
	multi_player_left(7, leaveinfo_t::LEAVE_DROP);
	const auto snapshot = Export();
	const auto expected = Canonical(snapshot);
	delta_init();
	InitPortals();
	gbDeltaSender = 6;
	msg_start_resync();
	for (const auto &message : snapshot)
		Receive(6, message);
	ASSERT_EQ(gbDeltaSender, 6);
	gbBufferMsgs = 0;
	DeltaSyncJunk();
	for (size_t owner = 0; owner < Players.size(); ++owner)
		EXPECT_EQ(Portals[owner].open, owner != 5 && owner != 7);
	EXPECT_EQ(Canonical(Export()), expected);
}

TEST_F(MultiplayerSyncTest, IncompleteSnapshotDoesNotReplaceCommittedState)
{
	SeedLevel(3, MaxMonsters);
	const auto incoming = Export();
	delta_init();
	SeedLevel(8);
	const auto expected = Canonical(Export());
	msg_start_resync();
	gbDeltaSender = 7;
	for (size_t i = 0; i + 1 < incoming.size(); ++i)
		Receive(7, incoming[i]);
	EXPECT_EQ(Canonical(Export()), expected);
}

TEST_F(MultiplayerSyncTest, DonorDepartureDiscardsPartialDataAndAllowsSlotReuse)
{
	SeedLevel(3, MaxMonsters);
	const auto oldSnapshot = Export();
	delta_init();
	SeedLevel(8, 5);
	const auto replacement = Export();
	const auto expected = Canonical(replacement);
	delta_init();
	msg_start_resync();
	gbDeltaSender = 7;
	Receive(7, oldSnapshot.front());
	// This is called immediately by the production leave event, even while
	// player commands are buffered for replay after resynchronization.
	multi_player_left(7, leaveinfo_t::LEAVE_DROP);
	EXPECT_EQ(gbDeltaSender, NoDeltaSender);
	for (const uint8_t donor : { uint8_t { 6 }, uint8_t { 7 } }) {
		for (const auto &message : replacement)
			Receive(donor, message);
		gbBufferMsgs = 0;
		EXPECT_EQ(Canonical(Export()), expected);
		delta_reset_transfer();
		msg_start_resync();
	}
}

TEST_F(MultiplayerSyncTest, PartySnapshotRetainsLocalFutureDamageUntilItsScaleCommits)
{
	LoadMonsterData();
	Players[7].plractive = true;
	InitItemTransfers();
	auto &level = GetPartyLevelState(2);
	level.participants = 5;
	auto &monster = level.monsters[4];
	monster.generation = uint64_t { 1 } << 63;
	monster.nativeMax = 6400;
	monster.type = MT_NZOMBIE;
	monster.flags = PartyMonsterKnown;
	monster.x = monster.y = 40;
	const auto snapshot = Export();
	PartyDamageMessage damage { CMD_PARTYMONSTDAMAGE, 2, 6, 0, 1, 4, 128,
		monster.generation, monster.nativeMax, monster.type, monster.unique, 40, 40 };
	ReceivePartyMonsterDamage(7, damage);
	ASSERT_EQ(level.pending.size(), 1U);
	msg_start_resync();
	gbDeltaSender = 7;
	for (const auto &message : snapshot)
		Receive(7, message);
	ASSERT_TRUE(TestHasCompleteDelta());
	gbBufferMsgs = 0;
	EXPECT_EQ(GetPartyLevelState(2).pending.size(), 1U);
	CommitPartyLevel(2, 6, 0, 0, 8);
	EXPECT_TRUE(GetPartyLevelState(2).pending.empty());
	EXPECT_EQ(GetPartyLevelState(2).monsters[4].receipts[7], 1U);
	EXPECT_EQ(GetPartyLevelState(2).monsters[4].damage, 128 * (3360 / 6));
}

TEST_F(MultiplayerSyncTest, PlayerInfoRefreshPreservesTheConnectedPeersDamageEpoch)
{
	LoadGameArchives();
	ASSERT_TRUE(HaveMainData());
	LoadMonsterData();
	InitItemTransfers();
	const auto packed = MakePlayer(1);
	Join(1, packed);
	ASSERT_TRUE(Players[1].plractive);
	auto &level = GetPartyLevelState(2);
	level.participants = 5;
	auto &monster = level.monsters[4];
	monster.generation = uint64_t { 1 } << 63;
	monster.nativeMax = 6400;
	monster.type = MT_NZOMBIE;
	monster.flags = PartyMonsterKnown;
	monster.x = monster.y = 40;
	const auto epoch = MultiplayerSlotEpoch(1);
	PartyDamageMessage damage { CMD_PARTYMONSTDAMAGE, 2, 5, epoch, 1, 4, 64,
		monster.generation, monster.nativeMax, monster.type, monster.unique, 40, 40 };
	ReceivePartyMonsterDamage(1, damage);
	ASSERT_EQ(monster.receipts[1], 1U);
	const auto previousDamage = monster.damage;
	// The same connected peer can acknowledge its information after entry messages.
	Join(1, packed);
	EXPECT_EQ(MultiplayerSlotEpoch(1), epoch);
	++damage.receipt;
	ReceivePartyMonsterDamage(1, damage);
	EXPECT_EQ(monster.receipts[1], 2U);
	EXPECT_GT(monster.damage, previousDamage);
}

TEST_F(MultiplayerSyncTest, AJoiningClientRestoresItemTransfersBeforeReplayingPlayerInfo)
{
	// The host in slot 0 coordinates item transfers and exports the world.
	MyPlayer->plractive = true;
	InitItemTransfers();
	const auto snapshot = Export();
	// A joining client restores the snapshot before replaying the host's buffered player
	// information, so the host is connected but not yet active.
	for (Player &player : Players)
		player.plractive = false;
	MyPlayerId = 4;
	MyPlayer = &Players[4];
	MyPlayer->plractive = true;
	player_state[0] = PS_CONNECTED;
	player_state[4] = PS_CONNECTED;
	// Same order as NetInit.
	delta_init();
	InitItemTransfers();
	gbDeltaSender = 0;
	msg_start_resync();
	for (const auto &message : snapshot)
		Receive(0, message);
	ASSERT_TRUE(TestHasCompleteDelta());
	run_delta_info();
	EXPECT_FALSE(gbGameDestroyed);
}

TEST_F(MultiplayerSyncTest, LosingEveryDeltaDonorClosesExtendedResynchronization)
{
	msg_start_resync();
	// Loopback deliberately leaves connection flags untouched. Model the turn
	// transport's membership snapshots while exercising the real resync update.
	player_state[MyPlayerId] = PS_CONNECTED;
	Players[7].plractive = true;
	gbActivePlayers = 2;
	player_state[7] = PS_CONNECTED;
	gbDeltaSender = 7;
	EXPECT_LT(TestWaitForTurns(), 100);
	EXPECT_FALSE(gbGameDestroyed);

	multi_player_left(7, leaveinfo_t::LEAVE_DROP);
	player_state[7] = 0; // The next transport snapshot contains only the local peer.
	EXPECT_EQ(TestWaitForTurns(), 100);
	EXPECT_TRUE(gbGameDestroyed);
	EXPECT_FALSE(TestHasCompleteDelta());
}

TEST_F(MultiplayerSyncTest, ResynchronizationWaitsUntilTransportMembershipIsKnown)
{
	msg_start_resync();
	const int previousTick = last_tick;
	last_tick = SDL_GetTicks() + 1000;
	EXPECT_LT(TestWaitForTurns(), 100);
	EXPECT_FALSE(gbGameDestroyed);
	EXPECT_FALSE(TestHasCompleteDelta());
	last_tick = previousTick;
}

TEST_F(MultiplayerSyncTest, PartySnapshotCannotRewindAppliedDamageReceipts)
{
	LoadMonsterData();
	InitItemTransfers();
	auto &level = GetPartyLevelState(2);
	level.participants = 5;
	auto &monster = level.monsters[4];
	monster.generation = uint64_t { 1 } << 63;
	monster.nativeMax = 6400;
	monster.type = MT_NZOMBIE;
	monster.flags = PartyMonsterKnown;
	monster.x = monster.y = 40;
	const auto snapshot = Export();
	monster.receipts[7] = 1;
	monster.damage = PartyDamageUnits;
	msg_start_resync();
	gbDeltaSender = 7;
	for (const auto &message : snapshot)
		Receive(7, message);
	EXPECT_EQ(gbDeltaSender, NoDeltaSender);
	EXPECT_EQ(GetPartyLevelState(2).monsters[4].receipts[7], 1U);
	EXPECT_EQ(GetPartyLevelState(2).monsters[4].damage, PartyDamageUnits);
	gbBufferMsgs = 0;
}

TEST_F(MultiplayerSyncTest, AnotherPeerCannotHijackAnActiveTransfer)
{
	SeedLevel(2, MaxMonsters);
	const auto snapshot = Export();
	const auto expected = Canonical(snapshot);
	delta_init();
	msg_start_resync();
	gbDeltaSender = 7;
	Receive(7, snapshot.front());
	Receive(6, snapshot.front());
	EXPECT_EQ(gbDeltaSender, 7);
	for (size_t i = 1; i < snapshot.size(); ++i)
		Receive(7, snapshot[i]);
	gbBufferMsgs = 0;
	EXPECT_EQ(Canonical(Export()), expected);
}

TEST_F(MultiplayerSyncTest, RejectsDiscontinuousDuplicateAndTruncatedDeltaFragments)
{
	SeedLevel(2, MaxMonsters);
	const auto snapshot = Export();
	ASSERT_GE(snapshot.size(), 4U);
	for (const uint16_t badOffset : { uint16_t { 1 }, uint16_t { 65535 }, uint16_t { 0 } }) {
		delta_init();
		msg_start_resync();
		gbDeltaSender = 7;
		Receive(7, snapshot.front());
		Message invalid = snapshot[1];
		reinterpret_cast<TCmdPlrInfoHdr *>(invalid.data())->wOffset = Swap16LE(badOffset);
		Receive(7, invalid);
		EXPECT_EQ(gbDeltaSender, NoDeltaSender);
	}
	for (const auto &payload : { Message {}, Message { std::byte { 0 } }, Message { std::byte { 2 }, std::byte { 0 } }, Message { std::byte { 1 }, std::byte { 0 } } }) {
		delta_init();
		msg_start_resync();
		gbDeltaSender = 7;
		Receive(7, Fragment(CMD_DLEVEL, 0, payload));
		Receive(7, snapshot.back());
		EXPECT_EQ(gbDeltaSender, NoDeltaSender);
	}
}

TEST_F(MultiplayerSyncTest, BoundsIncompleteDeltaAndBufferedCommandMemory)
{
	msg_start_resync();
	gbDeltaSender = 7;
	const Message data(479, std::byte { 0 });
	size_t offset = 0;
	while (gbDeltaSender == 7 && offset < 60000) {
		Receive(7, Fragment(CMD_DLEVEL, static_cast<uint16_t>(offset), data));
		offset += data.size();
	}
	EXPECT_EQ(gbDeltaSender, NoDeltaSender);
	EXPECT_LE(offset, MaxItemTransferSnapshotSize + MAXPORTAL * 5 + MAXQUESTS * 6 + data.size() + 1);
	RecordProperty("delta_rejected_by_received_bytes", offset);
	msg_start_resync();
	const TCmdParam1 command { CMD_SETSTR, 0 };
	size_t count = 0;
	while (!gbGameDestroyed && count < 2000000) {
		ParseCmd(7, reinterpret_cast<const TCmd *>(&command), sizeof(command));
		++count;
	}
	EXPECT_TRUE(gbGameDestroyed);
	EXPECT_LE(count * sizeof(command), size_t { 128 } * 32000);
	RecordProperty("commands_before_resync_cancel", count);
	msg_start_resync();
	gbGameDestroyed = false;
}

TEST_F(MultiplayerSyncTest, RejectsInvalidSpawnIndicesBeforeWritingDeltas)
{
	Players[7].plrlevel = 2;
	for (const auto &[type, monster, owner] : std::array<std::array<uint16_t, 3>, 5> { { { 0, 65535, 7 }, { 65535, 0, 7 }, { 0, 0, 255 }, { 1, MaxMonsters, 7 }, { 0, MonsterStorageCapacity, 7 } } }) {
		const TCmdSpawnMonster command { CMD_SPAWNMONSTER, 40, 40, Direction::South, Swap16LE(type), Swap16LE(monster), 0, static_cast<uint8_t>(owner), 1 };
		EXPECT_EQ(ParseCmd(7, reinterpret_cast<const TCmd *>(&command), sizeof(command)), sizeof(command));
		EXPECT_FALSE(Canonical(Export()).contains(2));
	}
}

TEST_F(MultiplayerSyncTest, RejectsDeltaEndWithoutACompleteSnapshot)
{
	msg_start_resync();
	gbDeltaSender = 7;
	const std::array<std::byte, 1> zero {};
	Receive(7, Fragment(CMD_DLEVEL_END, 0, zero));
	EXPECT_EQ(gbDeltaSender, NoDeltaSender);
}

TEST_F(MultiplayerSyncTest, MaximumLevelDeltaRoundTripsWithFixedSpawnRecords)
{
	// Fill every variable-size section through the production command handlers.
	Players[7].plrlevel = 2;
	MyPlayer->plrlevel = 2;
	for (int i = 0; i < MAXITEMS; ++i) {
		InitializeItem(Items[i], IDI_GOLD);
		Items[i]._iSeed = 0x12340000 + i;
		Items[i]._ivalue = 100 + i;
		Items[i].position = { static_cast<WorldTileCoord>(20 + i % 70), 30 };
		DeltaAddItem(i);
	}
	for (int i = 0; i < MAXOBJECTS; ++i) {
		const TCmdLoc command { CMD_OPENDOOR, static_cast<uint8_t>(20 + i % 70), static_cast<uint8_t>(20 + i / 70) };
		EXPECT_EQ(ParseCmd(7, reinterpret_cast<const TCmd *>(&command), sizeof(command)), sizeof(command));
	}
	for (unsigned i = 0; i < MonsterLimit(); ++i) {
		const TCmdSpawnMonster command { CMD_SPAWNMONSTER, 40, 40, Direction::South,
			Swap16LE(0), Swap16LE(static_cast<uint16_t>(i)), Swap32LE(0x78563412 + i), 7, 1 };
		EXPECT_EQ(ParseCmd(7, reinterpret_cast<const TCmd *>(&command), sizeof(command)), sizeof(command));
	}
	SeedLevel(2, MonsterLimit());
	const auto snapshot = Export();
	const auto expected = Canonical(snapshot);
	ASSERT_TRUE(expected.contains(2));
	const auto &level = expected.at(2);
	const size_t expectedSize = 1 + MAXITEMS * sizeof(TCmdPItem) + (ItemDeltaCapacity() - MAXITEMS) + 1 + MAXOBJECTS * 3 + MonsterLimit() * 9 + 2 + MonsterLimit() * 11;
	const size_t partyBytes = sgGameInitInfo.rules == ExtendedSessionRules ? EncodePartyLevel(PartyLevelState {}).size() : 0;
	ASSERT_EQ(level.size(), expectedSize + partyBytes);
	RecordProperty("raw_level_bytes_with_marker", level.size() + 1);
	RecordProperty("player_info_bytes", sizeof(PlayerNetPack));
	RecordProperty("packet_header_bytes", sizeof(TPktHdr));
	RecordProperty("fragment_header_bytes", sizeof(TCmdPlrInfoHdr));
	RecordProperty("snapshot_fragments", snapshot.size());
	size_t transmittedBytes = 0;
	for (const auto &message : snapshot)
		transmittedBytes += message.size() + sizeof(TPktHdr);
	RecordProperty("snapshot_wire_bytes", transmittedBytes);
	// Nine-byte packed spawn body after the two-byte monster index. This golden
	// record is independent of native size_t width, alignment and endianness.
	const size_t spawnOffset = 1 + MAXITEMS * sizeof(TCmdPItem) + (ItemDeltaCapacity() - MAXITEMS) + 1 + MAXOBJECTS * 3 + MonsterLimit() * 9 + 2;
	const uint16_t monsterId = static_cast<uint16_t>(static_cast<unsigned>(level[spawnOffset]) | (static_cast<unsigned>(level[spawnOffset + 1]) << 8));
	ASSERT_LT(monsterId, MonsterLimit());
	EXPECT_EQ(level[spawnOffset + 2], std::byte { 0 });
	EXPECT_EQ(level[spawnOffset + 3], std::byte { 0 });
	const uint32_t seed = 0x78563412 + monsterId;
	for (unsigned i = 0; i < 4; ++i)
		EXPECT_EQ(level[spawnOffset + 4 + i], static_cast<std::byte>((seed >> (8 * i)) & 0xFF));
	EXPECT_EQ(level[spawnOffset + 8], std::byte { 7 });
	EXPECT_EQ(level[spawnOffset + 9], std::byte { 1 });
	EXPECT_EQ(level[spawnOffset + 10], std::byte { 0 });

	for (const bool compressed : { false, true }) {
		SCOPED_TRACE(compressed);
		delta_init();
		msg_start_resync();
		gbDeltaSender = 7;
		if (compressed) {
			for (const auto &message : snapshot)
				Receive(7, message);
		} else {
			Message raw { std::byte { 0 } };
			raw.insert(raw.end(), level.begin(), level.end());
			for (size_t offset = 0; offset < raw.size(); offset += 479)
				Receive(7, Fragment(CMD_DLEVEL, static_cast<uint16_t>(offset), std::span(raw).subspan(offset, std::min(size_t { 479 }, raw.size() - offset))));
			for (const auto &message : snapshot) {
				if (message[0] != static_cast<std::byte>(CMD_DLEVEL))
					Receive(7, message);
			}
		}
		EXPECT_EQ(gbDeltaSender, 7);
		gbBufferMsgs = 0;
		EXPECT_EQ(Canonical(Export()), expected);
	}
}

TEST_F(MultiplayerSyncTest, FragmentsAtEverySupportedPacketBoundary)
{
	const Message data(65535, std::byte { 0x5A });
	for (const uint32_t size : { uint32_t { 128 }, uint32_t { 512 } }) {
		gdwLargestMsgSize = size;
		MyPlayerId = 7;
		multi_send_zero_packet(0, CMD_DLEVEL, data.data(), data.size());
		MyPlayerId = 0;
		const auto fragments = Drain();
		Message received;
		for (const auto &fragment : fragments) {
			const auto &header = *reinterpret_cast<const TCmdPlrInfoHdr *>(fragment.data());
			EXPECT_EQ(Swap16LE(header.wOffset), received.size());
			EXPECT_EQ(Swap16LE(header.wBytes), fragment.size() - sizeof(header));
			received.insert(received.end(), fragment.begin() + sizeof(header), fragment.end());
		}
		EXPECT_EQ(received, data);
	}
}

TEST_F(PlayerSynchronizationTest, JoinsAndReusesAllFourAdditionalPlayerSlots)
{
	Player *const localPlayer = MyPlayer;
	for (uint8_t slot = 4; slot < 8; ++slot) {
		SCOPED_TRACE(slot);
		const auto packed = MakePlayer(slot);
		Join(slot, packed);
		ASSERT_TRUE(IsNetPlayerValid(slot));
		ASSERT_TRUE(Players[slot].plractive);
		EXPECT_STREQ(Players[slot]._pName, "New arrival");
		EXPECT_EQ(Players[slot].plrlevel, packed.plrlevel);
		EXPECT_EQ(Players[slot].position.tile, (Point { packed.px, packed.py }));
		Players[slot].destAction = ACTION_ATTACK;
		Players[slot].walkpath[0] = WALK_N;
		Players[slot].wReflections = 99;
		InspectPlayer = &Players[slot];
		PlayerUnderCursor = &Players[slot];
		Monsters[0].whoHit = PlayerExperienceMaskFromByte(0xFF);
		player_state[slot] = PS_CONNECTED | PS_TURN_ARRIVED;
		sgwPackPlrOffsetTbl[slot] = 99;
		multi_player_left(slot, leaveinfo_t::LEAVE_DROP);
		EXPECT_FALSE(Players[slot].plractive);
		EXPECT_EQ(Players[slot]._pName[0], '\0');
		EXPECT_EQ(Players[slot]._pNumInv, 0);
		EXPECT_TRUE(Players[slot].InvBody[INVLOC_HAND_LEFT].isEmpty());
		EXPECT_EQ(Players[slot].position.tile, (Point {}));
		EXPECT_EQ(Players[slot].wReflections, 0);
		EXPECT_EQ(player_state[slot], 0U);
		EXPECT_EQ(sgwPackPlrOffsetTbl[slot], 0);
		EXPECT_FALSE(IsPlayerInExperienceMask(Monsters[0].whoHit, slot));
		EXPECT_EQ(InspectPlayer, localPlayer);
		EXPECT_EQ(PlayerUnderCursor, nullptr);
		const auto replacement = MakePlayer(slot, "Replacement");
		Join(slot, replacement, 1);
		EXPECT_STREQ(Players[slot]._pName, "Replacement");
		EXPECT_EQ(Players[slot].destAction, ACTION_NONE);
		EXPECT_EQ(Players[slot].walkpath[0], WALK_NONE);
		EXPECT_EQ(MyPlayer, localPlayer);
		multi_player_left(slot, leaveinfo_t::LEAVE_EXIT);
	}
	EXPECT_EQ(gbActivePlayers, 1);
}

TEST_F(PlayerSynchronizationTest, ClassicDeparturePreservesExperienceDivisors)
{
	ResizePlayersForSession(ClassicPlayerLimit);
	sgGameInitInfo.rules = ClassicSessionRules;
	Join(1, MakePlayer(1));
	Monsters[0].whoHit = PlayerExperienceMaskFromByte(3);
	multi_player_left(1, leaveinfo_t::LEAVE_EXIT);
	EXPECT_EQ(PlayerExperienceMaskToByte(Monsters[0].whoHit), 3);
	EXPECT_FALSE(Players[1].plractive);
}

TEST_F(PlayerSynchronizationTest, DepartureDuringPlayerInfoDoesNotRetainPartialBytes)
{
	for (uint8_t slot = 4; slot < 8; ++slot) {
		const auto packed = MakePlayer(slot);
		const auto bytes = std::as_bytes(std::span(&packed, 1));
		Receive(slot, Fragment(CMD_ACK_PLRINFO, 0, bytes.first(64)));
		ASSERT_EQ(sgwPackPlrOffsetTbl[slot], 64);
		multi_player_left(slot, leaveinfo_t::LEAVE_DROP);
		EXPECT_EQ(sgwPackPlrOffsetTbl[slot], 0);
		Receive(slot, Fragment(CMD_ACK_PLRINFO, 64, bytes.subspan(64, 64)));
		EXPECT_EQ(sgwPackPlrOffsetTbl[slot], 0);
		EXPECT_FALSE(IsNetPlayerValid(slot));
		Join(slot, packed);
		EXPECT_TRUE(IsNetPlayerValid(slot));
		multi_player_left(slot, leaveinfo_t::LEAVE_EXIT);
	}
}

TEST_F(PlayerSynchronizationTest, BufferedCommandsRespectDepartureAndReincarnationOrder)
{
	for (uint8_t slot = 4; slot < 8; ++slot) {
		const auto original = MakePlayer(slot, "Departing");
		const auto replacement = MakePlayer(slot, "Replacement");
		msg_start_resync();
		Join(slot, original);
		const TCmdParam1 strength { CMD_SETSTR, Swap16LE(99) };
		EXPECT_EQ(ParseCmd(slot, reinterpret_cast<const TCmd *>(&strength), sizeof(strength)), sizeof(strength));
		multi_player_left(slot, leaveinfo_t::LEAVE_DROP);
		Join(slot, replacement);
		run_delta_info();
		EXPECT_STREQ(Players[slot]._pName, "Replacement");
		EXPECT_EQ(Players[slot]._pBaseStr, replacement.pBaseStr);
		EXPECT_EQ(Players[slot].wReflections, 0);
		EXPECT_EQ(gbActivePlayers, 2);
		multi_player_left(slot, leaveinfo_t::LEAVE_EXIT);
	}
}

TEST_F(MultiplayerSyncTest, RejectsOversizedPlayerInfoAndInvalidPlayerIds)
{
	const Message data(512, std::byte { 0 });
	Receive(7, Fragment(CMD_ACK_PLRINFO, 65535, data));
	EXPECT_EQ(sgwPackPlrOffsetTbl[7], 0);
	EXPECT_FALSE(IsNetPlayerValid(7));
	const TCmd command { CMD_PLRALIVE };
	EXPECT_EQ(ParseCmd(8, &command, sizeof(command)), 0U);
	EXPECT_EQ(ParseCmd(255, &command, sizeof(command)), 0U);
	EXPECT_EQ(ParseCmd(0, &command, 0), 0U);
	multi_player_left(255, leaveinfo_t::LEAVE_DROP);
}

TEST_F(PlayerSynchronizationTest, DeathAndResurrectionSynchronizePlayersOnOtherLevels)
{
	for (uint8_t slot = 4; slot < 8; ++slot) {
		Join(slot, MakePlayer(slot));
		TCmdParam1 death { CMD_PLRDEAD, Swap16LE(static_cast<uint16_t>(DeathReason::MonsterOrTrap)) };
		EXPECT_EQ(ParseCmd(slot, reinterpret_cast<TCmd *>(&death), sizeof(death)), sizeof(death));
		EXPECT_EQ(Players[slot]._pHitPoints.raw(), 0);
		EXPECT_EQ(Players[slot]._pmode, PM_DEATH);
		const TCmd resurrect { CMD_PLRALIVE };
		EXPECT_EQ(ParseCmd(slot, &resurrect, sizeof(resurrect)), sizeof(resurrect));
		EXPECT_GT(Players[slot]._pHitPoints.raw(), 0);
		EXPECT_EQ(Players[slot]._pmode, PM_STAND);
		EXPECT_EQ(Players[slot]._pMana.raw(), 0);
		multi_player_left(slot, leaveinfo_t::LEAVE_EXIT);
	}
}

TEST_F(PlayerSynchronizationTest, PlayerDamageFromEveryRemoteSlotTargetsTheLocalPlayer)
{
	FreeCursor();
	CreatePlayer(*MyPlayer, HeroClass::Warrior);
	InitCursor();
	MyPlayer->setLevel(currlevel);

	for (uint8_t slot = 1; slot < PlayerStorageCapacity; ++slot) {
		Join(slot, MakePlayer(slot));
		Players[slot].setLevel(currlevel);
	}

	const int32_t damage = Fixed26_6::fromInt(1).raw();
	TCmdDamage message {};
	message.bCmd = CMD_PLRDAMAGE;
	message.bPlr = MyPlayerId;
	message.dwDam = Swap32LE(static_cast<uint32_t>(damage));
	message.damageType = DamageType::Physical;

	for (uint8_t slot = 1; slot < PlayerStorageCapacity; ++slot) {
		SCOPED_TRACE(slot);
		const int32_t previousHitPoints = MyPlayer->_pHitPoints.raw();
		ASSERT_EQ(ParseCmd(slot, reinterpret_cast<const TCmd *>(&message), sizeof(message)), sizeof(message));
		EXPECT_EQ(MyPlayer->_pHitPoints.raw(), previousHitPoints - damage);
	}
}

TEST_F(PlayerSynchronizationTest, PlayerDamageForRemoteRecipientDoesNotDamageLocalPlayer)
{
	FreeCursor();
	CreatePlayer(*MyPlayer, HeroClass::Warrior);
	InitCursor();
	MyPlayer->setLevel(currlevel);
	Join(4, MakePlayer(4));
	Join(7, MakePlayer(7));
	Players[4].setLevel(currlevel);
	Players[7].setLevel(currlevel);

	const int32_t localHitPoints = MyPlayer->_pHitPoints.raw();
	const int32_t remoteHitPoints = Players[7]._pHitPoints.raw();
	TCmdDamage message {};
	message.bCmd = CMD_PLRDAMAGE;
	message.bPlr = 7;
	message.dwDam = Swap32LE(static_cast<uint32_t>(Fixed26_6::fromInt(1).raw()));
	message.damageType = DamageType::Physical;

	EXPECT_EQ(ParseCmd(4, reinterpret_cast<const TCmd *>(&message), sizeof(message)), sizeof(message));
	EXPECT_EQ(MyPlayer->_pHitPoints.raw(), localHitPoints);
	EXPECT_EQ(Players[7]._pHitPoints.raw(), remoteHitPoints);
}

TEST_F(PlayerSynchronizationTest, PlayerDamageRejectsInvalidRecipientAndExcessiveAmount)
{
	FreeCursor();
	CreatePlayer(*MyPlayer, HeroClass::Warrior);
	InitCursor();
	MyPlayer->setLevel(currlevel);
	Join(7, MakePlayer(7));
	Players[7].setLevel(currlevel);

	const int32_t localHitPoints = MyPlayer->_pHitPoints.raw();
	TCmdDamage message {};
	message.bCmd = CMD_PLRDAMAGE;
	message.dwDam = Swap32LE(static_cast<uint32_t>(Fixed26_6::fromInt(1).raw()));
	message.damageType = DamageType::Physical;

	message.bPlr = PlayerStorageCapacity;
	EXPECT_EQ(ParseCmd(7, reinterpret_cast<const TCmd *>(&message), sizeof(message)), sizeof(message));
	EXPECT_EQ(MyPlayer->_pHitPoints.raw(), localHitPoints);

	message.bPlr = MyPlayerId;
	message.dwDam = Swap32LE(192001);
	EXPECT_EQ(ParseCmd(7, reinterpret_cast<const TCmd *>(&message), sizeof(message)), sizeof(message));
	EXPECT_EQ(MyPlayer->_pHitPoints.raw(), localHitPoints);
}

TEST_F(MultiplayerSyncTest, RejectsPartialMonsterSyncRecords)
{
	Message message(sizeof(TSyncHeader) + 1);
	auto &header = *reinterpret_cast<TSyncHeader *>(message.data());
	header.bCmd = CMD_SYNCDATA;
	header.bLevel = 2;
	header.wLen = Swap16LE(1);
	Receive(7, message);
	EXPECT_TRUE(Canonical(Export()).contains(256));
	EXPECT_FALSE(Canonical(Export()).contains(2));
}

#if !defined(NONET) && !defined(DISABLE_TCP)
class SyncServer {
public:
	SyncServer(const net::buffer_t &info, const std::string &password)
	    : factory_(password.empty() ? std::make_unique<net::packet_factory>() : std::make_unique<net::packet_factory>(password))
	    , server_(context_, "127.0.0.1", 0, *factory_, info)
	    , address(server_.LocalhostSelf())
	    , thread_([this] { context_.run(); })
	{
	}

	~SyncServer()
	{
		asio::post(context_, [this] { server_.Close(); });
		thread_.join();
	}

private:
	asio::io_context context_;
	std::unique_ptr<net::packet_factory> factory_;
	net::tcp_server server_;

public:
	std::string address;

private:
	std::thread thread_;
};

class SyncClient : public net::tcp_client {
public:
	explicit SyncClient(bool host)
	    : host_(host)
	{
	}

protected:
	bool IsGameHost() override { return host_; }

private:
	bool host_;
};

TEST_F(PlayerSynchronizationTest, TcpEngineReceivesSevenRemotePlayersAtTheSameTurn)
{
	for (const bool encrypted : { false, true }) {
		SCOPED_TRACE(encrypted);
		SNetDestroy();
		for (Player &player : Players)
			player = {};
		gbActivePlayers = 1;
		MyPlayerId = 7;
		MyPlayer = &Players[7];
		InspectPlayer = MyPlayer;
		MyPlayer->plrlevel = 1;
		GameData wire = sgGameInitInfo;
		SwapGameDataLE(wire);
		const auto *bytes = reinterpret_cast<const unsigned char *>(&wire);
		const net::buffer_t info(bytes, bytes + sizeof(wire));
		std::string password = encrypted ? "sync-test" : "";
		SyncServer server(info, password);
		std::array<std::unique_ptr<SyncClient>, 7> clients;
		const auto add = [&](uint8_t slot) {
			auto client = std::make_unique<SyncClient>(slot == 0);
			client->setup_gameinfo(info);
			if (encrypted)
				client->setup_password(password);
			else
				client->clear_password();
			const int id = client->join(server.address);
			clients[slot] = std::move(client);
			return id;
		};
		for (uint8_t slot = 0; slot < 7; ++slot)
			ASSERT_EQ(add(slot), slot);
		ASSERT_TRUE(SNetInitializeProvider(SELCONN_TCP, &sgGameInitInfo));
		int id;
		ASSERT_TRUE(SNetJoinGame(server.address.data(), encrypted ? password.data() : nullptr, &id));
		ASSERT_EQ(id, 7);
		ASSERT_TRUE(SNetRegisterEventHandler(EVENT_TYPE_PLAYER_LEAVE_GAME, [](_SNETEVENT *event) {
			multi_player_left(static_cast<uint8_t>(event->playerid), leaveinfo_t::LEAVE_DROP);
		}));

		const auto until = [&](auto condition) {
			const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
			do {
				for (auto &client : clients) {
					if (client) {
						EXPECT_TRUE(client->poll().has_value());
					}
				}
				ProcessGameMessagePackets();
				if (condition())
					return true;
				std::this_thread::sleep_for(std::chrono::milliseconds(1));
			} while (std::chrono::steady_clock::now() < deadline);
			return false;
		};
		const auto sendInfo = [&](uint8_t slot) {
			const PlayerNetPack player = MakePlayer(slot);
			const auto data = std::as_bytes(std::span(&player, 1));
			for (size_t offset = 0; offset < data.size(); offset += 479) {
				const auto message = Fragment(CMD_ACK_PLRINFO, static_cast<uint16_t>(offset), data.subspan(offset, std::min(size_t { 479 }, data.size() - offset)));
				TPkt packet {};
				packet.hdr.wCheck = Swap16LE(0x6970); // Classic "pi" signature on the wire.
				const size_t size = sizeof(TPktHdr) + message.size();
				packet.hdr.wLen = Swap16LE(static_cast<uint16_t>(size));
				std::copy(message.begin(), message.end(), packet.body);
				EXPECT_TRUE(clients[slot]->SNetSendMessage(7, &packet, size));
			}
			return player;
		};
		for (uint8_t slot = 0; slot < 7; ++slot) {
			const auto player = sendInfo(slot);
			ASSERT_TRUE(until([&] { return Players[slot].plractive; }));
			EXPECT_EQ(Players[slot].plrlevel, player.plrlevel);
			EXPECT_EQ(Players[slot].position.tile, (Point { player.px, player.py }));
		}
		EXPECT_EQ(gbActivePlayers, 8);
		gbIsMultiplayer = false;
		nthread_start(false);
		gbIsMultiplayer = true;
		std::fill_n(player_state, PlayerStorageCapacity, 0);
		nthread_send_and_recv_turn(37, 0);
		int32_t turn = 37;
		for (auto &client : clients)
			ASSERT_TRUE(client->SNetSendTurn(reinterpret_cast<char *>(&turn), sizeof(turn)));
		ASSERT_TRUE(until([] { return nthread_recv_turns() && (player_state[7] & PS_TURN_ARRIVED) != 0; }));
		for (uint8_t slot = 0; slot < 8; ++slot) {
			ASSERT_EQ(gdwMsgLenTbl[slot], sizeof(turn));
			int32_t received;
			std::memcpy(&received, reinterpret_cast<const void *>(glpMsgTbl[slot]), sizeof(received));
			EXPECT_EQ(received, 37);
		}
		nthread_cleanup();
		gdwLargestMsgSize = 512;
		for (uint8_t slot = 4; slot < 7; ++slot) {
			ASSERT_TRUE(clients[slot]->SNetLeaveGame(leaveinfo_t::LEAVE_EXIT));
			clients[slot].reset();
			ASSERT_TRUE(until([&] { return !Players[slot].plractive; }));
			EXPECT_EQ(Players[slot]._pName[0], '\0');
			ASSERT_EQ(add(slot), slot);
			sendInfo(slot);
			ASSERT_TRUE(until([&] { return Players[slot].plractive; }));
		}
		EXPECT_EQ(gbActivePlayers, 8);
		SNetDestroy();
	}
}
#endif

} // namespace
} // namespace devilution
