# Extended party balance: revision 1

Balance revision 1 runs in negotiated extended protocol 3 sessions. The pure
functions in `monsters/party_scaling.*` define its arithmetic;
`monsters/party_balance.*` integrates creation, damage, level state and rewards.
It applies only to games created in Extended mode. This is an initial model, not
a claim of campaign playtest results.

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
For one through four multiplayer participants, normal zombie and Diablo HP,
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

## Choice of dimensions

The controlled damage benchmark supports scaling health to offset the extra
human damage budget. Matching XP and ordinary random-roll budgets preserves
per-contributor progress in that model. Multiplying damage as well would also
increase individual hit bursts; adding density would increase grid/path and
shared-resource pressure. Neither increase is justified by the measured native
one-to-four reference, so revision 1 leaves them unchanged. The larger party's
healing, body blocking and total defensive resources can still change survival.
Campaign death/potion measurements below must check that effect before claiming
equal or slightly greater difficulty. HP-workload equality alone is insufficient.

## Participants and encounter lifetime

An encounter is the hostile population of one canonical multiplayer level,
including its dynamically spawned enemies. Set levels use their existing
separate level IDs. Scaling applies to ordinary enemies, uniques and bosses.
Player-controlled golems keep native spell-derived health/damage and do not
increase the human participant count.

Count active human slots assigned to that level. Include dead players and
players loading or changing levels until their committed level assignment
changes. Otherwise death/loading could lower difficulty. Exclude town, other
levels, unused room slots, spectators/nonparticipants and disconnected slots
after their departure is committed. Class, character level and gear do not
weight a human slot. A mixed party follows the same rule as an equal-level one.

Keep a high-water participant count `H` for the lifetime of the level in the
session: `H_next = max(H_previous, count_on_level)`. An initially empty level has
no encounter scale. Once established, an empty level retains its scale and loot
remainder. The maximum is eight, not the selected room capacity. A split group
of four and four starts with native scaling on each level; a level visited by
eight stays scaled for eight after they split or return to town. Starting a new
multiplayer world resets the record; leaving/reloading a level does not.

Before new participants can damage or receive rewards on a level, agree and
commit its next scale. A player cannot prepare native-health monsters with
four, bring eight and receive the larger reward pool at native difficulty.
Snapshots and late joins must import the committed scale and remainder rather
than recompute them from local occupancy. The coordinated level-entry commit establishes that ordering; the count helper
alone does not establish agreement.

## Exact arithmetic

Let `N = max(4, H)` and denominator `D = 4`. Compute native difficulty, unique and
quest-specific stats first, keeping their unscaled reference. Then:

| Dimension | Extended rule |
| --- | --- |
| Enemy maximum HP | `ceil(native_HP_raw * N / 4)` in 26.6 fixed point |
| Normal/special damage, accuracy, AC, resistance | Native values |
| Enemy density and map | Native generation and limits |
| XP pool | `floor(native_XP * N / 4)` before native contributor split/caps |
| Random monster loot | Native roll, plus the deterministic extra-roll credits below |
| Fixed quest/unique rewards | Native exactly-once reward; no extra copies |
| Player minions | Native spell stats/rewards, no party multiplier |

Use unsigned 64-bit intermediates. Clamp HP to `INT32_MAX` raw and XP to
`UINT32_MAX`; never wrap. A saturated mod value no longer satisfies proportional
workload targets and must be reported by calibration. HP scale inputs at or
below zero remain unchanged. Invalid counts, unsupported session rules and
invalid loot remainders are rejected. Classic counts above four are invalid.
One through four preserve every native HP/XP value and leave loot credits alone.

When the committed scale rises, derive the new maximum from the native base.
Keep damage as exact normalized work: one native raw HP has 840 units;
a hit of `d` raw HP at numerator `N` adds `d * (3360 / N)` units. Each supported
numerator 4–8 divides 3360, so old-scale hits and scale commits commute without
rounding intermediate HP. Derive remaining raw HP as
`ceil((native_raw * 840 - damage_units) * N / 3360)`. This preserves the completed
damage fraction; absolute remaining HP can rise with reinforcements, while its
percentage does not reset. Native healing updates that ledger at the scale in
which it occurred. The loaded level owner reconciles healing through sync only
when its receipt vector covers the receiver's applied hits.

Native death means less than one whole HP. Latch it before rebasing and never
revive a corpse, replay a kill or grant rewards during a scale commit. The pure
`RebaseMonsterHitPoints` helper retains the percentage formula for standalone
callers; runtime damage uses the normalized ledger to handle delayed packets.

### Random loot credits

