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
#include "player_mask.hpp"
#include "utils/attributes.h"

namespace devilution {

using net::leaveinfo_t;

// Defined in player.h, forward declared here to allow for functions which operate in the context of a player.
struct Player;

// The classic protocol and the session size used before a game's rules are known
// are both four, but remain separate so the protocol limit does not define engine storage.
constexpr int ClassicPlayerLimit = 4;
constexpr int SessionPlayerLimit = 4;
// Engine storage is independent of the negotiated four- or eight-slot session.
constexpr int PlayerStorageCapacity = 8;
constexpr uint8_t NoDeltaSender = static_cast<uint8_t>(PlayerStorageCapacity);
static_assert(SessionPlayerLimit <= PlayerStorageCapacity);
static_assert(PlayerStorageCapacity <= static_cast<int>(PlayerExperienceMaskBitCount));
static_assert(PlayerStorageCapacity <= static_cast<int>(PlayerRecipientMaskBitCount));

/* @brief Contains info of running public game (for game list browsing) */
struct GameInfo {
	std::string name;
	GameData gameData;
	std::vector<std::string> players;
	std::optional<int> latency;
	std::optional<bool> peerIsRelayed;
};

extern bool gbSomebodyWonGameKludge;
extern DVL_API_FOR_TEST uint16_t sgwPackPlrOffsetTbl[PlayerStorageCapacity];
extern DVL_API_FOR_TEST uint8_t gbActivePlayers; // Live participants, not the session limit.
extern DVL_API_FOR_TEST bool gbGameDestroyed;
extern bool gbSelectProvider;
extern std::string GameName;
extern std::string GamePassword;
extern bool PublicGame;
extern DVL_API_FOR_TEST uint8_t gbDeltaSender;
extern DVL_API_FOR_TEST uint32_t player_state[PlayerStorageCapacity];
extern bool IsLoopback;

void SwapGameDataLE(GameData &gameData);
DVL_API_FOR_TEST std::string DescribeLeaveReason(leaveinfo_t leaveReason);
std::string FormatGameSeed(const uint32_t gameSeed[4]);

DVL_API_FOR_TEST void InitGameInfo();
void NetSendLoPri(uint8_t playerId, const std::byte *data, size_t size);
void NetSendHiPri(uint8_t playerId, const std::byte *data, size_t size);
/** Send earlier buffered commands before a causally dependent item message. */
void FlushNetMessageBuffers();
void multi_send_msg_packet(PlayerRecipientMask recipients, const std::byte *data, size_t size);
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
#ifdef BUILD_TESTING
/** Replaces the provider and game menus so a test process can create or join a session itself. */
extern DVL_API_FOR_TEST bool (*TestSelectMultiplayerGame)(GameData *gameData, int *playerId);
/** Whether this engine will export its world snapshot to the player on the next delta pass. */
DVL_API_FOR_TEST bool TestDeltaRequested(uint8_t player);
/** Runs the turn-timeout drop of silent peers without waiting for the timeout. */
DVL_API_FOR_TEST void TestCheckDropPlayer();
#endif
void recv_plrinfo(Player &player, const TCmdPlrInfoHdr &header, bool recv);

} // namespace devilution
