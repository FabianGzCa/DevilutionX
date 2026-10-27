# Multiplayer session compatibility

## Scope

Classic sessions retain four player slots and the existing wire layout. The
extended descriptor reserves an eight-player protocol and balance revision;
it does not change balancing by itself. TCP and ZeroTier games can be created
in either mode; see [Selecting a mode](#selecting-a-mode). Classic remains the
default. See [Multiplayer synchronization](multiplayer-synchronization.md) for
transfer limits and the remaining work.

## GameData contract

`GameData` remains 40 bytes. Multi-byte integers use little endian, as before.
The session identity uses these byte offsets:

| Offset | Size | Meaning |
| --- | --- | --- |
| 0 | 4 | GameData size, exactly 40 |
| 4 | 1 | Shareware flag |
| 5 | 1 | Protocol revision |
| 6 | 1 | Session capacity |
| 7 | 1 | Balance revision |
| 8 | 4 | Cosmetic program ID, not a compatibility check |
| 12–14 | 3 | Application major/minor/patch version |
| 15–21 | 7 | Existing difficulty, tick rate and gameplay options |
| 22 | 1 | Hellfire flag in extended sessions; classic padding |
| 23 | 1 | Reserved zero in extended sessions; classic padding |
| 24–39 | 16 | Four existing level seeds |

Rules `(0, 0, 0)` identify classic four-player sessions. Extended rules are
`(3, 8, 1)`. Every other tuple is rejected. Protocol revision 3 adds level balance
snapshots, epoch-qualified entry/spawn/damage messages and synchronized monster
rewards to revision 2's reserved item transfers. Builds using revisions 1 or 2
cannot join revision 3 sessions. Balance revision 1 retains the mathematical
[extended party scaling rules](extended-party-balance.md); changing those formulas
requires a new balance revision. Classic padding at offsets 22–23 is ignored
because old binaries do not initialize it as an edition field.

Both sides validate the descriptor, exact application version, shareware flag
and session rules. Extended sessions additionally require the same Hellfire
flag. Flags must be boolean, difficulty must be valid, and tick rate must be
nonzero. Host gameplay options and seeds are authoritative, not client identity.
Mod hashes and `programid` do not establish rule compatibility.

## Admission and discovery

The packet header, cookie representation and existing packet kinds are unchanged.
New kinds use the existing serialization and encryption:

| Kind | Value | Payload after source/destination |
| --- | --- | --- |
| Extended join request | `0x15` | 32-bit cookie, 40-byte GameData |
| Extended join acceptance | `0x16` | Cookie, 8-bit assigned ID, GameData |
| Join rejection | `0x17` | Cookie, one-byte GameDataError |
| Extended discovery request | `0x23` | Empty |
| Extended discovery reply | `0x24` | GameData, existing player names, game name |

Rejection codes are 0: size, 1: settings, 2: unsupported rules, 3: version,
4: edition, 5: session mismatch, 6: session full. They are translated locally. Unknown codes
are invalid. Only the joining client's cookie can complete or reject its join.

TCP pins the host descriptor when creating the server. TCP and the peer protocol
validate requests before assigning slots or announcing players. Clients validate
acceptance before publishing game state. Gameplay packets before admission are
rejected. Discovery only answers the matching classic/extended request kind.
Distinct extended kinds prevent old peers from ignoring the new rule bytes and
silently accepting an extended session.

## Selecting a mode

Creating a TCP or ZeroTier game adds a mode step after the game speed. Classic
is listed first and selected by default; Extended allows eight players. The
chosen rules are validated with the other settings before the game is created,
and offline multiplayer always uses classic rules. Leaving the menu or failing
to create or join a game resets the mode to classic.

The ZeroTier browser sends both discovery kinds. Each host answers its own kind,
so classic and extended games are listed together with their mode and occupied
and total slots, for example `name 3/8`. Selecting a listed game requests its
advertised rules.

Any node can send discovery datagrams, including a peer of a password-protected
session that is browsing without that password. A peer therefore ignores
discovery datagrams it cannot read. Only malformed data on a peer's session
stream drops that peer.

A direct join by address or game ID cannot know the host's mode. It sends the
classic request first, which original hosts understand. If a new host rejects it
as a session mismatch, the client retries once with the other mode; a ZeroTier
host's advertisement provides the same answer without sending a request. Any
other result, including a full room or a version or edition mismatch, ends the
attempt with that error. A full room therefore reports "The multiplayer game is
full." only when the mode and identity are compatible.

## Legacy compatibility

New classic clients send GameData in the existing join kind; the original server
already accepts that payload. Original clients send an empty join payload, which
new hosts accept **only in classic mode**. Such a host cannot prevalidate the old
client's version or edition; the original client's existing UI performs its
compatibility check after the classic handshake. Classic Diablo/Hellfire behavior
remains unchanged. Extended sessions never permit this legacy exception.

Original clients do not understand the new rejection packet: a direct connection
to an extended host fails with their existing unknown-packet error. Original
hosts similarly reject extended packet kinds. New endpoints show specific errors.
This is admission compatibility, not a guarantee of gameplay interoperability
with arbitrary releases or mods.

## Validation

`ctest --preset classic-tests -R 'SessionCompatibility|DvlNetError'` covers wire
layout, malformed data, rule/version/edition mismatch, cookie correlation,
pre-admission state protection, and peer discovery/admission with an in-memory
transport. The same tests run under `classic-sanitizers`.
`ctest --preset classic-tests -R StormJoin` joins real localhost TCP rooms through
the menu's join path, covering mode detection in both directions and full or
incompatible rooms.

Interop testing must additionally use a separately built original revision,
checking classic admission and payload exchange in both directions, rejection
of original/extended pairs, and equal/different extended rules. Record the exact
reference revision and observed outcomes outside the source tree. The in-memory
peer tests do not establish real ZeroTier connectivity or eight-player gameplay.

## Transport capacity and resource bounds

Transport tables reserve eight slots and use the validated session capacity,
independently of the engine's `Players` vector. Classic rooms stop at four;
extended rooms assign IDs 0–7. `0xFE` remains the master sentinel and `0xFF`
remains broadcast/unassigned. The source of an admitted TCP packet must match
its socket's assigned ID. Addresses and embedded control IDs are checked before
indexing tables. Rejoining a peer already assigned an ID does not allocate another.

Discovery carries exactly four or eight fixed player-name fields according to
the advertised descriptor, followed by the game name. Game names are limited to
128 bytes and the browser retains at most 128 games. The single-player loopback
retains its local-only message semantics and respects the negotiated buffer
capacity; it does not create additional participants.

TCP uses one write at a time per socket and retains each frame until completion.
Partial frames and coalesced frames preserve stream order. Receive and send
queues are bounded by 4 MiB and 4096 entries. Application messages retain the
provider's 512-byte limit; turn queues are bounded to 128 entries because turn
sequence numbers wrap at 256. Queue exhaustion returns an error or disconnects
the affected network peer rather than growing memory without limit. Empty frames,
unknown frame flags and trailing data in fixed-size packet kinds are rejected.

TCP tracks at most one capacity's worth of pending admissions and releases them
on EOF, failure, shutdown or the existing 30-second admission timeout. Active
connections retain the 60-second timeout. Disconnect callbacks are idempotent,
so delayed cancellation cannot clear a newly reused player slot. ZeroTier stream
queues use the same bounds, retain partial writes, recognize EOF and expire idle
streams after 60 seconds; the native provider limits pending/active endpoints to
16. Real ZeroTier connectivity still requires separate integration testing.

`ctest --preset classic-tests -R 'Network|Transport' -V` exercises the TCP listener
and eight real TCP clients on an ephemeral localhost port, with and without
passwords. It verifies directed/broadcast order, turns, room limits, all reused
slots, fragmented input, corrupt headers, forged IDs, pending joins and incorrect
passwords. Peer protocol tests use an in-memory byte transport to exercise eight
participants, discovery, turns, duplicate joins and control IDs. They do not
establish that a real ZeroTier network is reachable.