Store one integer remainder `R` (0–3) per level. For each exactly-once eligible
random-loot death, add `N - 4`; extra rolls are `floor((R + N - 4) / 4)` and the
new remainder is `(R + N - 4) % 4`. Thus four eligible deaths earn 1/2/3/4 extra
rolls with five/six/seven/eight participants. A roll may produce no item, just
as a native roll may. Do not count only successful drops: that would bias the
probability. Do not accrue credits for minions, no-drop monsters, fixed rewards,
quest-item spawns or a replayed death. Random drops from a unique/boss remain
eligible when that native path is genuinely random; its fixed reward is not.

Execute the native roll first. If an extra roll is earned, continue its native
LCG state for that roll, then restore the post-native-roll LCG state. Preserve
the primary item and the unrelated simulation RNG sequence; do not reseed from
time, host ID, local inventory or transport order. Advance the agreed remainder
and reward decision once even when the floor is full. Use the existing bounded
item admission, and never bypass physical limits or duplicate a pending reward.
The lifecycle and engine tests cover synchronization and scoped extra rolls.

## Synchronized lifecycle

| Event | Agreed state and effect |
| --- | --- |
| Generate/load level | Keep native difficulty/unique HP, then apply saved high-water and damage; do not scale an already scaled maximum. |
| Place entrants | Lowest active slot freezes the destination human mask, including dead/loading humans; its epoch/serial-qualified commit raises `H` before entrants resume. |
| Receive old-scale hit | Check source slot epoch, monster generation and receipt; add normalized damage once at the hit's original scale. |
| Receive future scale/spawn hit | Retain in a bounded queue; drain after the required placement/spawn. |
| Leave, split or return to town | Clear departed slot contribution/receipt identity; keep high-water, damage, corpses and loot credits. |
| Die | Start native death visuals once; retain the monster slot while its reward is unresolved. |
| Reward | Killer publishes one request at a time; item-transfer coordinator orders it and all receivers wait for every voter fence before XP/loot/quest commit. |
| Snapshot/reload | Import high-water, credits, generations, receipts, pending damage and paid flags together with world/item state; replay cannot apply damage/rewards twice. |

Each sender flushes earlier damage before its entry or reward vote. A voting
replica waits for all entry votes before raising health or releasing entrants,
so old-scale damage settles first on every loaded participant. Non-voting
observers retain the coordinator commit even if they missed its assignment.
Each sender also flushes earlier damage before its reward vote. A receiver defers
reward commit until it has received every vote, so cross-connection delivery
cannot omit an earlier contribution. Later decisions/finalizations wait behind
that commit; their deferred state survives snapshots. The coordinator serializes
all rewards, giving each level a common loot-credit order. A loaded replacement
publisher handles a dead monster whose killer has already left; disconnects
inside an undecided ownership transaction retain the item protocol's explicit
session termination policy rather than guessing an outcome. A clean exit drains
pending rewards before leaving. Applied snapshot reward entries recover an
existing voter's missing private XP receipt without recreating loot or credits.

Dynamically spawned monsters carry a 64-bit generation composed of creator slot,
spawn counter and slot epoch; pregenerated monsters use a separate high-bit seed
namespace. Per-level spawn watermarks reject old incarnations while allowing a
hit from another connection to wait for its spawn. Source slot epochs reject
frames from previous occupants. Counter/queue exhaustion terminates the session
instead of wrapping or dropping already accepted work. Golem melee damage is
published by its owner, shared monster/trap damage by the lowest loaded resident;
environmental damage does not make that publisher an XP contributor.

Extended level snapshots append a 134-byte empty balance body, up to 204 indexed
61-byte monster records (plus two-byte indices), and up to 512 pending damage
records. Damage messages are 35 bytes; monster rewards occupy 21 bytes inside
the existing 29-byte item payload. A periodic record combines the native
10-byte monster record with its 61-byte balance state; the extended sync header
adds nine bytes for high-water, sender epoch and membership digest. Scalars are
explicitly little endian. Decoders bound lengths, counts, identities, health,
flags and positions, reject truncation/trailing bytes, and preserve the native
16-bit delta-transfer size ceiling. Snapshots that would lower an established
high-water or rewind known same-generation death/payment/receipt state are
rejected before publication. Classic snapshots have none of this suffix.

The eight-engine TCP test covers 4→5, 5→4, 7→8 and 8→1, a wounded boss, delayed
and duplicate damage, dead humans, resurrection placement, town/split groups,
return to a corpse, and snapshots both before and after reward. Unit tests cover
fractional boundaries, future spawn/scale delivery, malformed/truncated codecs,
slot identities, native golem HP and deferred reward fences. Existing engine
scenarios exercise concurrent melee, shared XP masks, boss quests and bounded
loot recovery. These fixtures do not replace campaign or real ZeroTier testing.

