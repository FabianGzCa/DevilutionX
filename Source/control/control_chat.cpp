#include "control_chat.hpp"
#include "control.hpp"
#include "control_panel.hpp"

#include <algorithm>
#include <array>
#include <span>
#include <string>

#include "control/chat_recipient_list.hpp"
#include "control/control_chat_commands.hpp"
#include "engine/backbuffer_state.hpp"
#include "engine/render/clx_render.hpp"
#include "options.h"
#include "panels/console.hpp"
#include "panels/mainpanel.hpp"
#include "panels/partypanel_layout.hpp"
#include "quick_messages.hpp"
#include "utils/display.h"
#include "utils/sdl_compat.h"
#include "utils/static_vector.hpp"
#include "utils/str_cat.hpp"

namespace devilution {

std::optional<TextInputState> ChatInputState;
char TalkMessage[MAX_SEND_STR_LEN];
int sgbPlrTalkTbl;
OptionalOwnedClxSpriteList talkButtons;

namespace {

char TalkSave[8][MAX_SEND_STR_LEN];
uint8_t TalkSaveIndex;
uint8_t NextTalkSave;
TextInputCursorState ChatCursor;

Rectangle ChatRecipientButtonRect { { 172, ChatRecipientFirstRowY }, { 61, ChatRecipientButtonHeight } };
Rectangle PreviousRecipientPageRect { { 430, 51 }, { 14, 16 } };
Rectangle NextRecipientPageRect { { 486, 51 }, { 14, 16 } };

enum class ChatRecipientButtonType {
	None,
	Player,
	PreviousPage,
	NextPage,
};

constexpr int ChatRecipientNameWidth = 204;

std::array<bool, PlayerStorageCapacity> ChatRecipientSelected;
ChatRecipientButtonType PressedChatRecipientButton;
std::optional<size_t> PressedChatRecipientPlayerId;
std::optional<size_t> FocusedChatRecipientPlayerId;
size_t ChatRecipientPage;

using ChatRecipientPlayerIds = StaticVector<size_t, PlayerStorageCapacity>;

void ResetInactiveChatRecipientSelections()
{
	for (size_t playerId = 0; playerId < std::min(Players.size(), ChatRecipientSelected.size()); playerId++) {
		if (playerId != MyPlayerId && !Players[playerId].plractive)
			ChatRecipientSelected[playerId] = true;
	}
}

ChatRecipientPlayerIds GetActiveChatRecipientPlayerIds()
{
	ResetInactiveChatRecipientSelections();
	ChatRecipientPlayerIds playerIds;
	const size_t playerCount = std::min(Players.size(), ChatRecipientSelected.size());
	for (size_t playerId = 0; playerId < playerCount; playerId++) {
		if (playerId == MyPlayerId)
			continue;
		if (!Players[playerId].plractive)
			continue;
		playerIds.push_back(playerId);
	}
	return playerIds;
}

void ResetPressedChatRecipientButton()
{
	PressedChatRecipientButton = ChatRecipientButtonType::None;
	PressedChatRecipientPlayerId.reset();
}

void NormalizeChatRecipientPage(const ChatRecipientPlayerIds &playerIds)
{
	ChatRecipientPage = ClampChatRecipientPage(ChatRecipientPage, playerIds.size());
	if (playerIds.empty()) {
		FocusedChatRecipientPlayerId.reset();
		return;
	}
	const size_t pageStart = ChatRecipientPage * ChatRecipientRowsPerPage;
	const size_t pageEnd = std::min(pageStart + ChatRecipientRowsPerPage, playerIds.size());
	if (FocusedChatRecipientPlayerId && std::find(playerIds.begin() + pageStart, playerIds.begin() + pageEnd, *FocusedChatRecipientPlayerId) != playerIds.begin() + pageEnd)
		return;
	FocusedChatRecipientPlayerId = playerIds[pageStart];
}

std::optional<size_t> GetVisibleChatRecipientPlayerId(const ChatRecipientPlayerIds &playerIds, size_t row)
{
	return GetChatRecipientPlayerId(std::span<const size_t> { playerIds.data(), playerIds.size() }, ChatRecipientPage, row);
}

void ChangeChatRecipientPage(int direction, const ChatRecipientPlayerIds &playerIds)
{
	if (direction == 0 || playerIds.empty())
		return;

	const size_t pageCount = GetChatRecipientPageCount(playerIds.size());
	const size_t currentPage = ClampChatRecipientPage(ChatRecipientPage, playerIds.size());
	if (direction < 0) {
		ChatRecipientPage = currentPage == 0 ? 0 : currentPage - 1;
	} else {
		ChatRecipientPage = std::min(currentPage + 1, pageCount - 1);
	}
	NormalizeChatRecipientPage(playerIds);
	RedrawEverything();
}

std::optional<size_t> GetChatRecipientRowAtMousePosition(const ChatRecipientPlayerIds &playerIds, Point mousePosition)
{
	Rectangle buttons = ChatRecipientButtonRect;
	SetPanelObjectPosition(UiPanels::Main, buttons);
	buttons.size.height = static_cast<int>((ChatRecipientRowsPerPage - 1) * ChatRecipientRowHeight) + ChatRecipientButtonHeight;
	if (!buttons.contains(mousePosition))
		return std::nullopt;
	return GetChatRecipientPlayerIdAtY(std::span<const size_t> { playerIds.data(), playerIds.size() }, ChatRecipientPage, mousePosition.y - GetMainPanel().position.y);
}

void ResetChatMessage()
{
	if (CheckChatCommand(TalkMessage))
		return;

	PlayerRecipientMask recipients { 0 };
	ResetInactiveChatRecipientSelections();

	for (size_t i = 0; i < Players.size(); i++) {
		if (i < ChatRecipientSelected.size() && ChatRecipientSelected[i])
			recipients = AddPlayerToRecipientMask(recipients, i);
	}

	NetSendCmdString(recipients, TalkMessage);
}

void ControlPressEnter()
{
	if (TalkMessage[0] != 0) {
		ResetChatMessage();
		uint8_t i = 0;
		for (; i < 8; i++) {
			if (strcmp(TalkSave[i], TalkMessage) == 0)
				break;
		}
		if (i >= 8) {
			strcpy(TalkSave[NextTalkSave], TalkMessage);
			NextTalkSave++;
			NextTalkSave &= 7;
		} else {
			uint8_t talkSave = NextTalkSave - 1;
			talkSave &= 7;
			if (i != talkSave) {
				strcpy(TalkSave[i], TalkSave[talkSave]);
				*BufCopy(TalkSave[talkSave], ChatInputState->value()) = '\0';
			}
		}
		TalkMessage[0] = '\0';
		TalkSaveIndex = NextTalkSave;
	}
	ResetChat();
}

void ControlUpDown(int v)
{
	for (int i = 0; i < 8; i++) {
		TalkSaveIndex = (v + TalkSaveIndex) & 7;
		if (TalkSave[TalkSaveIndex][0] != 0) {
			ChatInputState->assign(TalkSave[TalkSaveIndex]);
			return;
		}
	}
}

} // namespace

void ResetChatRecipientSelection()
{
	ChatRecipientSelected.fill(true);
	ChatRecipientPage = 0;
	FocusedChatRecipientPlayerId.reset();
	ResetPressedChatRecipientButton();
}

void ForgetChatRecipientSelection(size_t playerId)
{
	if (playerId >= ChatRecipientSelected.size())
		return;
	ChatRecipientSelected[playerId] = true;
	if (PressedChatRecipientPlayerId == playerId)
		ResetPressedChatRecipientButton();
	if (FocusedChatRecipientPlayerId == playerId)
		FocusedChatRecipientPlayerId.reset();
}

void ChangeChatRecipientPage(int direction)
{
	if (!ChatFlag)
		return;
	ChangeChatRecipientPage(direction, GetActiveChatRecipientPlayerIds());
}

void MoveChatRecipientFocus(int direction)
{
	if (!ChatFlag || direction == 0)
		return;

	const ChatRecipientPlayerIds playerIds = GetActiveChatRecipientPlayerIds();
	NormalizeChatRecipientPage(playerIds);
	if (playerIds.empty())
		return;

	auto focused = std::find(playerIds.begin(), playerIds.end(), *FocusedChatRecipientPlayerId);
	size_t index = focused == playerIds.end() ? ChatRecipientPage * ChatRecipientRowsPerPage : static_cast<size_t>(focused - playerIds.begin());
	if (direction < 0) {
		if (index == 0)
			return;
		--index;
	} else {
		if (index + 1 >= playerIds.size())
			return;
		++index;
	}

	ChatRecipientPage = index / ChatRecipientRowsPerPage;
	FocusedChatRecipientPlayerId = playerIds[index];
	RedrawEverything();
}

void ToggleFocusedChatRecipient()
{
	if (!ChatFlag)
		return;
	const ChatRecipientPlayerIds playerIds = GetActiveChatRecipientPlayerIds();
	NormalizeChatRecipientPage(playerIds);
	if (!FocusedChatRecipientPlayerId || *FocusedChatRecipientPlayerId >= ChatRecipientSelected.size())
		return;
	if (std::find(playerIds.begin(), playerIds.end(), *FocusedChatRecipientPlayerId) == playerIds.end())
		return;

	ChatRecipientSelected[*FocusedChatRecipientPlayerId] = !ChatRecipientSelected[*FocusedChatRecipientPlayerId];
	RedrawEverything();
}

void DrawChatBox(const Surface &out)
{
	if (!ChatFlag)
		return;

	const Point mainPanelPosition = GetMainPanel().position;
	const ChatRecipientPlayerIds playerIds = GetActiveChatRecipientPlayerIds();
	NormalizeChatRecipientPage(playerIds);
	const size_t pageCount = GetChatRecipientPageCount(playerIds.size());

	DrawPanelBox(out, MakeSdlRect(175, sgbPlrTalkTbl + 20, 294, 5), mainPanelPosition + Displacement { 175, 4 });
	int off = 0;
	for (int i = 293; i > 283; off++, i--) {
		DrawPanelBox(out, MakeSdlRect((off / 2) + 175, sgbPlrTalkTbl + off + 25, i, 1), mainPanelPosition + Displacement { (off / 2) + 175, off + 9 });
	}
	DrawPanelBox(out, MakeSdlRect(185, sgbPlrTalkTbl + 35, 274, 30), mainPanelPosition + Displacement { 185, 19 });
	DrawPanelBox(out, MakeSdlRect(180, sgbPlrTalkTbl + 65, 284, 5), mainPanelPosition + Displacement { 180, 49 });
	for (int i = 0; i < 10; i++) {
		DrawPanelBox(out, MakeSdlRect(180, sgbPlrTalkTbl + i + 70, i + 284, 1), mainPanelPosition + Displacement { 180, i + 54 });
	}
	DrawPanelBox(out, MakeSdlRect(170, sgbPlrTalkTbl + 80, 310, 55), mainPanelPosition + Displacement { 170, 64 });

	int x = mainPanelPosition.x + 200;
	const int y = mainPanelPosition.y + 10;

	const uint32_t len = DrawString(out, TalkMessage, { { x, y }, { 250, 39 } },
	    {
	        .flags = UiFlags::ColorWhite | UiFlags::PentaCursor,
	        .lineHeight = 13,
	        .cursorPosition = static_cast<int>(ChatCursor.position),
	        .highlightRange = { static_cast<int>(ChatCursor.selection.begin), static_cast<int>(ChatCursor.selection.end) },
	    });
	ChatInputState->truncate(len);

	x += 46;
	for (size_t row = 0; row < ChatRecipientRowsPerPage; row++) {
		const std::optional<size_t> recipientId = GetVisibleChatRecipientPlayerId(playerIds, row);
		if (!recipientId)
			continue;

		Player &player = Players[*recipientId];
		const bool selected = ChatRecipientSelected[*recipientId];
		const bool isPressed = PressedChatRecipientPlayerId == recipientId;
		const Point talkPanPosition = mainPanelPosition + Displacement { 172, ChatRecipientFirstRowY + ChatRecipientButtonHeight + (ChatRecipientRowHeight * static_cast<int>(row)) - 1 };
		if (selected) {
			// The selected-recipient button is pre-rendered on the panel, so only redraw it while held.
			if (isPressed) {
				const unsigned spriteIndex = row == 0 ? 2 : 3;
				ClxDraw(out, talkPanPosition, (*talkButtons)[spriteIndex]);
				RenderClxSprite(out, (*TalkButton)[2], talkPanPosition + Displacement { 4, -15 });
			}
		} else {
			unsigned spriteIndex = row == 0 ? 0 : 1;
			if (isPressed)
				spriteIndex += 4;
			ClxDraw(out, talkPanPosition, (*talkButtons)[spriteIndex]);
			RenderClxSprite(out, (*TalkButton)[isPressed ? 1 : 0], talkPanPosition + Displacement { 4, -15 });
		}

		UiFlags color = player.friendlyMode ? UiFlags::ColorWhitegold : UiFlags::ColorRed;
		if (FocusedChatRecipientPlayerId == recipientId)
			color = UiFlags::ColorYellow;
		const std::string playerName = TruncatePartyPanelName(player._pName, ChatRecipientNameWidth);
		DrawString(out, playerName, { { x, y + 60 + (ChatRecipientRowHeight * static_cast<int>(row)) }, { ChatRecipientNameWidth, 16 } }, { .flags = color });
	}

	const std::string pageLabel = StrCat(pageCount == 0 ? 1 : ChatRecipientPage + 1, "/", pageCount == 0 ? 1 : pageCount);
	DrawString(out, _("Send to:"), mainPanelPosition + Displacement { 246, 51 }, { .flags = UiFlags::ColorWhitegold });
	DrawString(out, "<", mainPanelPosition + Displacement { PreviousRecipientPageRect.position.x + 3, PreviousRecipientPageRect.position.y }, { .flags = PressedChatRecipientButton == ChatRecipientButtonType::PreviousPage ? UiFlags::ColorYellow : UiFlags::ColorWhite });
	DrawString(out, pageLabel, { mainPanelPosition + Displacement { 446, 51 }, { 38, 16 } }, { .flags = UiFlags::ColorWhitegold });
	DrawString(out, ">", mainPanelPosition + Displacement { NextRecipientPageRect.position.x + 3, NextRecipientPageRect.position.y }, { .flags = PressedChatRecipientButton == ChatRecipientButtonType::NextPage ? UiFlags::ColorYellow : UiFlags::ColorWhite });
}

bool CheckChatRecipientButton()
{
	if (!ChatFlag)
		return false;

	ResetPressedChatRecipientButton();
	const ChatRecipientPlayerIds playerIds = GetActiveChatRecipientPlayerIds();
	NormalizeChatRecipientPage(playerIds);

	Rectangle previousPageButton = PreviousRecipientPageRect;
	SetPanelObjectPosition(UiPanels::Main, previousPageButton);
	if (previousPageButton.contains(MousePosition)) {
		if (GetChatRecipientPageCount(playerIds.size()) > 1)
			PressedChatRecipientButton = ChatRecipientButtonType::PreviousPage;
		return true;
	}

	Rectangle nextPageButton = NextRecipientPageRect;
	SetPanelObjectPosition(UiPanels::Main, nextPageButton);
	if (nextPageButton.contains(MousePosition)) {
		if (GetChatRecipientPageCount(playerIds.size()) > 1)
			PressedChatRecipientButton = ChatRecipientButtonType::NextPage;
		return true;
	}

	Rectangle buttons = ChatRecipientButtonRect;
	SetPanelObjectPosition(UiPanels::Main, buttons);
	buttons.size.height = static_cast<int>((ChatRecipientRowsPerPage - 1) * ChatRecipientRowHeight) + ChatRecipientButtonHeight;
	if (!buttons.contains(MousePosition))
		return false;

	const std::optional<size_t> recipientId = GetChatRecipientRowAtMousePosition(playerIds, MousePosition);
	if (recipientId) {
		PressedChatRecipientButton = ChatRecipientButtonType::Player;
		PressedChatRecipientPlayerId = recipientId;
		FocusedChatRecipientPlayerId = recipientId;
	}
	return true;
}

void CheckChatRecipientButtonUp()
{
	if (!ChatFlag) {
		ResetPressedChatRecipientButton();
		return;
	}

	const ChatRecipientButtonType pressedButton = PressedChatRecipientButton;
	const std::optional<size_t> pressedPlayerId = PressedChatRecipientPlayerId;
	ResetPressedChatRecipientButton();
	const ChatRecipientPlayerIds playerIds = GetActiveChatRecipientPlayerIds();
	NormalizeChatRecipientPage(playerIds);

	if (pressedButton == ChatRecipientButtonType::PreviousPage || pressedButton == ChatRecipientButtonType::NextPage) {
		Rectangle button = pressedButton == ChatRecipientButtonType::PreviousPage ? PreviousRecipientPageRect : NextRecipientPageRect;
		SetPanelObjectPosition(UiPanels::Main, button);
		if (button.contains(MousePosition))
			ChangeChatRecipientPage(pressedButton == ChatRecipientButtonType::PreviousPage ? -1 : 1, playerIds);
		return;
	}
	if (pressedButton != ChatRecipientButtonType::Player || !pressedPlayerId || *pressedPlayerId >= ChatRecipientSelected.size())
		return;

	const std::optional<size_t> releasedPlayerId = GetChatRecipientRowAtMousePosition(playerIds, MousePosition);
	if (releasedPlayerId == pressedPlayerId) {
		ChatRecipientSelected[*pressedPlayerId] = !ChatRecipientSelected[*pressedPlayerId];
		FocusedChatRecipientPlayerId = pressedPlayerId;
		RedrawEverything();
	}
}

void TypeChatMessage()
{
	if (!IsChatAvailable())
		return;

	ChatFlag = true;
	ChatRecipientPage = 0;
	ResetPressedChatRecipientButton();
	NormalizeChatRecipientPage(GetActiveChatRecipientPlayerIds());
	TalkMessage[0] = '\0';
	ChatInputState.emplace(TextInputState::Options {
	    .value = TalkMessage,
	    .cursor = &ChatCursor,
	    .maxLength = sizeof(TalkMessage) - 1 });
	sgbPlrTalkTbl = GetMainPanel().size.height + PanelPaddingHeight;
	RedrawEverything();
	TalkSaveIndex = NextTalkSave;

	SDL_Rect rect = MakeSdlRect(GetMainPanel().position.x + 200, GetMainPanel().position.y + 22, 0, 27);
	SDL_SetTextInputArea(ghMainWnd, &rect, /*cursor=*/0);
	SDLC_StartTextInput(ghMainWnd);
}

void ResetChat()
{
	ChatFlag = false;
	ResetPressedChatRecipientButton();
	SDLC_StopTextInput(ghMainWnd);
	ChatCursor = {};
	ChatInputState = std::nullopt;
	sgbPlrTalkTbl = 0;
	RedrawEverything();
}

bool IsChatActive()
{
	if (!IsChatAvailable())
		return false;

	if (!ChatFlag)
		return false;

	return true;
}

bool CheckKeypress(SDL_Keycode vkey)
{
	if (!IsChatAvailable())
		return false;
	if (!ChatFlag)
		return false;

	switch (vkey) {
	case SDLK_ESCAPE:
		ResetChat();
		return true;
	case SDLK_RETURN:
	case SDLK_KP_ENTER:
		ControlPressEnter();
		return true;
	case SDLK_PAGEUP:
		ChangeChatRecipientPage(-1);
		return true;
	case SDLK_PAGEDOWN:
		ChangeChatRecipientPage(1);
		return true;
	case SDLK_DOWN:
		ControlUpDown(1);
		return true;
	case SDLK_UP:
		ControlUpDown(-1);
		return true;
	default:
		return vkey >= SDLK_SPACE && vkey <= SDLK_Z;
	}
}

void DiabloHotkeyMsg(uint32_t dwMsg)
{
	assert(dwMsg < QuickMessages.size());

#ifdef _DEBUG
	constexpr std::string_view LuaPrefix = "/lua ";
	for (const std::string &msg : GetOptions().Chat.szHotKeyMsgs[dwMsg]) {
		if (!msg.starts_with(LuaPrefix)) continue;
		InitConsole();
		RunInConsole(std::string_view(msg).substr(LuaPrefix.size()));
	}
#endif

	if (!IsChatAvailable()) {
		return;
	}

	for (const std::string &msg : GetOptions().Chat.szHotKeyMsgs[dwMsg]) {
#ifdef _DEBUG
		if (msg.starts_with(LuaPrefix)) continue;
#endif
		char charMsg[MAX_SEND_STR_LEN];
		CopyUtf8(charMsg, msg, sizeof(charMsg));
		NetSendCmdString(AllPlayerRecipients(), charMsg);
	}
}

bool IsChatAvailable()
{
	return gbIsMultiplayer;
}

bool HandleTalkTextInputEvent(const SDL_Event &event)
{
	return HandleInputEvent(event, ChatInputState);
}

} // namespace devilution
