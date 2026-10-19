/**
 * @file players/level_entry.hpp
 *
 * Coordinated placement for negotiated extended multiplayer sessions.
 */
#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <vector>

#include <function_ref.hpp>

#include "engine/point.hpp"
#include "game_mode.hpp"

namespace devilution {

struct Player;

constexpr size_t LevelEntryCapacity = ExtendedSessionRules.maxPlayers;
enum class LevelEntryPhase : uint8_t {
	Request,
	Assign,
	Propose,
	Vote,
	Commit,
};
enum class LevelEntryOutcome : uint8_t {
	Placed,
	NoSpace,
	Retry,
};
enum class LevelEntryStage {
	Prepare,
	Reserve,
	Commit,
	Release,
};

#pragma pack(push, 1)
struct LevelEntryMessage {
	uint8_t command;
	LevelEntryPhase phase;
	uint32_t serial;
	uint8_t authority;
	uint8_t participants;
	uint8_t voters;
	LevelEntryOutcome outcome;
	uint8_t level;
	uint8_t setLevel;
	uint32_t tickets[LevelEntryCapacity];
	uint8_t x[LevelEntryCapacity];
	uint8_t y[LevelEntryCapacity];
};
#pragma pack(pop)
static_assert(sizeof(LevelEntryMessage) == 60);

struct LevelEntryPeers {
	uint8_t active = 0;
	uint8_t ready = 0;
	std::array<uint8_t, LevelEntryCapacity> level { };
	std::array<bool, LevelEntryCapacity> setLevel { };
};

/** State is independent of local map occupancy; only the designated authority searches. */
class LevelEntryCoordinator {
public:
	using Update = tl::function_ref<bool(const LevelEntryMessage &, LevelEntryStage)>;
	using Resolve = tl::function_ref<bool(LevelEntryMessage &, std::span<const Point>)>;

	std::optional<LevelEntryMessage> Poll(uint8_t localPlayer, const LevelEntryPeers &peers, Resolve resolve);
	std::optional<LevelEntryMessage> Receive(uint8_t sender, uint8_t localPlayer, const LevelEntryPeers &peers, const LevelEntryMessage &message, Update update);
	void Cancel(uint8_t player, Update update, bool disconnected = false);
	bool Pending(uint8_t player) const;
	std::optional<LevelEntryMessage> StateFor(uint8_t player) const;
	bool Blocked(uint8_t player) const;
	bool IsResolving(uint8_t level, bool setLevel) const;

private:
	struct Request {
		uint32_t ticket = 0;
		Point position { };
		uint32_t retryAt = 0;
		uint8_t level = 0;
		bool setLevel = false;
		bool pending = false;
		bool blocked = false;
	};
	std::array<Request, LevelEntryCapacity> requests_ { };
	std::optional<LevelEntryMessage> transaction_;
	std::optional<LevelEntryMessage> issuedAssignment_;
	std::array<std::optional<LevelEntryMessage>, LevelEntryCapacity> earlyProposals_ { };
	std::array<std::optional<LevelEntryMessage>, LevelEntryCapacity> earlyVotes_ { };
	std::array<std::vector<Point>, NUMLEVELS + SL_LAST> rejectedTiles_;
	std::array<uint32_t, LevelEntryCapacity> seenSerials_ { };
	uint32_t outgoingSerial_ = 0;
	uint32_t ticks_ = 0;
	uint32_t transactionStart_ = 0;
	uint8_t votes_ = 0;
	bool rejected_ = false;
	bool sent_ = false;
	bool proposed_ = false;
	void Release(Update update);
};

void ResetLevelEntries();
void BeginLevelEntry();
void ProcessLevelEntries();
void ReceiveLevelEntry(uint8_t sender, LevelEntryMessage message);
void CancelLevelEntry(uint8_t player, bool disconnected = false);
bool RestoreLevelEntryPlayerInfo(Player &player);
bool IsLevelEntryPending(uint8_t player);
bool IsLevelEntryBlocked(uint8_t player);
bool IsLevelEntryResolving();
bool ShouldWaitForLevelEntry();
bool FindLevelEntryPositions(LevelEntryMessage &message, std::span<const Point> rejected = { });
void SwapLevelEntryMessageLE(LevelEntryMessage &message);

} // namespace devilution