## Numerical tables

Multipliers are identical in Normal, Nightmare and Hell, applied after native
difficulty. Selected room capacity never selects a row automatically.

| Effective high-water humans | HP and XP factor | Random rolls per 4 eligible deaths | Damage/density factor |
| --- | --- | --- | --- |
| 1 | 1 | 4 | 1 |
| 2 | 1 | 4 | 1 |
| 3 | 1 | 4 | 1 |
| 4 | 1 | 4 | 1 |
| 5 | 5/4 | 5 | 1 |
| 6 | 6/4 | 6 | 1 |
| 7 | 7/4 | 7 | 1 |
| 8 | 8/4 | 8 | 1 |

Diablo's native data is 1666 HP and 31666 XP. The following pool values precede
contributor division and caps. HP halves are exactly representable in 26.6.

| Humans | Normal HP | Nightmare HP | Hell HP | Normal XP | Nightmare XP | Hell XP |
| --- | --- | --- | --- | --- | --- | --- |
| 1 | 1666 | 5098 | 6864 | 31666 | 65332 | 130664 |
| 2 | 1666 | 5098 | 6864 | 31666 | 65332 | 130664 |
| 3 | 1666 | 5098 | 6864 | 31666 | 65332 | 130664 |
| 4 | 1666 | 5098 | 6864 | 31666 | 65332 | 130664 |
| 5 | 2082.5 | 6372.5 | 8580 | 39582 | 81665 | 163330 |
| 6 | 2499 | 7647 | 10296 | 47499 | 97998 | 195996 |
| 7 | 2915.5 | 8921.5 | 12012 | 55415 | 114331 | 228662 |
| 8 | 3332 | 10196 | 13728 | 63332 | 130664 | 261328 |

At ten damage per participant per controlled turn, the native engine damage
benchmark measures 167/84/56/42 turns for Normal with one/two/three/four humans;
Nightmare is 510/255/170/128 and Hell 687/344/229/172. Applying these functions
in that fixture gives 42/128/172 turns for every count from five through eight.
This isolates HP workload; it does not model attack animation, movement, AI,
regeneration, latency, corridor congestion or actual player builds. The Normal
XP share with four through eight contributors is 7916 before native caps;
Nightmare/Hell shares are 16333/32666. Equal DPS therefore preserves the
four-player baseline instead of halving fight duration with eight.

## Acceptance and later calibration

Automated acceptance: exact one-to-four identity; golden arithmetic in all
three difficulties; health/XP bounds; no corpse revival; identical native
maps/seeds/drops; exact controlled four-player turn count for five through eight;
XP share within one point of four after integer rounding; exact extra-roll
accounting over complete four-kill cycles. No clocks or floating point select
scale or credits.

For campaign calibration, predeclare comparisons against four equally equipped
contributors in the same edition/difficulty/seed: encounter duration target
100–115%, XP per living contributor per minute 90–110%, random gear/gold value
per contributor per minute 90–110% over at least 10000 eligible random deaths,
and deaths/potion use within 15% of the four-player reference. Report classes,
levels, layouts, sample dispersion and confidence intervals; do not hide
outliers with an overall mean. Check solo through four regressions, mixed
classes/levels, melee congestion, area spells, minions and bosses separately.
Fixed rewards are intentionally not duplicated; report their sharing separately.
These tolerances are gates for playtests, not results already obtained. Revise
the balance revision and repeat compatibility checks if formulas change.

Reward votes that overtake their offer on another connection are accumulated
by serial in a bounded 16-entry table. Offers fold those votes into their own
receipt set; snapshots preserve this table and restoration merges locally seen
early votes. This prevents a receiver waiting forever for a vote that arrived
before it knew the offer. The table adds at most 785 bytes to the junk snapshot,
within the existing 16-bit transfer limit.

## Revision 1 measurements

