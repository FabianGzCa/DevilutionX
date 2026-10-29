# Extended party balance: revision 2

Balance revision 2 runs in negotiated extended protocol 3 sessions, rules
`(3, 8, 2)`. The pure functions in `monsters/party_scaling.*` define its
arithmetic; `monsters/party_balance.*` integrates creation, damage, level state
and rewards. It applies only to games created in Extended mode; classic games
keep the native rules.

Revision 1 kept native values for one to four players and only scaled five to
eight, by `N/4`, never lowering the scale while a level stayed in the session.
Groups of two to four therefore killed monsters two to four times as fast as one
player and shared the same experience, so each player levelled up two to four
times more slowly per kill; a level visited by eight kept that health after
players left or disconnected. Revision 2 replaces both rules.

## Native reference

The native engine has single-player and multiplayer branches, rather than a
separate difficulty formula for two, three and four participants:

- `InitMonster` and `PrepareUniqueMonst` halve base HP in single player, with a
  minimum of one. Multiplayer uses the full base value. Nightmare applies
  `3 * HP + 100`; Hell applies `4 * HP + 200` in multiplayer. Single-player
  Hellfire instead adds 50/100 after its halving. Original Diablo adds 100/200.
- Nightmare damage is `2 * (damage + 2)`; Hell damage is `4 * damage + 6`.
  Special damage follows the same rule. No party-count factor exists.
- Scatter population starts at `walkable_tiles / 30`; multiplayer adds half,
  subject to the original enemy limit. This does not grow with party size.
- Native monster XP is `E`, `2 * (E + 1000)`, `4 * (E + 1000)` across the three
  difficulties, doubled for uniques. `AddPlrMonstExper` divides that pool among
  contributors. Existing level adjustment, death checks and anti-power-level
  caps run afterward. A room's capacity is not the contributor count.
- Native random drops do not grow with player count. Fixed quest rewards are
  separate from random monster loot.

The engine tests hold seeds, level, equipment assumptions and difficulty fixed.
Before any party scaling, for one through four multiplayer participants, normal zombie and Diablo HP,
normal/special damage, XP and item/AI seeds are identical. The single-player
Diablo reference is 833/2599/3532 HP (Diablo) and 833/2549/3432 (Hellfire).
These edition checks exercise the HP branches, not a complete Hellfire campaign.

With dungeon seed 2588 and monster seed 10011, levels 1/5/9/13 produce
145/148/125/167 active monsters, including reserved golem slots, for classic
four and extended one/two/three/four/eight. Enemy state and final RNG state
match. In the normal-zombie loot sample (seeds 16000–16255), each of one through
four participants produces 105 drops per 256 kills in each difficulty, with
identical identities, values and final RNG state. This finite sample is a
regression reference, not a measured universal drop probability.

## Goal and choice of dimensions

Every player in a party of one to eight should gain experience and find random
loot at about the pace of one player alone, while the party clears monsters
faster than one player, but not trivially. Each player present on a level from
the second one on therefore adds half of the native health, experience pool and
random loot rolls of its monsters: a factor of `(N + 1) / 2`.

With equal damage per player, a party of `N` kills a monster in `(N + 1) / (2N)`
of the solo time: 75% with two players, 63% with four and 56% with eight. The
experience pool and the random rolls grow by the same factor, so the experience
and loot per player per minute match one player alone. The anti-power-levelling
caps, level difference adjustment and contributor split stay native.

Diablo is the exception. His health and experience pool grow by `N`: the party
needs as long as one player alone and each contributor still earns a solo kill.
His damage, like every monster's, stays native, so his attacks are spread over
the party rather than concentrated on one player.

Monster damage, accuracy, armour, resistances and density stay native. Raising
them would make each hit more dangerous than the original game; the party's
extra healing and body blocking already change survival, which the measurements
below report as player deaths and potion use.

## Participants

An encounter is the hostile population of one canonical multiplayer level,
including its dynamically spawned enemies. Set levels use their own level IDs.
Scaling applies to ordinary enemies, uniques and bosses. Player-controlled
golems keep native spell-derived health and damage and do not count as
participants.

`N` counts the active human slots on that level now. Dead players and players
loading the level count; players in town, on another level or disconnected do
not. Class, character level and gear do not weight a slot.

The count follows the party in both directions. A coordinated level entry sets
the destination's count before entrants resume. Every game tick each engine
recounts each occupied level that already has an entry commit, so a departure,
a return to town or a disconnection lowers health at once. A level nobody has
entered keeps no count, and an empty level keeps its last one until someone
returns. Nothing in this count needs agreement between engines: as described
below, deaths and rewards do not depend on it.

