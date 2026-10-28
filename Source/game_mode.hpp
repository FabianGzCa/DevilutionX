#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string_view>

#include "levels/gendung_defs.hpp"
#include "utils/attributes.h"

namespace devilution {

/** All-zero rules identify the unchanged classic wire protocol. */
struct SessionRules {
	uint8_t protocolRevision;
	uint8_t maxPlayers;
	uint8_t balanceRevision;

	bool operator==(const SessionRules &) const = default;
};

constexpr SessionRules ClassicSessionRules {};
constexpr SessionRules ExtendedSessionRules { 3, 8, 1 };

struct GameData {
	int32_t size;
	/**
	 * Whether this is the shareware (spawn) edition. Part of the join-compatibility identity
	 * (edition + version + mod config), unlike `programid` which is now cosmetic branding.
	 */
	uint8_t isSpawn;
	SessionRules rules;
	/** Branding id (e.g. "DRTL"/"HRTL"/"DXMD") for display only. A mod may change it via its manifest. */
	uint32_t programid;
	uint8_t versionMajor;
	uint8_t versionMinor;
	uint8_t versionPatch;
	_difficulty nDifficulty;
	uint8_t nTickRate;
	uint8_t bRunInTown;
	uint8_t bTheoQuest;
	uint8_t bCowQuest;
	uint8_t bFriendlyFire;
	uint8_t fullQuests;
	uint8_t isHellfire; // Occupies classic padding; meaningful only for extended sessions.
	uint8_t reserved;
	/** Used to initialise the seed table for dungeon levels so players in multiplayer games generate the same layout */
	uint32_t gameSeed[4];

	bool isExtended() const { return rules != ClassicSessionRules; }
};

// These fields are sent to legacy clients verbatim. Never grow the classic layout.
static_assert(sizeof(SessionRules) == 3);
static_assert(sizeof(GameData) == 40);
static_assert(offsetof(GameData, rules) == 5);
static_assert(offsetof(GameData, isHellfire) == 22);
static_assert(offsetof(GameData, gameSeed) == 24);

enum class GameDataError : uint8_t {
	InvalidSize,
	InvalidParameters,
	UnsupportedRules,
	VersionMismatch,
	EditionMismatch,
	SessionMismatch,
	SessionFull,
};

DVL_API_FOR_TEST bool IsSupportedSessionRules(SessionRules rules);
/** Player slots of a supported session; zero for rules this build cannot use. */
DVL_API_FOR_TEST uint8_t SessionPlayerCapacity(SessionRules rules);
/** The other supported mode, tried once when a host rejects a join as a mode mismatch. */
DVL_API_FOR_TEST std::optional<SessionRules> AlternateSessionRules(SessionRules rules);
/** Select the session rules; only extended descriptors carry the edition, classic ones keep zero padding. */
DVL_API_FOR_TEST void SetSessionRules(GameData &data, SessionRules rules);
DVL_API_FOR_TEST std::optional<GameDataError> ValidateGameData(const GameData &data);
/** Validate settings chosen locally before hosting. The offline provider has a single participant. */
DVL_API_FOR_TEST std::optional<GameDataError> ValidateNewGame(const GameData &data, bool offline);
DVL_API_FOR_TEST std::optional<GameDataError> CheckGameCompatibility(const GameData &host, const GameData &client);
DVL_API_FOR_TEST std::string_view GameDataErrorMessage(GameDataError error);

extern DVL_API_FOR_TEST GameData sgGameInitInfo;

/* Are we in-game? If false, we're in the main menu. */
extern DVL_API_FOR_TEST bool gbRunGame;
/** Indicate if we are in a multiplayer game */
extern DVL_API_FOR_TEST bool gbIsMultiplayer;
/** Indicate if we only have access to demo data */
extern DVL_API_FOR_TEST bool gbIsSpawn;
/** Indicate if we have loaded the Hellfire expansion data */
extern DVL_API_FOR_TEST bool gbIsHellfire;
/** Indicate if we want vanilla savefiles */
extern DVL_API_FOR_TEST bool gbVanilla;
/** Whether the Hellfire mode is required (forced). */
extern bool forceHellfire;

} // namespace devilution