Revision 1 was measured with the tool described in
[multiplayer testing](multiplayer-testing.md#balance-measurements): one headless
engine process per player over loopback TCP, each driven by a bot that sends
the same commands a player would. The bots clear the 32 reachable enemies
nearest to the entrance. Five through eight players reuse the four-player target
list of the same seed. Friendly Fire is off and the logic cadence is raised
equally for all sizes; times are normalized to 20 ticks per second.

Main matrix: Normal level 2 (character level 6), Normal level 6 (14),
Nightmare level 3 (30) and Hell level 2 (40); all-Warrior, all-Rogue,
all-Sorcerer and mixed parties; seeds 20001–20005; four to eight players. Of the
400 runs, 371 cleared, 18 timed out and 10 ended in a party wipe (all three
excluded from the ratios). One run stalled while closing and was excluded; 23
repeats of that run did not stall. Five through eight players accounted for
20438 eligible random deaths. Pooled ratios against four players, paired by
cell and seed, with 95% t intervals:

| Players | Pairs | Duration | XP per living contributor per minute | Random drops per contributor per minute | Drops per eligible death ÷ (N/4) | Player deaths per player (difference) |
| --- | --- | --- | --- | --- | --- | --- |
| 5 | 72 | 108% [100–116] | 101% [96–106] | 97% [93–102] | 99.7% [97.9–101.5] | +0.02 [0.00–0.05] |
| 6 | 74 | 107% [100–114] | 101% [97–106] | 98% [94–102] | 99.7% [97.9–101.4] | +0.03 [0.00–0.05] |
| 7 | 73 | 112% [103–121] | 100% [95–105] | 100% [94–106] | 100.8% [98.4–103.2] | +0.02 [0.00–0.05] |
| 8 | 73 | 106% [101–112] | 104% [99–108] | 102% [97–107] | 100.1% [97.4–102.7] | +0.02 [0.00–0.03] |

The four-player references had almost no player deaths, so deaths and potions
are reported as absolute differences rather than ratios. Potion use per player
fell with eight players, by 0.6 [0.2–1.1] potions per encounter. Bots
spent 2–6% of their time with a target they could not attack; this share does
not grow with party size in the catacomb and cave layouts. Per-cell intervals are wide
(five seeds) and mostly inconclusive. The only cells entirely outside a
tolerance are isolated and point in opposite directions.

Random loot *value* per contributor per minute measured 110–149% of four
players, but this figure is not an independent sample. The extra roll continues
the monster's native item generator, so a monster gives the same extra item in
every run of its seed, whatever the party. The 1419 extra items in the matrix
are only 210 distinct items, and a few items worth 8000–14400 gold decide the
mean. Counted once each, extra items are worth 138% [94–206] of native items.
An engine-level comparison over 200000 monster seeds per type, with no bots,
found chained extra rolls within ±4% of native and independent rolls in value
and within 1% in item count, in Normal and Hell, for normal and unique monsters.
The number of drops matches the formula exactly (table above).

Special scenarios, on the same seeds and targets:

- Four players in a Classic session, against Extended with four present
  (Normal, all compositions, 37 pairs): identical heroes, kills, potion use and
  XP per contribution, but encounters took 110% [105–115] as long. Monsters
  survived 8% longer from first damage to death, while every peer saw each
  death within the same few ticks in both modes. Classic is unchanged native
  code; the likely cause is that Extended applies each published hit once in
  every engine. Measured against Classic four, five to eight players would
  take about 97–102%, near the lower duration bound. This is recorded for
  review, not adjusted.
- Bosses: the Butcher (Normal level 2) and the Skeleton King (Nightmare
  level 3) fell inside the matrix with no change in outcome by party size,
  apart from one Skeleton King seed with three to five player deaths for five
  to eight players. Diablo was not measured: these bots cannot survive deep
  Hell levels.
- Departures: with eight players, two left after 16 target deaths in 20 runs.
  The scale stayed at eight (high-water) with six present and no run failed;
  17 cleared and 3 timed out, taking 113% [99–127] as long as eight players.
- Friendly Fire, measured separately with mixed parties: eight players had more
  party wipes (7 of 20 against 3 of 20 with four) and used 3.0 [1.6–4.5] more
  potions per player than four. Bots do not avoid hitting allies, and the
  formulas do not change friendly damage, so this is left to human playtests.

Performance at the native 20 ticks per second, run one at a time on the same
machine (Hell level 2 mixed and Normal level 6 Warriors, three seeds each):
game logic took 0.04–0.05 ms per tick with four players and 0.10–0.12 ms with
eight, with a worst 95th percentile of 0.73 ms against the 50 ms tick. Ticks
stayed 50.0 ms apart on average for both sizes (95th percentile at most
50.6 ms, one 73.8 ms outlier with eight). Each process used about 21 MiB, 1–2%
more with eight, and at most two turns were in transit for both sizes.
Rendering cost was not measured, since these processes are headless.

Decision: no dimension has intervals consistently outside its tolerance, so
the revision 1 formulas are unchanged and no new balance revision is needed.
These are automated engine measurements with simple bots. They do not show that
five to eight human players find the game clear, uncongested or fun. Human
playtests remain pending.
