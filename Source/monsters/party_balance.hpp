/**
 * @file monsters/party_balance.hpp
 *
 * Extended level balance, damage receipts and exactly-once monster rewards.
 */
#pragma once

#include <array>
#include <cstdint>
#include <optional>
#include <span>
#include <vector>

#include "monster.h"

namespace devilution {

/**
 * Ledger units per native raw hit point. A raw hit point at scale N is worth
 * 2 * PartyDamageUnits / (N + 1) units, or PartyDamageUnits / N for Diablo;
 * LCM(1..9) keeps both exact for N = 1..8.
 */
constexpr int64_t PartyDamageUnits = 2520;
constexpr size_t MaxPendingPartyDamage = 512;
constexpr uint8_t PartyMonsterKnown = 1;
constexpr uint8_t PartyMonsterDead = 2;
constexpr uint8_t PartyMonsterRewarded = 4;
constexpr uint8_t PartyMonsterGolem = 8;
constexpr uint8_t PartyMonsterExpected = 16;

struct PartyMonsterState {
	uint64_t generation = 0;
	int32_t nativeMax = 0;
	int64_t damage = 0;
	std::array<uint32_t, 8> receipts {};
	uint16_t type = 0;
	uint8_t unique = static_cast<uint8_t>(UniqueMonsterType::None);
	uint8_t contributors = 0;
	uint8_t killer = 0;
	uint8_t deathParticipants = 0;
	uint8_t flags = 0;
	uint8_t x = 0;
	uint8_t y = 0;
};

#pragma pack(push, 1)
struct PartyMonsterWire {
	uint64_t generation;
	int32_t nativeMax;
	uint64_t damage;
	uint32_t receipts[8];
	uint16_t type;
	uint8_t unique;
	uint8_t contributors;
	uint8_t killer;
	uint8_t deathParticipants;
	uint8_t flags;
	uint8_t x;
	uint8_t y;
};
struct PartyDamageMessage {
	uint8_t command;
	uint8_t level;
	uint8_t participants;
	uint32_t epoch;
	uint32_t receipt;
	uint16_t monster;
	uint32_t damage;
	uint64_t generation;
	int32_t nativeMax;
	uint16_t type;
	uint8_t unique;
	uint8_t x;
	uint8_t y;
	uint8_t playerDamage = 1;
};
struct PartyMonsterReward {
	uint8_t command;
	uint16_t monster;
	uint64_t generation;
	int32_t nativeMax;
	uint16_t type;
	uint8_t unique;
	uint8_t participants;
	uint8_t x;
	uint8_t y;
};
#pragma pack(pop)

struct PendingPartyDamage {
	uint8_t sender;
	PartyDamageMessage message;
};
struct PartyLevelState {
	uint8_t participants = 0;
	uint8_t lootRemainder = 0;
	std::array<uint32_t, 8> entryEpochs {};
	std::array<uint32_t, 8> entrySerials {};
	std::array<uint64_t, 8> spawns {};
	std::array<PartyMonsterState, MonsterStorageCapacity> monsters;
	std::vector<PendingPartyDamage> pending;
};
constexpr size_t MaxPartyLevelSnapshotSize = 2 + 2 + 128 + MonsterStorageCapacity * (2 + sizeof(PartyMonsterWire))
    + 2 + MaxPendingPartyDamage * (1 + sizeof(PartyDamageMessage));

PartyMonsterWire EncodePartyMonster(const PartyMonsterState &state);
std::optional<PartyMonsterState> DecodePartyMonster(PartyMonsterWire wire);
void SwapPartyDamageLE(PartyDamageMessage &message);
void SwapPartyRewardLE(PartyMonsterReward &message);
std::vector<std::byte> EncodePartyLevel(const PartyLevelState &state);
std::optional<PartyLevelState> DecodePartyLevel(std::span<const std::byte> bytes);
Fixed26_6 PartyMonsterHitPoints(const PartyMonsterState &state, uint8_t participants);
/** Ledger units of one raw hit point at a scale; golems always keep the native one. */
int64_t PartyDamageUnitsPerHitPoint(const PartyMonsterState &state, uint8_t participants);
/** Less than one whole native hit point remains. Independent of the scale, so every peer agrees. */
bool IsPartyMonsterDepleted(const PartyMonsterState &state);

void InitPartyBalance();
void ResetPartyBalance();
bool IsPartyBalanceEnabled();
uint8_t CurrentPartyLevel();
void CommitPartyLevel(uint8_t level, uint8_t participants, uint8_t coordinator, uint32_t epoch, uint32_t serial);
/** Follow the players present on each occupied level, rescaling health when they arrive or leave. */
void UpdatePartyParticipants();
void InitializePartyMonster(Monster &monster);
uint64_t AssignPartySpawnGeneration(Monster &monster);
void CapturePartyMonsters();
void RestorePartyMonsters();
void ReconcilePartyMonster(Monster &monster);
bool IsPartyDamageAuthority(const Monster *attacker = nullptr);
bool ApplyPartyMonsterDamage(Monster &monster, Fixed26_6 damage, bool playerDamage);
void ReceivePartyMonsterDamage(uint8_t sender, const PartyDamageMessage &message);
bool ResetPartyMonster(uint8_t level, size_t monster, uint64_t generation);
void DrainPartyDamage(uint8_t level);
bool RestorePartyRewardReceipt(uint8_t level, const PartyMonsterReward &reward);
void LeavePartyBalance(uint8_t player);
void ProcessPartyRewards();
bool HasPendingPartyRewards();
bool IsPartyMonsterRewardPending(const Monster &monster);
bool ReservePartyReward(uint8_t level, const PartyMonsterReward &reward);
bool CommitPartyReward(uint8_t level, const PartyMonsterReward &reward, uint8_t publisher);
bool IsPartyRewardValid(const PartyMonsterReward &reward);

} // namespace devilution
