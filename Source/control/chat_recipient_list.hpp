/**
 * @file chat_recipient_list.hpp
 *
 * Helpers for mapping visible chat recipient rows to stable player IDs.
 */
#pragma once

#include <algorithm>
#include <cstddef>
#include <optional>
#include <span>

namespace devilution {

constexpr size_t ChatRecipientRowsPerPage = 3;
constexpr int ChatRecipientFirstRowY = 69;
constexpr int ChatRecipientRowHeight = 18;
constexpr int ChatRecipientButtonHeight = 16;

constexpr size_t GetChatRecipientPageCount(size_t recipientCount)
{
	return (recipientCount + ChatRecipientRowsPerPage - 1) / ChatRecipientRowsPerPage;
}

constexpr size_t ClampChatRecipientPage(size_t page, size_t recipientCount)
{
	const size_t pageCount = GetChatRecipientPageCount(recipientCount);
	return pageCount == 0 ? 0 : std::min(page, pageCount - 1);
}

inline std::optional<size_t> GetChatRecipientPlayerId(std::span<const size_t> recipientPlayerIds, size_t page, size_t row)
{
	if (row >= ChatRecipientRowsPerPage)
		return std::nullopt;

	const size_t clampedPage = ClampChatRecipientPage(page, recipientPlayerIds.size());
	const size_t index = clampedPage * ChatRecipientRowsPerPage + row;
	if (index >= recipientPlayerIds.size())
		return std::nullopt;

	return recipientPlayerIds[index];
}

inline std::optional<size_t> GetChatRecipientPlayerIdAtY(std::span<const size_t> recipientPlayerIds, size_t page, int panelY)
{
	if (panelY < ChatRecipientFirstRowY)
		return std::nullopt;
	const int offset = panelY - ChatRecipientFirstRowY;
	const size_t row = static_cast<size_t>(offset / ChatRecipientRowHeight);
	if (row >= ChatRecipientRowsPerPage || offset % ChatRecipientRowHeight >= ChatRecipientButtonHeight)
		return std::nullopt;
	return GetChatRecipientPlayerId(recipientPlayerIds, page, row);
}

} // namespace devilution
