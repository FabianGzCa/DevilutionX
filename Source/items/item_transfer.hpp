#pragma once

#include <array>
#include <cstdint>
#include <deque>
#include <functional>
#include <optional>
#include <span>
#include <vector>

#include "msg.h"

namespace devilution {

enum class ItemTransferPhase : uint8_t {
	Request,
	Offer,
	Ready,
	Reject,
	Commit,
	Cancel,
	Applied,
	Finalize,
	LeaveIntent,
	LeaveAccepted,
	Relocate,
};
enum class ItemTransferKind : uint8_t {
	ManualPickup,
	AutoPickup,
	HeldDrop,
	BodyDrop,
	GoldDrop,
	Creation,
	MonsterReward,
};

inline bool IsPrivateItemTransfer(ItemTransferKind kind)
{
	return kind != ItemTransferKind::Creation && kind != ItemTransferKind::MonsterReward;
}

#pragma pack(push, 1)
struct ItemTransferMessage {
	_cmd_id command = CMD_ITEMTRANSFER;
	ItemTransferPhase phase = ItemTransferPhase::Request;
	ItemTransferKind kind = ItemTransferKind::ManualPickup;
	uint8_t actor = 0;
	uint8_t voters = 0;
	uint8_t level = 0;
	uint32_t epoch = 0;
	uint32_t request = 0;
	uint32_t serial = 0;
	TCmdPItem item { };
};
#pragma pack(pop)

/** Bounded, pipelined admission for extended item transfers. World and inventory
 * resources are reserved before voting and are changed only by an ordered commit.
 */
class ItemTransferCoordinator {
public:
	static constexpr size_t PeerCount = 8;
	static constexpr size_t WindowSize = 16;
	static constexpr size_t DeferredSize = 2 * MAXITEMS;
	static constexpr size_t QueueSize = PeerCount * (MAXITEMS + 2);
	static constexpr size_t MaxQueuedRequests = QueueSize + WindowSize;

	struct Hooks {
		std::function<bool(const ItemTransferMessage &)> reserve;
		std::function<bool(const ItemTransferMessage &)> commit;
		std::function<void(const ItemTransferMessage &)> release;
		std::function<void(const ItemTransferMessage &)> send;
		std::function<void()> terminate;
		std::function<void(const ItemTransferMessage &, bool)> complete;
	};
	struct Entry {
		ItemTransferMessage message;
		uint8_t votes = 0;
		uint8_t acknowledgements = 0;
		bool rejected = false;
		bool reserved = false;
		bool voted = false;
		bool decisionSent = false;
		bool applied = false;
		bool committed = false;
		bool finalizeSent = false;
		std::optional<bool> deferredDecision { };
		bool deferredFinalize = false;
	};
	struct Snapshot {
		uint32_t issued = 0;
		uint32_t finished = 0;
		uint32_t applied = 0;
		uint8_t quiescing = 0;
		uint8_t coordinator = 0;
		std::array<uint32_t, PeerCount> epochs { };
		std::array<uint32_t, PeerCount> requests { };
		std::array<std::optional<Entry>, WindowSize> entries;
		std::array<std::optional<Entry>, WindowSize> earlyVotes;
		std::vector<ItemTransferMessage> deferred;
		std::deque<ItemTransferMessage> queued;
	};

	void Reset(uint8_t local, Hooks hooks);
	void Receive(uint8_t sender, ItemTransferMessage message, uint8_t active);
	void Pump(uint8_t active);
	void Leave(uint8_t player);
	bool Busy() const;
	bool Stopped() const { return stopped_; }
	bool Quiescing(uint8_t actor) const { return (state_.quiescing & (1U << actor)) != 0; }
	bool CanLeave() const;
	void BeginLeave();
	bool Pending(uint8_t actor) const;
	uint32_t Epoch(uint8_t actor) const { return state_.epochs[actor]; }
	const Snapshot &State() const { return state_; }
	bool Restore(const Snapshot &snapshot, uint8_t active);

private:
	uint8_t local_ = 0;
	uint8_t lastActive_ = 1;
	Hooks hooks_;
	Snapshot state_;
	bool stopped_ = false;
	uint8_t leaveApprovals_ = 0;
	uint8_t acknowledgedDepartures_ = 0;
	bool retryDeferred_ = false;

	void Vote(Entry &entry);
	void Decide();
	void Finalize();
	void DrainDecisions();
	void AcknowledgeDepartures();
	void Fail();
	Entry *Find(uint32_t serial);
};

constexpr size_t MaxItemTransferSnapshotSize = 4 * (3 + 2 * ItemTransferCoordinator::PeerCount) + 3
    + ItemTransferCoordinator::WindowSize * (sizeof(ItemTransferMessage) + 3)
    + 1 + ItemTransferCoordinator::WindowSize * (sizeof(ItemTransferMessage) + 2)
    + 4 + (ItemTransferCoordinator::DeferredSize + ItemTransferCoordinator::MaxQueuedRequests) * sizeof(ItemTransferMessage);
std::vector<std::byte> EncodeItemTransfers(const ItemTransferCoordinator::Snapshot &snapshot);
std::optional<ItemTransferCoordinator::Snapshot> DecodeItemTransfers(std::span<const std::byte> data);
void SwapItemTransferLE(ItemTransferMessage &message);

} // namespace devilution
