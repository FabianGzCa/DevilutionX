#include <algorithm>
#include <array>
#include <cstdint>
#include <deque>
#include <random>
#include <vector>

#include <gtest/gtest.h>

#include "items/item_transfer.hpp"

namespace devilution {
namespace {

class ItemTransferTest : public ::testing::Test {
protected:
	struct Envelope {
		uint8_t sender;
		ItemTransferMessage message;
	};
	std::array<ItemTransferCoordinator, 8> peers;
	std::array<std::deque<Envelope>, 8> inbox;
	std::array<unsigned, 8> committed {};
	std::array<unsigned, 8> cancelled {};
	std::array<unsigned, 8> reservations {};
	std::array<unsigned, 8> floors {};
	std::array<unsigned, 8> inventories {};
	std::array<bool, 8> allowed {};
	std::array<bool, 8> terminated {};
	bool duplicate = false;
	uint8_t active = 255;

	void SetUp() override
	{
		allowed.fill(true);
		inventories.fill(1);
		for (uint8_t id = 0; id < peers.size(); ++id) {
			peers[id].Reset(id, {
			                        .reserve = [&, id](const ItemTransferMessage &message) {
				    const bool pickup = message.kind == ItemTransferKind::ManualPickup || message.kind == ItemTransferKind::AutoPickup;
				    if (!allowed[id] || (message.kind != ItemTransferKind::MonsterReward && (pickup ? floors[id] == 0 : floors[id] + reservations[id] >= MAXITEMS)))
					    return false;
				    ++reservations[id];
				    return true; },
			                        .commit = [&, id](const ItemTransferMessage &message) {
				    ++committed[id];
				    const bool pickup = message.kind == ItemTransferKind::ManualPickup || message.kind == ItemTransferKind::AutoPickup;
				    if (message.kind != ItemTransferKind::MonsterReward) {
					    if (pickup)
						    --floors[id];
					    else
						    ++floors[id];
				    }
				    if (message.actor == id && IsPrivateItemTransfer(message.kind)) {
					    if (pickup)
						    ++inventories[id];
					    else
						    --inventories[id];
				    }
				    return true; },
			                        .release = [&, id](const ItemTransferMessage &) { --reservations[id]; },
			                        .send = [&, id](const ItemTransferMessage &message) { Broadcast(id, message); },
			                        .terminate = [&, id]() { terminated[id] = true; },
			                        .complete = [&, id](const ItemTransferMessage &, bool success) { cancelled[id] += !success; },
			                    });
		}
	}

	void Broadcast(uint8_t sender, const ItemTransferMessage &message)
	{
		for (uint8_t id = 0; id < peers.size(); ++id) {
			if ((active & (1U << id)) == 0)
				continue;
			inbox[id].push_back({ sender, message });
			if (duplicate)
				inbox[id].push_back({ sender, message });
		}
	}

	void Request(uint8_t actor, uint32_t request)
	{
		ItemTransferMessage message;
		message.kind = ItemTransferKind::HeldDrop;
		message.actor = actor;
		message.request = request;
		message.epoch = peers[actor].Epoch(actor);
		message.level = 1;
		Broadcast(actor, message);
	}

	bool Step(uint8_t id)
	{
		if (inbox[id].empty())
			return false;
		const Envelope envelope = inbox[id].front();
		inbox[id].pop_front();
		peers[id].Receive(envelope.sender, envelope.message, active);
		return true;
	}

	void DrainExcept(uint8_t excluded)
	{
		for (unsigned round = 0; round < 100000; ++round) {
			bool progress = false;
			for (uint8_t id = 0; id < 8; ++id) {
				if (id != excluded)
					progress |= Step(id);
			}
			if (!progress)
				return;
		}
		FAIL() << "Item transfers did not drain around the excluded peer";
	}

