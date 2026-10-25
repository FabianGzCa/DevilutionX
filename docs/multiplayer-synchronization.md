# Multiplayer synchronization

## Session storage

The engine allocates one player in single player, four in classic multiplayer,
and eight after accepting `ExtendedSessionRules`. `Players` reserves eight
entries before assigning player pointers. Network tables and player-info
assembly buffers have eight entries; `NoDeltaSender` is 8, outside every valid
player ID. Games use classic rules unless Extended is selected when creating them.

Extended sessions export at most one requested world snapshot per update, so
concurrent joins allow the transport to drain between transfers.

Turn buffers accept the negotiated transport capacity. Storm exchanges native
turn values with the engine; the transport serializes those values in little
endian. The existing bandwidth calculation divides the available budget by the
session capacity. TCP currently supplies 512-byte packets in either mode.

## Initial state transfer

Player information uses the unchanged `PlayerNetPack` layout and five-byte
fragment headers. Both fragment offset and length are checked before copying.
Leaving a slot clears its player object, assembly offset, timer, turn pointers,
pending delta request and cursor selection. Extended sessions also clear
live/stored damage contribution bits; classic sessions retain their original
XP divisors for departed contributors. Buffered leave and player-info messages are replayed in their original
order, including when the same slot is reused during resynchronization.

World deltas are assembled separately from the committed state. Fragments must
be contiguous, fit the receive buffer, and contain a valid compression marker.
Each level may occur once, followed by shared portal/quest data and the end
marker. A snapshot with no changed levels still imports shared data. Only a
complete, validated snapshot replaces the committed deltas.

The selected donor retains ownership of the transfer. If it disconnects, the
partial snapshot is discarded and the existing turn request selects another
connected player. Receiving an unrelated peer's first fragment cannot replace
an active transfer. Invalid transfers drop their sender and request a new donor.

## Wire sizes and bounds

- Game packets: at most 512 bytes, including the 28-byte packet header.
- Fragment header: 5 bytes; up to 479 payload bytes per packet.
- Player information: 1,692 bytes, normally four fragments.
- Fragment offsets: 16 bits. The receive-buffer bound is checked at compile time
  against that range and includes the larger of the maximum level and shared
  data sizes, reserving room for eight portal records.
- Buffered commands during resynchronization: at most 128 blocks of 32,000 bytes.
  Exceeding that budget cancels resynchronization through the existing game-end
  path.

Extended spawned-monster records contain a two-byte monster ID followed by a
nine-byte body: type index (u16), seed (u32), owner (u8), spell level (u16).
Multibyte fields are little endian, without native padding or `size_t` fields.
Classic sessions retain their existing native spawned-monster representation;
this compatibility path does not acquire cross-architecture guarantees.

## Monster and summon capacity

The classic table remains 200 entries, including four original golem placeholders
at IDs 0–3. Level generation and ordinary spawning retain that budget. Zhar's
fixed slot remains 4. Runtime storage has 204 entries; negotiated extended
sessions may allocate IDs 200–203 exclusively to additional golems. Consequently,
eight summons fit alongside the original maximum of 196 ordinary monsters.
Additional golems never borrow an ordinary enemy slot. Ownership remains in the
summon record, independent of its monster index.

The level owner assigns each spawn's ID and broadcasts it; receivers use that
ID. Extended sessions keep active IDs sorted after allocation/deletion so a
snapshot restored from an unordered spawn map has the same processing order.
Duplicate requests cannot create a second living golem for an owner. Dead
summons can reuse their entries immediately, and deleted additional entries
return to the summon pool. The original spell's dismiss/recast behavior and
summons surviving their owner's death until level departure are preserved.
A failed extended summon reports a translated message to its owner through
`CMD_GOLEMFAILED`; no enemy is removed to make room.

Classic encoded monster targets are 0–199 and player targets start at 200.
Extended monster targets are 0–203 and player targets are 204–211. Both fit the
existing byte fields, leaving the 255 leader sentinel intact. Validation,
rendering, synchronization priorities and AI seed updates use the negotiated
limit. Monster-type and kill-count tables retain their original sizes.

Classic deltas still write 200 monster states; extended deltas write 204.
Spawn sections are bounded by the same negotiated count, and additional IDs
only accept the golem type (level type index 0). Receive-buffer bounds include
all 204 entries. Spawn and state record layouts are unchanged.

Disk game and level entries still read/write exactly **200 active monster
indices (800 bytes)**. Loading clears the four additional runtime entries.
Multiplayer world state uses deltas; this change introduces no extended disk
world format. The four extra entries initialize without additional RNG calls,
keeping classic level generation and replay references unchanged.

## Town portals

Portal storage has eight entries, one per player ID. Classic snapshots write
four portal records before quest state; extended revision 3 snapshots write
eight. An open record is five bytes (x, y, level, dungeon type, set-level flag);
a closed record is a single `0xFF` byte. Import validates each record and clears
slots absent from classic snapshots without moving the quest-data boundary.
Portal commands reject unavailable owners and invalid destinations, including
level values that would become valid only after integer truncation. A closed
portal retains its validated destination so a warp already queued before the
closure can finish. New sessions and classic save loads clear stale entries.