## Exact arithmetic

Compute native difficulty, unique and quest-specific stats first and keep that
reference. With `N` in 1–8, the numerator over two is `N + 1`, or `2N` for
Diablo; classic sessions always use 2.

| Dimension | Extended rule |
| --- | --- |
| Enemy maximum HP | `ceil(native_HP_raw * numerator / 2)` in 26.6 fixed point |
| Damage, accuracy, AC, resistance | Native values |
| Enemy density and map | Native generation and limits |
| XP pool | `floor(native_XP * numerator / 2)` before the native contributor split and caps |
| Random monster loot | Native roll, plus the deterministic extra-roll credits below |
| Fixed quest/unique rewards | Native exactly-once reward; no extra copies |
| Player minions | Native spell stats and rewards |

Intermediates use unsigned 64 bits; HP saturates at `INT32_MAX` raw and XP at
`UINT32_MAX`. Invalid counts, unsupported session rules and invalid loot
remainders are rejected; classic counts above four are invalid. One player in an
extended game sees every native value.

### Damage ledger and death

Each monster keeps its damage as exact work: one native raw HP has 2520 units,
and a hit of `d` raw HP at scale `N` adds `d * 5040 / (N + 1)` units, or
`d * 2520 / N` for Diablo. 2520 is divisible by every number from 1 to 9, so no
rounding occurs. Remaining raw HP at scale `N` is
`ceil((native_raw * 2520 - damage_units) * (N + 1) / 5040)`. A change of scale
therefore keeps the fraction of health already removed: a monster at half
health stays at half health whether players arrive or leave.

Each hit carries the scale its sender saw. The receiver adds it at that scale,
even if it counts a different party at that moment, so every engine holds the
same ledger. A monster dies when less than one whole native hit point remains
in that ledger, which does not depend on the scale; every engine therefore
agrees on the death and on the hit that caused it. While a monster is alive its
displayed health is at least one whole point at any scale. The killing hit's
scale becomes the reward's scale, and the reward message carries it to every
engine.

Native healing updates the ledger at the scale in which it occurred. The loaded
level owner reconciles healing through sync only when its receipt vector covers
the receiver's applied hits. Death is latched: a scale change never revives a
corpse, replays a kill or grants a reward.

### Random loot credits

Store one integer remainder `R` (0–1) per level. For each exactly-once eligible
random-loot death at scale `N`, add `N - 1`; extra rolls are
`floor((R + N - 1) / 2)` and the new remainder is `(R + N - 1) % 2`. On average
a death therefore gets `(N + 1) / 2` rolls including the native one: two deaths
earn 1/2/3/4/5/6/7 extra rolls with two to eight participants. A roll may
produce no item, just as a native roll may. Credits never accrue for minions,
no-drop monsters, fixed rewards, quest items or a replayed death. Random drops
from a unique or boss stay eligible when their native path is random.

The native roll runs first. The extra rolls continue its LCG state, one after
another, and the post-native-roll state is then restored, so the primary item
and the rest of the simulation see the native RNG sequence. The remainder and
reward advance once even when the floor is full. Item admission stays bounded.

## Synchronized lifecycle

| Event | Agreed state and effect |
| --- | --- |
| Generate/load level | Keep native difficulty/unique HP, then apply the level's count and ledger; never scale an already scaled maximum. |
| Place entrants | The lowest active slot freezes the destination human mask; its epoch/serial-qualified commit sets the count before entrants resume. |
| Players arrive, leave or disconnect | Each engine recounts every tick and republishes health at the new scale, keeping each monster's fraction. |
| Receive a hit | Check source slot epoch, monster generation and receipt; add normalized damage once at the hit's own scale. |
| Receive a hit for a future spawn | Retain it in a bounded queue; drain it after the spawn. |
| Leave, split or return to town | Clear the departed slot's contribution and receipt identity; keep damage, corpses and loot credits. |
| Die | Start native death visuals once; retain the monster slot while its reward is unresolved. |
| Reward | Killer publishes one request at a time; the item-transfer coordinator orders it and all receivers wait for every voter fence before XP/loot/quest commit. |
| Snapshot/reload | Import counts, credits, generations, receipts, pending damage and paid flags with world and item state; replay cannot apply damage or rewards twice. |

Each engine still applies native monster regeneration locally. Before a received
hit replaces the local health with the agreed one, the regenerated amount is
folded into the agreed damage, just as a classic engine subtracts a remote hit
from its regenerated health.

