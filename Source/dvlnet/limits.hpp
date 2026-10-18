#pragma once

#include <cstddef>

namespace devilution::net {

// Bound stalled peers without imposing a smaller limit on eight-player traffic.
constexpr size_t MaxPendingPeers = 16;
constexpr size_t MaxDiscoveredGames = 128;
constexpr size_t MaxGameNameSize = 128;
constexpr size_t MaxMessageSize = 512;
constexpr size_t MaxQueuedBytes = 4 * 1024 * 1024;
constexpr size_t MaxQueuedPackets = 4096;
// Sequence numbers are eight bits; more than half the range is ambiguous.
constexpr size_t MaxQueuedTurns = 128;

} // namespace devilution::net