	void Drain(unsigned seed = 0)
	{
		std::mt19937 random(seed);
		for (unsigned step = 0; step < 100000; ++step) {
			std::vector<uint8_t> ready;
			for (uint8_t id = 0; id < peers.size(); ++id) {
				if (!inbox[id].empty())
					ready.push_back(id);
			}
			if (ready.empty())
				return;
			const uint8_t id = ready[random() % ready.size()];
			const Envelope envelope = inbox[id].front();
			inbox[id].pop_front();
			peers[id].Receive(envelope.sender, envelope.message, active);
		}
		FAIL() << "Item transfer protocol did not drain";
	}
};

TEST_F(ItemTransferTest, EveryActorCommitsExactlyOnceAcrossEightReplicas)
{
	for (uint8_t actor = 0; actor < 8; ++actor)
		Request(actor, 1);
	Drain();
	for (uint8_t id = 0; id < 8; ++id) {
		EXPECT_EQ(committed[id], 8U);
		EXPECT_EQ(floors[id], 8U);
		EXPECT_EQ(inventories[id], 0U);
		EXPECT_EQ(reservations[id], 0U);
		EXPECT_FALSE(terminated[id]);
		EXPECT_FALSE(peers[id].Busy());
	}
}

TEST_F(ItemTransferTest, RejectionPreservesTheSourceAndReleasesEveryReservation)
{
	allowed[7] = false;
	Request(4, 1);
	Drain();
	for (uint8_t id = 0; id < 8; ++id) {
		EXPECT_EQ(committed[id], 0U);
		EXPECT_EQ(cancelled[id], 1U);
		EXPECT_EQ(floors[id], 0U);
		EXPECT_EQ(inventories[id], 1U);
		EXPECT_EQ(reservations[id], 0U);
		EXPECT_FALSE(peers[id].Busy());
	}
	allowed[7] = true;
	Request(4, 2);
	Drain(1);
	EXPECT_EQ(inventories[4], 0U);
	for (const unsigned value : committed)
		EXPECT_EQ(value, 1U);
}

TEST_F(ItemTransferTest, OnlyOneOfEightContendersCanConsumeTheLastWorldSlot)
{
	floors.fill(MAXITEMS - 1);
	for (uint8_t actor = 0; actor < 8; ++actor)
		Request(actor, 1);
	Drain(7);
	unsigned retained = 0;
	for (uint8_t id = 0; id < 8; ++id) {
		EXPECT_EQ(floors[id], MAXITEMS);
		EXPECT_EQ(committed[id], 1U);
		EXPECT_EQ(cancelled[id], 7U);
		EXPECT_EQ(reservations[id], 0U);
		retained += inventories[id];
	}
	EXPECT_EQ(retained, 7U);
}

TEST_F(ItemTransferTest, DuplicatePacketsCannotDuplicateReservationsOrObjects)
{
	duplicate = true;
	for (uint8_t actor = 0; actor < 8; ++actor)
		Request(actor, 1);
	Drain(11);
	for (uint8_t id = 0; id < 8; ++id) {
		EXPECT_EQ(committed[id], 8U);
		EXPECT_EQ(inventories[id], 0U);
		EXPECT_EQ(reservations[id], 0U);
		EXPECT_FALSE(terminated[id]);
	}
}

TEST_F(ItemTransferTest, FinishedRequestsAndDecisionsCannotBeReplayed)
{
	Request(7, 1);
	const Envelope request = inbox[0].front();
	Drain(19);
	Broadcast(request.sender, request.message);
	ItemTransferMessage commit = request.message;
	commit.phase = ItemTransferPhase::Commit;
	commit.serial = 1;
	commit.voters = 255;
	Broadcast(0, commit);
	Drain(23);
	for (uint8_t id = 0; id < 8; ++id) {
		EXPECT_EQ(committed[id], 1U);
		EXPECT_EQ(reservations[id], 0U);
		EXPECT_FALSE(peers[id].Busy());
	}
}

TEST_F(ItemTransferTest, ARequestCannotImpersonateAnotherActor)
{
	ItemTransferMessage request;
	request.actor = 7;
	request.request = 1;
	Broadcast(3, request);
	Drain();
	for (uint8_t id = 0; id < 8; ++id)
		EXPECT_FALSE(peers[id].Busy());
}

TEST_F(ItemTransferTest, AReusedPlayerSlotRejectsRequestsFromItsPreviousEpoch)
{
	for (auto &peer : peers)
		peer.Leave(7);
	ItemTransferMessage stale;
	stale.actor = 7;
	stale.request = 1;
	stale.epoch = 0;
	Broadcast(7, stale);
	Drain();
	for (const unsigned value : committed)
		EXPECT_EQ(value, 0U);
	Request(7, 1);
	Drain(29);
	for (const unsigned value : committed)
		EXPECT_EQ(value, 1U);
}

TEST_F(ItemTransferTest, ADepartureDuringAnUndecidedTransferDoesNotGuessACommit)
{
	Request(7, 1);
	const Envelope request = inbox[0].front();
	inbox[0].pop_front();
	peers[0].Receive(request.sender, request.message, active);
	ASSERT_TRUE(peers[0].Busy());
	peers[0].Leave(7);
	EXPECT_TRUE(terminated[0]);
	EXPECT_EQ(committed[0], 0U);
	EXPECT_EQ(inventories[7], 1U);
}

TEST_F(ItemTransferTest, SnapshotRejectsGapsAndDuplicateSerialsBeforeReserving)
{
	ItemTransferCoordinator::Snapshot snapshot;
	snapshot.issued = 1;
	EXPECT_FALSE(peers[7].Restore(snapshot, active));
	EXPECT_EQ(reservations[7], 0U);
	snapshot.entries[0] = ItemTransferCoordinator::Entry {};
	snapshot.entries[0]->message.phase = ItemTransferPhase::Offer;
	snapshot.entries[0]->message.actor = 1;
	snapshot.entries[0]->message.serial = 1;
	snapshot.entries[0]->message.voters = 255;
	snapshot.entries[0]->message.request = 1;
	snapshot.entries[1] = snapshot.entries[0];
	EXPECT_FALSE(peers[7].Restore(snapshot, active));
	EXPECT_EQ(reservations[7], 0U);
}

TEST_F(ItemTransferTest, CommitRemainsPendingUntilEveryReplicaConfirmsApplication)
{
	Request(7, 1);
	DrainExcept(7);
	ASSERT_TRUE(Step(7)); // request
	ASSERT_TRUE(Step(7)); // offer, now ready
	DrainExcept(7);
	ASSERT_EQ(committed[0], 1U);
	EXPECT_EQ(committed[7], 0U);
	EXPECT_EQ(peers[0].State().finished, 0U);
	EXPECT_TRUE(peers[0].Busy());
	peers[0].Leave(7);
	EXPECT_TRUE(terminated[0]);
	EXPECT_EQ(inventories[7], 1U);
}

TEST_F(ItemTransferTest, CleanDepartureWaitsForOldVotesAndIsExcludedFromNewTransfers)
{
	Request(7, 1);
	peers[7].BeginLeave();
	Drain(31);
	ASSERT_TRUE(peers[7].CanLeave());
	Request(3, 1);
	DrainExcept(7);
	EXPECT_EQ(committed[0], 2U);
	EXPECT_FALSE(peers[0].Busy());
	for (auto &peer : peers)
		peer.Leave(7);
	active = 127;
	for (uint8_t id = 0; id < 7; ++id)
		EXPECT_FALSE(terminated[id]);
}

TEST_F(ItemTransferTest, SharedQuestCreationWaitsForSpaceAndSurvivesPublisherDeparture)
{
	floors.fill(MAXITEMS);
	ItemTransferMessage quest;
	quest.actor = 7;
	quest.request = 1;
	quest.kind = ItemTransferKind::Creation;
	quest.item.bCmd = CMD_SPAWNITEM;
	Broadcast(7, quest);
	Drain(37);
	for (uint8_t id = 0; id < 8; ++id) {
		ASSERT_EQ(peers[id].State().deferred.size(), 1U);
		EXPECT_EQ(floors[id], MAXITEMS);
	}
	peers[7].BeginLeave();
	Drain(41);
	ASSERT_TRUE(peers[7].CanLeave());
	for (auto &peer : peers)
		peer.Leave(7);
	active = 127;
	ItemTransferMessage pickup;
	pickup.kind = ItemTransferKind::ManualPickup;
	pickup.actor = 1;
	pickup.request = 1;
	Broadcast(1, pickup);
	Drain(43);
	for (uint8_t id = 0; id < 7; ++id) {
		EXPECT_EQ(floors[id], MAXITEMS);
		EXPECT_TRUE(peers[id].State().deferred.empty());
		EXPECT_FALSE(terminated[id]);
		EXPECT_FALSE(peers[id].Busy());
	}
}

TEST_F(ItemTransferTest, SnapshotCodecPreservesPendingVotesAndRejectsEveryTruncation)
{
	Request(1, 1);
	ASSERT_TRUE(Step(0));
	const auto encoded = EncodeItemTransfers(peers[0].State());
	const auto decoded = DecodeItemTransfers(encoded);
	ASSERT_TRUE(decoded);
	EXPECT_EQ(EncodeItemTransfers(*decoded), encoded);
	for (size_t length = 0; length < encoded.size(); ++length)
		EXPECT_FALSE(DecodeItemTransfers(std::span(encoded).first(length))) << length;
	auto extra = encoded;
	extra.push_back(std::byte { 0 });
	EXPECT_FALSE(DecodeItemTransfers(extra));
}

TEST_F(ItemTransferTest, AnAppliedSnapshotDoesNotApplyACommitTwice)
{
	Request(1, 1);
	DrainExcept(7);
	ASSERT_TRUE(Step(7));
	ASSERT_TRUE(Step(7));
	DrainExcept(7);
	ASSERT_EQ(peers[0].State().applied, 1U);
	ASSERT_EQ(committed[7], 0U);
	const auto decoded = DecodeItemTransfers(EncodeItemTransfers(peers[0].State()));
	ASSERT_TRUE(decoded);
	floors[7] = 1; // imported world snapshot, private inventory is unchanged
	ASSERT_TRUE(peers[7].Restore(*decoded, active));
	EXPECT_EQ(reservations[7], 0U);
	Drain(47);
	EXPECT_EQ(committed[7], 0U);
	EXPECT_EQ(floors[7], 1U);
	for (uint8_t id = 0; id < 8; ++id) {
		EXPECT_EQ(peers[id].State().finished, 1U);
		EXPECT_FALSE(peers[id].Busy());
		EXPECT_FALSE(terminated[id]);
	}
}

TEST_F(ItemTransferTest, APrivateOfferWaitsForTheActorsOrderedRequest)
{
	Request(7, 1);
	ASSERT_TRUE(Step(0)); // coordinator receives the request and sends an offer
	const Envelope request = inbox[6].front();
	inbox[6].pop_front();
	ASSERT_TRUE(Step(6)); // offer overtakes the actor on another connection
	EXPECT_EQ(reservations[6], 0U);
	peers[6].Receive(request.sender, request.message, active);
	EXPECT_EQ(reservations[6], 1U);
	Drain(51);
	for (unsigned id = 0; id < 8; ++id) {
		EXPECT_EQ(committed[id], 1U);
		EXPECT_FALSE(terminated[id]);
	}
}

TEST_F(ItemTransferTest, AQuiescentCoordinatorCanLeaveWithoutEndingTheSession)
{
	Request(7, 1);
	Drain();
	peers[0].BeginLeave();
	Drain(53);
	ASSERT_TRUE(peers[0].CanLeave());
	for (auto &peer : peers)
		peer.Leave(0);
	active = 254;
	Request(6, 1);
	Drain(57);
	for (unsigned id = 1; id < 8; ++id) {
		EXPECT_EQ(peers[id].State().coordinator, 1U);
		EXPECT_EQ(committed[id], 2U);
		EXPECT_FALSE(terminated[id]);
	}
}

TEST_F(ItemTransferTest, TheSnapshotRetainsRequestsBeyondTheAdmissionWindow)
{
	inventories.fill(20);
	for (unsigned request = 1; request <= ItemTransferCoordinator::WindowSize + 3; ++request)
		Request(7, request);
	while (Step(0)) {
		if (peers[0].State().queued.size() == 3)
			break;
	}
	ASSERT_EQ(peers[0].State().queued.size(), 3U);
	const auto decoded = DecodeItemTransfers(EncodeItemTransfers(peers[0].State()));
	ASSERT_TRUE(decoded);
	EXPECT_EQ(decoded->queued.size(), 3U);
	EXPECT_EQ(uint32_t { decoded->queued.front().request }, ItemTransferCoordinator::WindowSize + 1);
	Drain(59);
	for (unsigned id = 0; id < 8; ++id) {
		EXPECT_EQ(committed[id], ItemTransferCoordinator::WindowSize + 3);
		EXPECT_FALSE(terminated[id]);
	}
}

TEST_F(ItemTransferTest, RestoringARejectedOfferRejoinsTheCancellationInsteadOfEndingTheGame)
{
	floors.fill(MAXITEMS);
	Request(4, 1);
	DrainExcept(7);
	const auto snapshot = DecodeItemTransfers(EncodeItemTransfers(peers[0].State()));
	ASSERT_TRUE(snapshot);
	ASSERT_TRUE(peers[7].Restore(*snapshot, active));
	Drain(61);
	for (unsigned id = 0; id < 8; ++id) {
		EXPECT_EQ(floors[id], MAXITEMS);
		EXPECT_EQ(committed[id], 0U);
		EXPECT_FALSE(peers[id].Busy());
		EXPECT_FALSE(terminated[id]);
	}
}

TEST_F(ItemTransferTest, ADepartureWaitsForFinalizeOnEveryConnection)
{
	Request(7, 1);
	DrainExcept(6);
	while (committed[6] == 0) {
		ASSERT_TRUE(Step(6));
		DrainExcept(6);
	}
	DrainExcept(6);
	ASSERT_EQ(peers[7].State().finished, 1U);
	ASSERT_EQ(peers[6].State().finished, 0U);
	peers[7].BeginLeave();
	DrainExcept(6);
	const auto intent = std::find_if(inbox[6].begin(), inbox[6].end(), [](const Envelope &envelope) { return envelope.message.phase == ItemTransferPhase::LeaveIntent; });
	ASSERT_NE(intent, inbox[6].end());
	const Envelope overtaking = *intent;
	inbox[6].erase(intent);
	peers[6].Receive(overtaking.sender, overtaking.message, active);
	DrainExcept(6);
	EXPECT_FALSE(peers[7].CanLeave());
	Drain(67);
	EXPECT_TRUE(peers[7].CanLeave());
}

TEST_F(ItemTransferTest, DeferredRewardsCannotOverflowAnAlreadyFullRequestQueue)
{
	ItemTransferCoordinator::Snapshot initial;
	initial.issued = initial.applied = initial.finished = ItemTransferCoordinator::DeferredSize;
	initial.requests[4] = ItemTransferCoordinator::DeferredSize;
	for (uint32_t i = 0; i < ItemTransferCoordinator::DeferredSize; ++i) {
		ItemTransferMessage reward;
		reward.phase = ItemTransferPhase::Offer;
		reward.kind = ItemTransferKind::Creation;
		reward.actor = 4;
		reward.serial = reward.request = i + 1;
		reward.item.bCmd = CMD_SPAWNITEM;
		initial.deferred.push_back(reward);
	}
	for (auto &peer : peers)
		ASSERT_TRUE(peer.Restore(initial, active));
	floors.fill(1);
	inventories.fill(512);
	ItemTransferMessage pickup;
	pickup.kind = ItemTransferKind::ManualPickup;
	pickup.actor = 1;
	pickup.request = 1;
	Broadcast(1, pickup);
	for (uint32_t i = 1; i < ItemTransferCoordinator::QueueSize + ItemTransferCoordinator::WindowSize; ++i)
		Request(7, i);
	for (unsigned round = 0; round < 10000 && peers[0].State().finished == initial.finished; ++round) {
		for (uint8_t id = 0; id < 8; ++id)
			Step(id);
	}
	ASSERT_GT(peers[0].State().finished, initial.finished);
	EXPECT_LE(peers[0].State().queued.size(), ItemTransferCoordinator::QueueSize);
	EXPECT_TRUE(DecodeItemTransfers(EncodeItemTransfers(peers[0].State())));
}

TEST_F(ItemTransferTest, RelocationPreservesTheRewardOriginAndCannotReplayAfterPickup)
{
	floors.fill(MAXITEMS);
	ItemTransferMessage reward;
	reward.kind = ItemTransferKind::Creation;
	reward.actor = 7;
	reward.request = 1;
	reward.item.bCmd = CMD_SPAWNITEM;
	Broadcast(7, reward);
	Drain();
	ASSERT_EQ(peers[0].State().deferred.size(), 1U);
	ItemTransferMessage proposal = peers[0].State().deferred.front();
	proposal.phase = ItemTransferPhase::Relocate;
	proposal.serial = 0;
	proposal.voters = 0;
	proposal.item.x = 40;
	floors.fill(MAXITEMS - 1);
	auto forged = proposal;
	forged.item.item.wValue = 100;
	Broadcast(1, forged);
	Drain();
	EXPECT_EQ(peers[0].State().deferred.size(), 1U);
	Broadcast(1, proposal);
	Drain(71);
	for (unsigned id = 0; id < 8; ++id) {
		EXPECT_EQ(floors[id], MAXITEMS);
		EXPECT_TRUE(peers[id].State().deferred.empty());
	}
	ItemTransferMessage pickup;
	pickup.kind = ItemTransferKind::ManualPickup;
	pickup.actor = 1;
	pickup.request = 1;
	Broadcast(1, pickup);
	Drain();
	Broadcast(1, proposal);
	Drain(73);
	for (unsigned id = 0; id < 8; ++id) {
		EXPECT_EQ(floors[id], MAXITEMS - 1);
		EXPECT_EQ(committed[id], 2U);
		EXPECT_FALSE(peers[id].Busy());
		EXPECT_FALSE(terminated[id]);
	}
}

TEST_F(ItemTransferTest, RewardVotesBeforeTheOfferSurviveSnapshotAndCompleteOnEveryReplica)
{
	ItemTransferMessage request;
	request.kind = ItemTransferKind::MonsterReward;
	request.actor = 4;
	request.request = 1;
	Broadcast(4, request);
	DrainExcept(5);
	std::deque<Envelope> coordinator;
	for (auto it = inbox[5].begin(); it != inbox[5].end();) {
		if (it->sender == 0) {
			coordinator.push_back(*it);
			it = inbox[5].erase(it);
		} else {
			++it;
		}
	}
	ASSERT_FALSE(coordinator.empty());
	while (Step(5)) { }
	EXPECT_EQ(reservations[5], 0U);
	const auto snapshot = DecodeItemTransfers(EncodeItemTransfers(peers[5].State()));
	ASSERT_TRUE(snapshot);
	ASSERT_TRUE(std::any_of(snapshot->earlyVotes.begin(), snapshot->earlyVotes.end(), [](const auto &entry) { return entry.has_value(); }));
	ASSERT_TRUE(peers[5].Restore(*snapshot, active));
	for (const auto &envelope : coordinator)
		peers[5].Receive(envelope.sender, envelope.message, active);
	Drain(17019);
	for (uint8_t peer = 0; peer < 8; ++peer) {
		EXPECT_EQ(committed[peer], 1U);
		EXPECT_FALSE(peers[peer].Busy());
		EXPECT_FALSE(terminated[peer]);
	}
}

TEST_F(ItemTransferTest, MonsterRewardsWaitForEveryContributionFenceBeforeLaterDecisions)
{
	ItemTransferMessage request;
	request.kind = ItemTransferKind::MonsterReward;
	request.actor = 4;
	request.request = 1;
	Broadcast(4, request);
	DrainExcept(7);
	while (reservations[7] == 0)
		ASSERT_TRUE(Step(7));
	const auto delayed = std::find_if(inbox[7].begin(), inbox[7].end(), [](const auto &envelope) { return envelope.sender == 6 && envelope.message.phase == ItemTransferPhase::Ready; });
	ASSERT_NE(delayed, inbox[7].end());
	const Envelope fence = *delayed;
	inbox[7].erase(delayed);
	Drain();
	EXPECT_EQ(committed[0], 1U);
	EXPECT_EQ(committed[7], 0U);
	Request(5, 1);
	Drain();
	EXPECT_EQ(committed[0], 2U);
	EXPECT_EQ(committed[7], 0U);
	const auto snapshot = DecodeItemTransfers(EncodeItemTransfers(peers[7].State()));
	ASSERT_TRUE(snapshot);
	ASSERT_TRUE(peers[7].Restore(*snapshot, active));
	EXPECT_EQ(committed[7], 0U);
	peers[7].Receive(fence.sender, fence.message, active);
	Drain(17001);
	for (unsigned id = 0; id < 8; ++id) {
		EXPECT_EQ(committed[id], 2U);
		EXPECT_FALSE(peers[id].Busy());
		EXPECT_FALSE(terminated[id]);
	}
}

TEST_F(ItemTransferTest, PipelinedTransfersRemainConsistentAcrossManyDeliveryOrders)
{
	for (unsigned seed = 0; seed < 20; ++seed) {
		SCOPED_TRACE(seed);
		for (uint8_t actor = 0; actor < 8; ++actor) {
			++inventories[actor];
			Request(actor, seed + 1);
		}
		Drain(seed);
		for (uint8_t id = 0; id < 8; ++id) {
			EXPECT_EQ(committed[id], (seed + 1) * 8);
			EXPECT_EQ(reservations[id], 0U);
			EXPECT_FALSE(terminated[id]);
		}
		floors.fill(0);
	}
}

} // namespace
} // namespace devilution
