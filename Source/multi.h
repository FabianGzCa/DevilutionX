/**
 * @file multi.h
 *
 * Interface of functions for keeping multiplayer games in sync.
 */
#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

#include "dvlnet/leaveinfo.hpp"
#include "game_mode.hpp"
#include "msg.h"
#include "utils/attributes.h"

namespace devilution {

using net::leaveinfo_t;

// Defined in player.h, forward declared here to allow for functions which operate in the context of a player.
struct Player;

// The classic protocol and currently available session size are both four, but
// remain separate so the protocol limit does not define engine storage.
constexpr int ClassicPlayerLimit = 4;
constexpr int SessionPlayerLimit = 4;
constexpr uint8_t NoDeltaSender = static_cast<uint8_t>(SessionPlayerLimit);
// Reserve eight Player objects without changing the current four-slot session.
constexpr int PlayerStorageCapacity = 8;
static_assert(SessionPlayerLimit <= PlayerStorageCapacity);

/* @brief Contains info of running public game (for game list browsing) */
struct GameInfo {
	std::string name;
	GameData gameData;
	std::vector<std::string> players;
	std::optional<int> latency;
	std::optional<bool> peerIsRelayed;
};

extern bool gbSomebodyWonGameKludge;
extern uint16_t sgwPackPlrOffsetTbl[ClassicPlayerLimit];
extern uint8_t gbActivePlayers; // Live participants, not the session limit.
extern bool gbGameDestroyed;
extern bool gbSelectProvider;
extern std::string GameName;
extern std::string GamePassword;
extern bool PublicGame;
extern uint8_t gbDeltaSender;
extern uint32_t player_state[ClassicPlayerLimit];
extern bool IsLoopback;

void SwapGameDataLE(GameData &gameData);
DVL_API_FOR_TEST std::string DescribeLeaveReason(leaveinfo_t leaveReason);
std::string FormatGameSeed(const uint32_t gameSeed[4]);

void InitGameInfo();
void NetSendLoPri(uint8_t playerId, const std::byte *data, size_t size);
void NetSendHiPri(uint8_t playerId, const std::byte *data, size_t size);
void multi_send_msg_packet(uint32_t pmask, const std::byte *data, size_t size);
void multi_msg_countdown();
void multi_player_left(uint8_t pnum, leaveinfo_t reason);
void multi_net_ping();

/**
 * @return Always true for singleplayer
 */
bool multi_handle_delta();
void ProcessGameMessagePackets();
void multi_send_zero_packet(uint8_t pnum, _cmd_id bCmd, const std::byte *data, size_t size);
void NetClose();
bool NetInit(bool bSinglePlayer);
void recv_plrinfo(Player &player, const TCmdPlrInfoHdr &header, bool recv);

} // namespace devilution
