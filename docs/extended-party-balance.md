# Extended party balance: revision 1

This defines the mathematical rules for the existing negotiated balance revision
1. The functions in `monsters/party_scaling.*` are tested independently; live
creation, damage, rewards and synchronized level state are integrated in the
next stage. Public session selection remains classic. These measurements are
an initial model, not a claim that campaign balance has been playtested.

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
than recompute them from local occupancy. This ordering and persistence belong
to lifecycle integration; the count helper alone does not establish agreement.

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

When the committed scale rises, calculate the new maximum from the native base,
not by multiplying an already rounded/scaled maximum. For a living injured enemy:

`next_HP_raw = ceil(current_HP_raw * next_max_raw / previous_max_raw)`.

This preserves percentage damage progress within one raw unit (1/64 HP), rather
than resetting the monster to full HP. Absolute remaining HP can increase with
reinforcements: the health percentage does not reset and no healing event is
emitted. Native death means less than one whole HP; preserve that exact value
instead of multiplying it. Never revive, replay a kill or issue rewards during
rebasing. Reject invalid maxima, health above its old maximum, and a decreasing
maximum. High-water scaling never lowers the difficulty of an existing level.

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
The synchronization and scoped extra-roll execution must be tested at integration.

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
