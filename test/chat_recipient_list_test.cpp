/**
 * @file chat_recipient_list_test.cpp
 *
 * Regression tests for paged chat recipient ID mapping.
 */

#include <array>
#include <optional>

#include <gtest/gtest.h>

#include "control/chat_recipient_list.hpp"
#include "player_mask.hpp"

namespace devilution {
namespace {

TEST(ChatRecipientListTest, PagesAllSevenRemotePlayerIdsWithoutChangingTheirIdentity)
{
	constexpr std::array<size_t, 7> RecipientPlayerIds { 1, 2, 3, 4, 5, 6, 7 };
	EXPECT_EQ(GetChatRecipientPageCount(RecipientPlayerIds.size()), 3U);

	for (size_t index = 0; index < RecipientPlayerIds.size(); index++) {
		SCOPED_TRACE(index);
		EXPECT_EQ(GetChatRecipientPlayerId(RecipientPlayerIds, index / ChatRecipientRowsPerPage, index % ChatRecipientRowsPerPage), RecipientPlayerIds[index]);
	}

	const std::optional<size_t> finalRecipient = GetChatRecipientPlayerId(RecipientPlayerIds, 2, 0);
	ASSERT_TRUE(finalRecipient.has_value());
	EXPECT_EQ(*finalRecipient, 7U);
}

TEST(ChatRecipientListTest, MapsRowsToPlayerIdsWhenTheLocalPlayerIsInAnotherSlot)
{
	constexpr std::array<size_t, 7> RecipientPlayerIds { 0, 1, 2, 3, 5, 6, 7 };
	EXPECT_EQ(GetChatRecipientPlayerId(RecipientPlayerIds, 0, 0), 0U);
	EXPECT_EQ(GetChatRecipientPlayerId(RecipientPlayerIds, 1, 0), 3U);
	EXPECT_EQ(GetChatRecipientPlayerId(RecipientPlayerIds, 2, 0), 7U);
}

TEST(ChatRecipientListTest, ClampsPageWhenRosterShrinksAndRejectsMissingRows)
{
	constexpr std::array<size_t, 4> RecipientPlayerIds { 1, 2, 3, 7 };
	EXPECT_EQ(ClampChatRecipientPage(2, RecipientPlayerIds.size()), 1U);
	EXPECT_EQ(GetChatRecipientPlayerId(RecipientPlayerIds, 2, 0), 7U);
	EXPECT_FALSE(GetChatRecipientPlayerId(RecipientPlayerIds, 1, 1).has_value());
	EXPECT_FALSE(GetChatRecipientPlayerId(RecipientPlayerIds, 0, ChatRecipientRowsPerPage).has_value());
	EXPECT_EQ(ClampChatRecipientPage(9, 0), 0U);
}

TEST(ChatRecipientListTest, HitTestsOnlyVisibleButtonRowsAndRetainsThePlayerId)
{
	constexpr std::array<size_t, 7> RecipientPlayerIds { 1, 2, 3, 4, 5, 6, 7 };
	EXPECT_EQ(GetChatRecipientPlayerIdAtY(RecipientPlayerIds, 2, 69), 7U);
	EXPECT_FALSE(GetChatRecipientPlayerIdAtY(RecipientPlayerIds, 2, 85).has_value());
	EXPECT_EQ(GetChatRecipientPlayerIdAtY(RecipientPlayerIds, 1, 87), 5U);
	EXPECT_FALSE(GetChatRecipientPlayerIdAtY(RecipientPlayerIds, 1, 104).has_value());
	EXPECT_EQ(GetChatRecipientPlayerIdAtY(RecipientPlayerIds, 1, 105), 6U);

	const std::optional<size_t> clickedRecipient = GetChatRecipientPlayerIdAtY(RecipientPlayerIds, 2, 69);
	ASSERT_TRUE(clickedRecipient.has_value());
	const PlayerRecipientMask recipients = AddPlayerToRecipientMask(PlayerRecipientMask { 0 }, *clickedRecipient);
	EXPECT_EQ(recipients.bits, 0x80U);
}

} // namespace
} // namespace devilution
