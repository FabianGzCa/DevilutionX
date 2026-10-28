#include "panels/partypanel.hpp"

#include <algorithm>
#include <array>
#include <expected>
#include <optional>
#include <string>

#include "automap.h"
#include "control/control.hpp"
#include "engine/backbuffer_state.hpp"
#include "engine/clx_sprite.hpp"
#include "engine/load_cel.hpp"
#include "engine/load_clx.hpp"
#include "engine/palette.h"
#include "engine/rectangle.hpp"
#include "engine/render/clx_render.hpp"
#include "engine/render/primitive_render.hpp"
#include "engine/size.hpp"
#include "inv.h"
#include "options.h"
#include "panels/partypanel_layout.hpp"
#include "pfile.h"
#include "player_visual_identity.hpp"
#include "qol/monhealthbar.h"
#include "qol/stash.h"
#include "stores.h"
#include "tables/playerdat.hpp"
#include "utils/status_macros.hpp"
#include "utils/surface_to_clx.hpp"

namespace devilution {

namespace {

OptionalOwnedClxSpriteList PartyMemberFrame;
OptionalOwnedClxSpriteList PlayerTags;

Point PartyPanelPos = { 8, 8 };
std::array<Rectangle, PlayerStorageCapacity> PortraitFrameRects {};
int RightClickedPortraitIndex = -1;
constexpr Size PortraitFrameSize = PartyPanelPortraitSize;

constexpr uint8_t FrameBackgroundColor = PAL16_BLUE + 14;

void DrawBar(const Surface &out, Rectangle rect, uint8_t color)
{
	for (int x = 0; x < rect.size.width; x++) {
		DrawVerticalLine(out, { rect.position.x + x, rect.position.y }, rect.size.height, color);
	}
}

void DrawMemberFrame(const Surface &out, OwnedClxSpriteList &frame, Point pos)
{
	// Draw the frame background
	FillRect(out, pos.x, pos.y, PortraitFrameSize.width, PortraitFrameSize.height, FrameBackgroundColor);

	// Now draw the frame border
	const Size adjustedFrame = { PartyPanelFrameSections.width - 1, PartyPanelFrameSections.height - 1 };
	for (int x = 0; x <= adjustedFrame.width; x++) {
		for (int y = 0; y <= adjustedFrame.height; y++) {
			// Get what section of the frame we're drawing
			int spriteIndex = -1;
			if (x == 0 && y == 0)
				spriteIndex = 0; // Top-left corner
			else if (x == 0 && y == adjustedFrame.height)
				spriteIndex = 1; // Bottom-left corner
			else if (x == adjustedFrame.width && y == adjustedFrame.height)
				spriteIndex = 2; // Bottom-right corner
			else if (x == adjustedFrame.width && y == 0)
				spriteIndex = 3; // Top-right corner
			else if (y == 0)
				spriteIndex = 4; // Top border
			else if (x == 0)
				spriteIndex = 5; // Left border
			else if (y == adjustedFrame.height)
				spriteIndex = 6; // Bottom border
			else if (x == adjustedFrame.width)
				spriteIndex = 7; // Right border

			if (spriteIndex != -1) {
				// Draw the frame section
				RenderClxSprite(out, frame[spriteIndex], { pos.x + (x * PartyPanelFrameSpriteSize), pos.y + (y * PartyPanelFrameSpriteSize) });
			}
		}
	}
}

void HandleRightClickPortrait()
{
	const int playerId = RightClickedPortraitIndex;
	RightClickedPortraitIndex = -1;
	if (playerId < 0 || playerId >= static_cast<int>(Players.size()) || playerId >= PlayerStorageCapacity)
		return;
	Player &player = Players[playerId];
	if (player.plractive && player.friendlyMode && &player != MyPlayer) {
		InspectPlayer = &player;
		OpenCharPanel();
		if (!SpellbookFlag)
			invflag = true;
		RedrawEverything();
	}
}

struct PartyPanelEntry {
	Player *player = nullptr;
	Monster *summon = nullptr;
};

void ClearPortraitHitboxes()
{
	PortraitFrameRects.fill({ { 0, 0 }, { 0, 0 } });
}

} // namespace

bool PartySidePanelOpen = true;
bool InspectingFromPartyPanel;
int PortraitIdUnderCursor = -1;

std::expected<void, std::string> LoadPartyPanel()
{
	ASSIGN_OR_RETURN(OwnedClxSpriteList frame, LoadCelWithStatus("data\\textslid", PartyPanelFrameSpriteSize));
	ASSIGN_OR_RETURN(PlayerTags, LoadClxWithStatus("data\\monstertags.clx"));
	const OwnedSurface out(PortraitFrameSize.width, PortraitFrameSize.height + PartyPanelHealthBarHeight + PartyPanelManaBarHeight);

	// Draw the health bar background
	DrawBar(out, { { 0, 0 }, { PortraitFrameSize.width, PartyPanelHealthBarHeight } }, PAL16_GRAY + 10);
	// Draw the frame the character portrait sprite will go
	DrawMemberFrame(out, frame, { 0, PartyPanelHealthBarHeight });
	// Draw the mana bar background
	DrawBar(out, { { 0, PartyPanelHealthBarHeight + PortraitFrameSize.height }, { PortraitFrameSize.width, PartyPanelManaBarHeight } }, PAL16_GRAY + 10);

	PartyMemberFrame = SurfaceToClx(out);

	return {};
}

void FreePartyPanel()
{
	PartyMemberFrame = std::nullopt;
	PlayerTags = std::nullopt;
	ClearPortraitHitboxes();
	RightClickedPortraitIndex = -1;
}

void DrawPartyMemberInfoPanel(const Surface &out)
{
	ClearPortraitHitboxes();
	PortraitIdUnderCursor = -1;
	if (!PartyMemberFrame.has_value()) {
		RightClickedPortraitIndex = -1;
		return;
	}

	if (MyPlayer == nullptr || CharFlag || !gbIsMultiplayer || !MyPlayer->friendlyMode || IsPlayerInStore() || IsStashOpen) {
		RightClickedPortraitIndex = -1;
		return;
	}

	const PartyPortraits partyOption = *GetOptions().Gameplay.showMultiplayerPartyInfo;
	if (!PartySidePanelOpen || partyOption == PartyPortraits::Disabled) {
		RightClickedPortraitIndex = -1;
		return;
	}
	std::array<PartyPanelEntry, PartyPanelMemberCapacity> entries {};
	size_t entryCount = 0;
	if (partyOption == PartyPortraits::Players || partyOption == PartyPortraits::PlayersAndSummons) {
		for (Player &player : Players) {
			if (entryCount == entries.size() || player.getId() >= PlayerStorageCapacity || !player.plractive || !player.friendlyMode)
				continue;
#ifndef _DEBUG
			if (&player == MyPlayer)
				continue;
#endif
			entries[entryCount++] = { &player, nullptr };
		}
	}

	if (partyOption == PartyPortraits::Summons || partyOption == PartyPortraits::PlayersAndSummons) {
		if (entryCount < entries.size()) {
			if (Monster *summoned = FindGolemForPlayer(*MyPlayer); summoned != nullptr)
				entries[entryCount++] = { nullptr, summoned };
		}
	}

	const int viewportHeight = std::min<int>(gnViewportHeight, out.h());
	const Surface gameScreen = out.subregionY(0, viewportHeight);
	const Rectangle viewportBounds { { 0, 0 }, { out.w(), viewportHeight } };
	int preferredTop = PartyPanelPos.y;
	if (AutomapActive)
		preferredTop += PartyPanelFrameGap * 4;
	if (*GetOptions().Graphics.showFPS)
		preferredTop += PartyPanelFrameGap;
	int layoutWidth = out.w();
	if (IsRightPanelOpen() && GetRightPanel().position.x > 0)
		layoutWidth = std::min(layoutWidth, GetRightPanel().position.x);
	const PartyPanelLayout layout = ComputePartyPanelLayout(entryCount, layoutWidth, viewportHeight, preferredTop);

	for (size_t i = 0; i < layout.count; i++) {
		const PartyPanelEntry &entry = entries[i];
		const PartyPanelCardLayout &card = layout.cards[i];
		const Rectangle portraitHitbox = ClipPartyPanelRectangle(card.portrait, viewportBounds);
		if (portraitHitbox.size.width == 0 || portraitHitbox.size.height == 0)
			continue;

		RenderClxSprite(gameScreen, (*PartyMemberFrame)[0], card.healthBarPosition);
		if (entry.player != nullptr) {
			Player &player = *entry.player;
			const int lifeTicks = GetPartyPanelBarWidth(player._pHitPoints.raw(), player._pMaxHP.raw());
			const uint8_t hpBarColor = player.pManaShield ? PAL8_YELLOW + 5 : PAL8_RED + 4;
			DrawBar(gameScreen, { card.healthBarPosition, { lifeTicks, PartyPanelHealthBarHeight } }, hpBarColor);

			const ClxSprite playerPortraitSprite = GetPlayerPortraitSprite(player);
			const PlayerSpriteData &spriteData = GetPlayerSpriteDataForClass(player._pClass);
			Point offset = player.isOnLevel(0) ? spriteData.partyOffsetTown : spriteData.partyOffsetDungeon;
			if (player._pHitPoints <= 0 && IsPlayerUnarmed(player))
				offset = spriteData.partyOffsetDead;
			const Point portraitPos = { (-(playerPortraitSprite.width() / 2) + (PortraitFrameSize.width / 2)) + offset.x, offset.y };
			const Surface frameSubregion = gameScreen.subregion(
			    card.portrait.position.x + PartyPanelFrameBorderSize,
			    card.portrait.position.y + PartyPanelFrameBorderSize,
			    PortraitFrameSize.width - (PartyPanelFrameBorderSize * 2),
			    PortraitFrameSize.height - (PartyPanelFrameBorderSize * 2));
			RenderClxSprite(frameSubregion, playerPortraitSprite, portraitPos);

			const PlayerVisualIdentity &identity = GetPlayerVisualIdentity(player.getId());
			if (PlayerTags.has_value() && identity.tagSprite < (*PlayerTags).numSprites()) {
				const int tagWidth = (*PlayerTags)[identity.tagSprite].width();
				RenderClxSprite(frameSubregion, (*PlayerTags)[identity.tagSprite],
				    { PortraitFrameSize.width - (tagWidth + (tagWidth / 2)), 0 });
			}

			if (player._pHitPoints <= 0)
				DrawHalfTransparentRectTo(frameSubregion, 0, 0, PortraitFrameSize.width, PortraitFrameSize.height, PAL8_RED + 4);

			const int manaTicks = GetPartyPanelBarWidth(player._pMana.raw(), player._pMaxMana.raw());
			DrawBar(gameScreen, { card.manaBarPosition, { manaTicks, PartyPanelManaBarHeight } }, PAL8_BLUE + 3);

			const int labelWidth = GetLineWidth(identity.nameLabel) + 2;
			const int visibleNameWidth = std::max(0, card.nameArea.size.width - labelWidth);
			DrawString(gameScreen, identity.nameLabel, card.nameArea,
			    { .flags = UiFlags::ColorGold | UiFlags::Outlined | UiFlags::FontSize12 });
			if (visibleNameWidth > 0) {
				const std::string name = TruncatePartyPanelName(player._pName, visibleNameWidth);
				const Rectangle nameArea {
					card.nameArea.position + Displacement { labelWidth, 0 },
					{ visibleNameWidth, PartyPanelNameHeight }
				};
				DrawString(gameScreen, name, nameArea,
				    { .flags = UiFlags::ColorGold | UiFlags::Outlined | UiFlags::FontSize12 });
			}

			PortraitFrameRects[player.getId()] = portraitHitbox;
			if (portraitHitbox.contains(MousePosition)) {
				PortraitIdUnderCursor = player.getId();
			}
		} else if (entry.summon != nullptr) {
			Monster &summoned = *entry.summon;
			const int lifeTicks = GetPartyPanelBarWidth(summoned.hitPoints.raw(), summoned.maxHitPoints.raw());
			DrawBar(gameScreen, { card.healthBarPosition, { lifeTicks, PartyPanelHealthBarHeight } }, PAL8_RED + 4);

			OptionalClxSpriteList monsterPortraitSpriteList = summoned.type().getAnimData(MonsterGraphic::Stand).spritesForDirection(Direction::South);
			if (!monsterPortraitSpriteList.has_value())
				monsterPortraitSpriteList = summoned.type().getAnimData(MonsterGraphic::Walk).spritesForDirection(Direction::South);
			if (monsterPortraitSpriteList.has_value()) {
				const ClxSprite monsterPortraitSprite = (*monsterPortraitSpriteList)[0];
				const Point portraitPos = { (-(monsterPortraitSprite.width() * 17 / 32) + (PortraitFrameSize.width / 2)), -(monsterPortraitSprite.height() / 4) };
				const Surface frameSubregion = gameScreen.subregion(
				    card.portrait.position.x + PartyPanelFrameBorderSize,
				    card.portrait.position.y + PartyPanelFrameBorderSize,
				    PortraitFrameSize.width - (PartyPanelFrameBorderSize * 2),
				    PortraitFrameSize.height - (PartyPanelFrameBorderSize * 2));
				RenderClxSprite(frameSubregion, monsterPortraitSprite, portraitPos);
				if (summoned.hitPoints <= 0)
					DrawHalfTransparentRectTo(frameSubregion, 0, 0, PortraitFrameSize.width, PortraitFrameSize.height, PAL8_RED + 4);
			}

			DrawString(gameScreen, summoned.name(), card.nameArea,
			    { .flags = UiFlags::ColorGold | UiFlags::Outlined | UiFlags::FontSize12 });
		}
	}

	if (RightClickedPortraitIndex != -1)
		HandleRightClickPortrait();
}

bool DidRightClickPartyPortrait()
{
	const PartyPortraits partyOption = *GetOptions().Gameplay.showMultiplayerPartyInfo;
	if (!PartySidePanelOpen || MyPlayer == nullptr || !gbIsMultiplayer || !MyPlayer->friendlyMode || CharFlag || IsPlayerInStore() || IsStashOpen
	    || (partyOption != PartyPortraits::Players && partyOption != PartyPortraits::PlayersAndSummons)) {
		ClearPortraitHitboxes();
		RightClickedPortraitIndex = -1;
		return false;
	}

	for (size_t i = 0; i < PortraitFrameRects.size(); i++) {
		if (i >= Players.size() || !Players[i].plractive || !Players[i].friendlyMode || &Players[i] == MyPlayer)
			continue;
		if (PortraitFrameRects[i].contains(MousePosition)) {
			RightClickedPortraitIndex = static_cast<int>(i);
			InspectingFromPartyPanel = true;
			return true;
		}
	}

	return false;
}

} // namespace devilution
