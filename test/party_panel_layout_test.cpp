/**
 * @file party_panel_layout_test.cpp
 *
 * Regression tests for party portrait placement, clipping, and bar sizing.
 */

#include <climits>
#include <string>
#include <string_view>

#include <gtest/gtest.h>

#include "automap.h"
#include "engine/backbuffer_state.hpp"
#include "engine/surface.hpp"
#include "multi.h"
#include "panels/partypanel.hpp"
#include "panels/partypanel_layout.hpp"
#include "player.h"
#include "ui_test.hpp"
#include "utils/ui_fwd.h"
#include "utils/utf8.hpp"

namespace devilution {
namespace {

bool Overlaps(const Rectangle &left, const Rectangle &right)
{
	return left.position.x < right.position.x + right.size.width
	    && right.position.x < left.position.x + left.size.width
	    && left.position.y < right.position.y + right.size.height
	    && right.position.y < left.position.y + left.size.height;
}

class PartyPanelLayoutTest : public ::testing::Test {
};

TEST_F(PartyPanelLayoutTest, EightPlayersAndSummonFitAtClassicResolution)
{
	constexpr int ScreenWidth = 640;
	constexpr int ViewportHeight = 352;
	const PartyPanelLayout layout = ComputePartyPanelLayout(PartyPanelMemberCapacity, ScreenWidth, ViewportHeight, 83);

	ASSERT_EQ(layout.count, PartyPanelMemberCapacity);
	EXPECT_EQ(layout.cards[0].healthBarPosition, Point(8, 83));
	EXPECT_EQ(layout.cards[1].portrait.position.y, layout.cards[0].portrait.position.y + PartyPanelRowAdvance);
	EXPECT_EQ(layout.cards[3].portrait.position.x, layout.cards[0].portrait.position.x + PartyPanelColumnWidthLimit);

	const Rectangle viewport { { 0, 0 }, { ScreenWidth, ViewportHeight } };
	for (size_t i = 0; i < layout.count; i++) {
		const PartyPanelCardLayout &card = layout.cards[i];
		EXPECT_EQ(ClipPartyPanelRectangle(card.portrait, viewport).size, card.portrait.size);
		EXPECT_LE(card.healthBarPosition.y + PartyPanelHealthBarHeight, ViewportHeight);
		EXPECT_LE(card.manaBarPosition.y + PartyPanelManaBarHeight, ViewportHeight);
		EXPECT_LE(card.nameArea.position.x + card.nameArea.size.width, ScreenWidth - PartyPanelColumnMargin);
		EXPECT_LE(card.nameArea.position.y + card.nameArea.size.height, ViewportHeight);
		for (size_t j = 0; j < i; j++)
			EXPECT_FALSE(Overlaps(card.portrait, layout.cards[j].portrait));
	}
}

TEST_F(PartyPanelLayoutTest, ThreeRemotePlayersKeepTheClassicSingleColumnLayout)
{
	const PartyPanelLayout layout = ComputePartyPanelLayout(3, 640, 352, PartyPanelColumnMargin);
	ASSERT_EQ(layout.count, 3U);
	for (size_t i = 0; i < layout.count; i++) {
		EXPECT_EQ(layout.cards[i].portrait.position.x, PartyPanelColumnMargin);
		EXPECT_EQ(layout.cards[i].portrait.position.y, PartyPanelColumnMargin + PartyPanelHealthBarHeight + static_cast<int>(i) * PartyPanelRowAdvance);
	}
}

TEST_F(PartyPanelLayoutTest, EightPlayersAndSummonFitInWidescreenViewport)
{
	constexpr int ScreenWidth = 1280;
	constexpr int ViewportHeight = 592;
	const PartyPanelLayout layout = ComputePartyPanelLayout(PartyPanelMemberCapacity, ScreenWidth, ViewportHeight, 83);

	ASSERT_EQ(layout.count, PartyPanelMemberCapacity);
	const Rectangle viewport { { 0, 0 }, { ScreenWidth, ViewportHeight } };
	for (size_t i = 0; i < layout.count; i++) {
		const PartyPanelCardLayout &card = layout.cards[i];
		EXPECT_EQ(ClipPartyPanelRectangle(card.portrait, viewport).size, card.portrait.size);
		EXPECT_LE(card.nameArea.position.y + card.nameArea.size.height, ViewportHeight);
		EXPECT_LE(card.nameArea.position.x + card.nameArea.size.width, ScreenWidth - PartyPanelColumnMargin);
	}
	EXPECT_EQ(layout.cards[6].portrait.position.x, layout.cards[0].portrait.position.x + PartyPanelColumnWidthLimit);
}

TEST_F(PartyPanelLayoutTest, ClipsPortraitHitboxesToVisibleViewport)
{
	const Rectangle clipped = ClipPartyPanelRectangle({ { -10, 340 }, { 48, 48 } }, { { 0, 0 }, { 640, 352 } });
	EXPECT_EQ(clipped.position, Point(0, 340));
	EXPECT_EQ(clipped.size, Size(38, 12));
	EXPECT_FALSE(clipped.contains(Point(-1, 345)));
	EXPECT_FALSE(clipped.contains(Point(10, 352)));
}

TEST_F(PartyPanelLayoutTest, HealthAndManaBarsHandleEmptyAndOutOfRangeValues)
{
	EXPECT_EQ(GetPartyPanelBarWidth(0, 100), 0);
	EXPECT_EQ(GetPartyPanelBarWidth(-1, 100), 0);
	EXPECT_EQ(GetPartyPanelBarWidth(100, 0), 0);
	EXPECT_EQ(GetPartyPanelBarWidth(100, -1), 0);
	EXPECT_EQ(GetPartyPanelBarWidth(50, 100), 24);
	EXPECT_EQ(GetPartyPanelBarWidth(100, 100), PartyPanelPortraitSize.width);
	EXPECT_EQ(GetPartyPanelBarWidth(INT_MAX, INT_MAX), PartyPanelPortraitSize.width);
	EXPECT_EQ(GetPartyPanelBarWidth(INT_MAX, 1), PartyPanelPortraitSize.width);
}

class PartyPanelNameTest : public UITest {
};

TEST_F(PartyPanelNameTest, TruncatesLongUtf8NamesAtCharacterBoundaries)
{
	constexpr std::string_view Name = "Jos\xC3\xA9\x20\xE5\x86\x92\xE9\x99\xBA\xE8\x80\x85TheEightPlayer";
	constexpr int MaxWidth = 48;
	const std::string visibleName = TruncatePartyPanelName(Name, MaxWidth);

	EXPECT_LT(visibleName.size(), Name.size());
	EXPECT_TRUE(visibleName.ends_with("..."));
	EXPECT_LE(GetLineWidth(visibleName), MaxWidth);
	for (char32_t codePoint : Utf8CodePoints(visibleName))
		EXPECT_NE(codePoint, Utf8DecodeError);
}

TEST_F(PartyPanelNameTest, KeepsNamesThatAlreadyFit)
{
	constexpr std::string_view Name = "4. Warrior";
	EXPECT_EQ(TruncatePartyPanelName(Name, 160), std::string(Name));
}

TEST_F(PartyPanelNameTest, DropsAnInvalidUtf8Suffix)
{
	std::string name = "PartyMember";
	name.push_back(static_cast<char>(0xFF));
	name += "InvalidSuffix";
	EXPECT_EQ(TruncatePartyPanelName(name, 160), "PartyMember");
}

class PartyPanelInteractionTest : public UITest {
protected:
	void SetUp() override
	{
		UITest::SetUp();

		previousPartyOption_ = *GetOptions().Gameplay.showMultiplayerPartyInfo;
		previousShowFps_ = *GetOptions().Graphics.showFPS;
		previousAutomapActive_ = AutomapActive;
		previousPartySidePanelOpen_ = PartySidePanelOpen;
		previousScreenWidth_ = gnScreenWidth;
		previousScreenHeight_ = gnScreenHeight;
		previousViewportHeight_ = gnViewportHeight;
		previousMousePosition_ = MousePosition;
		Players.resize(PlayerStorageCapacity);
		MyPlayer = &Players[0];
		FreeCursor();
		for (size_t i = 0; i < Players.size(); i++) {
			CreatePlayer(Players[i], HeroClass::Warrior);
			Players[i].plractive = true;
			Players[i].friendlyMode = true;
			Players[i].plrlevel = i == 7 ? 1 : 0;
			CopyUtf8(Players[i]._pName, i == 7 ? "LongPartyMemberNameEight" : "PartyMember", sizeof(Players[i]._pName));
		}
		InitCursor();
		Players[6]._pHitPoints = Fixed26_6::fromInt(0);
		Players[7]._pMaxHP = Fixed26_6::fromInt(0);
		Players[7]._pMaxMana = Fixed26_6::fromInt(0);
		MyPlayer = &Players[0];
		InspectPlayer = MyPlayer;
		gbIsMultiplayer = true;
		PartySidePanelOpen = true;
		InspectingFromPartyPanel = false;
		GetOptions().Gameplay.showMultiplayerPartyInfo.SetValue(PartyPortraits::Players);
		GetOptions().Graphics.showFPS.SetValue(true);
		AutomapActive = true;
		gnScreenWidth = 640;
		gnScreenHeight = 480;
		CalculatePanelAreas();
		gnViewportHeight = 352;

		const auto result = LoadPartyPanel();
		ASSERT_TRUE(result) << (result ? "" : result.error());
	}

