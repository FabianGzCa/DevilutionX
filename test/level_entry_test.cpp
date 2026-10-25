#include <algorithm>
#include <array>
#include <cstdint>
#include <cstring>
#include <deque>
#include <iterator>
#include <random>
#include <vector>

#include <gtest/gtest.h>

#include "cursor.h"
#include "engine/assets.hpp"
#include "engine/load_file.hpp"
#include "engine/path.h"
#include "engine/random.hpp"
#include "game_mode.hpp"
#include "levels/dun_tile_data.hpp"
#include "levels/tile_properties.hpp"
#include "levels/town.h"
#include "levels/trigs.h"
#include "lighting.h"
#include "missiles.h"
#include "multi.h"
#include "nthread.h"
#include "objects.h"
#include "pack.h"
#include "player.h"
#include "players/level_entry.hpp"
#include "portal.h"
#include "storm/storm_net.hpp"
#include "tables/misdat.h"
#include "tables/playerdat.hpp"
#include "towners.h"
#include "utils/endian_swap.hpp"
#include "utils/fixed_point.hpp"

namespace devilution {
namespace {

constexpr uint8_t Bit(uint8_t id) { return static_cast<uint8_t>(1U << id); }

LevelEntryMessage Request(uint8_t id, Point position = { 40, 50 }, uint32_t ticket = 1, uint8_t level = 1, bool special = false)
{
	LevelEntryMessage request { };
	request.command = CMD_LEVELENTRY;
	request.phase = LevelEntryPhase::Request;
	request.level = level;
	request.setLevel = special ? 1 : 0;
	request.participants = Bit(id);
	request.tickets[id] = ticket;
	request.x[id] = position.x;
	request.y[id] = position.y;
	return request;
}

/** Eight distinct state machines and per-sender FIFO channels, not eight calls on one state. */
class EntryNetwork {
public:
	std::array<LevelEntryCoordinator, 8> nodes;
	std::array<LevelEntryPeers, 8> views;
	std::array<std::array<Point, 8>, 8> committed { };
	std::array<unsigned, 8> searches { };
	unsigned assignments = 0;
	unsigned commits = 0;
	unsigned retries = 0;
	bool space = true;
	int rejectOnce = -1;
	std::vector<LevelEntryMessage> decisions;

	EntryNetwork()
	{
		for (auto &view : views) {
			view.active = 0xFF;
			view.level.fill(1);
		}
	}

	void Broadcast(uint8_t sender, LevelEntryMessage message)
	{
		if (message.phase == LevelEntryPhase::Assign)
			++assignments;
		if (message.phase == LevelEntryPhase::Commit) {
			++commits;
			retries += message.outcome == LevelEntryOutcome::Retry;
			decisions.push_back(message);
		}
		for (uint8_t receiver = 0; receiver < 8; ++receiver) {
			if ((views[receiver].active & Bit(receiver)) != 0)
				channels_[sender * 8 + receiver].push_back(message);
		}
	}

	void Receive(uint8_t receiver, uint8_t sender, const LevelEntryMessage &message)
	{
		const auto update = [&](const LevelEntryMessage &batch, LevelEntryStage stage) {
			if (stage == LevelEntryStage::Prepare) {
				for (uint8_t i = 0; i < 8; ++i) {
					if ((batch.participants & Bit(i)) != 0) {
						views[receiver].ready |= Bit(i);
						views[receiver].level[i] = batch.level;
						views[receiver].setLevel[i] = batch.setLevel != 0;
					}
				}
			}
			if (stage == LevelEntryStage::Reserve && receiver == rejectOnce) {
				rejectOnce = -1;
				return false;
			}
			if (stage == LevelEntryStage::Commit) {
				for (uint8_t i = 0; i < 8; ++i) {
					if ((batch.participants & Bit(i)) != 0)
						committed[receiver][i] = { batch.x[i], batch.y[i] };
				}
			}
			return true;
		};
		if (auto reply = nodes[receiver].Receive(sender, receiver, views[receiver], message, update))
			Broadcast(receiver, *reply);
	}

	void Requests(uint8_t mask = 0xFF)
	{
		for (uint8_t id = 0; id < 8; ++id) {
			if ((mask & Bit(id)) == 0)
				continue;
			for (uint8_t receiver = 0; receiver < 8; ++receiver)
				Receive(receiver, id, Request(id));
		}
	}