The first four town positions remain (57,40), (59,40), (61,40), (63,40).
The additional row uses (57,44), (59,44), (61,44), (63,44). Town arrival remains
one tile southeast of the selected portal; dungeon visitors retain the same
offset, while the owner arrives at the portal itself. Map tests load the actual
town tiles and collision data, check NPC/entrance clearance, and verify walking
access to services with the portal tiles excluded from through routes.

The disk `game` entry always contains **four 24-byte portal records**, followed
immediately by monster kill counts. Loading clears all eight runtime entries
before reading the four classic records. Extended portals belong to the live
session snapshot; multiplayer games persist heroes rather than world sessions,
so no disk extension or new save version is introduced.

## Coordinated level entry

Classic sessions keep their original placement and join command. Extended
sessions append `CMD_LEVELENTRY` without changing existing command IDs. Its
packed 65-byte body carries a phase, a transaction serial, coordinator slot
epoch, a frozen human membership mask, authority/participant/voter masks, an
outcome, a level and set-level flag, eight request tickets and eight coordinate
pairs. Serial, epoch and ticket fields are little endian. A successful placement
also commits the destination high-water balance before entrants resume. Local
resurrection uses the same placement flow to finish its level-changing state.

An entrant requests admission after loading its map. The lowest connected player
coordinates requests for the same destination, ordered by player ID. A resident
of that destination searches when available; otherwise the lowest entrant does.
Only this authority searches. A bounded breadth-first search visits `PathDirs`
in their existing order, up to 50 tiles from each entrance, without consuming
random numbers. It excludes blocked or disconnected floor, occupied tiles,
entrance triggers and portals, reserving a neighboring escape tile per entrant
and protecting resident exits.

The authority proposes all positions together. Destination residents and entrants
validate and reserve those exact coordinates, then vote. The coordinator commits
only after all votes arrive. A disagreement aborts the batch and excludes its
rejected coordinates from the next search. A full destination produces an
explicit `NoSpace` result: entrants stay pending and invincible, the local player
sees a waiting message, and the coordinator retries after 20 logic ticks. A
transaction missing an authority or vote aborts after 200 logic ticks. Simulation
on the destination pauses during voting; other levels continue. Pending entrants
do not simulate until committed. Peers never substitute their own landing tile.

Per-sender FIFO channels may deliver a proposal or vote before the assignment
from another sender. Bounded early-message storage handles that ordering.
Coordinator IDs occupy the low three serial bits, preventing an election from
reusing another coordinator's transaction. Leaving or changing levels releases
reservations; request tickets reject obsolete admissions. Late player-info
assembly restores pending or committed admission state rather than the stale
position sent before loading. This transaction state is transient, with no new
save layout or changes to classic snapshots.

Extended teleport and phasing effects resolve in player-ID order, independently
of missile insertion order, and require a walkable adjacent exit. Ordinary
missile processing and classic placement keep their original ordering.

## Regression coverage and remaining work

`multiplayer_sync_test` exercises real TCP delivery to the engine with eight
clients, a common turn, slot reuse, player-info fragmentation, buffered command
ordering, death/resurrection on other levels, donor replacement, malformed
fragments, and compressed/uncompressed maximum-section deltas. Packet sizes and
transfer measurements are included as GoogleTest XML properties.

These are synchronization tests, not complete eight-player gameplay sessions.
`level_entry_test` covers eight independent admission state machines with
reordered cross-sender delivery, consecutive entrants, blocked maps, coordinator
replacement, actual dungeon stairs, quest map geometry, and town portals. It
also sends the new command through the real local engine network path.

`monster_summons_test` covers a full enemy pool with eight summons, replacement,
slot reuse, owner death/departure, authoritative spawn replay, extra-index sync
and delta restoration. It compares actual monster generation in all four
dungeon types and checks the classic RNG initialization sequence.

Full campaign and visual integration checks remain separate work. Focused
eight-engine TCP gameplay scenarios are covered by `multiplayer_gameplay_test`.

## Extended item ownership

Protocol revision 3 keeps 254 item-delta records per level: taken pregenerated
items retain their history independently of the 127 physical floor slots.
Classic snapshots still contain 127 records. Reloading removes taken items
before allocating dropped items. Extended junk snapshots also carry the bounded
item-transfer state; see [Extended item transfers](extended-item-transfers.md).

## Extended encounter state

Revision 3 adds `CMD_PARTYMONSTDAMAGE`, `CMD_PARTYSYNCDATA` and `CMD_PARTYSPAWN`;
classic command IDs, damage/sync layouts and disk saves are unchanged. The
`CMD_PARTYREWARD` payload belongs inside an item-transfer request, rather than
being a standalone gameplay command. See the
[balance lifecycle](extended-party-balance.md#synchronized-lifecycle) for ordering,
wire sizes, damage receipts, bounded queues and reward recovery.
