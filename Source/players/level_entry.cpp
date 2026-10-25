#include "players/level_entry.hpp"

#include <algorithm>
#include <bit>
#include <cstdlib>
#include <iterator>
#include <vector>

#include "engine/path.h"
#include "levels/dun_tile_data.hpp"
#include "levels/tile_properties.hpp"
#include "levels/trigs.h"
#include "lighting.h"
#include "missiles.h"
#include "multi.h"
#include "player.h"
#include "players/validation.hpp"
#include "plrmsg.h"
#include "portal.h"
#include "utils/endian_swap.hpp"
#include "utils/format.hpp"
#include "utils/language.h"

namespace devilution {
namespace {

constexpr uint8_t Bit(uint8_t player) { return static_cast<uint8_t>(1U << player); }
uint8_t Coordinator(uint8_t active) { return static_cast<uint8_t>(std::countr_zero(active)); }

bool Matches(const LevelEntryMessage &a, const LevelEntryMessage &b)
{
	if (a.epoch != b.epoch || a.members != b.members || a.serial != b.serial || a.authority != b.authority || a.participants != b.participants
	    || a.voters != b.voters || a.level != b.level || a.setLevel != b.setLevel)
		return false;
	for (size_t i = 0; i < LevelEntryCapacity; ++i) {
		if (a.tickets[i] != b.tickets[i])
			return false;
	}
	return true;
}

bool OnLevel(const LevelEntryMessage &message)
{
	return message.level == (setlevel ? static_cast<uint8_t>(setlvlnum) : currlevel) && (message.setLevel != 0) == setlevel;
}

size_t LevelIndex(const LevelEntryMessage &message)
{
	return message.setLevel != 0 ? NUMLEVELS + message.level - 1 : message.level;
}

LevelEntryCoordinator Entries;
uint32_t LocalTicket = 0;

LevelEntryPeers Peers()
{
	LevelEntryPeers peers;
	for (uint8_t i = 0; i < Players.size(); ++i) {
		const Player &player = Players[i];
		if (!player.plractive)
			continue;
		peers.active |= Bit(i);
		peers.epochs[i] = MultiplayerSlotEpoch(i);
		peers.level[i] = player.plrlevel;
		peers.setLevel[i] = player.plrIsOnSetLevel;
		if (!player._pLvlChanging || Entries.Pending(i))
			peers.ready |= Bit(i);
	}
	return peers;
}

bool EntryTile(Point position, uint8_t participants, bool landing = true)
{
	// Player transparency routines inspect the surrounding 3x3 area.
	if (position.x <= 0 || position.y <= 0 || position.x >= MAXDUNX - 1 || position.y >= MAXDUNY - 1 || !IsTileWalkable(position))
		return false;
	if (dMonster[position.x][position.y] != 0)
		return false;
	const int occupant = std::abs(dPlayer[position.x][position.y]);
	if (occupant > static_cast<int>(LevelEntryCapacity))
		return false;
	if (occupant != 0 && (participants & Bit(static_cast<uint8_t>(occupant - 1))) == 0)
		return false;
	for (const Player &player : Players) {
		if (player.plractive && player.isOnActiveLevel() && !player._pLvlChanging && !player.hasNoLife()
		    && (participants & Bit(static_cast<uint8_t>(player.getId()))) == 0
		    && (player.position.tile == position || player.position.future == position))
			return false;
	}
	if (!landing)
		return true;
	for (int i = 0; i < numtrigs; ++i) {
		if (trigs[i].position.WalkingDistance(position) < 2)
			return false;
	}
	for (const Missile &missile : Missiles) {
		if (missile._mitype == MissileID::TownPortal && missile.duration > 0
		    && (missile.position.tile == position || missile.position.tile + Displacement(1, 1) == position))
			return false;
	}
	for (const Portal &portal : Portals) {
		if (portal.open && portal.setlvl == setlevel && portal.level == (setlevel ? static_cast<int>(setlvlnum) : currlevel)
		    && (portal.position == position || portal.position + Displacement(1, 1) == position))
			return false;
	}
	return true;
}

bool UpdateWorld(const LevelEntryMessage &message, LevelEntryStage stage)
{
	if (stage == LevelEntryStage::Commit && IsPartyBalanceEnabled()) {
		const uint8_t level = message.setLevel != 0 ? NUMLEVELS + message.level : message.level;
		CommitPartyLevel(level, static_cast<uint8_t>(std::popcount(message.members)), message.serial & 7U, message.epoch, message.serial);
	}
	if (stage == LevelEntryStage::Reserve) {
		if (!OnLevel(message) || message.outcome != LevelEntryOutcome::Placed)
			return true;
		const auto hasExit = [&](Point position) {
			return std::any_of(std::begin(PathDirs), std::end(PathDirs), [&](Displacement offset) {
				const Point exit = position + offset;
				if (!EntryTile(exit, message.participants) || !CanStep(position, exit))
					return false;
				for (uint8_t other = 0; other < LevelEntryCapacity; ++other) {
					if ((message.participants & Bit(other)) != 0 && exit == Point(message.x[other], message.y[other]))
						return false;
				}
				return true;
			});
		};
		for (uint8_t i = 0; i < Players.size(); ++i) {
			if ((message.participants & Bit(i)) == 0)
				continue;
			const Point position { message.x[i], message.y[i] };
			if (!IsNetPlayerValid(i) || !EntryTile(position, message.participants) || !hasExit(position))
				return false;
		}
		for (const Player &resident : Players) {
			if (!resident.plractive || !resident.isOnActiveLevel() || resident._pLvlChanging || resident.hasNoLife()
			    || (message.participants & Bit(static_cast<uint8_t>(resident.getId()))) != 0)
				continue;
			const bool hadExit = std::any_of(std::begin(PathDirs), std::end(PathDirs), [&](Displacement offset) {
				const Point exit = resident.position.tile + offset;
				return EntryTile(exit, message.participants) && CanStep(resident.position.tile, exit);
			});
			if (hadExit && !hasExit(resident.position.tile))
				return false;
		}
	}
	for (uint8_t i = 0; i < Players.size(); ++i) {
		if ((message.participants & Bit(i)) == 0)
			continue;
		Player &player = Players[i];
		if (stage == LevelEntryStage::Release) {
			if (player.isOnActiveLevel())
				FixPlrWalkTags(player);
			continue;
		}
		if (stage == LevelEntryStage::Prepare) {
			if (player.isOnActiveLevel()) {
				FixPlrWalkTags(player);
				DeactivateVision(i);
			}
			if (message.setLevel != 0)
				player.setLevel(static_cast<_setlevels>(message.level));
			else
				player.setLevel(message.level);
			if (!player.plractive && player._pName[0] != '\0') {
				ResetPlayerGFX(player);
				player.plractive = true;
				++gbActivePlayers;
				EventPlrMsg(FormatRuntime(_("Player '{:s}' (level {:d}) just joined the game"), player._pName, player.getCharacterLevel()));
			}
			player._pLvlChanging = true;
			player._pInvincible = true;
			player._pmode = PM_NEWLVL;
			continue;
		}
		if (message.outcome != LevelEntryOutcome::Placed)
			continue;
		player.position.tile = { message.x[i], message.y[i] };
		player.position.future = player.position.tile;
		SetPlayerOld(player);
		if (player.isOnActiveLevel()) {
			// Reservations block walking and monsters until the common commit.
			player.occupyTile(player.position.tile, false);
		}
		if (stage != LevelEntryStage::Commit)
			continue;
		player._pLvlChanging = false;
		player._pInvincible = false;
		if (player.isOnActiveLevel()) {
			SetPlrAnims(player);
			if (!player.hasNoLife()) {
				StartStand(player, Direction::South);
			} else {
				player._pgfxnum &= ~0xFU;
				player._pmode = PM_DEATH;
				NewPlrAnim(player, player_graphic::Death, Direction::South);
				player.AnimInfo.currentFrame = static_cast<int8_t>(player.AnimInfo.numberOfFrames - 2);
				dFlags[player.position.tile.x][player.position.tile.y] |= DungeonFlag::DeadPlayer;
			}
			ActivateVision(player.position.tile, player._pLightRad, i);
		}
		if (&player == MyPlayer) {
			ViewPosition = player.position.tile;
			PlrDoTrans(player.position.tile);
			ChangeLightXY(player.lightId, player.position.tile);
		}
	}
	return true;
}

void Send(LevelEntryMessage message)
{
	message.command = CMD_LEVELENTRY;
	FlushNetMessageBuffers();
	SwapLevelEntryMessageLE(message);
	NetSendHiPri(MyPlayerId, reinterpret_cast<const std::byte *>(&message), sizeof(message));
}

} // namespace

std::optional<LevelEntryMessage> LevelEntryCoordinator::StateFor(uint8_t player) const
{
	if (player >= requests_.size() || requests_[player].ticket == 0)
		return std::nullopt;
	const Request &request = requests_[player];
	LevelEntryMessage message { };
	message.phase = request.pending ? LevelEntryPhase::Request : LevelEntryPhase::Commit;
	message.participants = Bit(player);
	message.level = request.level;
	message.setLevel = request.setLevel ? 1 : 0;
	message.tickets[player] = request.ticket;
	message.x[player] = static_cast<uint8_t>(request.position.x);
	message.y[player] = static_cast<uint8_t>(request.position.y);
	return message;
}

bool LevelEntryCoordinator::Pending(uint8_t player) const { return player < requests_.size() && requests_[player].pending; }
bool LevelEntryCoordinator::Blocked(uint8_t player) const { return player < requests_.size() && requests_[player].blocked; }
bool LevelEntryCoordinator::IsResolving(uint8_t level, bool special) const
{
	return transaction_ && transaction_->level == level && (transaction_->setLevel != 0) == special;
}

void LevelEntryCoordinator::Release(Update update)
{
	if (transaction_)
		update(*transaction_, LevelEntryStage::Release);
	transaction_.reset();
	deferredCommit_.reset();
	votes_ = 0;
	rejected_ = false;
	proposed_ = false;
	sent_ = false;
}

void LevelEntryCoordinator::Cancel(uint8_t player, Update update, bool disconnected)
{
	if (player >= requests_.size())
		return;
	requests_[player] = { };
	if (disconnected)
		seenSerials_[player] = 0;
	if (issuedAssignment_ && ((issuedAssignment_->voters & Bit(player)) != 0 || (issuedAssignment_->serial & 7U) == player)) {
		const uint32_t serial = issuedAssignment_->serial;
		const uint8_t issuer = serial & 7U;
		if (!disconnected || issuer != player)
			seenSerials_[issuer] = std::max(seenSerials_[issuer], serial);
		issuedAssignment_.reset();
		sent_ = false;
	}
	for (auto &message : earlyProposals_) {
		if (message && ((message->serial & 7U) == player || (message->voters & Bit(player)) != 0))
			message.reset();
	}
	for (auto &message : earlyVotes_) {
		if (message && ((message->serial & 7U) == player || (message->voters & Bit(player)) != 0))
			message.reset();
	}
	if (transaction_ && ((transaction_->serial & 7U) == player || (transaction_->voters & Bit(player)) != 0))
		Release(update);
}

std::optional<LevelEntryMessage> LevelEntryCoordinator::Poll(uint8_t local, const LevelEntryPeers &peers, Resolve resolve)
{
	++ticks_;
	if (peers.active == 0 || local >= LevelEntryCapacity || sent_)
		return std::nullopt;
	if (!transaction_) {
		if (local != Coordinator(peers.active))
			return std::nullopt;
		for (uint8_t first = 0; first < requests_.size(); ++first) {
			const Request &request = requests_[first];
			if (!request.pending || static_cast<int32_t>(ticks_ - request.retryAt) < 0 || (peers.active & Bit(first)) == 0)
				continue;
			LevelEntryMessage assignment { };
			assignment.phase = LevelEntryPhase::Assign;
			// Low bits identify the coordinator; elections cannot reuse another
			// coordinator's transaction even if its last assignment was delayed.
			assignment.serial = ((std::max(outgoingSerial_, seenSerials_[local]) & ~7U) + 8U) | local;
			outgoingSerial_ = assignment.serial;
			assignment.epoch = peers.epochs[local];
			assignment.level = request.level;
			assignment.setLevel = request.setLevel ? 1 : 0;
			for (uint8_t i = 0; i < requests_.size(); ++i) {
				const Request &other = requests_[i];
				if (other.pending && (peers.active & Bit(i)) != 0 && other.level == request.level && other.setLevel == request.setLevel) {
					assignment.participants |= Bit(i);
					assignment.tickets[i] = other.ticket;
					assignment.x[i] = static_cast<uint8_t>(other.position.x);
					assignment.y[i] = static_cast<uint8_t>(other.position.y);
				}
				if ((peers.ready & Bit(i)) != 0 && peers.level[i] == request.level && peers.setLevel[i] == request.setLevel)
					assignment.voters |= Bit(i);
				if ((peers.active & Bit(i)) != 0 && peers.level[i] == request.level && peers.setLevel[i] == request.setLevel)
					assignment.members |= Bit(i);
			}
			assignment.voters |= assignment.participants;
			assignment.members |= assignment.participants;
			const uint8_t residents = assignment.voters & ~assignment.participants;
			assignment.authority = Coordinator(residents != 0 ? residents : assignment.participants);
			issuedAssignment_ = assignment;
			sent_ = true;
			return assignment;
		}
		return std::nullopt;
	}
	if (local == Coordinator(peers.active) && ticks_ - transactionStart_ >= 200) {
		LevelEntryMessage abort = *transaction_;
		abort.phase = LevelEntryPhase::Commit;
		abort.outcome = LevelEntryOutcome::Retry;
		sent_ = true;
		return abort;
	}
	if (!proposed_ && local == transaction_->authority) {
		LevelEntryMessage proposal = *transaction_;
		proposal.phase = LevelEntryPhase::Propose;
		proposal.outcome = resolve(proposal, rejectedTiles_[LevelIndex(proposal)]) ? LevelEntryOutcome::Placed : LevelEntryOutcome::NoSpace;
		sent_ = true;
		return proposal;
	}
	if (proposed_ && local == Coordinator(peers.active) && votes_ == transaction_->voters) {
		LevelEntryMessage commit = *transaction_;
		commit.phase = LevelEntryPhase::Commit;
		if (rejected_)
			commit.outcome = LevelEntryOutcome::Retry;
		sent_ = true;
		return commit;
	}
	return std::nullopt;
}

std::optional<LevelEntryMessage> LevelEntryCoordinator::Receive(uint8_t sender, uint8_t local, const LevelEntryPeers &peers, const LevelEntryMessage &message, Update update)
{
	if (sender >= LevelEntryCapacity || local >= LevelEntryCapacity || message.phase > LevelEntryPhase::Commit || message.outcome > LevelEntryOutcome::Retry || message.setLevel > 1
	    || (message.setLevel != 0 ? message.level == 0 || message.level > SL_LAST : message.level >= NUMLEVELS))
		return std::nullopt;
	if (message.epoch != peers.epochs[message.phase == LevelEntryPhase::Request ? sender : (message.serial & 7U)])
		return std::nullopt;
	if (message.phase == LevelEntryPhase::Request) {
		if (message.participants != Bit(sender) || message.tickets[sender] == 0 || message.tickets[sender] <= requests_[sender].ticket
		    || !InDungeonBounds({ message.x[sender], message.y[sender] }))
			return std::nullopt;
		if (transaction_ && (transaction_->participants & Bit(sender)) != 0)
			Release(update);
		requests_[sender] = { message.tickets[sender], { message.x[sender], message.y[sender] }, ticks_, message.level, message.setLevel != 0, true, false };
		update(message, LevelEntryStage::Prepare);
		return std::nullopt;
	}
	if (message.phase == LevelEntryPhase::Assign) {
		if (message.members == 0 || (message.members & ~peers.active) != 0 || (message.participants & ~message.members) != 0 || (message.voters & ~message.members) != 0)
			return std::nullopt;
		if (peers.active == 0 || sender != Coordinator(peers.active) || (message.serial & 7U) != sender || message.serial <= seenSerials_[sender]
		    || message.participants == 0 || message.authority >= LevelEntryCapacity
		    || (message.participants & ~message.voters) != 0 || (message.voters & Bit(message.authority)) == 0)
			return std::nullopt;
		for (uint8_t i = 0; i < requests_.size(); ++i) {
			if ((message.participants & Bit(i)) != 0 && (message.tickets[i] == 0 || !InDungeonBounds({ message.x[i], message.y[i] })))
				return std::nullopt;
		}
		if (issuedAssignment_ && issuedAssignment_->serial == message.serial)
			issuedAssignment_.reset();
		seenSerials_[sender] = message.serial;
		if (sender == local && (message.voters & ~peers.active) != 0) {
			sent_ = false;
			return std::nullopt;
		}
		Release(update);
		transaction_ = message;
		transactionStart_ = ticks_;
		for (uint8_t i = 0; i < requests_.size(); ++i) {
			if ((message.participants & Bit(i)) == 0 || requests_[i].ticket > message.tickets[i])
				continue;
			const bool blocked = requests_[i].ticket == message.tickets[i] && requests_[i].blocked;
			requests_[i] = { message.tickets[i], { message.x[i], message.y[i] }, ticks_, message.level, message.setLevel != 0, true, blocked };
		}
		update(message, LevelEntryStage::Prepare);
		std::optional<LevelEntryMessage> reply;
		for (uint8_t peer = 0; peer < LevelEntryCapacity; ++peer) {
			if (earlyProposals_[peer] && earlyProposals_[peer]->serial == message.serial) {
				const auto proposal = *earlyProposals_[peer];
				earlyProposals_[peer].reset();
				reply = Receive(peer, local, peers, proposal, update);
			}
		}
		for (uint8_t peer = 0; peer < LevelEntryCapacity; ++peer) {
			if (earlyVotes_[peer] && earlyVotes_[peer]->serial == message.serial) {
				const auto vote = *earlyVotes_[peer];
				earlyVotes_[peer].reset();
				Receive(peer, local, peers, vote, update);
			}
		}
		return reply;
	}
	if (!transaction_ || !Matches(message, *transaction_)) {
		const uint8_t coordinator = message.serial & 7U;
		if (peers.active != 0 && coordinator == Coordinator(peers.active) && message.serial > seenSerials_[coordinator]
		    && message.serial - seenSerials_[coordinator] <= 8U * LevelEntryCapacity) {
			if (message.phase == LevelEntryPhase::Propose)
				earlyProposals_[sender] = message;
			if (message.phase == LevelEntryPhase::Vote)
				earlyVotes_[sender] = message;
		}
		return std::nullopt;
	}
	if (message.phase == LevelEntryPhase::Propose) {
		if (sender != message.authority || proposed_ || message.outcome > LevelEntryOutcome::NoSpace)
			return std::nullopt;
		bool accepted = true;
		for (uint8_t i = 0; i < requests_.size(); ++i) {
			if ((message.participants & Bit(i)) == 0)
				continue;
			accepted &= requests_[i].ticket == message.tickets[i];
			if (message.outcome != LevelEntryOutcome::Placed)
				continue;
			accepted &= message.x[i] > 0 && message.y[i] > 0 && message.x[i] < MAXDUNX - 1 && message.y[i] < MAXDUNY - 1;
			for (uint8_t j = 0; j < i; ++j) {
				if ((message.participants & Bit(j)) != 0 && message.x[i] == message.x[j] && message.y[i] == message.y[j])
					accepted = false;
			}
		}
		*transaction_ = message;
		proposed_ = true;
		sent_ = false;
		if (accepted)
			accepted = update(message, LevelEntryStage::Reserve);
		if ((message.voters & Bit(local)) == 0)
			return std::nullopt;
		LevelEntryMessage vote = message;
		vote.phase = LevelEntryPhase::Vote;
		vote.outcome = accepted ? message.outcome : LevelEntryOutcome::Retry;
		return vote;
	}
	if (message.phase == LevelEntryPhase::Vote) {
		if ((message.voters & Bit(sender)) == 0)
			return std::nullopt;
		votes_ |= Bit(sender);
		rejected_ |= message.outcome == LevelEntryOutcome::Retry;
		if (deferredCommit_ && votes_ == deferredCommit_->voters) {
			const auto commit = *deferredCommit_;
			deferredCommit_.reset();
			return Receive(commit.serial & 7U, local, peers, commit, update);
		}
		return std::nullopt;
	}
	if (message.phase != LevelEntryPhase::Commit || peers.active == 0 || sender != Coordinator(peers.active)
	    || (!proposed_ && (message.voters & Bit(local)) != 0 && message.outcome != LevelEntryOutcome::Retry))
		return std::nullopt;
	if (proposed_ && message.outcome != LevelEntryOutcome::Retry && message.outcome != transaction_->outcome)
		return std::nullopt;
	// The coordinator's commit is authoritative; peers never run a second search.
	if (message.outcome == LevelEntryOutcome::Placed) {
		if (proposed_ && (!std::equal(std::begin(message.x), std::end(message.x), std::begin(transaction_->x)) || !std::equal(std::begin(message.y), std::end(message.y), std::begin(transaction_->y))))
			return std::nullopt;
		for (uint8_t i = 0; i < LevelEntryCapacity; ++i) {
			if ((message.participants & Bit(i)) == 0)
				continue;
			if (message.x[i] == 0 || message.y[i] == 0 || message.x[i] >= MAXDUNX - 1 || message.y[i] >= MAXDUNY - 1)
				return std::nullopt;
			for (uint8_t j = 0; j < i; ++j) {
				if ((message.participants & Bit(j)) != 0 && message.x[i] == message.x[j] && message.y[i] == message.y[j])
					return std::nullopt;
			}
		}
		// Each voter flushes its previous damage before voting. Wait on every
		// connection so delayed old-scale hits settle before health is rebased.
		if ((message.voters & Bit(local)) != 0 && votes_ != message.voters) {
			deferredCommit_ = message;
			return std::nullopt;
		}
		update(message, LevelEntryStage::Commit);
	} else {
		update(message, LevelEntryStage::Release);
	}
	auto &rejectedTiles = rejectedTiles_[LevelIndex(message)];
	if (message.outcome == LevelEntryOutcome::Retry) {
		for (uint8_t i = 0; i < LevelEntryCapacity; ++i) {
			if ((message.participants & Bit(i)) == 0)
				continue;
			const Point position { message.x[i], message.y[i] };
			if (InDungeonBounds(position) && std::find(rejectedTiles.begin(), rejectedTiles.end(), position) == rejectedTiles.end())
				rejectedTiles.push_back(position);
		}
	} else {
		rejectedTiles.clear();
	}
	for (uint8_t i = 0; i < requests_.size(); ++i) {
		if ((message.participants & Bit(i)) == 0 || requests_[i].ticket != message.tickets[i])
			continue;
		if (message.outcome == LevelEntryOutcome::Placed)
			requests_[i].position = { message.x[i], message.y[i] };
		requests_[i].pending = message.outcome != LevelEntryOutcome::Placed;
		requests_[i].blocked = message.outcome == LevelEntryOutcome::NoSpace;
		requests_[i].retryAt = ticks_ + 20;
	}
	transaction_.reset();
	deferredCommit_.reset();
	votes_ = 0;
	rejected_ = false;
	proposed_ = false;
	sent_ = false;
	return std::nullopt;
}

bool FindLevelEntryPositions(LevelEntryMessage &message, std::span<const Point> rejected)
{
	if (!OnLevel(message))
		return false;
	std::array<bool, MAXDUNX * MAXDUNY> reserved { };
	std::array<bool, MAXDUNX * MAXDUNY> forbidden { };
	for (const Point position : rejected) {
		if (InDungeonBounds(position)) {
			reserved[position.y * MAXDUNX + position.x] = true;
			forbidden[position.y * MAXDUNX + position.x] = true;
		}
	}
	for (const Player &resident : Players) {
		if (!resident.plractive || !resident.isOnActiveLevel() || resident._pLvlChanging || resident.hasNoLife()
		    || (message.participants & Bit(static_cast<uint8_t>(resident.getId()))) != 0)
			continue;
		for (const Displacement offset : PathDirs) {
			const Point escape = resident.position.tile + offset;
			if (EntryTile(escape, message.participants) && CanStep(resident.position.tile, escape)) {
				reserved[escape.y * MAXDUNX + escape.x] = true;
				break;
			}
		}
	}
	for (uint8_t i = 0; i < LevelEntryCapacity; ++i) {
		if ((message.participants & Bit(i)) == 0)
			continue;
		const Point origin { message.x[i], message.y[i] };
		if (!InDungeonBounds(origin))
			return false;
		std::array<bool, MAXDUNX * MAXDUNY> visited { };
		std::vector<Point> queue { origin };
		visited[origin.y * MAXDUNX + origin.x] = true;
		std::optional<Point> chosen;
		for (size_t head = 0; head < queue.size() && !chosen; ++head) {
			const Point candidate = queue[head];
			const auto available = [&](Point tile) { return InDungeonBounds(tile) && !reserved[tile.y * MAXDUNX + tile.x] && EntryTile(tile, message.participants); };
			if (available(candidate)) {
				// Keep an escape tile free, including from subsequent entrants.
				for (const Displacement offset : PathDirs) {
					const Point escape = candidate + offset;
					if (available(escape) && CanStep(candidate, escape)) {
						reserved[escape.y * MAXDUNX + escape.x] = true;
						chosen = candidate;
						break;
					}
				}
			}
			for (const Displacement offset : PathDirs) {
				const Point next = candidate + offset;
				if (!InDungeonBounds(next) || visited[next.y * MAXDUNX + next.x] || origin.WalkingDistance(next) > 50
				    || forbidden[next.y * MAXDUNX + next.x] || !EntryTile(next, message.participants, false)
				    || (origin.WalkingDistance(next) > 2 && !EntryTile(next, message.participants)) || !CanStep(candidate, next))
					continue;
				visited[next.y * MAXDUNX + next.x] = true;
				queue.push_back(next);
			}
		}
		if (!chosen)
			return false;
		message.x[i] = static_cast<uint8_t>(chosen->x);
		message.y[i] = static_cast<uint8_t>(chosen->y);
		reserved[chosen->y * MAXDUNX + chosen->x] = true;
	}
	return true;
}

void SwapLevelEntryMessageLE(LevelEntryMessage &message)
{
	message.serial = Swap32LE(message.serial);
	message.epoch = Swap32LE(message.epoch);
	for (size_t i = 0; i < LevelEntryCapacity; ++i)
		message.tickets[i] = Swap32LE(message.tickets[i]);
}
void ResetLevelEntries()
{
	Entries = { };
	LocalTicket = 0;
}
void CancelLevelEntry(uint8_t player, bool disconnected) { Entries.Cancel(player, UpdateWorld, disconnected); }
bool IsLevelEntryPending(uint8_t player) { return sgGameInitInfo.rules == ExtendedSessionRules && Entries.Pending(player); }
bool IsLevelEntryBlocked(uint8_t player) { return sgGameInitInfo.rules == ExtendedSessionRules && Entries.Blocked(player); }
bool IsLevelEntryResolving() { return sgGameInitInfo.rules == ExtendedSessionRules && Entries.IsResolving(setlevel ? static_cast<uint8_t>(setlvlnum) : currlevel, setlevel); }
bool ShouldWaitForLevelEntry() { return sgGameInitInfo.rules == ExtendedSessionRules && (MyPlayer->_pLvlChanging || IsLevelEntryResolving()); }

bool RestoreLevelEntryPlayerInfo(Player &player)
{
	if (sgGameInitInfo.rules != ExtendedSessionRules)
		return false;
	const uint8_t id = static_cast<uint8_t>(player.getId());
	const auto admission = Entries.StateFor(id);
	if (!admission)
		return false;
	if (!player.plractive) {
		player.plractive = true;
		++gbActivePlayers;
	}
	if (admission->setLevel != 0)
		player.setLevel(static_cast<_setlevels>(admission->level));
	else
		player.setLevel(admission->level);
	player.position.tile = { admission->x[id], admission->y[id] };
	player.position.future = player.position.tile;
	SetPlayerOld(player);
	player._pLvlChanging = Entries.Pending(id);
	player._pInvincible = player._pLvlChanging;
	if (player._pLvlChanging && player.isOnActiveLevel())
		DeactivateVision(id);
	return !player._pLvlChanging;
}

void BeginLevelEntry()
{
	LevelEntryMessage request { };
	request.phase = LevelEntryPhase::Request;
	request.participants = Bit(MyPlayerId);
	request.epoch = MultiplayerSlotEpoch(MyPlayerId);
	request.tickets[MyPlayerId] = ++LocalTicket;
	request.level = MyPlayer->plrlevel;
	request.setLevel = MyPlayer->plrIsOnSetLevel ? 1 : 0;
	request.x[MyPlayerId] = MyPlayer->position.tile.x;
	request.y[MyPlayerId] = MyPlayer->position.tile.y;
	Send(request);
}
void ReceiveLevelEntry(uint8_t sender, LevelEntryMessage message)
{
	if (sgGameInitInfo.rules != ExtendedSessionRules)
		return;
	SwapLevelEntryMessageLE(message);
	if (IsPartyBalanceEnabled() && (message.voters & Bit(MyPlayerId)) == 0
	    && message.phase == LevelEntryPhase::Commit && message.outcome == LevelEntryOutcome::Placed
	    && Peers().active != 0 && sender == Coordinator(Peers().active) && message.epoch == MultiplayerSlotEpoch(sender)
	    && message.members != 0 && (message.members & ~Peers().active) == 0 && (message.participants & ~message.members) == 0
	    && message.serial != 0 && (message.serial & 7U) == sender
	    && message.setLevel <= 1 && (message.setLevel != 0 ? message.level > 0 && message.level <= SL_LAST : message.level < NUMLEVELS)) {
		const uint8_t level = message.setLevel != 0 ? NUMLEVELS + message.level : message.level;
		CommitPartyLevel(level, static_cast<uint8_t>(std::popcount(message.members)), sender, message.epoch, message.serial);
	}
	const bool wasBlocked = Entries.Blocked(MyPlayerId);
	if (const auto reply = Entries.Receive(sender, MyPlayerId, Peers(), message, UpdateWorld))
		Send(*reply);
	if (!wasBlocked && Entries.Blocked(MyPlayerId))
		EventPlrMsg(_("The entrance is blocked. Waiting for a free position."));
}
void ProcessLevelEntries()
{
	if (sgGameInitInfo.rules != ExtendedSessionRules || gbBufferMsgs != 0)
		return;
	if (const auto message = Entries.Poll(MyPlayerId, Peers(), FindLevelEntryPositions))
		Send(*message);
}

} // namespace devilution
