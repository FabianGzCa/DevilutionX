# Multiplayer synchronization

## Session storage

The engine allocates one player in single player, four in classic multiplayer,
and eight after accepting `ExtendedSessionRules`. `Players` reserves eight
entries before assigning player pointers. Network tables and player-info
assembly buffers have eight entries; `NoDeltaSender` is 8, outside every valid
player ID. The public game selector still creates classic sessions.

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

## Regression coverage and remaining work

`multiplayer_sync_test` exercises real TCP delivery to the engine with eight
clients, a common turn, slot reuse, player-info fragmentation, buffered command
ordering, death/resurrection on other levels, donor replacement, malformed
fragments, and compressed/uncompressed maximum-section deltas. Packet sizes and
transfer measurements are included as GoogleTest XML properties.

These are synchronization tests, not complete eight-player gameplay sessions.
Vision, full portal storage/persistence, spawning, UI and balancing must be
completed before enabling the extended session selector. Until portal storage
is expanded, player departure only accesses existing portal slots.
