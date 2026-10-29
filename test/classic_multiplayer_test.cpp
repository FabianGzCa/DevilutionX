#include <algorithm>
#include <array>
#include <bit>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <expected>
#include <set>
#include <string>
#include <vector>

#include <gtest/gtest.h>

#include "automap.h"
#include "engine/assets.hpp"
#include "engine/load_clx.hpp"
#include "engine/load_file.hpp"
#include "engine/render/clx_render.hpp"
#include "engine/render/primitive_render.hpp"
#include "engine/render/text_render.hpp"
#include "engine/surface.hpp"
#include "game_mode.hpp"
#include "levels/dun_tile_data.hpp"
#include "monster.h"
#include "multi.h"
#include "options.h"
#include "player.h"
#include "player_mask.hpp"
#include "player_visual_identity.hpp"
#include "portal.h"
#include "storm/storm_net.hpp"
#include "tables/playerdat.hpp"
#include "utils/fixed_point.hpp"
#include "utils/sdl_ptrs.h"
#include "utils/sdl_wrap.h"
#include "utils/surface_to_png.hpp"

namespace devilution {
namespace {

std::expected<void, std::string> WritePlayerVisualGallery(const ClxSpriteList &playerTags, std::string_view outputDirectory, int width, int height)
{
	struct PaletteColor {
		uint8_t r;
		uint8_t g;
		uint8_t b;
	};
	static_assert(sizeof(PaletteColor) == 3);
	std::array<PaletteColor, 256> paletteData;
	LoadFileInMem("ui_art\\diablo.pal", paletteData);
	SDLPaletteUniquePtr palette = SDLWrap::AllocPalette(static_cast<int>(paletteData.size()));
	for (size_t i = 0; i < paletteData.size(); ++i) {
		palette->colors[i] = { paletteData[i].r, paletteData[i].g, paletteData[i].b, SDL_ALPHA_OPAQUE };
	}

	OwnedSurface out(width, height);
	SDL_SetSurfacePalette(out.surface, palette.get());
	FillRect(out, 0, 0, width, height, 239);
	const int left = (width - 640) / 2;
	DrawString(out, "PLAYER IDENTITIES", { left + 32, 24 }, { .flags = UiFlags::ColorWhite | UiFlags::Outlined | UiFlags::FontSize12 });
	DrawString(out, "MAP NUMBER / OUTLINE", { left + 250, 44 }, { .flags = UiFlags::ColorWhite | UiFlags::Outlined | UiFlags::FontSize12 });

	for (int playerId = 0; playerId < PlayerStorageCapacity; ++playerId) {
		const PlayerVisualIdentity &identity = GetPlayerVisualIdentity(playerId);
		const int row = 64 + playerId * 42;
		DrawPlayerTag(out, playerTags, { left + 32, row }, playerId);
		std::string label(identity.nameLabel);
		label += " Player ";
		label += identity.number;
		DrawString(out, label, { left + 56, row + 4 }, { .flags = UiFlags::ColorWhitegold | UiFlags::Outlined | UiFlags::FontSize12 });
		DrawAutomapPlayerIdentifier(out, { left + 300, row + 12 }, playerId);
		FillRect(out, left + 250, row + 3, 18, 18, identity.outlineColor);
		FillRect(out, left + 254, row + 7, 10, 10, identity.automapColor);
	}

	const std::string filename = StrCat(outputDirectory, "/player-identities-", width, "x", height, ".png");
#ifdef USE_SDL3
	SDL_IOStream *file = SDL_IOFromFile(filename.c_str(), "wb");
#else
	SDL_RWops *file = SDL_RWFromFile(filename.c_str(), "wb");
#endif
	if (file == nullptr)
		return std::unexpected(SDL_GetError());
	return WriteSurfaceToFilePng(out, file);
}

class ClassicMultiplayerTest : public ::testing::Test {
protected:
	static void SetUpTestSuite()
	{
		LoadCoreArchives();
		LoadPlayerDataFiles();
	}