Each sender flushes earlier damage before its reward vote. A receiver defers
reward commit until it has received every vote, so cross-connection delivery
cannot omit an earlier contribution. The coordinator serializes all rewards,
giving each level a common loot-credit order. A loaded replacement publisher
handles a dead monster whose killer has already left; disconnects inside an
undecided ownership transaction keep the item protocol's explicit session
termination policy rather than guessing an outcome. A clean exit drains pending
rewards before leaving. Applied snapshot reward entries recover an existing
voter's missing private XP receipt without recreating loot or credits.

Dynamically spawned monsters carry a 64-bit generation composed of creator slot,
spawn counter and slot epoch; pregenerated monsters use a separate high-bit seed
namespace. Per-level spawn watermarks reject old incarnations while allowing a
hit from another connection to wait for its spawn. Source slot epochs reject
frames from previous occupants. Counter or queue exhaustion terminates the
session instead of wrapping or dropping accepted work. Golem melee damage is
published by its owner, shared monster/trap damage by the lowest loaded
resident; environmental damage does not make that publisher an XP contributor.

Extended level snapshots append a 134-byte empty balance body, up to 204 indexed
61-byte monster records (plus two-byte indices), and up to 512 pending damage
records. Damage messages are 35 bytes; monster rewards occupy 21 bytes inside
the existing 29-byte item payload. A periodic record combines the native
10-byte monster record with its 61-byte balance state; the extended sync header
adds nine bytes for the sender's count, epoch and membership digest. The count
in a sync header or snapshot is informative: receivers do not require it to
match their own. Scalars are explicitly little endian. Decoders bound lengths,
counts, identities, health, flags and positions, reject truncation and trailing
bytes, and keep the native 16-bit delta-transfer size ceiling. Snapshots that
would rewind known same-generation death, payment or receipt state are rejected
before publication. Classic snapshots have none of this suffix.

The eight-engine TCP test covers 4→5, 5→4, 7→8 and 8→1, a wounded boss, delayed
and duplicate damage, dead humans, resurrection placement, town and split
groups, return to a corpse, and snapshots before and after reward. Unit tests
cover departures and disconnections in the middle of a fight, peers that count
different parties, fractional boundaries, future spawns, malformed and truncated
codecs, slot identities, native golem HP, several extra loot rolls and deferred
reward fences. These fixtures do not replace campaign or real ZeroTier testing.

## Numerical tables

Multipliers are identical in Normal, Nightmare and Hell and apply after native
difficulty. The room's capacity never selects a row.

| Humans on the level | HP, XP and random rolls | Kill time against one player | Diablo HP and XP | Damage and density |
| --- | --- | --- | --- | --- |
| 1 | 1 | 100% | 1 | 1 |
| 2 | 1.5 | 75% | 2 | 1 |
| 3 | 2 | 67% | 3 | 1 |
| 4 | 2.5 | 63% | 4 | 1 |
| 5 | 3 | 60% | 5 | 1 |
| 6 | 3.5 | 58% | 6 | 1 |
| 7 | 4 | 57% | 7 | 1 |
| 8 | 4.5 | 56% | 8 | 1 |

Kill time assumes equal damage per player and ignores movement; Diablo's kill
time is 100% for every party size. Diablo's native multiplayer data is
1666/5098/6864 HP and 31666/65332/130664 XP in Normal/Nightmare/Hell. Pools
precede the contributor split and caps:

| Humans | Normal HP | Nightmare HP | Hell HP | Normal XP | Nightmare XP | Hell XP |
| --- | --- | --- | --- | --- | --- | --- |
| 1 | 1666 | 5098 | 6864 | 31666 | 65332 | 130664 |
| 2 | 3332 | 10196 | 13728 | 63332 | 130664 | 261328 |
| 4 | 6664 | 20392 | 27456 | 126664 | 261328 | 522656 |
| 8 | 13328 | 40784 | 54912 | 253328 | 522656 | 1045312 |

The engine damage benchmark holds the health of one spawn fixed and gives every
player the same damage per turn, about 200 turns for one player. In all three
difficulties a zombie's turns fall to 75/67/63/60/58/57/56% for two to eight
players, rounded to whole turns, and each player's experience per turn stays
within 2% of one player's before native integer rounding. Diablo needs the same
number of turns, within one, for every party size, and each contributor's share
is exactly the solo pool. This isolates HP workload; it does not model attack
animation, movement, AI, regeneration, latency, congestion or real builds.

## Revision 2 measurements

