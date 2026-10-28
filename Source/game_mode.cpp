#include "game_mode.hpp"

#include <function_ref.hpp>

#include "options.h"
#include "utils/language.h"

namespace devilution {
namespace {
void OptionSharewareChanged()
{
	gbIsSpawn = *GetOptions().GameMode.shareware;
}
const auto OptionChangeHandlerShareware = (GetOptions().GameMode.shareware.SetValueChangedCallback(OptionSharewareChanged), true);
} // namespace

bool IsSupportedSessionRules(SessionRules rules)
{
	return rules == ClassicSessionRules || rules == ExtendedSessionRules;
}

uint8_t SessionPlayerCapacity(SessionRules rules)
{
	if (rules == ClassicSessionRules)
		return 4;
	if (rules == ExtendedSessionRules)
		return ExtendedSessionRules.maxPlayers;
	return 0;
}

std::optional<SessionRules> AlternateSessionRules(SessionRules rules)
{
	if (rules == ClassicSessionRules)
		return ExtendedSessionRules;
	if (rules == ExtendedSessionRules)
		return ClassicSessionRules;
	return std::nullopt;
}

void SetSessionRules(GameData &data, SessionRules rules)
{
	data.rules = rules;
	data.isHellfire = data.isExtended() && gbIsHellfire ? 1 : 0;
}

std::optional<GameDataError> ValidateGameData(const GameData &data)
{
	if (data.size != sizeof(GameData))
		return GameDataError::InvalidSize;
	if (data.isSpawn > 1 || data.nDifficulty > DIFF_LAST || data.nTickRate == 0
	    || data.bRunInTown > 1 || data.bTheoQuest > 1 || data.bCowQuest > 1
	    || data.bFriendlyFire > 1 || data.fullQuests > 1)
		return GameDataError::InvalidParameters;
	if (data.isExtended()) {
		if (data.rules != ExtendedSessionRules)
			return GameDataError::UnsupportedRules;
		if (data.isHellfire > 1 || (data.isSpawn != 0 && data.isHellfire != 0) || data.reserved != 0)
			return GameDataError::InvalidParameters;
	}
	return std::nullopt;
}

std::optional<GameDataError> ValidateNewGame(const GameData &data, bool offline)
{
	if (auto error = ValidateGameData(data))
		return error;
	if (offline && data.isExtended())
		return GameDataError::UnsupportedRules;
	return std::nullopt;
}

std::optional<GameDataError> CheckGameCompatibility(const GameData &host, const GameData &client)
{
	if (auto error = ValidateGameData(host))
		return error;
	if (auto error = ValidateGameData(client))
		return error;
	if (host.versionMajor != client.versionMajor || host.versionMinor != client.versionMinor || host.versionPatch != client.versionPatch)
		return GameDataError::VersionMismatch;
	if (host.isSpawn != client.isSpawn || (host.isExtended() && host.isHellfire != client.isHellfire))
		return GameDataError::EditionMismatch;
	if (host.rules != client.rules)
		return GameDataError::SessionMismatch;
	return std::nullopt;
}

std::string_view GameDataErrorMessage(GameDataError error)
{
	switch (error) {
	case GameDataError::InvalidSize:
		return _("Invalid multiplayer game data size.");
	case GameDataError::InvalidParameters:
		return _("Invalid multiplayer game settings.");
	case GameDataError::UnsupportedRules:
		return _("Unsupported multiplayer protocol, capacity, or balance revision.");
	case GameDataError::VersionMismatch:
		return _("The client and host game versions do not match.");
	case GameDataError::EditionMismatch:
		return _("The client and host game editions do not match.");
	case GameDataError::SessionFull:
		return _("The multiplayer game is full.");
	case GameDataError::SessionMismatch:
		return _("The client and host must use the same classic or extended multiplayer mode.");
	}
	return _("Incompatible multiplayer session.");
}

GameData sgGameInitInfo;
bool gbRunGame;
bool gbIsMultiplayer;
bool gbIsSpawn;
bool gbIsHellfire;
bool gbVanilla;
bool forceHellfire;

} // namespace devilution