	void SetUp() override
	{
		savedPlayerIdentifiers = *GetOptions().Gameplay.showPlayerIdentifiers;
		GetOptions().Gameplay.showPlayerIdentifiers.SetValue(false);
		gbIsMultiplayer = true;
		gbIsHellfire = false;
		gbIsSpawn = false;
		leveltype = DTYPE_CATHEDRAL;
		Players.clear();
		ResizePlayersForSession(SessionPlayerLimit);
		MyPlayerId = 0;
		MyPlayer = &Players[MyPlayerId];
		InspectPlayer = MyPlayer;
		for (Player &player : Players) {
			player.setCharacterLevel(1);
			player._pHitPoints = Fixed26_6::fromInt(100);
			player._pMaxHP = player._pHitPoints;
		}
		InitPortals();
		ASSERT_TRUE(SNetInitializeProvider(SELCONN_LOOPBACK, nullptr));
	}

	void TearDown() override
	{
		GetOptions().Gameplay.showPlayerIdentifiers.SetValue(savedPlayerIdentifiers);
		SNetDestroy();
		InitPortals();
		Players.clear();
		MyPlayer = nullptr;
		InspectPlayer = nullptr;
		MyPlayerId = 0;
		gbIsMultiplayer = false;
		leveltype = DTYPE_TOWN;
	}

private:
	bool savedPlayerIdentifiers = false;
};

TEST_F(ClassicMultiplayerTest, AdvertisesFourPlayerSlots)
{
	_SNETCAPS caps {};
	SNetGetProviderCaps(&caps);
	EXPECT_EQ(caps.maxplayers, 4U);
}

TEST_F(ClassicMultiplayerTest, PlayerLimitsAndReservedMonsterSlotRemainDistinct)
{
	EXPECT_EQ(ClassicPlayerLimit, 4);
	EXPECT_EQ(SessionPlayerLimit, 4);
	EXPECT_GE(NoDeltaSender, PlayerStorageCapacity);
	EXPECT_EQ(PlayerStorageCapacity, 8);
	EXPECT_EQ(ZharMonsterIndex, 4U);
	EXPECT_EQ(Players.size(), static_cast<size_t>(SessionPlayerLimit));
	EXPECT_GE(Players.capacity(), static_cast<size_t>(PlayerStorageCapacity));
}

TEST_F(ClassicMultiplayerTest, GrowingPlayerStorageDoesNotMoveSessionPointers)
{
	ResizePlayersForSession(1);
	MyPlayer = &Players[0];
	InspectPlayer = MyPlayer;
	Player *const localPlayer = MyPlayer;

	Players.resize(PlayerStorageCapacity);

	EXPECT_EQ(Players.size(), static_cast<size_t>(PlayerStorageCapacity));
	EXPECT_EQ(MyPlayer, localPlayer);
	EXPECT_EQ(InspectPlayer, localPlayer);
}

TEST_F(ClassicMultiplayerTest, PlayerVisualIdentitiesCoverAllEightPlayers)
{
	std::set<uint8_t> automapColors;
	std::set<uint8_t> outlineColors;
	for (int playerId = 0; playerId < PlayerStorageCapacity; ++playerId) {
		const PlayerVisualIdentity &identity = GetPlayerVisualIdentity(playerId);
		EXPECT_EQ(identity.number, '1' + playerId);
		EXPECT_TRUE(automapColors.insert(identity.automapColor).second);
		EXPECT_TRUE(outlineColors.insert(identity.outlineColor).second);
		uint8_t glyphPixels = 0;
		for (uint8_t row : identity.automapNumber)
			glyphPixels |= row;
		EXPECT_NE(glyphPixels, 0);
		for (uint8_t row : identity.automapNumber)
			EXPECT_LE(row, 0b111);
	}
	EXPECT_EQ(automapColors.size(), static_cast<size_t>(PlayerStorageCapacity));
	EXPECT_EQ(outlineColors.size(), static_cast<size_t>(PlayerStorageCapacity));
}

TEST_F(ClassicMultiplayerTest, PlayerVisualIdentityPreservesClassicColorsAndBoundsInvalidIds)
{
	EXPECT_EQ(GetPlayerVisualIdentity(0).automapColor, PAL8_ORANGE + 1);
	EXPECT_EQ(GetPlayerVisualIdentity(1).automapColor, PAL8_YELLOW + 1);
	EXPECT_EQ(GetPlayerVisualIdentity(2).automapColor, PAL8_RED + 1);
	EXPECT_EQ(GetPlayerVisualIdentity(3).automapColor, PAL8_BLUE + 1);
	EXPECT_EQ(GetPlayerVisualIdentity(0).outlineColor, PAL16_ORANGE + 7);
	EXPECT_EQ(GetPlayerVisualIdentity(1).outlineColor, PAL16_YELLOW + 7);
	EXPECT_EQ(GetPlayerVisualIdentity(2).outlineColor, PAL16_RED + 7);
	EXPECT_EQ(GetPlayerVisualIdentity(3).outlineColor, PAL16_BLUE + 7);
	EXPECT_EQ(&GetPlayerVisualIdentity(-1), &PlayerVisualIdentities[0]);
	EXPECT_EQ(&GetPlayerVisualIdentity(PlayerStorageCapacity), &PlayerVisualIdentities[0]);
}

TEST_F(ClassicMultiplayerTest, AutomapPlayerIdentifiersRenderAllEightNumbers)
{
	GetOptions().Gameplay.showPlayerIdentifiers.SetValue(true);
	OwnedSurface marker(5, 7);
	for (int playerId = 0; playerId < PlayerStorageCapacity; ++playerId) {
		FillRect(marker, 0, 0, marker.w(), marker.h(), 0);
		DrawAutomapPlayerIdentifier(marker, { 2, 3 }, playerId);
		const PlayerVisualIdentity &identity = GetPlayerVisualIdentity(playerId);
		for (int y = 0; y < 7; ++y) {
			for (int x = 0; x < 5; ++x) {
				uint8_t expectedColor = 239;
				if (x > 0 && x < 4 && y > 0 && y < 6) {
					const bool isNumberPixel = (identity.automapNumber[y - 1] & (1 << (3 - x))) != 0;
					if (isNumberPixel)
						expectedColor = 255;
				}
				const Point pixelPosition { x, y };
				EXPECT_EQ(marker[pixelPosition], expectedColor) << "player " << playerId + 1 << ", pixel " << x << ',' << y;
			}
		}
	}
	DrawAutomapPlayerIdentifier(marker, { 2, 3 }, -1);
	const Point centerPixel { 2, 3 };
	EXPECT_EQ(marker[centerPixel], 255);
}

TEST_F(ClassicMultiplayerTest, PlayerIdentifiersAreDisabledByDefaultAndCanChangeLocally)
{
	GameplayOptions defaults;
	EXPECT_FALSE(*defaults.showPlayerIdentifiers);
	const std::vector<OptionEntryBase *> entries = defaults.GetEntries();
	EXPECT_NE(std::find(entries.begin(), entries.end(), &defaults.showPlayerIdentifiers), entries.end());
	EXPECT_EQ(defaults.showPlayerIdentifiers.GetFlags(), OptionEntryFlags::None);

	// Compare contents: on Windows the test executable and the engine DLL each hold
	// their own copy of the inline identity table.
	const auto sameIdentity = [](const PlayerVisualIdentity &a, const PlayerVisualIdentity &b) {
		return a.number == b.number && a.automapColor == b.automapColor && a.outlineColor == b.outlineColor;
	};
	OwnedSurface marker(5, 7);
	for (const bool enabled : { false, true, false }) {
		GetOptions().Gameplay.showPlayerIdentifiers.SetValue(enabled);
		for (int playerId = 0; playerId < PlayerStorageCapacity; ++playerId) {
			SCOPED_TRACE(playerId);
			FillRect(marker, 0, 0, marker.w(), marker.h(), 1);
			DrawAutomapPlayerIdentifier(marker, { 2, 3 }, playerId);
			if (enabled) {
				EXPECT_FALSE(GetPlayerNameLabel(playerId).empty());
				EXPECT_TRUE(sameIdentity(GetConfiguredPlayerVisualIdentity(playerId), GetPlayerVisualIdentity(playerId)));
				EXPECT_EQ(marker[Point(0, 0)], 239);
			} else {
				EXPECT_TRUE(GetPlayerNameLabel(playerId).empty());
				EXPECT_TRUE(sameIdentity(GetConfiguredPlayerVisualIdentity(playerId), GetPlayerVisualIdentity(playerId % ClassicPlayerLimit)));
				for (int y = 0; y < marker.h(); ++y) {
					for (int x = 0; x < marker.w(); ++x)
						EXPECT_EQ(marker[Point(x, y)], 1);
				}
			}
		}
	}
}

TEST_F(ClassicMultiplayerTest, PlayerTagsKeepClassicPixelsUnlessIdentifiersAreEnabled)
{
	const OwnedClxSpriteList playerTags = LoadClx("data\\monstertags.clx");
	ASSERT_EQ(playerTags.numSprites(), static_cast<uint32_t>(ClassicPlayerLimit + 1));
	OwnedSurface expected(13, 22);
	OwnedSurface actual(13, 22);
	for (int playerId = 0; playerId < PlayerStorageCapacity; ++playerId) {
		SCOPED_TRACE(playerId);
		const ClxSprite tag = playerTags[playerId % ClassicPlayerLimit + 1];
		ASSERT_EQ(tag.width(), expected.w());
		ASSERT_EQ(tag.height(), expected.h());
		FillRect(expected, 0, 0, expected.w(), expected.h(), 1);
		RenderClxSprite(expected, tag, { 0, 0 });
		for (const bool enabled : { false, true, false }) {
			GetOptions().Gameplay.showPlayerIdentifiers.SetValue(enabled);
			FillRect(actual, 0, 0, actual.w(), actual.h(), 1);
			DrawPlayerTag(actual, playerTags, { 0, 0 }, playerId);
			const PlayerVisualIdentity &identity = GetPlayerVisualIdentity(playerId);
			for (int y = 0; y < actual.h(); ++y) {
				for (int x = 0; x < actual.w(); ++x) {
					const Point pixel { x, y };
					uint8_t expectedColor = expected[pixel];
					if (enabled && x >= 4 && x <= 8 && y >= 3 && y <= 9) {
						expectedColor = 239;
						if (x >= 5 && x <= 7 && y >= 4 && y <= 8 && (identity.automapNumber[y - 4] & (1 << (7 - x))) != 0)
							expectedColor = 255;
					}
					EXPECT_EQ(actual[pixel], expectedColor) << "pixel " << x << ',' << y;
				}
			}
		}
	}
	GetOptions().Gameplay.showPlayerIdentifiers.SetValue(true);
	if (const char *captureDirectory = std::getenv("DEVILUTIONX_PLAYER_VISUAL_CAPTURE_DIR")) {
		ASSERT_TRUE(WritePlayerVisualGallery(playerTags, captureDirectory, 640, 480).has_value());
		ASSERT_TRUE(WritePlayerVisualGallery(playerTags, captureDirectory, 1280, 720).has_value());
	}
}

TEST_F(ClassicMultiplayerTest, TurnBuffersRespectTheSessionPlayerCount)
{
	for (const size_t count : { size_t { 1 }, static_cast<size_t>(SessionPlayerLimit) }) {
		SCOPED_TRACE(count);
		Players.resize(count);
		MyPlayer = &Players[0];
		char sentinel = 0;
		std::array<char *, ClassicPlayerLimit + 1> messages {};
		messages.fill(&sentinel);
		std::array<size_t, ClassicPlayerLimit + 1> sizes {};
		sizes.fill(99);
		std::array<uint32_t, ClassicPlayerLimit + 1> status {};

		ASSERT_TRUE(SNetReceiveTurns(ClassicPlayerLimit, messages.data(), sizes.data(), status.data()));

		for (size_t i = 0; i < count; i++) {
			EXPECT_EQ(messages[i], nullptr);
			EXPECT_EQ(sizes[i], 0U);
		}
		for (size_t i = count; i < messages.size(); i++) {
			EXPECT_EQ(messages[i], &sentinel);
			EXPECT_EQ(sizes[i], 99U);
		}
	}
}

TEST_F(ClassicMultiplayerTest, ExperienceIsSharedOnlyAmongContributors)
{
	for (unsigned mask = 0; mask < (1U << ClassicPlayerLimit); mask++) {
		SCOPED_TRACE(mask);
		const unsigned contributors = std::popcount(mask);
		for (uint8_t localPlayer = 0; localPlayer < ClassicPlayerLimit; localPlayer++) {
			SCOPED_TRACE(localPlayer);
			MyPlayerId = localPlayer;
			MyPlayer = &Players[localPlayer];
			for (Player &player : Players)
				player._pExperience = 0;

			const PlayerExperienceMask contributorsMask = PlayerExperienceMaskFromByte(static_cast<uint8_t>(mask));
			AddPlrMonstExper(1, 37, contributorsMask);

			const bool contributed = IsPlayerInExperienceMask(contributorsMask, localPlayer);
			const unsigned expected = contributed ? 37 / contributors : 0;
			EXPECT_EQ(MyPlayer->_pExperience, expected);
			for (const Player &player : Players) {
				if (&player != MyPlayer) {
					EXPECT_EQ(player._pExperience, 0U);
				}
			}
		}
	}
}

TEST_F(ClassicMultiplayerTest, ExperienceMasksDistributeAcrossAllEightPlayers)
{
	Players.resize(PlayerStorageCapacity);
	for (Player &player : Players) {
		player.setCharacterLevel(1);
		player._pHitPoints = Fixed26_6::fromInt(100);
	}
	for (unsigned rawMask = 0; rawMask < (1U << PlayerExperienceMaskBitCount); rawMask++) {
		SCOPED_TRACE(rawMask);
		const PlayerExperienceMask contributorsMask = PlayerExperienceMaskFromByte(static_cast<uint8_t>(rawMask));
		const unsigned contributorCount = std::popcount(rawMask);
		for (uint8_t localPlayer = 0; localPlayer < PlayerStorageCapacity; localPlayer++) {
			SCOPED_TRACE(localPlayer);
			MyPlayerId = localPlayer;
			MyPlayer = &Players[localPlayer];
			for (Player &player : Players)
				player._pExperience = 0;

			AddPlrMonstExper(1, 37, contributorsMask);

			const bool contributed = IsPlayerInExperienceMask(contributorsMask, localPlayer);
			const unsigned expected = contributed ? 37 / contributorCount : 0;
			EXPECT_EQ(MyPlayer->_pExperience, expected);
			for (const Player &player : Players) {
				if (&player != MyPlayer) {
					EXPECT_EQ(player._pExperience, 0U);
				}
			}
		}
	}
}

TEST_F(ClassicMultiplayerTest, ExperienceUsesCurrentParticipantCountNotStorageCapacity)
{
	ASSERT_GE(Players.capacity(), static_cast<size_t>(PlayerStorageCapacity));
	const PlayerExperienceMask contributorsMask = PlayerExperienceMaskFromByte(0xFF);
	for (const size_t participantCount : { size_t { 1 }, static_cast<size_t>(ClassicPlayerLimit), static_cast<size_t>(PlayerStorageCapacity) }) {
		SCOPED_TRACE(participantCount);
		Players.resize(participantCount);
		for (Player &player : Players) {
			player.setCharacterLevel(1);
			player._pHitPoints = Fixed26_6::fromInt(100);
			player._pExperience = 0;
		}
		MyPlayerId = static_cast<uint8_t>(participantCount - 1);
		MyPlayer = &Players[MyPlayerId];
		MyPlayer->_pExperience = 0;

		AddPlrMonstExper(1, 37, contributorsMask);

		EXPECT_EQ(MyPlayer->_pExperience, 37 / participantCount);
	}
}

TEST_F(ClassicMultiplayerTest, MonsterDamageMaskTracksAllEightPlayers)
{
	Players.resize(PlayerStorageCapacity);
	Monster monster {};
	for (size_t playerId = 0; playerId < Players.size(); playerId++) {
		monster.tag(Players[playerId]);
		EXPECT_EQ(PlayerExperienceMaskToByte(monster.whoHit), (1U << (playerId + 1)) - 1);
	}
}

TEST_F(ClassicMultiplayerTest, ExperienceMaskByteAndSyncPacketRoundTripAllValues)
{
	for (unsigned rawMask = 0; rawMask < (1U << PlayerExperienceMaskBitCount); rawMask++) {
		SCOPED_TRACE(rawMask);
		const uint8_t rawByte = static_cast<uint8_t>(rawMask);
		const PlayerExperienceMask mask = PlayerExperienceMaskFromByte(rawByte);
		EXPECT_EQ(PlayerExperienceMaskToByte(mask), rawByte);
		const PlayerExperienceMask complement = PlayerExperienceMaskFromByte(static_cast<uint8_t>(0xFFU ^ rawMask));
		EXPECT_EQ(PlayerExperienceMaskToByte(MergePlayerExperienceMasks(mask, complement)), 0xFFU);

		TSyncMonster sent {};
		sent.mWhoHit = mask;
		std::array<std::byte, sizeof(TSyncMonster)> packet {};
		std::memcpy(packet.data(), &sent, sizeof(sent));
		EXPECT_EQ(packet[offsetof(TSyncMonster, mWhoHit)], std::byte { rawByte });

		TSyncMonster received {};
		std::memcpy(&received, packet.data(), sizeof(received));
		EXPECT_EQ(PlayerExperienceMaskToByte(received.mWhoHit), rawByte);
	}
}

TEST_F(ClassicMultiplayerTest, RecipientMasksCoverAllEightPlayerBits)
{
	PlayerRecipientMask allRecipients { 0 };
	for (size_t playerId = 0; playerId < PlayerStorageCapacity; playerId++)
		allRecipients = AddPlayerToRecipientMask(allRecipients, playerId);

	EXPECT_EQ(allRecipients.bits, 0xFFU);
	EXPECT_EQ(AllPlayerRecipients().bits, UINT32_MAX);
	EXPECT_FALSE(IsPlayerInRecipientMask(allRecipients, PlayerStorageCapacity));

	for (unsigned rawMask = 0; rawMask < (1U << PlayerStorageCapacity); rawMask++) {
		SCOPED_TRACE(rawMask);
		const PlayerRecipientMask recipients { rawMask };
		for (size_t playerId = 0; playerId < PlayerStorageCapacity; playerId++) {
			const bool expected = (rawMask & (1U << playerId)) != 0;
			EXPECT_EQ(IsPlayerInRecipientMask(recipients, playerId), expected);
		}
	}
}

TEST_F(ClassicMultiplayerTest, MultiplayerExperienceRetainsThePowerLevelingCap)
{
	AddPlrMonstExper(1, 10000, PlayerExperienceMaskFromByte(1));
	EXPECT_EQ(MyPlayer->_pExperience, 100U);
}

TEST_F(ClassicMultiplayerTest, SinglePlayerIgnoresUnusedContributorSlots)
{
	Players.resize(1);
	MyPlayer = &Players[0];
	gbIsMultiplayer = false;

	AddPlrMonstExper(1, 150, PlayerExperienceMaskFromByte(15));

	EXPECT_EQ(MyPlayer->_pExperience, 150U);
}

TEST_F(ClassicMultiplayerTest, DeadPlayersDoNotGainExperienceInTheDungeon)
{
	MyPlayer->_pHitPoints = Fixed26_6::fromInt(0);
	AddPlrMonstExper(1, 37, PlayerExperienceMaskFromByte(1));
	EXPECT_EQ(MyPlayer->_pExperience, 0U);
}

TEST_F(ClassicMultiplayerTest, PortalTownArrivalPositionsRemainClassic)
{
	leveltype = DTYPE_TOWN;
	const std::array<Point, ClassicPlayerLimit> expected = { { { 58, 41 }, { 60, 41 }, { 62, 41 }, { 64, 41 } } };
	for (size_t owner = 0; owner < expected.size(); owner++) {
		SCOPED_TRACE(owner);
		SetCurrentPortal(owner);
		GetPortalLvlPos();
		EXPECT_EQ(ViewPosition, expected[owner]);
	}
}

TEST_F(ClassicMultiplayerTest, PortalArrivalDistinguishesTheOwnerFromOtherPlayers)
{
	for (size_t owner = 0; owner < ClassicPlayerLimit; owner++) {
		SCOPED_TRACE(owner);
		const Point destination { 40, 45 };
		SetPortalStats(static_cast<int>(owner), true, destination, 5, DTYPE_CATACOMBS, false);
		SetCurrentPortal(owner);
		for (uint8_t visitor = 0; visitor < ClassicPlayerLimit; visitor++) {
			SCOPED_TRACE(visitor);
			MyPlayerId = visitor;
			MyPlayer = &Players[visitor];
			GetPortalLvlPos();
			const Point expected = visitor == owner ? destination : destination + Displacement { 1, 1 };
			EXPECT_EQ(ViewPosition, expected);
		}
	}
}

TEST_F(ClassicMultiplayerTest, ClosingAPortalDoesNotCloseOtherPlayersPortals)
{
	for (int owner = 0; owner < ClassicPlayerLimit; owner++)
		SetPortalStats(owner, true, { 40 + owner * 4, 45 }, 5, DTYPE_CATACOMBS, false);

	DeactivatePortal(Players[2]);

	for (int owner = 0; owner < ClassicPlayerLimit; owner++) {
		SCOPED_TRACE(owner);
		EXPECT_EQ(PosOkPortal(5, { 40 + owner * 4, 45 }), owner != 2);
		EXPECT_FALSE(PosOkPortal(6, { 40 + owner * 4, 45 }));
	}
}

} // namespace
} // namespace devilution