These are automated engine measurements with the tool described in
[multiplayer testing](multiplayer-testing.md#balance-measurements): one headless
engine process per player over loopback TCP, each driven by a bot that sends the
commands a player would. The bots clear the 32 reachable enemies nearest to the
entrance; larger parties reuse the solo target list of the same seed. Friendly
Fire is off and the logic cadence is raised equally for every size; times are
normalized to 20 ticks per second.

Matrix: Normal level 2 (character level 6), Normal level 6 (14) and Nightmare
level 3 (30); all-Warrior and mixed Warrior/Rogue/Sorcerer parties; seeds
20001–20005; one, two, four, six and eight players. All 150 runs finished
without an engine failure. Runs where the party was wiped out or hit the tick
limit are excluded from the ratios:

| Players | Runs | Cleared | Party wiped out | Tick limit | Player deaths per player | Potions per player |
| --- | --- | --- | --- | --- | --- | --- |
| 1 | 30 | 18 | 8 | 4 | 0.27 | 9.5 |
| 2 | 30 | 24 | 4 | 2 | 0.15 | 6.0 |
| 4 | 30 | 26 | 4 | 0 | 0.17 | 6.3 |
| 6 | 30 | 25 | 3 | 2 | 0.17 | 6.0 |
| 8 | 30 | 26 | 1 | 3 | 0.07 | 3.8 |

Ratios against one player, paired by cell and seed where both runs cleared, with
95% t intervals:

| Players | Pairs | Duration (kill-time model) | XP per living player per minute | Random drops per player per minute | Player deaths per player (difference) |
| --- | --- | --- | --- | --- | --- |
| 2 | 18 | 84% [78–90] (75%) | 93% [89–97] | 96% [88–105] | +0.00 [0.00–0.00] |
| 4 | 18 | 72% [64–80] (63%) | 100% [91–109] | 101% [89–112] | +0.01 [−0.01–0.04] |
| 6 | 17 | 72% [63–81] (58%) | 100% [92–109] | 95% [84–106] | +0.03 [−0.01–0.07] |
| 8 | 17 | 76% [65–88] (56%) | 94% [85–103] | 84% [71–96] | +0.01 [−0.01–0.03] |

Against two players, with more pairs (22–24) because fewer duos were wiped out,
XP per living player per minute is 101% [91–111], 102% [92–112] and 99%
[90–108] with four, six and eight players.

- **Experience:** each player's pace stays within a few percent of one player
  alone for every party size. This is the problem revision 2 set out to fix.
- **Duration:** parties clear faster than one player, though less than the
  kill-time model predicts, in parentheses: walking between enemies does not
  shrink with the party.
- **Difficulty:** one bot alone was wiped out in 8 of 30 runs, against 1–4 for
  parties, and used the most potions. Per player, deaths and potion use never
  rise with party size, so these parties are not harder than playing alone.
- **Loot:** extra rolls give exactly the expected number of rolls: an engine test
  over 400 monster seeds gives 839 items for eight players against 166 native,
  5.05 rolls per kill for an expected 5. The matrix counts 0.44 drops per roll
  for one and two players and 0.42 with six and eight, including 183 and 485
  drops lost to a full floor, because bots never pick items up. That limit
  explains most of the lower rate with eight players; people who pick up their
  loot do not reach it. Random loot *value* per player per minute is 138–214%
  of one player, but it is not an independent sample: an extra roll continues
  the monster's native item generator, so a monster gives the same extra items
  in every run of its seed, and a few valuable items decide the mean.

Departures: with eight bots, two left after 16 target deaths (Normal level 2,
mixed party, seeds 20001–20002). The remaining six engines lowered the scale to
six and cleared every target in both runs.

Decision: the formulas are not changed. The measurements reached Nightmare;
Hell and Diablo were not measured, since these bots cannot survive deep Hell
levels, and Diablo's scale rests on the engine benchmark above. Bots do not
dodge, retreat or trade; these results do not show that people find the
game clear, uncongested or fun, which still needs human playtests.

While measuring, joins at accelerated cadence exposed an unrelated defect in
extended joins: a message from the host that arrived before the host's player
information made the joiner discard that information and later drop the host.
It is fixed in the buffered replay described in
[synchronization](multiplayer-synchronization.md).

Reward votes that overtake their offer on another connection are accumulated
by serial in a bounded 16-entry table. Offers fold those votes into their own
receipt set; snapshots preserve this table and restoration merges locally seen
early votes. This prevents a receiver waiting forever for a vote that arrived
before it knew the offer. The table adds at most 785 bytes to the junk snapshot,
within the existing 16-bit transfer limit.
