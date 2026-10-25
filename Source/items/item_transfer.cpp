#include "items/item_transfer.hpp"

#include <algorithm>
#include <cstring>
#include <limits>
#include <utility>

#include "utils/endian_swap.hpp"

namespace devilution {
namespace {
uint8_t Bit(uint8_t player) { return static_cast<uint8_t>(1U << player); }
bool Same(const ItemTransferMessage &a, const ItemTransferMessage &b)
{
	return a.kind == b.kind && a.actor == b.actor && a.voters == b.voters && a.level == b.level
	    && a.epoch == b.epoch && a.request == b.request && a.serial == b.serial
	    && std::memcmp(&a.item, &b.item, sizeof(a.item)) == 0;
}
} // namespace

void ItemTransferCoordinator::Reset(uint8_t local, Hooks hooks)
{
	for (const auto &entry : state_.entries) {
		if (entry && entry->reserved && hooks_.release)
			hooks_.release(entry->message);
	}
	local_ = local;
	lastActive_ = Bit(local);
	hooks_ = std::move(hooks);
	state_ = { };
	stopped_ = false;
	leaveApprovals_ = 0;
	acknowledgedDepartures_ = 0;
	retryDeferred_ = false;
	state_.queued.clear();
}

ItemTransferCoordinator::Entry *ItemTransferCoordinator::Find(uint32_t serial)
{
	for (auto &entry : state_.entries) {
		if (entry && entry->message.serial == serial)
			return &*entry;
	}
	return nullptr;
}

bool ItemTransferCoordinator::Busy() const
{
	return !state_.queued.empty() || std::any_of(state_.earlyVotes.begin(), state_.earlyVotes.end(), [](const auto &entry) { return entry.has_value(); })
	    || std::any_of(state_.entries.begin(), state_.entries.end(), [](const auto &entry) { return entry.has_value(); });
}

bool ItemTransferCoordinator::Pending(uint8_t actor) const
{
	return std::any_of(state_.entries.begin(), state_.entries.end(), [=](const auto &entry) { return entry && entry->message.actor == actor; });
}

void ItemTransferCoordinator::Pump(uint8_t active)
{
	lastActive_ = active;
	if (stopped_ || local_ != state_.coordinator || (active & Bit(state_.coordinator)) == 0)
		return;
	if (retryDeferred_ && !Quiescing(state_.coordinator)) {
		const size_t count = state_.queued.size() >= QueueSize ? 0 : std::min(state_.deferred.size(), QueueSize - state_.queued.size());
		for (auto message : std::span(state_.deferred).first(count)) {
			message.phase = ItemTransferPhase::Request;
			message.serial = 0;
			message.voters = 0;
			state_.queued.push_back(message);
		}
		state_.deferred.erase(state_.deferred.begin(), state_.deferred.begin() + count);
		retryDeferred_ = false;
	}
	for (auto &slot : state_.entries) {
		if (slot || state_.queued.empty())
			continue;
		if (state_.issued == std::numeric_limits<uint32_t>::max()) {
			Fail();
			return;
		}
		ItemTransferMessage message = state_.queued.front();
		state_.queued.pop_front();
		message.phase = ItemTransferPhase::Offer;
		message.serial = ++state_.issued;
		message.voters = static_cast<uint8_t>((active & ~state_.quiescing) | Bit(state_.coordinator) | (message.kind == ItemTransferKind::Creation ? 0 : Bit(message.actor)));
		// Reserve the serial before sending, but reserve world resources on the
		// local echo, in exactly the same order as every other participant.
		slot = Entry { message };
		hooks_.send(message);
	}
}

void ItemTransferCoordinator::Vote(Entry &entry)
{
	const auto &message = entry.message;
	if (entry.voted || (message.kind != ItemTransferKind::Creation && state_.requests[message.actor] < message.request))
		return;
	entry.voted = true;
	const uint8_t expected = static_cast<uint8_t>((lastActive_ & ~state_.quiescing) | Bit(state_.coordinator) | (message.kind == ItemTransferKind::Creation ? 0 : Bit(message.actor)));
	entry.reserved = hooks_.reserve(message);
	ItemTransferMessage vote = message;
	vote.phase = entry.reserved && message.voters == expected ? ItemTransferPhase::Ready : ItemTransferPhase::Reject;
	if ((message.voters & Bit(local_)) != 0)
		hooks_.send(vote);
	else if (!entry.reserved)
		Fail();
}

void ItemTransferCoordinator::Decide()
{
	if (stopped_ || local_ != state_.coordinator)
		return;
	Entry *entry = Find(state_.applied + 1);
	if (entry == nullptr || entry->decisionSent || (!entry->rejected && entry->votes != entry->message.voters))
		return;
	entry->decisionSent = true;
	entry->committed = !entry->rejected;
	ItemTransferMessage decision = entry->message;
	decision.phase = entry->rejected ? ItemTransferPhase::Cancel : ItemTransferPhase::Commit;
	hooks_.send(decision);
}

void ItemTransferCoordinator::Finalize()
{
	if (stopped_ || local_ != state_.coordinator)
		return;
	Entry *entry = Find(state_.finished + 1);
	if (entry == nullptr || !entry->applied || entry->finalizeSent || entry->acknowledgements != entry->message.voters)
		return;
	entry->finalizeSent = true;
	ItemTransferMessage message = entry->message;
	message.phase = ItemTransferPhase::Finalize;
	hooks_.send(message);
}

void ItemTransferCoordinator::AcknowledgeDepartures()
{
	for (uint8_t actor = 0; actor < PeerCount; ++actor) {
		if (!Quiescing(actor) || (acknowledgedDepartures_ & Bit(actor)) != 0)
			continue;
		const bool pending = actor == state_.coordinator ? Busy()
		                                                 : std::any_of(state_.entries.begin(), state_.entries.end(), [=](const auto &entry) { return entry && (entry->message.voters & Bit(actor)) != 0; })
		        || std::any_of(state_.queued.begin(), state_.queued.end(), [=](const auto &request) { return request.actor == actor; })
		        || std::any_of(state_.earlyVotes.begin(), state_.earlyVotes.end(), [=](const auto &entry) { return entry && (entry->message.voters & Bit(actor)) != 0; });
		if (pending)
			continue;
		acknowledgedDepartures_ |= Bit(actor);
		ItemTransferMessage acknowledgement;
		acknowledgement.phase = ItemTransferPhase::LeaveAccepted;
		acknowledgement.actor = actor;
		acknowledgement.epoch = state_.epochs[actor];
		hooks_.send(acknowledgement);
	}
}

bool ItemTransferCoordinator::CanLeave() const
{
	return (leaveApprovals_ & lastActive_) == lastActive_ && (local_ != state_.coordinator || state_.queued.empty()) && std::none_of(state_.entries.begin(), state_.entries.end(), [&](const auto &entry) {
		return entry && (entry->message.voters & Bit(local_)) != 0;
	});
}

void ItemTransferCoordinator::BeginLeave()
{
	if (stopped_ || Quiescing(local_))
		return;
	ItemTransferMessage message;
	message.phase = ItemTransferPhase::LeaveIntent;
	message.actor = local_;
	message.epoch = state_.epochs[local_];
	hooks_.send(message);
}

void ItemTransferCoordinator::Receive(uint8_t sender, ItemTransferMessage message, uint8_t active)
{
	lastActive_ = active;
	if (stopped_ || sender >= PeerCount || message.actor >= PeerCount || (active & Bit(sender)) == 0
	    || message.command != CMD_ITEMTRANSFER
	    || (message.kind != ItemTransferKind::Creation || message.phase == ItemTransferPhase::Request
	            ? message.epoch != state_.epochs[message.actor]
	            : message.epoch > state_.epochs[message.actor])
	    || message.kind > ItemTransferKind::MonsterReward || message.phase > ItemTransferPhase::Relocate)
		return;
	if (message.phase == ItemTransferPhase::LeaveIntent) {
		if (sender != message.actor)
			return;
		state_.quiescing |= Bit(message.actor);
		AcknowledgeDepartures();
		return;
	}
	if (message.phase == ItemTransferPhase::LeaveAccepted) {
		if (message.actor == local_ && Quiescing(local_))
			leaveApprovals_ |= Bit(sender);
		return;
	}
	if (message.request == 0)
		return;
	if (message.phase == ItemTransferPhase::Relocate) {
		if (local_ != state_.coordinator || message.kind != ItemTransferKind::Creation || message.serial != 0 || message.voters != 0 || state_.queued.size() >= QueueSize)
			return;
		const auto origin = [&](const ItemTransferMessage &pending) { return pending.actor == message.actor && pending.epoch == message.epoch && pending.request == message.request; };
		const auto pending = std::find_if(state_.deferred.begin(), state_.deferred.end(), origin);
		if (pending == state_.deferred.end()
		    || std::any_of(state_.entries.begin(), state_.entries.end(), [&](const auto &entry) { return entry && origin(entry->message); })
		    || std::any_of(state_.queued.begin(), state_.queued.end(), origin))
			return;
		ItemTransferMessage comparison = message;
		comparison.serial = pending->serial;
		comparison.voters = pending->voters;
		comparison.item.x = pending->item.x;
		comparison.item.y = pending->item.y;
		if (!Same(comparison, *pending))
			return;
		message.phase = ItemTransferPhase::Request;
		state_.queued.push_back(message);
		state_.deferred.erase(pending);
		Pump(active);
		return;
	}
	if (message.phase == ItemTransferPhase::Request) {
		if (sender != message.actor || Quiescing(message.actor) || message.serial != 0 || message.voters != 0
		    || message.request <= state_.requests[message.actor])
			return;
		state_.requests[message.actor] = message.request;
		for (auto &entry : state_.entries) {
			if (entry && entry->message.actor == message.actor && entry->message.request <= message.request)
				Vote(*entry);
		}
		if (std::any_of(state_.entries.begin(), state_.entries.end(), [&](const auto &entry) { return entry && entry->message.actor == message.actor && entry->message.epoch == message.epoch && entry->message.request == message.request; }))
			return;
		if (state_.queued.size() >= (local_ == state_.coordinator ? QueueSize : MaxQueuedRequests)) {
			Fail();
			return;
		}
		state_.queued.push_back(message);
		Pump(active);
		return;
	}
	if (message.serial == 0 || message.serial <= state_.finished)
		return;
	Entry *entry = Find(message.serial);
	if (message.phase == ItemTransferPhase::Offer) {
		if (sender != state_.coordinator || message.voters == 0 || (message.voters & Bit(state_.coordinator)) == 0
		    || (message.kind != ItemTransferKind::Creation && (message.voters & Bit(message.actor)) == 0))
			return;
		if (entry == nullptr) {
			if (message.serial != state_.issued + 1)
				return;
			auto slot = std::find_if(state_.entries.begin(), state_.entries.end(), [](const auto &value) { return !value; });
			if (slot == state_.entries.end()) {
				Fail();
				return;
			}
			state_.issued = message.serial;
			*slot = Entry { message };
			entry = &**slot;
		} else if (!Same(entry->message, message)) {
			return;
		}
		std::erase_if(state_.deferred, [&](const ItemTransferMessage &pending) {
			return pending.actor == message.actor && pending.epoch == message.epoch && pending.request == message.request;
		});
		std::erase_if(state_.queued, [&](const ItemTransferMessage &request) {
			return request.actor == message.actor && request.epoch == message.epoch && request.request == message.request;
		});
		if (message.kind == ItemTransferKind::Creation && message.epoch == state_.epochs[message.actor])
			state_.requests[message.actor] = std::max(state_.requests[message.actor], uint32_t { message.request });
		for (auto &early : state_.earlyVotes) {
			if (early && early->message.serial == message.serial) {
				if (Same(early->message, message)) {
					entry->votes |= early->votes;
					entry->rejected |= early->rejected;
				}
				early.reset();
			}
		}
		Vote(*entry);
		return;
	}
	if (entry == nullptr && message.kind == ItemTransferKind::MonsterReward
	    && (message.phase == ItemTransferPhase::Ready || message.phase == ItemTransferPhase::Reject)
	    && message.serial > state_.issued && message.serial - state_.issued <= WindowSize
	    && (message.voters & Bit(sender)) != 0 && (message.voters & Bit(state_.coordinator)) != 0 && (message.voters & Bit(message.actor)) != 0) {
		// Other peers can vote before this connection delivers the offer.
		auto slot = std::find_if(state_.earlyVotes.begin(), state_.earlyVotes.end(), [&](const auto &value) { return value && value->message.serial == message.serial; });
		if (slot == state_.earlyVotes.end()) {
			slot = std::find_if(state_.earlyVotes.begin(), state_.earlyVotes.end(), [](const auto &value) { return !value; });
			if (slot == state_.earlyVotes.end()) {
				Fail();
				return;
			}
			*slot = Entry { message };
			(*slot)->message.phase = ItemTransferPhase::Offer;
		}
		if (Same((*slot)->message, message)) {
			(*slot)->votes |= Bit(sender);
			(*slot)->rejected |= message.phase == ItemTransferPhase::Reject;
		}
		return;
	}
	if (entry == nullptr || !Same(entry->message, message))
		return;
	if (message.phase == ItemTransferPhase::Ready || message.phase == ItemTransferPhase::Reject) {
		if ((message.voters & Bit(sender)) == 0)
			return;
		entry->votes |= Bit(sender);
		entry->rejected |= message.phase == ItemTransferPhase::Reject;
		DrainDecisions();
		Decide();
		return;
	}
	if (message.phase == ItemTransferPhase::Applied) {
		if ((message.voters & Bit(sender)) == 0 || (!entry->applied && !entry->decisionSent))
			return;
		entry->acknowledgements |= Bit(sender);
		Finalize();
		return;
	}
	if (sender != state_.coordinator)
		return;
	if (message.phase == ItemTransferPhase::Commit || message.phase == ItemTransferPhase::Cancel) {
		if (!entry->applied && (message.serial != state_.applied + 1 || (message.kind == ItemTransferKind::MonsterReward && message.phase == ItemTransferPhase::Commit && entry->votes != message.voters))) {
			entry->deferredDecision = message.phase == ItemTransferPhase::Commit;
			return;
		}
		if (entry->applied) {
			if (entry->committed != (message.phase == ItemTransferPhase::Commit)) {
				Fail();
				return;
			}
		} else {
			if (message.serial != state_.applied + 1)
				return;
			if (message.phase == ItemTransferPhase::Commit && (!entry->reserved || entry->rejected)) {
				Fail();
				return;
			}
			entry->committed = message.phase == ItemTransferPhase::Commit;
			if (entry->reserved) {
				hooks_.release(message);
				entry->reserved = false;
				if (entry->committed && !hooks_.commit(message)) {
					Fail();
					return;
				}
			}
			entry->applied = true;
			state_.applied = message.serial;
		}
		ItemTransferMessage acknowledgement = message;
		acknowledgement.phase = ItemTransferPhase::Applied;
		if ((message.voters & Bit(local_)) != 0)
			hooks_.send(acknowledgement);
		Decide();
		Finalize();
		DrainDecisions();
		return;
	}
	if (message.phase == ItemTransferPhase::Finalize && (message.serial != state_.finished + 1 || !entry->applied)) {
		entry->deferredFinalize = true;
		return;
	}
	if (message.phase != ItemTransferPhase::Finalize)
		return;
	if (!entry->committed && message.kind == ItemTransferKind::Creation && message.item.bCmd == CMD_SPAWNITEM && Swap16LE(message.item.def.wIndx) != IDI_EAR) {
		if (state_.deferred.size() >= DeferredSize) {
			Fail();
			return;
		}
		state_.deferred.push_back(entry->message);
	}
	if (entry->committed && (message.kind == ItemTransferKind::ManualPickup || message.kind == ItemTransferKind::AutoPickup))
		retryDeferred_ = true;
	if (hooks_.complete)
		hooks_.complete(message, entry->committed);
	for (auto &slot : state_.entries) {
		if (slot && slot->message.serial == message.serial) {
			slot.reset();
			break;
		}
	}
	state_.finished = message.serial;
	DrainDecisions();
	AcknowledgeDepartures();
	Finalize();
	Decide();
	Pump(active);
}

void ItemTransferCoordinator::DrainDecisions()
{
	if (stopped_)
		return;
	if (auto *entry = Find(state_.applied + 1); entry != nullptr && entry->deferredDecision) {
		if (entry->message.kind != ItemTransferKind::MonsterReward || !*entry->deferredDecision || entry->votes == entry->message.voters) {
			auto message = entry->message;
			message.phase = *entry->deferredDecision ? ItemTransferPhase::Commit : ItemTransferPhase::Cancel;
			entry->deferredDecision.reset();
			Receive(state_.coordinator, message, lastActive_);
		}
	}
	if (auto *entry = Find(state_.finished + 1); entry != nullptr && entry->deferredFinalize && entry->applied) {
		auto message = entry->message;
		message.phase = ItemTransferPhase::Finalize;
		entry->deferredFinalize = false;
		Receive(state_.coordinator, message, lastActive_);
	}
}

void ItemTransferCoordinator::Leave(uint8_t player)
{
	if (player >= PeerCount)
		return;
	// A clean departure drains its transfers first. A transport failure during
	// an undecided transfer must stop the session rather than guess its outcome.
	if (stopped_)
		return;
	const bool involved = std::any_of(state_.entries.begin(), state_.entries.end(), [=](const auto &entry) { return entry && (entry->message.voters & Bit(player)) != 0; })
	    || std::any_of(state_.earlyVotes.begin(), state_.earlyVotes.end(), [=](const auto &entry) { return entry && (entry->message.voters & Bit(player)) != 0; });
	if (involved || (player == state_.coordinator && Busy())) {
		Fail();
		return;
	}
	if (state_.epochs[player] == std::numeric_limits<uint32_t>::max()) {
		Fail();
		return;
	}
	if (player == state_.coordinator) {
		// The same membership change elects the lowest remaining slot everywhere.
		for (uint8_t candidate = 0; candidate < PeerCount; ++candidate) {
			if (candidate != player && (lastActive_ & Bit(candidate)) != 0) {
				state_.coordinator = candidate;
				break;
			}
		}
	}
	++state_.epochs[player];
	state_.requests[player] = 0;
	state_.quiescing &= static_cast<uint8_t>(~Bit(player));
	acknowledgedDepartures_ &= static_cast<uint8_t>(~Bit(player));
	leaveApprovals_ &= static_cast<uint8_t>(~Bit(player));
}

bool ItemTransferCoordinator::Restore(const Snapshot &snapshot, uint8_t active)
{
	if (stopped_ || snapshot.finished > snapshot.applied || snapshot.applied > snapshot.issued || snapshot.issued - snapshot.finished > WindowSize || snapshot.coordinator >= PeerCount
	    || (active & Bit(snapshot.coordinator)) == 0 || !DecodeItemTransfers(EncodeItemTransfers(snapshot)))
		return false;
	for (const auto &early : snapshot.earlyVotes) {
		if (early && (early->message.voters & ~active) != 0)
			return false;
	}
	std::array<bool, WindowSize> serials { };
	for (const auto &entry : snapshot.entries) {
		if (!entry)
			continue;
		const auto &message = entry->message;
		if (message.command != CMD_ITEMTRANSFER || message.phase != ItemTransferPhase::Offer
		    || message.kind > ItemTransferKind::MonsterReward || message.request == 0
		    || message.serial <= snapshot.finished || message.serial > snapshot.issued
		    || message.actor >= PeerCount
		    || (message.kind == ItemTransferKind::Creation ? message.epoch > snapshot.epochs[message.actor] : message.epoch != snapshot.epochs[message.actor])
		    || (message.voters & ~active) != 0 || message.voters == 0 || (message.voters & Bit(snapshot.coordinator)) == 0
		    || (message.kind != ItemTransferKind::Creation && (message.voters & Bit(message.actor)) == 0) || (entry->votes & ~message.voters) != 0 || (entry->acknowledgements & ~message.voters) != 0
		    || entry->applied != (message.serial <= snapshot.applied))
			return false;
		const size_t offset = message.serial - snapshot.finished - 1;
		if (serials[offset])
			return false;
		serials[offset] = true;
	}
	for (size_t i = 0; i < snapshot.issued - snapshot.finished; ++i) {
		if (!serials[i])
			return false;
	}
	const Snapshot previous = state_;
	for (const auto &entry : state_.entries) {
		if (entry && entry->reserved)
			hooks_.release(entry->message);
	}
	state_ = snapshot;
	for (auto &entry : state_.entries) {
		if (entry) {
			entry->reserved = false;
			entry->voted = false;
		}
	}
	for (auto &entry : state_.entries) {
		if (!entry)
			continue;
		entry->voted = true;
		if (entry->applied)
			continue;
		entry->reserved = hooks_.reserve(entry->message);
		if (!entry->reserved && entry->committed) {
			for (const auto &candidate : state_.entries) {
				if (candidate && candidate->reserved)
					hooks_.release(candidate->message);
			}
			state_ = previous;
			for (auto &original : state_.entries) {
				if (original && original->reserved && !hooks_.reserve(original->message)) {
					Fail();
					break;
				}
			}
			return false;
		}
	}
	lastActive_ = active;
	for (const auto &early : previous.earlyVotes) {
		if (!early)
			continue;
		for (uint8_t sender = 0; sender < PeerCount; ++sender) {
			if ((early->votes & Bit(sender)) == 0)
				continue;
			auto vote = early->message;
			vote.phase = early->rejected ? ItemTransferPhase::Reject : ItemTransferPhase::Ready;
			Receive(sender, vote, active);
		}
	}
	for (const auto &entry : state_.entries) {
		if (!entry || (entry->message.voters & Bit(local_)) == 0)
			continue;
		ItemTransferMessage acknowledgement = entry->message;
		acknowledgement.phase = entry->applied ? ItemTransferPhase::Applied : entry->reserved ? ItemTransferPhase::Ready
		                                                                                      : ItemTransferPhase::Reject;
		hooks_.send(acknowledgement);
	}
	acknowledgedDepartures_ = 0;
	DrainDecisions();
	AcknowledgeDepartures();
	Decide();
	Finalize();
	Pump(active);
	return true;
}

void ItemTransferCoordinator::Fail()
{
	if (stopped_)
		return;
	stopped_ = true;
	hooks_.terminate();
}

void SwapItemTransferLE(ItemTransferMessage &message)
{
	message.epoch = Swap32LE(message.epoch);
	message.request = Swap32LE(message.request);
	message.serial = Swap32LE(message.serial);
}

std::vector<std::byte> EncodeItemTransfers(const ItemTransferCoordinator::Snapshot &snapshot)
{
	std::vector<std::byte> data;
	data.reserve(MaxItemTransferSnapshotSize);
	const auto append32 = [&](uint32_t value) {
		value = Swap32LE(value);
		const auto bytes = std::as_bytes(std::span(&value, 1));
		data.insert(data.end(), bytes.begin(), bytes.end());
	};
	append32(snapshot.issued);
	append32(snapshot.finished);
	append32(snapshot.applied);
	data.push_back(static_cast<std::byte>(snapshot.quiescing));
	data.push_back(static_cast<std::byte>(snapshot.coordinator));
	for (const uint32_t epoch : snapshot.epochs)
		append32(epoch);
	for (const uint32_t request : snapshot.requests)
		append32(request);
	const auto count = std::count_if(snapshot.entries.begin(), snapshot.entries.end(), [](const auto &entry) { return entry.has_value(); });
	data.push_back(static_cast<std::byte>(count));
	for (const auto &entry : snapshot.entries) {
		if (!entry)
			continue;
		ItemTransferMessage message = entry->message;
		SwapItemTransferLE(message);
		const auto bytes = std::as_bytes(std::span(&message, 1));
		data.insert(data.end(), bytes.begin(), bytes.end());
		data.push_back(static_cast<std::byte>(entry->votes));
		data.push_back(static_cast<std::byte>((entry->rejected ? 1 : 0) | (entry->decisionSent ? 2 : 0)
		    | (entry->applied ? 4 : 0) | (entry->committed ? 8 : 0) | (entry->finalizeSent ? 16 : 0)
		    | (entry->deferredDecision ? (*entry->deferredDecision ? 32 : 64) : 0) | (entry->deferredFinalize ? 128 : 0)));
		data.push_back(static_cast<std::byte>(entry->acknowledgements));
	}
	const uint16_t countDeferred = Swap16LE(static_cast<uint16_t>(snapshot.deferred.size()));
	const auto countBytes = std::as_bytes(std::span(&countDeferred, 1));
	data.insert(data.end(), countBytes.begin(), countBytes.end());
	for (ItemTransferMessage message : snapshot.deferred) {
		SwapItemTransferLE(message);
		const auto bytes = std::as_bytes(std::span(&message, 1));
		data.insert(data.end(), bytes.begin(), bytes.end());
	}
	const uint16_t queuedCount = Swap16LE(static_cast<uint16_t>(snapshot.queued.size()));
	const auto queuedBytes = std::as_bytes(std::span(&queuedCount, 1));
	data.insert(data.end(), queuedBytes.begin(), queuedBytes.end());
	for (ItemTransferMessage message : snapshot.queued) {
		SwapItemTransferLE(message);
		const auto bytes = std::as_bytes(std::span(&message, 1));
		data.insert(data.end(), bytes.begin(), bytes.end());
	}
	const auto earlyCount = std::count_if(snapshot.earlyVotes.begin(), snapshot.earlyVotes.end(), [](const auto &entry) { return entry.has_value(); });
	data.push_back(static_cast<std::byte>(earlyCount));
	for (const auto &early : snapshot.earlyVotes) {
		if (!early)
			continue;
		auto message = early->message;
		SwapItemTransferLE(message);
		const auto bytes = std::as_bytes(std::span(&message, 1));
		data.insert(data.end(), bytes.begin(), bytes.end());
		data.push_back(static_cast<std::byte>(early->votes));
		data.push_back(static_cast<std::byte>(early->rejected ? 1 : 0));
	}
	return data;
}

std::optional<ItemTransferCoordinator::Snapshot> DecodeItemTransfers(std::span<const std::byte> data)
{
	constexpr size_t HeaderSize = 4 * (3 + 2 * ItemTransferCoordinator::PeerCount) + 3;
	if (data.size() < HeaderSize + 4 || data.size() > MaxItemTransferSnapshotSize)
		return std::nullopt;
	ItemTransferCoordinator::Snapshot snapshot;
	size_t offset = 0;
	const auto read32 = [&]() {
		uint32_t value;
		std::memcpy(&value, data.data() + offset, sizeof(value));
		offset += sizeof(value);
		return Swap32LE(value);
	};
	snapshot.issued = read32();
	snapshot.finished = read32();
	snapshot.applied = read32();
	snapshot.quiescing = static_cast<uint8_t>(data[offset++]);
	snapshot.coordinator = static_cast<uint8_t>(data[offset++]);
	if (snapshot.coordinator >= ItemTransferCoordinator::PeerCount)
		return std::nullopt;
	for (uint32_t &epoch : snapshot.epochs)
		epoch = read32();
	for (uint32_t &request : snapshot.requests)
		request = read32();
	const size_t count = static_cast<uint8_t>(data[offset++]);
	if (count > ItemTransferCoordinator::WindowSize || snapshot.finished > snapshot.applied || snapshot.applied > snapshot.issued
	    || snapshot.issued - snapshot.finished != count
	    || data.size() < HeaderSize + count * (sizeof(ItemTransferMessage) + 3) + 4)
		return std::nullopt;
	std::array<bool, ItemTransferCoordinator::WindowSize> serials { };
	for (size_t i = 0; i < count; ++i) {
		ItemTransferCoordinator::Entry entry;
		std::memcpy(&entry.message, data.data() + offset, sizeof(entry.message));
		offset += sizeof(entry.message);
		SwapItemTransferLE(entry.message);
		entry.votes = static_cast<uint8_t>(data[offset++]);
		const uint8_t flags = static_cast<uint8_t>(data[offset++]);
		if ((flags & 96U) == 96U)
			return std::nullopt;
		entry.rejected = (flags & 1U) != 0;
		entry.decisionSent = (flags & 2U) != 0;
		entry.applied = (flags & 4U) != 0;
		entry.committed = (flags & 8U) != 0;
		entry.finalizeSent = (flags & 16U) != 0;
		if ((flags & 96U) != 0)
			entry.deferredDecision = (flags & 32U) != 0;
		entry.deferredFinalize = (flags & 128U) != 0;
		entry.acknowledgements = static_cast<uint8_t>(data[offset++]);
		const auto &message = entry.message;
		if (message.command != CMD_ITEMTRANSFER || message.phase != ItemTransferPhase::Offer
		    || message.kind > ItemTransferKind::MonsterReward || message.actor >= ItemTransferCoordinator::PeerCount
		    || message.request == 0 || message.serial <= snapshot.finished || message.serial > snapshot.issued
		    || (message.kind == ItemTransferKind::Creation ? message.epoch > snapshot.epochs[message.actor] : message.epoch != snapshot.epochs[message.actor]) || (message.voters & Bit(snapshot.coordinator)) == 0
		    || (message.kind != ItemTransferKind::Creation && (message.voters & Bit(message.actor)) == 0) || (entry.votes & ~message.voters) != 0 || (entry.acknowledgements & ~message.voters) != 0
		    || entry.applied != (message.serial <= snapshot.applied))
			return std::nullopt;
		const size_t index = message.serial - snapshot.finished - 1;
		if (serials[index])
			return std::nullopt;
		serials[index] = true;
		snapshot.entries[i] = entry;
	}
	uint16_t deferredCount;
	std::memcpy(&deferredCount, data.data() + offset, sizeof(deferredCount));
	offset += sizeof(deferredCount);
	deferredCount = Swap16LE(deferredCount);
	if (deferredCount > ItemTransferCoordinator::DeferredSize || data.size() - offset < deferredCount * sizeof(ItemTransferMessage) + 2)
		return std::nullopt;
	for (size_t i = 0; i < deferredCount; ++i) {
		ItemTransferMessage message;
		std::memcpy(&message, data.data() + offset, sizeof(message));
		offset += sizeof(message);
		SwapItemTransferLE(message);
		if (message.command != CMD_ITEMTRANSFER || message.phase != ItemTransferPhase::Offer
		    || message.kind != ItemTransferKind::Creation || message.item.bCmd != CMD_SPAWNITEM
		    || message.actor >= ItemTransferCoordinator::PeerCount || message.request == 0 || message.serial > snapshot.finished
		    || message.epoch > snapshot.epochs[message.actor])
			return std::nullopt;
		for (const auto &previous : snapshot.deferred) {
			if (previous.actor == message.actor && previous.epoch == message.epoch && previous.request == message.request)
				return std::nullopt;
		}
		snapshot.deferred.push_back(message);
	}
	uint16_t queuedCount;
	std::memcpy(&queuedCount, data.data() + offset, sizeof(queuedCount));
	offset += sizeof(queuedCount);
	queuedCount = Swap16LE(queuedCount);
	if (queuedCount > ItemTransferCoordinator::MaxQueuedRequests || data.size() - offset < queuedCount * sizeof(ItemTransferMessage) + 1)
		return std::nullopt;
	for (size_t i = 0; i < queuedCount; ++i) {
		ItemTransferMessage message;
		std::memcpy(&message, data.data() + offset, sizeof(message));
		offset += sizeof(message);
		SwapItemTransferLE(message);
		if (message.command != CMD_ITEMTRANSFER || message.phase != ItemTransferPhase::Request || message.kind > ItemTransferKind::MonsterReward
		    || message.actor >= ItemTransferCoordinator::PeerCount || message.request == 0 || message.serial != 0 || message.voters != 0
		    || (message.kind == ItemTransferKind::Creation ? message.epoch > snapshot.epochs[message.actor] : message.epoch != snapshot.epochs[message.actor])
		    || (message.epoch == snapshot.epochs[message.actor] && message.request > snapshot.requests[message.actor]))
			return std::nullopt;
		if (std::any_of(snapshot.queued.begin(), snapshot.queued.end(), [&](const auto &previous) { return previous.actor == message.actor && previous.epoch == message.epoch && previous.request == message.request; }))
			return std::nullopt;
		snapshot.queued.push_back(message);
	}
	const auto earlyCount = static_cast<uint8_t>(data[offset++]);
	if (earlyCount > ItemTransferCoordinator::WindowSize || data.size() - offset != earlyCount * (sizeof(ItemTransferMessage) + 2))
		return std::nullopt;
	for (size_t i = 0; i < earlyCount; ++i) {
		ItemTransferCoordinator::Entry early;
		std::memcpy(&early.message, data.data() + offset, sizeof(early.message));
		offset += sizeof(early.message);
		SwapItemTransferLE(early.message);
		early.votes = static_cast<uint8_t>(data[offset++]);
		const uint8_t rejected = static_cast<uint8_t>(data[offset++]);
		early.rejected = rejected != 0;
		const auto &message = early.message;
		if (message.command != CMD_ITEMTRANSFER || message.phase != ItemTransferPhase::Offer || message.kind != ItemTransferKind::MonsterReward
		    || message.actor >= ItemTransferCoordinator::PeerCount || message.request == 0 || message.epoch != snapshot.epochs[message.actor]
		    || message.serial <= snapshot.issued || message.serial - snapshot.issued > ItemTransferCoordinator::WindowSize
		    || (message.voters & Bit(snapshot.coordinator)) == 0 || (message.voters & Bit(message.actor)) == 0
		    || early.votes == 0 || (early.votes & ~message.voters) != 0 || rejected > 1
		    || std::any_of(snapshot.earlyVotes.begin(), snapshot.earlyVotes.end(), [&](const auto &previous) { return previous && previous->message.serial == message.serial; }))
			return std::nullopt;
		snapshot.earlyVotes[i] = early;
	}
	return snapshot;
}

} // namespace devilution
