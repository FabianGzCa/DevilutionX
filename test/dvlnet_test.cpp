#include <cstdint>
#include <expected>
#include <memory>
#include <optional>
#include <string>
#include <string_view>

#include <gtest/gtest.h>

#include "dvlnet/base.h"

namespace devilution::net {
namespace {

class test_net : public base {
public:
	int create(std::string_view /*addrstr*/) override
	{
		return -1;
	}

	int join(std::string_view /*addrstr*/) override
	{
		return -1;
	}

	std::string make_default_gamename() override
	{
		return {};
	}

	void SetPlayerId(plr_t player)
	{
		plr_self = player;
	}

	std::expected<void, PacketError> ConnectPlayer(plr_t player)
	{
		return Connect(player);
	}

	std::expected<void, PacketError> Receive(packet &pkt)
	{
		return RecvLocal(pkt);
	}

	bool IsPlayerConnected(plr_t player) const
	{
		return IsConnected(player);
	}

	void FailSends()
	{
		failSends_ = true;
	}

	void AllowSends()
	{
		failSends_ = false;
	}

	void SetGameHost(bool isGameHost)
	{
		isGameHost_ = isGameHost;
	}

	int SendCount() const
	{
		return sendCount_;
	}

	std::optional<seq_t> LastTurnSequence() const
	{
		return lastTurnSequence_;
	}

	std::expected<void, PacketError> poll() override
	{
		return {};
	}

	std::expected<void, PacketError> send(packet &pkt) override
	{
		++sendCount_;
		if (pkt.Type() == PT_TURN) {
			if (std::expected<turn_t, PacketError> turn = pkt.Turn(); turn.has_value())
				lastTurnSequence_ = turn->SequenceNumber;
		}
		if (failSends_)
			return std::unexpected(PacketError("injected send failure"));
		return {};
	}

protected:
	bool IsGameHost() override
	{
		return isGameHost_;
	}

private:
	bool failSends_ = false;
	bool isGameHost_ = true;
	int sendCount_ = 0;
	std::optional<seq_t> lastTurnSequence_;
};

class DvlNetErrorTest : public ::testing::Test {
protected:
	void SetUp() override
	{
		net_.setup_password("");
	}

	test_net net_;
	packet_factory packetFactory_;
};

TEST_F(DvlNetErrorTest, SendTurnReportsTransportFailure)
{
	net_.SetPlayerId(0);
	ASSERT_TRUE(net_.ConnectPlayer(0));
	net_.FailSends();

	int32_t turnValue = 42;
	EXPECT_FALSE(net_.SNetSendTurn(reinterpret_cast<char *>(&turnValue), sizeof(turnValue)));
	ASSERT_EQ(net_.SendCount(), 1);

	EXPECT_TRUE(net_.ConnectPlayer(1));
	EXPECT_EQ(net_.SendCount(), 1);

	net_.AllowSends();
	EXPECT_TRUE(net_.SNetSendTurn(reinterpret_cast<char *>(&turnValue), sizeof(turnValue)));
	EXPECT_EQ(net_.SendCount(), 2);
	ASSERT_TRUE(net_.LastTurnSequence().has_value());
	EXPECT_EQ(*net_.LastTurnSequence(), 0);
}

TEST_F(DvlNetErrorTest, ConnectFailureIsPropagatedAndRolledBack)
{
	net_.SetPlayerId(0);
	ASSERT_TRUE(net_.ConnectPlayer(0));

	int32_t turnValue = 42;
	ASSERT_TRUE(net_.SNetSendTurn(reinterpret_cast<char *>(&turnValue), sizeof(turnValue)));
	ASSERT_EQ(net_.SendCount(), 1);

	net_.FailSends();
	std::expected<std::unique_ptr<packet>, PacketError> pkt
	    = packetFactory_.make_packet<PT_CONNECT>(static_cast<plr_t>(0), PLR_BROADCAST, static_cast<plr_t>(1));
	ASSERT_TRUE(pkt.has_value());

	std::expected<void, PacketError> result = net_.Receive(**pkt);
	ASSERT_FALSE(result.has_value());
	EXPECT_EQ(result.error().what(), "injected send failure");
	EXPECT_FALSE(net_.IsPlayerConnected(1));
	EXPECT_EQ(net_.SendCount(), 2);
}

TEST_F(DvlNetErrorTest, TurnHandlingPropagatesFailureToSendQueuedTurns)
{
	net_.SetPlayerId(1);
	net_.SetGameHost(false);
	ASSERT_TRUE(net_.ConnectPlayer(1));

	int32_t turnValue = 42;
	ASSERT_TRUE(net_.SNetSendTurn(reinterpret_cast<char *>(&turnValue), sizeof(turnValue)));
	EXPECT_EQ(net_.SendCount(), 0);

	net_.FailSends();
	std::expected<std::unique_ptr<packet>, PacketError> pkt
	    = packetFactory_.make_packet<PT_TURN>(static_cast<plr_t>(0), PLR_BROADCAST, turn_t { 0, 7 });
	ASSERT_TRUE(pkt.has_value());

	std::expected<void, PacketError> result = net_.Receive(**pkt);
	ASSERT_FALSE(result.has_value());
	EXPECT_EQ(result.error().what(), "injected send failure");
	EXPECT_EQ(net_.SendCount(), 1);
}

} // namespace
} // namespace devilution::net