	void Run(unsigned seed, bool stopAtFirstCommit = false)
	{
		std::mt19937 rng(seed);
		for (unsigned step = 0; step < 10000; ++step) {
			for (uint8_t node = 0; node < 8 && (!stopAtFirstCommit || commits == 0); ++node) {
				if ((views[node].active & Bit(node)) == 0)
					continue;
				const auto resolve = [&](LevelEntryMessage &message, std::span<const Point> rejected) {
					++searches[node];
					for (uint8_t id = 0; id < 8; ++id) {
						message.x[id] = 40 + id * 2;
						message.y[id] = 50;
					}
					return space;
				};
				if (auto message = nodes[node].Poll(node, views[node], resolve))
					Broadcast(node, *message);
			}
			std::vector<size_t> available;
			for (size_t channel = 0; channel < channels_.size(); ++channel) {
				if (!channels_[channel].empty())
					available.push_back(channel);
			}
			if (available.empty()) {
				bool pending = false;
				for (const auto &node : nodes) {
					for (uint8_t player = 0; player < 8; ++player)
						pending |= node.Pending(player);
				}
				if (!pending || (stopAtFirstCommit && commits != 0))
					return;
				continue;
			}
			const size_t channel = available[rng() % available.size()];
			const auto message = channels_[channel].front();
			channels_[channel].pop_front();
			Receive(channel % 8, channel / 8, message);
			if (stopAtFirstCommit && commits != 0 && std::all_of(channels_.begin(), channels_.end(), [](const auto &queue) { return queue.empty(); }))
				return;
		}
		ADD_FAILURE() << "Admission did not finish: assignments=" << assignments << " commits=" << commits;
	}

private:
	std::array<std::deque<LevelEntryMessage>, 64> channels_;
};

TEST(LevelEntryProtocolTest, EightClientsAgreeDespiteCrossSenderDeliveryOrder)
{
	for (unsigned seed = 1; seed <= 30; ++seed) {
		SCOPED_TRACE(seed);
		EntryNetwork network;
		network.Requests();
		network.Run(seed);
		EXPECT_EQ(network.commits, 1U);
		EXPECT_EQ(network.searches[0], 1U);
		for (uint8_t node = 0; node < 8; ++node) {
			SCOPED_TRACE(node);
			for (uint8_t id = 0; id < 8; ++id) {
				EXPECT_FALSE(network.nodes[node].Pending(id));
				EXPECT_EQ(network.committed[node][id], Point(40 + id * 2, 50));
			}
		}
	}
}

TEST(LevelEntryProtocolTest, ACommitWaitsForEveryDamageFenceOnEachVotingReplica)
{
	LevelEntryCoordinator node;
	LevelEntryPeers peers;
	peers.active = peers.ready = 255;
	peers.level.fill(1);
	unsigned commits = 0;
	const auto update = [&](const auto &, LevelEntryStage stage) {
		commits += stage == LevelEntryStage::Commit;
		return true;
	};
	const auto request = Request(7);
	node.Receive(7, 7, peers, request, update);
	auto message = request;
	message.phase = LevelEntryPhase::Assign;
	message.serial = 8;
	message.authority = 0;
	message.voters = message.members = 255;
	node.Receive(0, 7, peers, message, update);
	message.phase = LevelEntryPhase::Propose;
	message.x[7] = 42;
	ASSERT_TRUE(node.Receive(0, 7, peers, message, update));
	message.phase = LevelEntryPhase::Vote;
	for (uint8_t sender = 0; sender < 8; ++sender) {
		if (sender != 6)
			node.Receive(sender, 7, peers, message, update);
	}
	message.phase = LevelEntryPhase::Commit;
	node.Receive(0, 7, peers, message, update);
	EXPECT_EQ(commits, 0U);
	EXPECT_TRUE(node.Pending(7));
	message.phase = LevelEntryPhase::Vote;
	node.Receive(6, 7, peers, message, update);
	EXPECT_EQ(commits, 1U);
	EXPECT_FALSE(node.Pending(7));
}

TEST(LevelEntryProtocolTest, EightConsecutiveAdmissionsAgreeOnEveryClient)
{
	for (unsigned seed = 1; seed <= 20; ++seed) {
		SCOPED_TRACE(seed);
		EntryNetwork network;
		for (uint8_t id = 0; id < 8; ++id) {
			network.Requests(Bit(id));
			network.Run(seed * 8 + id);
			ASSERT_EQ(network.decisions.size(), id + 1U);
			EXPECT_EQ(network.decisions.back().participants, Bit(id));
			for (uint8_t node = 0; node < 8; ++node) {
				for (uint8_t admitted = 0; admitted <= id; ++admitted) {
					EXPECT_FALSE(network.nodes[node].Pending(admitted));
					EXPECT_EQ(network.committed[node][admitted], Point(40 + admitted * 2, 50));
				}
			}
		}
	}
}

TEST(LevelEntryProtocolTest, OrdinaryAndQuestLevelsWithTheSameNumberStaySeparate)
{
	EntryNetwork network;
	for (uint8_t node = 0; node < 8; ++node) {
		network.Receive(node, 4, Request(4));
		network.Receive(node, 5, Request(5, { 40, 50 }, 1, 1, true));
	}
	network.Run(412);
	ASSERT_EQ(network.decisions.size(), 2U);
	EXPECT_EQ(network.decisions[0].participants, Bit(4));
	EXPECT_EQ(network.decisions[0].setLevel, 0);
	EXPECT_EQ(network.decisions[1].participants, Bit(5));
	EXPECT_EQ(network.decisions[1].setLevel, 1);
	for (const auto &node : network.nodes) {
		EXPECT_FALSE(node.Pending(4));
		EXPECT_FALSE(node.Pending(5));
	}
}

TEST(LevelEntryProtocolTest, CoordinatorDelegatesToAResidentOnAnotherLevel)
{
	EntryNetwork network;
	for (auto &view : network.views) {
		view.level.fill(0);
		view.level[2] = 1;
		view.ready = Bit(2);
	}
	network.Requests(0xF0);
	network.Run(123);
	EXPECT_EQ(network.searches[2], 1U);
	EXPECT_EQ(network.searches[0], 0U);
	for (const auto &decision : network.decisions)
		EXPECT_EQ(decision.authority, 2);
	for (uint8_t node = 0; node < 8; ++node) {
		for (uint8_t id = 4; id < 8; ++id)
			EXPECT_EQ(network.committed[node][id], Point(40 + id * 2, 50));
	}
}

TEST(LevelEntryProtocolTest, OneOccupancyDisagreementRejectsTheWholeCohort)
{
	EntryNetwork network;
	network.rejectOnce = 6;
	network.Requests();
	network.Run(317);
	ASSERT_GE(network.decisions.size(), 2U);
	EXPECT_EQ(network.decisions.front().outcome, LevelEntryOutcome::Retry);
	EXPECT_EQ(network.retries, 1U);
	for (uint8_t node = 0; node < 8; ++node) {
		for (uint8_t id = 0; id < 8; ++id) {
			EXPECT_FALSE(network.nodes[node].Pending(id));
			EXPECT_EQ(network.committed[node][id], Point(40 + id * 2, 50));
		}
	}
}

TEST(LevelEntryProtocolTest, NoSpaceIsExplicitAndLeavesAllParticipantsUnplaced)
{
	EntryNetwork network;
	network.space = false;
	network.Requests();
	network.Run(17, true);
	ASSERT_FALSE(network.decisions.empty());
	EXPECT_EQ(network.decisions.front().outcome, LevelEntryOutcome::NoSpace);
	for (uint8_t node = 0; node < 8; ++node) {
		for (uint8_t id = 0; id < 8; ++id) {
			EXPECT_TRUE(network.nodes[node].Pending(id));
			EXPECT_TRUE(network.nodes[node].Blocked(id));
			EXPECT_EQ(network.committed[node][id], Point(0, 0));
		}
	}
	// A retry assignment must not clear the blocked state and repeat the UI notice.
	std::optional<LevelEntryMessage> retry;
	const auto unusedResolver = [](LevelEntryMessage &, std::span<const Point>) { ADD_FAILURE() << "Unexpected search"; return false; };
	for (unsigned tick = 0; tick < 25 && !retry; ++tick)
		retry = network.nodes[0].Poll(0, network.views[0], unusedResolver);
	ASSERT_TRUE(retry);
	ASSERT_EQ(retry->phase, LevelEntryPhase::Assign);
	for (uint8_t node = 0; node < 8; ++node) {
		network.Receive(node, 0, *retry);
		for (uint8_t id = 0; id < 8; ++id)
			EXPECT_TRUE(network.nodes[node].Blocked(id));
	}
	network.space = true;
	network.Run(19);
	for (uint8_t node = 0; node < 8; ++node) {
		for (uint8_t id = 0; id < 8; ++id)
			EXPECT_FALSE(network.nodes[node].Pending(id));
	}
}

TEST(LevelEntryProtocolTest, InvalidRequestCannotEnterTheQueue)
{
	LevelEntryCoordinator node;
	LevelEntryPeers peers;
	peers.active = 0xFF;
	const auto update = [](const LevelEntryMessage &, LevelEntryStage) { return true; };
	for (const unsigned kind : { 0U, 1U, 2U, 3U }) {
		auto request = Request(7);
		if (kind == 0) request.level = 255;
		if (kind == 1) request.setLevel = 2;
		if (kind == 2) request.x[7] = 255;
		if (kind == 3) request.tickets[7] = 0;
		node.Receive(7, 0, peers, request, update);
		EXPECT_FALSE(node.Pending(7));
	}
}

TEST(LevelEntryProtocolTest, CoordinatorDepartureCannotReuseAnOldTransaction)
{
	std::array<LevelEntryCoordinator, 8> nodes;
	LevelEntryPeers peers;
	peers.active = 0xFF;
	peers.level.fill(1);
	const auto update = [](const LevelEntryMessage &, LevelEntryStage) { return true; };
	const auto resolve = [](LevelEntryMessage &message, std::span<const Point>) {
		for (uint8_t i = 0; i < 8; ++i) {
			message.x[i] = 40 + i * 2;
			message.y[i] = 50;
		}
		return true;
	};
	for (uint8_t i = 0; i < 8; ++i) {
		for (uint8_t receiver = 0; receiver < 8; ++receiver)
			nodes[receiver].Receive(i, receiver, peers, Request(i), update);
	}
	const auto oldAssignment = nodes[0].Poll(0, peers, resolve);
	ASSERT_TRUE(oldAssignment);
	for (uint8_t i = 0; i < 8; ++i)
		nodes[i].Receive(0, i, peers, *oldAssignment, update);
	const auto oldProposal = nodes[0].Poll(0, peers, resolve);
	ASSERT_TRUE(oldProposal);
	peers.active = 0xFE;
	for (auto &node : nodes)
		node.Cancel(0, update, true);
	const auto replacement = nodes[1].Poll(1, peers, resolve);
	ASSERT_TRUE(replacement);
	EXPECT_NE(static_cast<uint32_t>(replacement->serial), static_cast<uint32_t>(oldAssignment->serial));
	EXPECT_EQ(replacement->serial & 7U, 1U);
	for (uint8_t i = 1; i < 8; ++i) {
		nodes[i].Receive(1, i, peers, *replacement, update);
		EXPECT_FALSE(nodes[i].Receive(0, i, peers, *oldProposal, update));
	}
	const auto proposal = nodes[1].Poll(1, peers, resolve);
	ASSERT_TRUE(proposal);
	for (uint8_t i = 1; i < 8; ++i) {
		const auto vote = nodes[i].Receive(1, i, peers, *proposal, update);
		ASSERT_TRUE(vote);
		for (uint8_t receiver = 1; receiver < 8; ++receiver)
			nodes[receiver].Receive(i, receiver, peers, *vote, update);
	}
	const auto commit = nodes[1].Poll(1, peers, resolve);
	ASSERT_TRUE(commit);
	for (uint8_t i = 1; i < 8; ++i) {
		nodes[i].Receive(1, i, peers, *commit, update);
		for (uint8_t entrant = 1; entrant < 8; ++entrant)
			EXPECT_FALSE(nodes[i].Pending(entrant));
	}
}

TEST(LevelEntryProtocolTest, MissingAuthorityHasABoundedCoordinatedAbort)
{
	LevelEntryCoordinator node;
	LevelEntryPeers peers;
	peers.active = 0xFF;
	peers.ready = Bit(2);
	peers.level.fill(0);
	peers.level[2] = 1;
	const auto update = [](const LevelEntryMessage &, LevelEntryStage) { return true; };
	const auto resolve = [](LevelEntryMessage &, std::span<const Point>) { ADD_FAILURE() << "Wrong authority"; return false; };
	node.Receive(7, 0, peers, Request(7), update);
	const auto assignment = node.Poll(0, peers, resolve);
	ASSERT_TRUE(assignment);
	ASSERT_EQ(assignment->authority, 2);
	node.Receive(0, 0, peers, *assignment, update);
	std::optional<LevelEntryMessage> abort;
	for (int tick = 0; tick < 205 && !abort; ++tick)
		abort = node.Poll(0, peers, resolve);
	ASSERT_TRUE(abort);
	EXPECT_EQ(abort->phase, LevelEntryPhase::Commit);
	EXPECT_EQ(abort->outcome, LevelEntryOutcome::Retry);
	node.Receive(0, 0, peers, *abort, update);
	EXPECT_FALSE(node.IsResolving(1, false));
	EXPECT_TRUE(node.Pending(7));
}

TEST(LevelEntryProtocolTest, DelayedOwnAssignmentCannotReviveADepartedSlot)
{
	LevelEntryCoordinator node;
	LevelEntryPeers peers;
	peers.active = 0xFF;
	peers.level.fill(1);
	const auto update = [](const LevelEntryMessage &, LevelEntryStage) { return true; };
	const auto resolve = [](LevelEntryMessage &, std::span<const Point>) { return true; };
	node.Receive(7, 0, peers, Request(7), update);
	const auto obsolete = node.Poll(0, peers, resolve);
	ASSERT_TRUE(obsolete);
	// The self-delivery was still queued when slot 7 disconnected.
	node.Cancel(7, update, true);
	node.Receive(0, 0, peers, *obsolete, update);
	EXPECT_FALSE(node.Pending(7));
	node.Receive(7, 0, peers, Request(7, { 60, 70 }), update);
	const auto replacement = node.Poll(0, peers, resolve);
	ASSERT_TRUE(replacement);
	EXPECT_NE(static_cast<uint32_t>(replacement->serial), static_cast<uint32_t>(obsolete->serial));
	EXPECT_EQ(replacement->x[7], 60);
	EXPECT_EQ(replacement->y[7], 70);
}

TEST(LevelEntryProtocolTest, ANewRequestCannotBeOverwrittenByAnOldCommit)
{
	LevelEntryCoordinator node;
	LevelEntryPeers peers;
	peers.active = 0xFF;
	peers.level.fill(1);
	const auto update = [](const LevelEntryMessage &, LevelEntryStage) { return true; };
	const auto resolve = [](LevelEntryMessage &, std::span<const Point>) { return true; };
	node.Receive(7, 0, peers, Request(7), update);
	const auto assignment = node.Poll(0, peers, resolve);
	ASSERT_TRUE(assignment);
	node.Receive(0, 0, peers, *assignment, update);
	node.Receive(7, 0, peers, Request(7, { 60, 70 }, 2), update);
	auto oldCommit = *assignment;
	oldCommit.phase = LevelEntryPhase::Commit;
	node.Receive(0, 0, peers, oldCommit, update);
	const auto state = node.StateFor(7);
	ASSERT_TRUE(state);
	EXPECT_EQ(static_cast<uint32_t>(state->tickets[7]), 2U);
	EXPECT_EQ(state->x[7], 60);
	EXPECT_TRUE(node.Pending(7));
}

class LevelEntryPlacementTest : public ::testing::Test {
protected:
	static void SetUpTestSuite()
	{
		LoadCoreArchives();
		LoadPlayerDataFiles();
		LoadItemData();
		LoadSpellData();
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
		gbRunGame = false;
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
		InitPortals();
		InitNoTriggers();
		InitLighting();
		Missiles.clear();
		ResetLevelEntries();
		std::memset(dPlayer, 0, sizeof(dPlayer));
		std::memset(dMonster, 0, sizeof(dMonster));
		std::memset(dObject, 0, sizeof(dObject));
		std::memset(dFlags, 0, sizeof(dFlags));
		std::memset(dPiece, 0, sizeof(dPiece));
		SOLData[0] = TileProperties::Solid;
		SOLData[1] = TileProperties::None;
		for (Player &player : Players) {
			player.plractive = true;
			player.setLevel(1);
			player._pLvlChanging = true;
			player._pHitPoints = Fixed26_6::fromInt(100);
			player._pMaxHP = player._pHitPoints;
			player.position.tile = { 40, 50 };
		}
	}
	void TearDown() override
	{
		Missiles.clear();
		ResetLevelEntries();
		SNetDestroy();
		Players.clear();
		MyPlayer = nullptr;
		InspectPlayer = nullptr;
		gbIsMultiplayer = false;
		leveltype = DTYPE_TOWN;
		setlevel = false;
	}
	static void Room(int left = 20, int top = 20, int right = 80, int bottom = 80)
	{
		for (int y = top; y <= bottom; ++y) {
			for (int x = left; x <= right; ++x)
				dPiece[x][y] = 1;
		}
	}
	static LevelEntryMessage Batch(Point origin = { 40, 50 })
	{
		LevelEntryMessage batch { };
		batch.participants = 0xFF;
		batch.level = setlevel ? static_cast<uint8_t>(setlvlnum) : currlevel;
		batch.setLevel = setlevel ? 1 : 0;
		for (uint8_t id = 0; id < 8; ++id) {
			batch.x[id] = origin.x;
			batch.y[id] = origin.y;
		}
		return batch;
	}
};

TEST_F(LevelEntryPlacementTest, EightEntrantsHaveDistinctConnectedPositionsAndAnExit)
{
	Room();
	const uint32_t seed = GetLCGEngineState();
	auto batch = Batch();
	ASSERT_TRUE(FindLevelEntryPositions(batch));
	EXPECT_EQ(GetLCGEngineState(), seed);
	for (uint8_t id = 0; id < 8; ++id) {
		const Point position { batch.x[id], batch.y[id] };
		for (uint8_t other = 0; other < id; ++other)
			EXPECT_NE(position, Point(batch.x[other], batch.y[other]));
		Players[id].position.tile = position;
		Players[id].position.future = position;
		Players[id].occupyTile(position, false);
	}
	for (uint8_t id = 0; id < 8; ++id) {
		const Point position = Players[id].position.tile;
		EXPECT_TRUE(std::any_of(std::begin(PathDirs), std::end(PathDirs), [&](Displacement offset) {
			const Point exit = position + offset;
			return !IsTileOccupied(exit) && CanStep(position, exit);
		}));
	}
}

TEST_F(LevelEntryPlacementTest, SearchStaysConnectedInsteadOfCrossingASealedWall)
{
	Room(44, 45, 55, 55);
	auto batch = Batch();
	EXPECT_FALSE(FindLevelEntryPositions(batch));
	EXPECT_EQ(dPlayer[0][0], 0);
}

TEST_F(LevelEntryPlacementTest, NarrowCorridorLeavesGapsForAllEightPlayers)
{
	Room(40, 25, 40, 70);
	auto batch = Batch({ 40, 40 });
	ASSERT_TRUE(FindLevelEntryPositions(batch));
	for (uint8_t i = 0; i < 8; ++i) {
		EXPECT_EQ(batch.x[i], 40);
		for (uint8_t j = 0; j < i; ++j)
			EXPECT_NE(batch.y[i], batch.y[j]);
	}
}

TEST_F(LevelEntryPlacementTest, FullOrBorderOnlyMapsNeverUseZeroZeroAsAFallback)
{
	for (const Point origin : { Point(0, 0), Point(111, 111), Point(40, 50) }) {
		auto batch = Batch(origin);
		EXPECT_FALSE(FindLevelEntryPositions(batch));
		for (const auto &row : dPlayer) {
			for (const auto occupant : row)
				EXPECT_EQ(occupant, 0);
		}
	}
}

TEST_F(LevelEntryPlacementTest, TownPortalAndQuestEntrancesAvoidTriggersAndOtherPlayers)
{
	Room();
	for (const bool special : { false, true }) {
		setlevel = special;
		setlvlnum = SL_SKELKING;
		for (Player &player : Players) {
			if (special)
				player.setLevel(SL_SKELKING);
			else
				player.setLevel(1);
		}
		trigs[0].position = { 40, 50 };
		numtrigs = 1;
		auto batch = Batch();
		ASSERT_TRUE(FindLevelEntryPositions(batch));
		for (uint8_t id = 0; id < 8; ++id)
			EXPECT_GE(Point(batch.x[id], batch.y[id]).WalkingDistance(trigs[0].position), 2);
	}
}

TEST_F(LevelEntryPlacementTest, TeleportDestinationsDoNotDependOnMissileInsertionOrder)
{
	std::array<Point, 8> expected { };
	for (const bool reverse : { false, true }) {
		Room();
		Missiles.clear();
		std::memset(dPlayer, 0, sizeof(dPlayer));
		for (uint8_t i = 0; i < 8; ++i) {
			Players[i].position.tile = { static_cast<WorldTileCoord>(25 + i * 3), 25 };
			Players[i].position.future = Players[i].position.tile;
			Players[i].occupyTile(Players[i].position.tile, false);
		}
		for (uint8_t i = 0; i < 8; ++i) {
			const uint8_t caster = reverse ? 7 - i : i;
			ASSERT_NE(AddMissile(Players[caster].position.tile, { 50, 50 }, Direction::South, MissileID::Teleport, TARGET_MONSTERS, Players[caster], 0, 0), nullptr);
		}
		ProcessMissiles();
		for (uint8_t i = 0; i < 8; ++i) {
			if (!reverse)
				expected[i] = Players[i].position.tile;
			else
				EXPECT_EQ(Players[i].position.tile, expected[i]);
			for (uint8_t j = 0; j < i; ++j)
				EXPECT_NE(Players[i].position.tile, Players[j].position.tile);
		}
	}
}

TEST_F(LevelEntryPlacementTest, TeleportCannotTakeAnotherPlayersOnlyExit)
{
	Room(40, 50, 60, 50);
	for (uint8_t id = 0; id < 8; ++id) {
		Players[id].plractive = id < 2;
		Players[id]._pLvlChanging = false;
	}
	MyPlayer->position.tile = { 60, 50 };
	Players[1].position.tile = { 40, 50 };
	for (uint8_t id = 0; id < 2; ++id) {
		Players[id].position.future = Players[id].position.tile;
		Players[id].occupyTile(Players[id].position.tile, false);
	}
	ASSERT_NE(AddMissile(MyPlayer->position.tile, { 41, 50 }, Direction::South, MissileID::Teleport, TARGET_MONSTERS, *MyPlayer, 0, 0), nullptr);
	ProcessMissiles();
	EXPECT_NE(MyPlayer->position.tile, Point(41, 50));
	EXPECT_NE(MyPlayer->position.tile, Point(60, 50));
	EXPECT_TRUE(PosOkPlayer(Players[1], { 41, 50 }));
}

TEST_F(LevelEntryPlacementTest, SaturatedTeleportDestinationsLeaveTheCasterInPlace)
{
	Room(40, 50, 41, 50);
	for (uint8_t id = 0; id < 8; ++id)
		Players[id].plractive = id < 2;
	Players[1].position.tile = { 41, 50 };
	for (uint8_t id = 0; id < 2; ++id)
		Players[id].occupyTile(Players[id].position.tile, false);
	AddMissile(MyPlayer->position.tile, { 41, 50 }, Direction::South, MissileID::Teleport, TARGET_MONSTERS, *MyPlayer, 0, 0);
	ProcessMissiles();
	EXPECT_EQ(MyPlayer->position.tile, Point(40, 50));
	EXPECT_EQ(dPlayer[40][50], 1);
	EXPECT_EQ(dPlayer[41][50], 2);
}

TEST_F(LevelEntryPlacementTest, RejectedCoordinatesAreNotProposedAgain)
{
	Room();
	auto first = Batch();
	ASSERT_TRUE(FindLevelEntryPositions(first));
	std::array<Point, 8> rejected;
	for (uint8_t i = 0; i < 8; ++i)
		rejected[i] = { first.x[i], first.y[i] };
	auto retry = Batch();
	ASSERT_TRUE(FindLevelEntryPositions(retry, rejected));
	for (uint8_t i = 0; i < 8; ++i)
		EXPECT_EQ(std::find(rejected.begin(), rejected.end(), Point(retry.x[i], retry.y[i])), rejected.end());
}

TEST_F(LevelEntryPlacementTest, RealNetworkCommandsCompleteLocalAdmissionOnlyAfterCommit)
{
	Room();
	for (uint8_t i = 1; i < 8; ++i)
		Players[i].plractive = false;
	std::strcpy(MyPlayer->_pName, "Entrant");
	MyPlayer->setCharacterLevel(1);
	ASSERT_TRUE(SNetInitializeProvider(SELCONN_LOOPBACK, &sgGameInitInfo));
	GameData wire = sgGameInitInfo;
	SwapGameDataLE(wire);
	int playerId;
	ASSERT_TRUE(SNetCreateGame("entry-test", nullptr, reinterpret_cast<char *>(&wire), sizeof(wire), &playerId));
	gdwLargestMsgSize = 512;
	gdwNormalMsgSize = 512;
	BeginLevelEntry();
	for (int tick = 0; tick < 15 && MyPlayer->_pLvlChanging; ++tick) {
		ProcessGameMessagePackets();
		ProcessLevelEntries();
	}
	ProcessGameMessagePackets();
	EXPECT_FALSE(IsLevelEntryPending(0));
	EXPECT_FALSE(MyPlayer->_pLvlChanging);
	EXPECT_FALSE(ShouldWaitForLevelEntry());
	EXPECT_FALSE(MyPlayer->_pInvincible);
	EXPECT_EQ(dPlayer[MyPlayer->position.tile.x][MyPlayer->position.tile.y], 1);
	EXPECT_EQ(ViewPosition, MyPlayer->position.tile);
	EXPECT_TRUE(VisionActive[0]);
	const Point committed = MyPlayer->position.tile;
	// A full info packet from another channel may finish after the commit.
	*MyPlayer = { };
	std::strcpy(MyPlayer->_pName, "Late player info");
	MyPlayer->position.tile = { 10, 10 };
	MyPlayer->setLevel(5);
	ASSERT_TRUE(RestoreLevelEntryPlayerInfo(*MyPlayer));
	EXPECT_EQ(MyPlayer->position.tile, committed);
	EXPECT_EQ(MyPlayer->plrlevel, 1);
	EXPECT_TRUE(MyPlayer->plractive);
	EXPECT_FALSE(MyPlayer->_pLvlChanging);
}

TEST_F(LevelEntryPlacementTest, BlockedEngineAdmissionRetriesAfterSpaceBecomesAvailable)
{
	for (uint8_t i = 1; i < 8; ++i)
		Players[i].plractive = false;
	std::strcpy(MyPlayer->_pName, "Waiting entrant");
	MyPlayer->setCharacterLevel(1);
	ASSERT_TRUE(SNetInitializeProvider(SELCONN_LOOPBACK, &sgGameInitInfo));
	GameData wire = sgGameInitInfo;
	SwapGameDataLE(wire);
	int playerId;
	ASSERT_TRUE(SNetCreateGame("blocked-entry-test", nullptr, reinterpret_cast<char *>(&wire), sizeof(wire), &playerId));
	gdwLargestMsgSize = 512;
	gdwNormalMsgSize = 512;
	BeginLevelEntry();
	for (int tick = 0; tick < 15 && !IsLevelEntryBlocked(0); ++tick) {
		ProcessGameMessagePackets();
		ProcessLevelEntries();
	}
	ASSERT_TRUE(IsLevelEntryBlocked(0));
	EXPECT_TRUE(MyPlayer->_pLvlChanging);
	EXPECT_EQ(dPlayer[0][0], 0);
	Room();
	for (int tick = 0; tick < 50 && MyPlayer->_pLvlChanging; ++tick) {
		ProcessGameMessagePackets();
		ProcessLevelEntries();
	}
	ProcessGameMessagePackets();
	EXPECT_FALSE(MyPlayer->_pLvlChanging);
	EXPECT_FALSE(IsLevelEntryBlocked(0));
	EXPECT_NE(MyPlayer->position.tile, Point(0, 0));
}

TEST_F(LevelEntryPlacementTest, AProposalCannotRemoveAResidentsOnlyExit)
{
	Room(40, 50, 60, 50);
	for (uint8_t id = 1; id < 7; ++id)
		Players[id].plractive = false;
	std::strcpy(MyPlayer->_pName, "Entrant");
	MyPlayer->setCharacterLevel(1);
	MyPlayer->position.tile = { 60, 50 };
	Players[7]._pLvlChanging = false;
	Players[7].position.tile = { 40, 50 };
	Players[7].position.future = Players[7].position.tile;
	Players[7].occupyTile(Players[7].position.tile, false);
	ASSERT_TRUE(SNetInitializeProvider(SELCONN_LOOPBACK, &sgGameInitInfo));
	GameData wire = sgGameInitInfo;
	SwapGameDataLE(wire);
	int playerId;
	ASSERT_TRUE(SNetCreateGame("exit-test", nullptr, reinterpret_cast<char *>(&wire), sizeof(wire), &playerId));
	gdwLargestMsgSize = 512;
	gdwNormalMsgSize = 512;
	const auto receive = [](LevelEntryMessage message) {
		SwapLevelEntryMessageLE(message);
		ReceiveLevelEntry(0, message);
	};
	auto message = Request(0, { 60, 50 });
	receive(message);
	message.phase = LevelEntryPhase::Assign;
	message.serial = 8;
	message.authority = 0;
	message.voters = Bit(0) | Bit(7);
	receive(message);
	message.phase = LevelEntryPhase::Propose;
	message.x[0] = 41;
	message.y[0] = 50;
	receive(message);
	// The entrant has an exit at (42,50), but would trap resident 7.
	EXPECT_EQ(dPlayer[41][50], 0);
	EXPECT_EQ(dPlayer[40][50], 8);
	EXPECT_TRUE(IsLevelEntryPending(0));
}

TEST_F(LevelEntryPlacementTest, FragmentedPlayerInfoPreservesPendingAndCommittedAdmission)
{
	if (!HaveMainData())
		LoadGameArchives();
	if (!HaveMainData())
		GTEST_SKIP() << "DIABDAT.MPQ not found - skipping player info assembly";
	Room();
	for (uint8_t id = 1; id < 8; ++id)
		Players[id].plractive = false;
	MyPlayer->_pLvlChanging = false;
	MyPlayer->occupyTile(MyPlayer->position.tile, false);
	FreeCursor();
	CreatePlayer(Players[7], HeroClass::Warrior);
	InitCursor();
	std::strcpy(Players[7]._pName, "Late info");
	Players[7].setLevel(1);
	Players[7].position.tile = { 40, 50 };
	Players[7].position.future = Players[7].position.tile;
	PlayerNetPack packed { };
	PackNetPlayer(packed, Players[7]);
	Players[7] = { };
	ASSERT_TRUE(SNetInitializeProvider(SELCONN_LOOPBACK, &sgGameInitInfo));
	GameData wire = sgGameInitInfo;
	SwapGameDataLE(wire);
	int playerId;
	ASSERT_TRUE(SNetCreateGame("info-test", nullptr, reinterpret_cast<char *>(&wire), sizeof(wire), &playerId));
	gdwLargestMsgSize = 512;
	gdwNormalMsgSize = 512;
	delta_init();
	const auto receive = [](uint8_t sender, LevelEntryMessage message) {
		SwapLevelEntryMessageLE(message);
		ReceiveLevelEntry(sender, message);
	};
	const auto receiveInfo = [&] {
		for (size_t offset = 0; offset < sizeof(packed); offset += 193) {
			const size_t bytes = std::min(size_t(193), sizeof(packed) - offset);
			std::vector<std::byte> fragment(sizeof(TCmdPlrInfoHdr) + bytes);
			const TCmdPlrInfoHdr header { CMD_ACK_PLRINFO, Swap16LE(static_cast<uint16_t>(offset)), Swap16LE(static_cast<uint16_t>(bytes)) };
			std::memcpy(fragment.data(), &header, sizeof(header));
			std::memcpy(fragment.data() + sizeof(header), reinterpret_cast<const std::byte *>(&packed) + offset, bytes);
			recv_plrinfo(Players[7], *reinterpret_cast<const TCmdPlrInfoHdr *>(fragment.data()), true);
		}
	};
	auto message = Request(7);
	receive(7, message);
	receiveInfo();
	ASSERT_TRUE(IsLevelEntryPending(7));
	EXPECT_TRUE(Players[7]._pLvlChanging);
	EXPECT_EQ(Players[7]._pmode, PM_NEWLVL);
	EXPECT_EQ(dPlayer[40][50], 1);
	message.phase = LevelEntryPhase::Assign;
	message.serial = 8;
	message.authority = 0;
	message.voters = Bit(0) | Bit(7);
	message.members = message.voters;
	receive(0, message);
	message.phase = LevelEntryPhase::Propose;
	message.x[7] = 45;
	receive(0, message);
	message.phase = LevelEntryPhase::Vote;
	receive(0, message);
	receive(7, message);
	message.phase = LevelEntryPhase::Commit;
	receive(0, message);
	ASSERT_FALSE(IsLevelEntryPending(7));
	ASSERT_TRUE(VisionActive[7]);
	// Information from a different sender can complete after this decision.
	Players[7]._pName[0] = '\0';
	receiveInfo();
	EXPECT_EQ(Players[7].position.tile, Point(45, 50));
	EXPECT_FALSE(Players[7]._pLvlChanging);
	EXPECT_EQ(dPlayer[40][50], 1);
	EXPECT_EQ(dPlayer[45][50], 8);
	EXPECT_TRUE(VisionActive[7]);
	FreeCursor();
	delta_init();
}

TEST_F(LevelEntryPlacementTest, ObjectsMonstersAndResidentPlayersAreNeverOverwritten)
{
	Room();
	Objects[0] = { };
	Objects[0].position = { 41, 50 };
	Objects[0]._oSolidFlag = true;
	dObject[41][50] = 1;
	dMonster[40][51] = 1;
	Players[7]._pLvlChanging = false;
	Players[7].position.tile = { 39, 50 };
	Players[7].position.future = Players[7].position.tile;
	Players[7].occupyTile(Players[7].position.tile, false);
	auto batch = Batch();
	batch.participants = 0x7F;
	ASSERT_TRUE(FindLevelEntryPositions(batch));
	for (uint8_t i = 0; i < 7; ++i) {
		EXPECT_NE(Point(batch.x[i], batch.y[i]), Point(41, 50));
		EXPECT_NE(Point(batch.x[i], batch.y[i]), Point(40, 51));
		EXPECT_NE(Point(batch.x[i], batch.y[i]), Point(39, 50));
	}
	EXPECT_EQ(dPlayer[39][50], 8);
	EXPECT_EQ(dMonster[40][51], 1);
	EXPECT_EQ(dObject[41][50], 1);
}

TEST_F(LevelEntryPlacementTest, ActualDungeonStairsKeepEightEntrantsOnReachableFloor)
{
	if (!HaveMainData())
		LoadGameArchives();
	if (!HaveMainData())
		GTEST_SKIP() << "MPQ assets (spawn.mpq or DIABDAT.MPQ) not found - skipping generated maps";
	InitQuests();
	MyPlayer->pOriginalCathedral = true;
	for (const int level : { 1, 5, 9, 13 }) {
		SCOPED_TRACE(level);
		currlevel = level;
		leveltype = GetLevelType(level);
		const char *tiles = level == 1 ? "levels\\l1data\\l1.til" : level == 5 ? "levels\\l2data\\l2.til"
		    : level == 9                                                       ? "levels\\l3data\\l3.til"
		                                                                       : "levels\\l4data\\l4.til";
		pMegaTiles = LoadFileInMem<MegaTile>(tiles);
		ASSERT_TRUE(LoadLevelSOLData());
		for (const lvl_entry entry : { ENTRY_MAIN, ENTRY_PREV, ENTRY_TWARPDN }) {
			if (level == 1 && entry == ENTRY_TWARPDN)
				continue;
			SCOPED_TRACE(entry);
			for (Player &player : Players)
				player.setLevel(level);
			CreateDungeon(2588, entry);
			switch (leveltype) {
			case DTYPE_CATHEDRAL: InitL1Triggers(); break;
			case DTYPE_CATACOMBS: InitL2Triggers(); break;
			case DTYPE_CAVES: InitL3Triggers(); break;
			case DTYPE_HELL: InitL4Triggers(); break;
			default: break;
			}
			auto batch = Batch(ViewPosition);
			ASSERT_TRUE(FindLevelEntryPositions(batch)) << ViewPosition;
			for (uint8_t i = 0; i < 8; ++i) {
				const Point position { batch.x[i], batch.y[i] };
				EXPECT_TRUE(IsTileWalkable(position));
				for (int trigger = 0; trigger < numtrigs; ++trigger)
					EXPECT_GE(position.WalkingDistance(trigs[trigger].position), 2);
			}
		}
	}
	pMegaTiles.reset();
}

TEST_F(LevelEntryPlacementTest, ActualQuestMapEntrancesKeepEightPlayersOffReturnTriggers)
{
	if (!HaveMainData())
		LoadGameArchives();
	if (!HaveMainData())
		GTEST_SKIP() << "DIABDAT.MPQ not found - skipping quest maps";
	setlevel = true;
	for (const _setlevels quest : { SL_SKELKING, SL_BONECHAMB, SL_POISONWATER }) {
		SCOPED_TRACE(quest);
		setlvlnum = quest;
		leveltype = quest == SL_SKELKING ? DTYPE_CATHEDRAL : quest == SL_BONECHAMB ? DTYPE_CATACOMBS
		                                                                           : DTYPE_CAVES;
		setlvltype = leveltype;
		for (Player &player : Players)
			player.setLevel(quest);
		const char *tiles = quest == SL_SKELKING ? "levels\\l1data\\l1.til" : quest == SL_BONECHAMB ? "levels\\l2data\\l2.til"
		                                                                                            : "levels\\l3data\\l3.til";
		pMegaTiles = LoadFileInMem<MegaTile>(tiles);
		ASSERT_TRUE(LoadLevelSOLData());
		// Load the shipped geometry and collision data without spawning quest actors.
		const char *map = quest == SL_SKELKING ? "levels\\l1data\\sklkng2.dun" : quest == SL_BONECHAMB ? "levels\\l2data\\bonecha1.dun"
		                                                                                               : "levels\\l3data\\foulwatr.dun";
		const int floor = quest == SL_SKELKING ? 13 : quest == SL_BONECHAMB ? 3
		                                                                    : 7;
		const int dirt = quest == SL_SKELKING ? 22 : quest == SL_BONECHAMB ? 12
		                                                                   : 8;
		std::memset(dungeon, dirt, sizeof(dungeon));
		auto data = LoadFileInMem<uint16_t>(map);
		PlaceDunTiles(data.get(), { 0, 0 }, floor);
		DRLG_LPass3(dirt - 1);
		ViewPosition = quest == SL_SKELKING ? Point(83, 44) : quest == SL_BONECHAMB ? Point(70, 40)
		                                                                            : Point(31, 83);
		if (quest == SL_SKELKING)
			InitSKingTriggers();
		else if (quest == SL_BONECHAMB)
			InitSChambTriggers();
		else
			InitPWaterTriggers();
		auto batch = Batch(ViewPosition);
		ASSERT_TRUE(FindLevelEntryPositions(batch));
		for (uint8_t id = 0; id < 8; ++id) {
			const Point position { batch.x[id], batch.y[id] };
			EXPECT_TRUE(IsTileWalkable(position));
			for (int trigger = 0; trigger < numtrigs; ++trigger)
				EXPECT_GE(position.WalkingDistance(trigs[trigger].position), 2);
		}
	}
	pMegaTiles.reset();
}

TEST_F(LevelEntryPlacementTest, ActualTownPortalArrivalsAvoidAllEightPortalTiles)
{
	if (!HaveMainData())
		LoadGameArchives();
	if (!HaveMainData())
		GTEST_SKIP() << "MPQ assets (spawn.mpq or DIABDAT.MPQ) not found - skipping town map";
	currlevel = 0;
	leveltype = DTYPE_TOWN;
	for (Player &player : Players)
		player.setLevel(0);
	InitQuests();
	pMegaTiles = LoadFileInMem<MegaTile>("levels\\towndata\\town.til");
	ASSERT_TRUE(LoadLevelSOLData());
	CreateTown(ENTRY_MAIN);
	InitTownTriggers();
	InitTowners();
	for (int owner = 0; owner < 8; ++owner)
		SetPortalStats(owner, true, { 40, 50 }, 1, DTYPE_CATHEDRAL, false);
	SyncPortals();
	for (uint8_t owner = 0; owner < 8; ++owner) {
		SCOPED_TRACE(owner);
		SetCurrentPortal(owner);
		GetPortalLvlPos();
		auto batch = Batch(ViewPosition);
		ASSERT_TRUE(FindLevelEntryPositions(batch));
		for (uint8_t i = 0; i < 8; ++i) {
			const Point position { batch.x[i], batch.y[i] };
			EXPECT_FALSE(IsTileOccupied(position));
			for (const Missile &portal : Missiles) {
				EXPECT_NE(position, portal.position.tile);
				EXPECT_NE(position, portal.position.tile + Displacement(1, 1));
			}
		}
	}
	FreeTownerGFX();
	Towners.clear();
	pMegaTiles.reset();
}

TEST_F(LevelEntryPlacementTest, LoadedExtendedPlayersWaitForACommonCommit)
{
	Room();
	SyncInitPlrPos(*MyPlayer);
	EXPECT_TRUE(MyPlayer->_pLvlChanging);
	EXPECT_TRUE(MyPlayer->_pInvincible);
	EXPECT_EQ(dPlayer[40][50], 0);
	EXPECT_TRUE(ShouldWaitForLevelEntry());
}

TEST_F(LevelEntryPlacementTest, LegacyPlacementKeepsItsOriginalOffsetOrder)
{
	Room();
	sgGameInitInfo.rules = ClassicSessionRules;
	const std::array<Point, 4> expected = { { { 40, 50 }, { 41, 50 }, { 40, 51 }, { 41, 51 } } };
	for (uint8_t id = 0; id < 4; ++id) {
		SyncInitPlrPos(Players[id]);
		EXPECT_EQ(Players[id].position.tile, expected[id]);
	}
}

} // namespace
} // namespace devilution