	void TearDown() override
	{
		if (CharFlag)
			CloseCharPanel();
		invflag = false;
		SpellbookFlag = false;
		ActiveStore = TalkID::None;
		IsStashOpen = false;
		FreePartyPanel();
		gbIsMultiplayer = false;
		PartySidePanelOpen = previousPartySidePanelOpen_;
		GetOptions().Gameplay.showMultiplayerPartyInfo.SetValue(previousPartyOption_);
		GetOptions().Graphics.showFPS.SetValue(previousShowFps_);
		AutomapActive = previousAutomapActive_;
		gnScreenWidth = previousScreenWidth_;
		gnScreenHeight = previousScreenHeight_;
		CalculatePanelAreas();
		gnViewportHeight = previousViewportHeight_;
		MousePosition = previousMousePosition_;
		InspectingFromPartyPanel = false;
		Players.resize(1);
		MyPlayer = &Players[0];
		InspectPlayer = MyPlayer;
		UITest::TearDown();
	}

	Point LastPlayerPortraitPosition(int screenWidth, int viewportHeight, int preferredTop) const
	{
#ifdef _DEBUG
		constexpr size_t LastPlayerEntryIndex = 7;
#else
		constexpr size_t LastPlayerEntryIndex = 6;
#endif
		return ComputePartyPanelLayout(
		    LastPlayerEntryIndex + 1,
		    screenWidth,
		    viewportHeight,
		    preferredTop)
		    .cards[LastPlayerEntryIndex]
		    .portrait.position;
	}

