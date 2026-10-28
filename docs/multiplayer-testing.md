# Multiplayer integration testing

The Unix network test target `multiplayer_gameplay_test` launches separate engine
workers over native TCP sockets. Each process owns its engine globals. It uses the
existing GoogleTest/CTest infrastructure; it does not add a game CLI headless mode.
See [Classic regression testing](testing-classic.md) for dependencies, isolated
profiles, sanitizer presets, and legally supplied game data. These tests require
`DIABDAT.MPQ`; a missing archive or shareware-only installation skips them.

## Run the integration matrix

After configuring and building `classic-tests`, run from the repository root:

```sh
DEVILUTIONX_GAMEPLAY_EVIDENCE_DIR=/absolute/private/evidence \
  ctest --preset classic-tests \
  -R 'MultiplayerSessionTest|MultiplayerFailureTest|MultiplayerSplitLevelTest' \
  --output-junit /absolute/private/evidence/results.xml
```

Create the evidence directory first. Use `classic-sanitizers` to repeat with
ASan/UBSan/leak detection. Do not run two CTest processes against one build at once.
Without the evidence variable, successful scenarios remove their temporary files;
failures retain them and print the location. Each directory contains the case,
capacity, participant count, privacy, seed, commands, per-peer logs, and checkpoints.
Never commit proprietary data, generated profiles, or logs.

The matrix covers Extended participant counts 1–8 (seeds 19001–19008), Classic
1–4 (19401–19404), and additional delayed eight-player cases (19081–19082).
Password-protected and unprotected sessions use native password handling. Clients
start direct joins with Classic rules, exercising the normal Extended fallback.
Scenarios use native admission, portal, summon, item, melee, boss, quest, experience,
and world-snapshot handlers. The split-level case starts with four dungeon players
and four in town, regroups them, and imports a buffered world snapshot.

## Checkpoint contract

Workers drain messages, send native transport-turn fences, drain effects, and
confirm a second fence before reporting. Every participating peer must supply the
same turn payload. The checkpoint serializes fields rather than memory layouts:
active player identities and slot generations, level and position, changing-level flags, health,
portals, quests, sorted monsters and items, monster AI seeds and contributors,
item generation properties, and pool counts. It retains both an FNV-1a digest and
its complete canonical text. A negative-control scenario intentionally changes a
quest, a monster RNG seed, and a slot generation on one peer and requires unequal checkpoints.

The generation RNG is captured at world initialization; admission separately
asserts that it consumes no world RNG. Private inventories and the local global
combat RNG are not replicated state. Inventory/experience outcomes have separate
assertions. Dead player summons alone normalize their contributor masks: native
local disconnect cleanup attributes their death differently on each peer, while
summons award neither experience nor loot. The cleanup test checks those outcomes;
ordinary enemy contributor masks remain strict. Split-level peers compare the
shared manifest globally and loaded-world state only with peers on the same level.

These scripted scenarios exercise engine handlers, not the full running game
loop or every object/missile property. Equal checkpoints establish agreement for
the observed fields and confirmed actions, not campaign-wide determinism.

## Transport faults

The loopback relay fragments streams into 1, 7, 31, and 251-byte reads, adds
10–30 ms FIFO delay selected by the case seed, pauses forwarding for 450 ms, and
can disconnect an established peer. Queues apply backpressure. EOF is propagated
as a half-close only after queued bytes are forwarded, preserving rejection
responses needed by mode negotiation. TCP ordering remains intact: stream
fragmentation is not simulated UDP packet loss. Scheduling and OS read boundaries
can vary; record the seed, logs, and failed stage when reproducing a failure.

Guest-loss tests require native player, portal, and summon cleanup, then compare
survivor checkpoints. They also require the departed slot to have no recoverable
level-admission record. Native leave events and replayed drop messages must both
clear that record before a new connection can reuse its entry ticket. A separate
regression delivers a new generation request between SEND/ACK player information:
it must activate the entrant while rejecting the previous generation. Killing the TCP server must close every client's transport
and reject further turns; the test does not assume host migration.

## Running-game and provider checks

Headless workers bypass menu-driven `NetInit`, full resynchronization, rendering,
and SDL input. Before accepting changes to these paths, additionally create and
join with independently configured game processes and private saves. Observe
admission completion and movement, fill eight slots, and verify the ninth rejection.
Exercise late joins, slot reuse, directed/broadcast chat, every local player ID,
UTF-8 names, low-resolution pages, and recipient changes during a pending click.
Test controller navigation through SDL and label virtual versus physical devices.

Keep TCP listeners on loopback when testing locally. For ZeroTier, use distinct
node storage per concurrent process, verify network readiness, and test private
joins and authorized public discovery. Record timeouts and actual host-loss
behavior. Close all test rooms afterward. Real provider traffic on one computer
is not evidence of multiple machines, WAN conditions, another platform, or a long
campaign. Record those limits separately instead of inferring them from CTest.

## Balance measurements

`balance_measurement_worker` plays a real headless game: one process per player,
native TCP on loopback, the complete game loop, level generation, monster AI,
missiles, damage, potions, experience and loot. Two test-only hooks replace the
game menus with a direct create or join and give a bot one decision per logic
tick. The bot sends the same network commands a player's input would (walk,
melee, ranged attack, spell, potion), so each player's damage and potions are
applied in that player's own engine. Observers count monster rewards and loot
without changing them.

Heroes are deterministic for a class and character level. Each slot receives
native magic items: the generator scans a fixed seed range and keeps the first
item that reaches a damage or armor target, with an item level that native
validation accepts. The first peer records the nearest reachable enemies as the
encounter targets. Larger parties can reuse that list with `--targets-file`, so
four and eight players face the same enemies of the same level.

Run one encounter from the repository root after building `classic-tests`:

```sh
python3 tools/measure_multiplayer_balance.py --build-dir build/classic-tests \
  --output /absolute/private/evidence/p4 --players 4 --seed 20001 \
  --difficulty 0 --level 2 --composition WRS --character-level 6
```

The output directory holds per-player logs, hero manifests, the target list,
`result-N.json` and `summary.json` with the commands, exit codes, worker hash,
sampled loopback TCP queues, time spent in each logic tick and the interval
between ticks. Durations are logic ticks; divide by 20 for seconds at native game
speed. `--tick-rate` runs the game faster than native speed for balance batches;
measure performance at 20. Wall-clock time is never a rendering frame rate.

Bots do not dodge, retreat, trade or judge risk the way people do. Their results
measure the engine's workload and rewards under controlled play, not how clear,
crowded or enjoyable a session feels; that still needs human playtests.
