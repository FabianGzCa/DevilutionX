#include "monsters/party_balance.hpp"

#include <algorithm>
#include <cstring>
#include <limits>
#include <map>

#include "monsters/party_scaling.hpp"
#include "msg.h"
#include "multi.h"
#include "player.h"
#include "utils/endian_swap.hpp"

namespace devilution {
namespace {
bool Enabled = false;
uint32_t NextDamageReceipt = 0;
uint32_t NextSpawnReceipt = 0;
std::map<uint8_t, std::array<uint64_t, MonsterStorageCapacity>> LocalRewardReceipts;

bool ValidInfo(uint16_t type, uint8_t unique)
{
	return type < MonstersData.size() && (unique == static_cast<uint8_t>(UniqueMonsterType::None) || unique < UniqueMonstersData.size());
}
void PublishHealth(Monster &monster, PartyMonsterState &state, uint8_t participants)
{
	const auto scale = PartyScaling::ForSession(ExtendedSessionRules, std::max<uint8_t>(1, participants));
	monster.maxHitPoints = scale->HitPoints(Fixed26_6::fromRaw(state.nativeMax), monster.type().type == MT_GOLEM);
	monster.hitPoints = PartyMonsterHitPoints(state, participants);
	monster.partyLastHitPoints = monster.hitPoints;
	monster.partyParticipants = (state.flags & PartyMonsterGolem) != 0 ? 4 : std::max<uint8_t>(4, participants);
	monster.whoHit = PlayerExperienceMaskFromByte(state.contributors);
}
// Native regeneration changes local health between published values. Keep that change
// before the agreed health replaces it, as a classic engine keeps it before a remote hit.
void FoldLocalHealth(const Monster &monster, PartyMonsterState &state)
{
	if ((state.flags & PartyMonsterDead) != 0 || monster.hitPoints == monster.partyLastHitPoints)
		return;
	const int64_t delta = static_cast<int64_t>(monster.partyLastHitPoints.raw()) - monster.hitPoints.raw();
	state.damage = std::clamp<int64_t>(state.damage + delta * (4 * PartyDamageUnits / monster.partyParticipants), 0, static_cast<int64_t>(state.nativeMax) * PartyDamageUnits);
}
void ApplyReceipt(uint8_t sender, const PartyDamageMessage &message)
{
	auto &level = GetPartyLevelState(message.level);
	auto &state = level.monsters[message.monster];
	if (state.flags != 0 && state.generation != message.generation)
		return;
	if ((state.flags & PartyMonsterKnown) == 0) {
		state.generation = message.generation;
		state.nativeMax = message.nativeMax;
		state.type = message.type;
		state.unique = message.unique;
		state.x = message.x;
		state.y = message.y;
		state.flags = PartyMonsterKnown | (message.type == MT_GOLEM ? PartyMonsterGolem : 0);
	}
	if (message.receipt <= state.receipts[sender] || (state.flags & PartyMonsterRewarded) != 0)
		return;
	const bool local = CurrentPartyLevel() == message.level && MyPlayer != nullptr && Monsters[message.monster].partyGeneration == message.generation;
	if (local)
		FoldLocalHealth(Monsters[message.monster], state);
	state.receipts[sender] = message.receipt;
	if (message.playerDamage != 0)
		state.contributors |= static_cast<uint8_t>(1U << sender);
	const auto numerator = (state.flags & PartyMonsterGolem) != 0 ? 4 : std::max<uint8_t>(4, message.participants);
	const int64_t units = static_cast<int64_t>(message.damage) * (4 * PartyDamageUnits / numerator);
	state.damage = std::min(static_cast<int64_t>(state.nativeMax) * PartyDamageUnits, state.damage + units);
	if ((state.flags & PartyMonsterDead) == 0 && PartyMonsterHitPoints(state, level.participants).whole() <= 0) {
		state.flags |= PartyMonsterDead;
		state.killer = sender;
		state.deathParticipants = std::max(message.participants, level.participants);
		if ((state.flags & PartyMonsterGolem) != 0)
			state.flags |= PartyMonsterRewarded;
	}
	SyncPartyMonsterDelta(message.level, message.monster);
	if (CurrentPartyLevel() == message.level && MyPlayer != nullptr) {
		auto &monster = Monsters[message.monster];
		if (monster.partyGeneration != message.generation)
			return;
		PublishHealth(monster, state, level.participants);
		if (monster.hasNoLife() && !monster.partyDeathStarted && sender != MyPlayerId)
			StartPartyMonsterDeath(monster, Players[sender]);
	}
}
} // namespace

Fixed26_6 PartyMonsterHitPoints(const PartyMonsterState &state, uint8_t participants)
{
	if ((state.flags & PartyMonsterDead) != 0)
		return Fixed26_6::fromInt(0);
	const int64_t remaining = std::max<int64_t>(0, static_cast<int64_t>(state.nativeMax) * PartyDamageUnits - state.damage);
	const int64_t denominator = 4 * PartyDamageUnits;
	const int64_t raw = (remaining * ((state.flags & PartyMonsterGolem) != 0 ? 4 : std::max<uint8_t>(4, participants)) + denominator - 1) / denominator;
	return Fixed26_6::fromRaw(static_cast<int32_t>(std::min<int64_t>(raw, std::numeric_limits<int32_t>::max())));
}

PartyMonsterWire EncodePartyMonster(const PartyMonsterState &state)
{
	PartyMonsterWire wire {};
	wire.generation = Swap64LE(state.generation);
	wire.nativeMax = SwapSigned32LE(state.nativeMax);
	wire.damage = Swap64LE(static_cast<uint64_t>(state.damage));
	for (size_t i = 0; i < state.receipts.size(); ++i)
		wire.receipts[i] = Swap32LE(state.receipts[i]);
	wire.type = Swap16LE(state.type);
	wire.unique = state.unique;
	wire.contributors = state.contributors;
	wire.killer = state.killer;
	wire.deathParticipants = state.deathParticipants;
	wire.flags = state.flags;
	wire.x = state.x;
	wire.y = state.y;
	return wire;
}
std::optional<PartyMonsterState> DecodePartyMonster(PartyMonsterWire wire)
{
	PartyMonsterState state;
	state.generation = Swap64LE(wire.generation);
	state.nativeMax = SwapSigned32LE(wire.nativeMax);
	const uint64_t damage = Swap64LE(wire.damage);
	if (wire.flags == PartyMonsterExpected && state.generation != 0 && state.nativeMax == 0 && damage == 0
	    && std::all_of(std::begin(wire.receipts), std::end(wire.receipts), [](uint32_t value) { return value == 0; })
	    && wire.type == 0 && wire.unique == static_cast<uint8_t>(UniqueMonsterType::None)
	    && wire.contributors == 0 && wire.killer == 0 && wire.deathParticipants == 0 && wire.x == 0 && wire.y == 0) {
		state.flags = PartyMonsterExpected;
		return state;
	}
	if (wire.flags == 0 || (wire.flags & ~15U) != 0 || (wire.flags & PartyMonsterKnown) == 0 || state.generation == 0
	    || ((wire.flags & PartyMonsterGolem) != 0) != (Swap16LE(wire.type) == MT_GOLEM)
	    || state.nativeMax <= 0 || damage > static_cast<uint64_t>(state.nativeMax) * PartyDamageUnits
	    || wire.killer >= 8 || wire.deathParticipants > 8 || !ValidInfo(Swap16LE(wire.type), wire.unique)
	    || ((wire.flags & PartyMonsterDead) != 0 && wire.deathParticipants == 0)
	    || ((wire.flags & PartyMonsterRewarded) != 0 && (wire.flags & PartyMonsterDead) == 0)
	    || !InDungeonBounds({ wire.x, wire.y }))
		return std::nullopt;
	state.damage = static_cast<int64_t>(damage);
	for (size_t i = 0; i < state.receipts.size(); ++i)
		state.receipts[i] = Swap32LE(wire.receipts[i]);
	state.type = Swap16LE(wire.type);
	state.unique = wire.unique;
	state.contributors = wire.contributors;
	state.killer = wire.killer;
	state.deathParticipants = wire.deathParticipants;
	state.flags = wire.flags;
	state.x = wire.x;
	state.y = wire.y;
	return state;
}
void SwapPartyDamageLE(PartyDamageMessage &message)
{
	message.epoch = Swap32LE(message.epoch);
	message.receipt = Swap32LE(message.receipt);
	message.monster = Swap16LE(message.monster);
	message.damage = Swap32LE(message.damage);
	message.generation = Swap64LE(message.generation);
	message.nativeMax = SwapSigned32LE(message.nativeMax);
	message.type = Swap16LE(message.type);
}
void SwapPartyRewardLE(PartyMonsterReward &message)
{
	message.monster = Swap16LE(message.monster);
	message.generation = Swap64LE(message.generation);
	message.nativeMax = SwapSigned32LE(message.nativeMax);
	message.type = Swap16LE(message.type);
}
std::vector<std::byte> EncodePartyLevel(const PartyLevelState &state)
{
	std::vector<std::byte> bytes;
	const auto append = [&](const auto &value) {
		const auto data = std::as_bytes(std::span(&value, 1));
		bytes.insert(bytes.end(), data.begin(), data.end());
	};
	append(state.participants);
	append(state.lootRemainder);
	for (uint32_t value : state.entryEpochs)
		append(Swap32LE(value));
	for (uint32_t value : state.entrySerials)
		append(Swap32LE(value));
	for (uint64_t value : state.spawns)
		append(Swap64LE(value));
	const auto count = std::count_if(state.monsters.begin(), state.monsters.end(), [](const auto &monster) { return monster.flags != 0; });
	append(Swap16LE(static_cast<uint16_t>(count)));
	for (size_t i = 0; i < state.monsters.size(); ++i) {
		if (state.monsters[i].flags == 0)
			continue;
		append(Swap16LE(static_cast<uint16_t>(i)));
		append(EncodePartyMonster(state.monsters[i]));
	}
	append(Swap16LE(static_cast<uint16_t>(state.pending.size())));
	for (const auto &pending : state.pending) {
		append(pending.sender);
		auto message = pending.message;
		SwapPartyDamageLE(message);
		append(message);
	}
	return bytes;
}
std::optional<PartyLevelState> DecodePartyLevel(std::span<const std::byte> bytes)
{
	if (bytes.size() > MaxPartyLevelSnapshotSize)
		return std::nullopt;
	size_t offset = 0;
	const auto read = [&](auto &value) {
		if (bytes.size() - offset < sizeof(value))
			return false;
		std::memcpy(&value, bytes.data() + offset, sizeof(value));
		offset += sizeof(value);
		return true;
	};
	PartyLevelState state;
	if (!read(state.participants) || !read(state.lootRemainder) || state.participants > 8 || state.lootRemainder >= 4)
		return std::nullopt;
	for (uint32_t &value : state.entryEpochs) {
		if (!read(value)) return std::nullopt;
		value = Swap32LE(value);
	}
	for (uint32_t &value : state.entrySerials) {
		if (!read(value)) return std::nullopt;
		value = Swap32LE(value);
	}
	for (uint64_t &value : state.spawns) {
		if (!read(value)) return std::nullopt;
		value = Swap64LE(value);
		if ((value >> 63) != 0) return std::nullopt;
	}
	uint16_t count;
	if (!read(count) || Swap16LE(count) > state.monsters.size())
		return std::nullopt;
	count = Swap16LE(count);
	for (unsigned i = 0; i < count; ++i) {
		uint16_t index;
		PartyMonsterWire wire;
		if (!read(index) || !read(wire) || Swap16LE(index) >= state.monsters.size())
			return std::nullopt;
		const auto monster = DecodePartyMonster(wire);
		if (!monster || state.monsters[Swap16LE(index)].flags != 0)
			return std::nullopt;
		state.monsters[Swap16LE(index)] = *monster;
	}
	if (!read(count) || Swap16LE(count) > MaxPendingPartyDamage)
		return std::nullopt;
	count = Swap16LE(count);
	for (unsigned i = 0; i < count; ++i) {
		PendingPartyDamage pending;
		if (!read(pending.sender) || !read(pending.message))
			return std::nullopt;
		SwapPartyDamageLE(pending.message);
		if (pending.sender >= 8 || pending.message.command != CMD_PARTYMONSTDAMAGE || pending.message.level == 0 || !IsValidLevelForMultiplayer(pending.message.level) || pending.message.monster >= MonsterStorageCapacity || pending.message.participants == 0 || pending.message.participants > 8
		    || pending.message.playerDamage > 1 || pending.message.receipt == 0 || pending.message.damage > static_cast<uint32_t>(std::numeric_limits<int32_t>::max()) || pending.message.nativeMax <= 0
		    || !ValidInfo(pending.message.type, pending.message.unique) || !InDungeonBounds({ pending.message.x, pending.message.y }))
			return std::nullopt;
		state.pending.push_back(pending);
	}
	if (offset != bytes.size())
		return std::nullopt;
	return state;
}

void InitPartyBalance()
{
	Enabled = gbIsMultiplayer && sgGameInitInfo.rules == ExtendedSessionRules;
	NextDamageReceipt = 0;
	NextSpawnReceipt = 0;
	LocalRewardReceipts.clear();
}
void ResetPartyBalance()
{
	Enabled = false;
	NextDamageReceipt = 0;
	NextSpawnReceipt = 0;
	LocalRewardReceipts.clear();
}
bool IsPartyBalanceEnabled() { return Enabled; }
uint8_t CurrentPartyLevel() { return setlevel ? NUMLEVELS + static_cast<uint8_t>(setlvlnum) : currlevel; }

void CommitPartyLevel(uint8_t level, uint8_t participants, uint8_t coordinator, uint32_t epoch, uint32_t serial)
{
	if (!Enabled || level == 0 || !IsValidLevelForMultiplayer(level) || participants == 0 || participants > 8 || coordinator >= Players.size())
		return;
	auto &state = GetPartyLevelState(level);
	if (epoch < state.entryEpochs[coordinator] || (epoch == state.entryEpochs[coordinator] && serial <= state.entrySerials[coordinator]))
		return;
	state.entryEpochs[coordinator] = epoch;
	state.entrySerials[coordinator] = serial;
	state.participants = std::max(state.participants, participants);
	if (CurrentPartyLevel() == level)
		CapturePartyMonsters();
	DrainPartyDamage(level);
	if (CurrentPartyLevel() == level)
		RestorePartyMonsters();
}

void InitializePartyMonster(Monster &monster)
{
	monster.partyNativeMax = monster.maxHitPoints;
	monster.partyGeneration = (uint64_t { 1 } << 63) | monster.rndItemSeed;
	if (Enabled) {
		const auto &saved = GetPartyLevelState(CurrentPartyLevel()).monsters[monster.getId()];
		if ((saved.flags & PartyMonsterExpected) != 0 || (gbBufferMsgs == 1 && saved.flags != 0))
			monster.partyGeneration = saved.generation;
	}
	monster.partyLastHitPoints = monster.hitPoints;
	monster.partyParticipants = 4;
	monster.partyDeathStarted = false;
	if (Enabled && gbBufferMsgs == 0 && MyPlayer != nullptr && MyPlayer->pLvlLoad == 0 && leveltype != DTYPE_TOWN) {
		auto &state = GetPartyLevelState(CurrentPartyLevel()).monsters[monster.getId()];
		state = {};
		ReconcilePartyMonster(monster);
	}
}
uint64_t AssignPartySpawnGeneration(Monster &monster)
{
	if (NextSpawnReceipt == (1U << 29) - 1 || MultiplayerSlotEpoch(MyPlayerId) >= (1U << 31)) {
		gbGameDestroyed = true;
		gbRunGame = false;
		return 0;
	}
	const uint64_t generation = (static_cast<uint64_t>(MultiplayerSlotEpoch(MyPlayerId)) << 32)
	    | (static_cast<uint64_t>(++NextSpawnReceipt) << 3) | MyPlayerId;
	auto &state = GetPartyLevelState(CurrentPartyLevel()).monsters[monster.getId()];
	state.generation = generation;
	GetPartyLevelState(CurrentPartyLevel()).spawns[MyPlayerId] = generation;
	monster.partyGeneration = generation;
	return generation;
}

void ReconcilePartyMonster(Monster &monster)
{
	if (!Enabled || monster.partyNativeMax.raw() <= 0)
		return;
	auto &level = GetPartyLevelState(CurrentPartyLevel());
	auto &state = level.monsters[monster.getId()];
	if ((state.flags & PartyMonsterKnown) == 0 || state.generation != monster.partyGeneration) {
		state = {};
		state.generation = monster.partyGeneration;
		state.nativeMax = monster.partyNativeMax.raw();
		state.flags = PartyMonsterKnown | (monster.type().type == MT_GOLEM ? PartyMonsterGolem : 0);
		state.type = monster.type().type;
		state.unique = static_cast<uint8_t>(monster.uniqueType);
		state.x = monster.position.tile.x;
		state.y = monster.position.tile.y;
	}
	FoldLocalHealth(monster, state);
	if ((state.flags & PartyMonsterDead) == 0 && monster.hasNoLife()) {
		state.flags |= PartyMonsterDead;
		state.killer = MyPlayerId;
		state.deathParticipants = std::max<uint8_t>(1, level.participants);
		if (monster.isPlayerMinion())
			state.flags |= PartyMonsterRewarded;
	}
	state.contributors |= PlayerExperienceMaskToByte(monster.whoHit);
	state.x = monster.position.tile.x;
	state.y = monster.position.tile.y;
	PublishHealth(monster, state, level.participants);
	SyncPartyMonsterDelta(CurrentPartyLevel(), monster.getId());
}
void CapturePartyMonsters()
{
	if (!Enabled || leveltype == DTYPE_TOWN)
		return;
	for (size_t i = 0; i < ActiveMonsterCount; ++i)
		ReconcilePartyMonster(Monsters[ActiveMonsters[i]]);
}
void RestorePartyMonsters()
{
	if (!Enabled || leveltype == DTYPE_TOWN)
		return;
	auto &level = GetPartyLevelState(CurrentPartyLevel());
	for (size_t i = 0; i < ActiveMonsterCount; ++i) {
		auto &monster = Monsters[ActiveMonsters[i]];
		auto &state = level.monsters[monster.getId()];
		if ((state.flags & PartyMonsterKnown) == 0) {
			monster.maxHitPoints = monster.partyNativeMax;
			monster.hitPoints = monster.partyNativeMax;
			monster.partyLastHitPoints = monster.hitPoints;
			monster.partyParticipants = 4;
			ReconcilePartyMonster(monster);
			continue;
		}
		if (state.generation != monster.partyGeneration)
			continue;
		PublishHealth(monster, state, level.participants);
	}
}

bool IsPartyDamageAuthority(const Monster *attacker)
{
	if (!Enabled)
		return true;
	if (attacker != nullptr && attacker->isPlayerMinion())
		return attacker->goalVar3 == MyPlayerId;
	for (const auto &player : Players) {
		if (player.plractive && !player._pLvlChanging && GetLevelForMultiplayer(player) == CurrentPartyLevel())
			return player.getId() == MyPlayerId;
	}
	return false;
}

bool ApplyPartyMonsterDamage(Monster &monster, Fixed26_6 damage, bool playerDamage)
{
	if (!Enabled)
		return false;
	if (damage.raw() < 0)
		return true;
	if (NextDamageReceipt == std::numeric_limits<uint32_t>::max()) {
		gbGameDestroyed = true;
		gbRunGame = false;
		return true;
	}
	ReconcilePartyMonster(monster);
	auto &level = GetPartyLevelState(CurrentPartyLevel());
	auto &state = level.monsters[monster.getId()];
	if ((state.flags & PartyMonsterDead) != 0)
		return true;
	PartyDamageMessage message {};
	message.command = CMD_PARTYMONSTDAMAGE;
	message.level = CurrentPartyLevel();
	message.participants = std::max<uint8_t>(1, level.participants);
	message.epoch = MultiplayerSlotEpoch(MyPlayerId);
	message.receipt = ++NextDamageReceipt;
	message.monster = static_cast<uint16_t>(monster.getId());
	message.damage = damage.raw();
	message.generation = state.generation;
	message.nativeMax = state.nativeMax;
	message.type = state.type;
	message.unique = state.unique;
	message.x = state.x;
	message.y = state.y;
	message.playerDamage = playerDamage ? 1 : 0;
	ApplyReceipt(MyPlayerId, message);
	NetSendPartyDamage(message);
	return true;
}
void ReceivePartyMonsterDamage(uint8_t sender, const PartyDamageMessage &message)
{
	if (!Enabled || message.command != CMD_PARTYMONSTDAMAGE || sender >= Players.size() || !Players[sender].plractive || !IsValidLevelForMultiplayer(message.level) || message.level == 0
	    || message.monster >= MonsterLimit() || message.participants == 0 || message.participants > 8 || message.receipt == 0 || message.damage > static_cast<uint32_t>(std::numeric_limits<int32_t>::max())
	    || message.generation == 0 || message.playerDamage > 1 || message.nativeMax <= 0 || message.epoch != MultiplayerSlotEpoch(sender) || !ValidInfo(message.type, message.unique)
	    || !InDungeonBounds({ message.x, message.y }))
		return;
	auto &level = GetPartyLevelState(message.level);
	const auto &target = level.monsters[message.monster];
	const bool futureSpawn = ((message.generation >> 63) == 0 && message.generation > level.spawns[message.generation & 7U])
	    || (target.flags != 0 && target.generation != message.generation);
	if (futureSpawn && ((message.generation >> 63) != 0 || message.generation <= level.spawns[message.generation & 7U] || (message.generation >> 32) != MultiplayerSlotEpoch(message.generation & 7U)))
		return;
	if (level.participants < message.participants || futureSpawn) {
		if (std::any_of(level.pending.begin(), level.pending.end(), [&](const auto &pending) { return pending.sender == sender && pending.message.receipt == message.receipt && pending.message.epoch == message.epoch; }))
			return;
		if (level.pending.size() >= MaxPendingPartyDamage) {
			gbGameDestroyed = true;
			gbRunGame = false;
			return;
		}
		level.pending.push_back({ sender, message });
		return;
	}
	ApplyReceipt(sender, message);
}
bool ResetPartyMonster(uint8_t level, size_t monster, uint64_t generation)
{
	if (!Enabled || monster >= MonsterLimit())
		return false;
	auto &encounter = GetPartyLevelState(level);
	auto &state = encounter.monsters[monster];
	if (state.flags != 0 && state.generation == generation) {
		encounter.spawns[generation & 7U] = std::max(encounter.spawns[generation & 7U], generation);
		return true;
	}
	if ((generation >> 63) != 0 || generation <= encounter.spawns[generation & 7U])
		return false;
	encounter.spawns[generation & 7U] = generation;
	state = {};
	state.flags = PartyMonsterExpected;
	state.generation = generation;
	return true;
}
void DrainPartyDamage(uint8_t level)
{
	auto &state = GetPartyLevelState(level);
	auto pending = std::move(state.pending);
	state.pending.clear();
	for (const auto &damage : pending)
		ReceivePartyMonsterDamage(damage.sender, damage.message);
}
void LeavePartyBalance(uint8_t player)
{
	// Existing receipt counters are reset by the delta owner alongside contribution
	// bits; the item-transfer slot epoch rejects frames from the old occupant.
	ClearPartyContributor(player);
}

bool HasPendingPartyRewards()
{
	if (!Enabled || MyPlayer == nullptr || leveltype == DTYPE_TOWN)
		return false;
	const auto &state = GetPartyLevelState(CurrentPartyLevel());
	return std::any_of(state.monsters.begin(), state.monsters.end(), [](const auto &monster) {
		return (monster.flags & (PartyMonsterDead | PartyMonsterRewarded)) == PartyMonsterDead;
	});
}
bool IsPartyMonsterRewardPending(const Monster &monster)
{
	if (!Enabled || monster.type().type == MT_GOLEM)
		return false;
	const auto &state = GetPartyLevelState(CurrentPartyLevel()).monsters[monster.getId()];
	return (state.flags & (PartyMonsterDead | PartyMonsterRewarded)) == PartyMonsterDead;
}
void ProcessPartyRewards()
{
	if (!Enabled || gbBufferMsgs != 0 || leveltype == DTYPE_TOWN || HasLocalPartyRewardTransfer())
		return;
	const auto &level = GetPartyLevelState(CurrentPartyLevel());
	for (size_t i = 0; i < level.monsters.size(); ++i) {
		const auto &state = level.monsters[i];
		if ((state.flags & (PartyMonsterDead | PartyMonsterRewarded)) != PartyMonsterDead)
			continue;
		uint8_t publisher = state.killer;
		if (!Players[publisher].plractive) {
			publisher = 255;
			for (const auto &player : Players) {
				if (player.plractive && GetLevelForMultiplayer(player) == CurrentPartyLevel() && !player._pLvlChanging) {
					publisher = static_cast<uint8_t>(player.getId());
					break;
				}
			}
		}
		if (publisher != MyPlayerId)
			continue;
		PartyMonsterReward reward { CMD_PARTYREWARD, static_cast<uint16_t>(i), state.generation, state.nativeMax, state.type, state.unique, state.deathParticipants, state.x, state.y };
		NetSendPartyReward(CurrentPartyLevel(), reward);
		break;
	}
}
bool IsPartyRewardValid(const PartyMonsterReward &reward)
{
	return reward.command == CMD_PARTYREWARD && reward.type != MT_GOLEM && reward.monster < MonsterLimit() && reward.participants > 0 && reward.participants <= 8 && reward.nativeMax > 0 && reward.generation != 0
	    && ValidInfo(reward.type, reward.unique) && InDungeonBounds({ reward.x, reward.y });
}
bool ReservePartyReward(uint8_t level, const PartyMonsterReward &reward)
{
	if (!IsPartyRewardValid(reward))
		return false;
	auto &state = GetPartyLevelState(level).monsters[reward.monster];
	if ((state.flags & PartyMonsterRewarded) != 0 || (state.flags != 0 && state.generation != reward.generation))
		return false;
	if ((state.flags & PartyMonsterKnown) != 0 && (state.nativeMax != reward.nativeMax || state.type != reward.type || state.unique != reward.unique))
		return false;
	if (CurrentPartyLevel() == level && MyPlayer != nullptr) {
		const auto &monster = Monsters[reward.monster];
		if (monster.partyGeneration != reward.generation || monster.type().type != reward.type || static_cast<uint8_t>(monster.uniqueType) != reward.unique)
			return false;
	}
	state.generation = reward.generation;
	state.nativeMax = reward.nativeMax;
	state.type = reward.type;
	state.unique = reward.unique;
	state.flags |= PartyMonsterKnown | PartyMonsterDead;
	state.deathParticipants = reward.participants;
	state.x = reward.x;
	state.y = reward.y;
	if (CurrentPartyLevel() == level && MyPlayer != nullptr) {
		auto &monster = Monsters[reward.monster];
		if (monster.partyGeneration != reward.generation || monster.type().type != reward.type || static_cast<uint8_t>(monster.uniqueType) != reward.unique)
			return false;
		monster.hitPoints = Fixed26_6::fromInt(0);
		monster.partyLastHitPoints = monster.hitPoints;
		if (!monster.partyDeathStarted)
			StartPartyMonsterDeath(monster, Players[state.killer]);
	}
	return true;
}
bool CommitPartyReward(uint8_t levelId, const PartyMonsterReward &reward, uint8_t publisher)
{
	auto &level = GetPartyLevelState(levelId);
	auto &state = level.monsters[reward.monster];
	if (state.generation != reward.generation)
		return false;
	if ((state.flags & PartyMonsterRewarded) != 0)
		return true;
	const auto scale = PartyScaling::ForSession(ExtendedSessionRules, reward.participants);
	if (!scale)
		return false;
	const bool local = CurrentPartyLevel() == levelId && leveltype != DTYPE_TOWN;
	const bool eligible = IsPartyRandomLoot(reward.type, reward.unique);
	const auto roll = scale->LootRoll(level.lootRemainder, eligible);
	if (!roll)
		return false;
	level.lootRemainder = roll->remainder;
	state.flags |= PartyMonsterDead | PartyMonsterRewarded;
	if (local) {
		LocalRewardReceipts[levelId][reward.monster] = reward.generation;
		auto &monster = Monsters[reward.monster];
		monster.whoHit = PlayerExperienceMaskFromByte(state.contributors);
		FinalizePartyMonsterReward(monster, publisher == MyPlayerId, reward.participants, roll->extraRolls);
	}
	return true;
}

bool RestorePartyRewardReceipt(uint8_t level, const PartyMonsterReward &reward)
{
	if (!IsPartyRewardValid(reward))
		return false;
	if (CurrentPartyLevel() != level || leveltype == DTYPE_TOWN || LocalRewardReceipts[level][reward.monster] == reward.generation)
		return true;
	const auto &state = GetPartyLevelState(level).monsters[reward.monster];
	if (state.generation != reward.generation || (state.flags & PartyMonsterRewarded) == 0)
		return false;
	LocalRewardReceipts[level][reward.monster] = reward.generation;
	const auto &monster = Monsters[reward.monster];
	const auto scale = PartyScaling::ForSession(ExtendedSessionRules, reward.participants);
	AddPlrMonstExper(monster.level(sgGameInitInfo.nDifficulty), scale->Experience(monster.exp(sgGameInitInfo.nDifficulty)), PlayerExperienceMaskFromByte(state.contributors));
	return true;
}

} // namespace devilution