	Point PlayerPortraitPosition(size_t playerId, int screenWidth, int viewportHeight, int preferredTop) const
	{
#ifdef _DEBUG
		const size_t entryIndex = playerId;
#else
		const size_t entryIndex = playerId - 1;
#endif
		return ComputePartyPanelLayout(entryIndex + 1, screenWidth, viewportHeight, preferredTop).cards[entryIndex].portrait.position;
	}

private:
	PartyPortraits previousPartyOption_ = PartyPortraits::Disabled;
	bool previousShowFps_ = false;
	bool previousAutomapActive_ = false;
	bool previousPartySidePanelOpen_ = true;
	decltype(gnScreenWidth) previousScreenWidth_ = 0;
	decltype(gnScreenHeight) previousScreenHeight_ = 0;
	decltype(gnViewportHeight) previousViewportHeight_ = 0;
	Point previousMousePosition_;
};

TEST_F(PartyPanelInteractionTest, LastPlayerInDifferentLevelCanBeInspected)
{
	OwnedSurface out(640, 480);
	MousePosition = LastPlayerPortraitPosition(640, 352, 83) + Displacement { 5, 5 };

	DrawPartyMemberInfoPanel(out);
	EXPECT_EQ(PortraitIdUnderCursor, 7);

	GetOptions().Graphics.showFPS.SetValue(true);
	AutomapActive = true;
	gnScreenWidth = 1280;
	gnScreenHeight = 720;
	CalculatePanelAreas();
	gnViewportHeight = 592;
	OwnedSurface wideOut(1280, 720);
	MousePosition = LastPlayerPortraitPosition(1280, 592, 83) + Displacement { 5, 5 };
	DrawPartyMemberInfoPanel(wideOut);
	EXPECT_EQ(PortraitIdUnderCursor, 7);

	ASSERT_TRUE(DidRightClickPartyPortrait());
	DrawPartyMemberInfoPanel(wideOut);

	EXPECT_TRUE(CharFlag);
	EXPECT_TRUE(invflag);
	EXPECT_EQ(InspectPlayer, &Players[7]);
}

TEST_F(PartyPanelInteractionTest, EveryRemotePortraitSelectsItsOwnPlayer)
{
	OwnedSurface out(640, 480);
	for (size_t playerId = 1; playerId < Players.size(); playerId++) {
		MousePosition = PlayerPortraitPosition(playerId, 640, 352, 83) + Displacement { 5, 5 };
		DrawPartyMemberInfoPanel(out);
		ASSERT_EQ(PortraitIdUnderCursor, static_cast<int>(playerId));
		ASSERT_TRUE(DidRightClickPartyPortrait());
		DrawPartyMemberInfoPanel(out);
		EXPECT_EQ(InspectPlayer, &Players[playerId]);
		CloseCharPanel();
		invflag = false;
	}
}

TEST_F(PartyPanelInteractionTest, InactiveOrHiddenPortraitCannotUseStaleHitbox)
{
	OwnedSurface out(640, 480);
	MousePosition = LastPlayerPortraitPosition(640, 352, 83) + Displacement { 5, 5 };
	DrawPartyMemberInfoPanel(out);
	ASSERT_EQ(PortraitIdUnderCursor, 7);

	Players[7].plractive = false;
	EXPECT_FALSE(DidRightClickPartyPortrait());

	Players[7].plractive = true;
	DrawPartyMemberInfoPanel(out);
	PartySidePanelOpen = false;
	EXPECT_FALSE(DidRightClickPartyPortrait());
}

TEST_F(PartyPanelInteractionTest, CharacterStoreAndStashOverlaysClearPortraitHitboxes)
{
	OwnedSurface out(640, 480);
	MousePosition = PlayerPortraitPosition(7, 640, 352, 83) + Displacement { 5, 5 };

	DrawPartyMemberInfoPanel(out);
	ASSERT_EQ(PortraitIdUnderCursor, 7);
	CharFlag = true;
	EXPECT_FALSE(DidRightClickPartyPortrait());
	DrawPartyMemberInfoPanel(out);
	EXPECT_EQ(PortraitIdUnderCursor, -1);
	CharFlag = false;

	DrawPartyMemberInfoPanel(out);
	ASSERT_EQ(PortraitIdUnderCursor, 7);
	ActiveStore = TalkID::Smith;
	EXPECT_FALSE(DidRightClickPartyPortrait());
	DrawPartyMemberInfoPanel(out);
	EXPECT_EQ(PortraitIdUnderCursor, -1);
	ActiveStore = TalkID::None;

	DrawPartyMemberInfoPanel(out);
	ASSERT_EQ(PortraitIdUnderCursor, 7);
	IsStashOpen = true;
	EXPECT_FALSE(DidRightClickPartyPortrait());
	DrawPartyMemberInfoPanel(out);
	EXPECT_EQ(PortraitIdUnderCursor, -1);
	IsStashOpen = false;
}

TEST_F(PartyPanelInteractionTest, InventoryAndSpellbookKeepPortraitsOutsideTheRightPanel)
{
	OwnedSurface out(640, 480);
	for (bool useSpellbook : { false, true }) {
		invflag = !useSpellbook;
		SpellbookFlag = useSpellbook;
		const int visibleWidth = GetRightPanel().position.x;
		ASSERT_GT(visibleWidth, PartyPanelPortraitSize.width + PartyPanelColumnMargin);
		MousePosition = PlayerPortraitPosition(7, visibleWidth, 352, 83) + Displacement { 5, 5 };
		EXPECT_FALSE(GetRightPanel().contains(MousePosition));

		DrawPartyMemberInfoPanel(out);
		ASSERT_EQ(PortraitIdUnderCursor, 7);
		ASSERT_TRUE(DidRightClickPartyPortrait());
		DrawPartyMemberInfoPanel(out);
		EXPECT_EQ(InspectPlayer, &Players[7]);
		CloseCharPanel();
		invflag = false;
		SpellbookFlag = false;
	}
}

} // namespace
} // namespace devilution
