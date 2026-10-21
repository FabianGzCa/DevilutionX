#pragma once

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>

#include "engine/rectangle.hpp"
#include "engine/render/text_render.hpp"
#include "engine/size.hpp"
#include "multi.h"
#include "utils/utf8.hpp"

namespace devilution {

constexpr int PartyPanelHealthBarHeight = 7;
constexpr int PartyPanelManaBarHeight = 7;
constexpr int PartyPanelFrameGap = 15;
constexpr int PartyPanelFrameBorderSize = 3;
constexpr int PartyPanelFrameSpriteSize = 12;
constexpr int PartyPanelNameHeight = 16;
constexpr int PartyPanelColumnMargin = 8;
constexpr int PartyPanelColumnWidthLimit = 160;
constexpr int PartyPanelRowAdvance = 82;
constexpr Size PartyPanelFrameSections = { 4, 4 };
constexpr Size PartyPanelPortraitSize = {
	PartyPanelFrameSections.width * PartyPanelFrameSpriteSize,
	PartyPanelFrameSections.height * PartyPanelFrameSpriteSize
};
constexpr size_t PartyPanelMemberCapacity = PlayerStorageCapacity + 1; // Includes the local player's golem.

struct PartyPanelCardLayout {
	Point healthBarPosition;
	Rectangle portrait;
	Point manaBarPosition;
	Rectangle nameArea;
};

struct PartyPanelLayout {
	std::array<PartyPanelCardLayout, PartyPanelMemberCapacity> cards { };
	size_t count = 0;
};

/**
 * @brief Computes visible card positions in columns, filling each column from top to bottom.
 */
[[nodiscard]] inline PartyPanelLayout ComputePartyPanelLayout(size_t count, int screenWidth, int viewportHeight, int preferredTop)
{
	PartyPanelLayout result;
	result.count = std::min(count, result.cards.size());
	if (result.count == 0)
		return result;

	constexpr int CardContentHeight = PartyPanelHealthBarHeight + PartyPanelPortraitSize.height + PartyPanelManaBarHeight + PartyPanelNameHeight;
	const int maxTop = std::max(0, viewportHeight - CardContentHeight);
	const int top = std::clamp(preferredTop, 0, maxTop);
	const int availableHeight = std::max(0, viewportHeight - top);
	const int rows = std::max(1, std::min<int>(static_cast<int>(result.count), availableHeight < CardContentHeight ? 1 : 1 + (availableHeight - CardContentHeight) / PartyPanelRowAdvance));
	const int columns = static_cast<int>((result.count + rows - 1) / rows);
	const int availableWidth = std::max(PartyPanelPortraitSize.width, screenWidth - (PartyPanelColumnMargin * 2));
	const int columnWidth = std::min(PartyPanelColumnWidthLimit, availableWidth / columns);

	for (size_t i = 0; i < result.count; i++) {
		const int column = static_cast<int>(i) / rows;
		const int row = static_cast<int>(i) % rows;
		const int cardTop = top + row * PartyPanelRowAdvance;
		const int x = PartyPanelColumnMargin + column * columnWidth;
		const int portraitTop = cardTop + PartyPanelHealthBarHeight;
		const int nameTop = portraitTop + PartyPanelPortraitSize.height + PartyPanelManaBarHeight;
		result.cards[i] = {
			{ x, cardTop },
			{ { x, portraitTop }, PartyPanelPortraitSize },
			{ x, nameTop - PartyPanelManaBarHeight },
			{ { x, nameTop }, { columnWidth, PartyPanelNameHeight } }
		};
	}

	return result;
}

/** @brief Clips an interaction rectangle to the visible viewport. */
[[nodiscard]] constexpr Rectangle ClipPartyPanelRectangle(Rectangle rect, Rectangle bounds)
{
	const int left = std::max(rect.position.x, bounds.position.x);
	const int top = std::max(rect.position.y, bounds.position.y);
	const int right = std::min(rect.position.x + rect.size.width, bounds.position.x + bounds.size.width);
	const int bottom = std::min(rect.position.y + rect.size.height, bounds.position.y + bounds.size.height);
	return { { left, top }, { std::max(0, right - left), std::max(0, bottom - top) } };
}

/** @brief Returns a clamped health or mana bar width without dividing by zero. */
[[nodiscard]] inline int GetPartyPanelBarWidth(int current, int maximum)
{
	if (current <= 0 || maximum <= 0)
		return 0;
	const int64_t clampedCurrent = std::min<int64_t>(current, maximum);
	return static_cast<int>((clampedCurrent * PartyPanelPortraitSize.width + maximum / 2) / maximum);
}

/** @brief Truncates a name at a UTF-8 code point boundary so it fits one panel line. */
[[nodiscard]] inline std::string TruncatePartyPanelName(std::string_view name, int maxWidth)
{
	if (maxWidth <= 0)
		return { };
	size_t validBytes = 0;
	const Utf8CodePoints codePoints(name);
	for (auto codePoint = codePoints.begin(); codePoint != codePoints.end(); ++codePoint) {
		if (*codePoint == Utf8DecodeError)
			break;
		validBytes += codePoint.size();
	}
	name = name.substr(0, validBytes);
	if (GetLineWidth(name) <= maxWidth)
		return std::string(name);

	constexpr std::string_view Ellipsis = "...";
	const int ellipsisWidth = GetLineWidth(Ellipsis);
	size_t visibleBytes = 0;
	const Utf8CodePoints validCodePoints(name);
	for (auto codePoint = validCodePoints.begin(); codePoint != validCodePoints.end(); ++codePoint) {
		const size_t nextBytes = visibleBytes + codePoint.size();
		std::string candidate(name.substr(0, nextBytes));
		candidate += Ellipsis;
		if (GetLineWidth(candidate) > maxWidth)
			break;
		visibleBytes = nextBytes;
	}

	std::string result(name.substr(0, visibleBytes));
	if (maxWidth >= ellipsisWidth)
		result += Ellipsis;
	return result;
}

} // namespace devilution
