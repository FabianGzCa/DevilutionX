#include <chrono>
#include <cstdint>
#include <expected>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

#include <gtest/gtest.h>

#include "dvlnet/base.h"
#if !defined(NONET) && !defined(DISABLE_TCP)
#include "dvlnet/tcp_client.h"
#include "dvlnet/tcp_server.h"
#endif

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

#if !defined(NONET) && !defined(DISABLE_TCP)
std::vector<uint8_t> LeftPlayers;

void RecordLeftPlayer(_SNETEVENT *event)
{
	LeftPlayers.push_back(static_cast<uint8_t>(event->playerid));
}

TEST(TcpClientConnectionLoss, LostServerKeepsTheSessionUsableUntilTheHostIsDropped)
{
	GameData data {};
	data.size = sizeof(data);
	data.nTickRate = 20;
	SwapGameDataLE(data);
	const auto *bytes = reinterpret_cast<const unsigned char *>(&data);
	const buffer_t info { bytes, bytes + sizeof(data) };
	asio::io_context context;
	packet_factory factory;
	auto server = std::make_unique<tcp_server>(context, "127.0.0.1", 0, factory, info);
	const std::string address = server->LocalhostSelf();
	std::thread thread([&] { context.run(); });
	tcp_client host;
	tcp_client client;
	for (tcp_client *peer : { &host, &client }) {
		peer->clear_password();
		peer->setup_gameinfo(info);
	}
	ASSERT_EQ(host.join(address), 0);
	ASSERT_EQ(client.join(address), 1);
	LeftPlayers.clear();
	ASSERT_TRUE(client.SNetRegisterEventHandler(EVENT_TYPE_PLAYER_LEAVE_GAME, RecordLeftPlayer));
	char marker = 7;
	ASSERT_TRUE(host.SNetSendMessage(1, &marker, 1));
	uint8_t sender = 0;
	void *payload = nullptr;
	size_t size = 0;
	const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
	while (!client.SNetReceiveMessage(&sender, &payload, &size) && std::chrono::steady_clock::now() < deadline)
		std::this_thread::sleep_for(std::chrono::milliseconds(1));
	ASSERT_EQ(size, 1U);

	// The host disappears without a goodbye, as when its process or network fails.
	asio::post(context, [&] { server->Close(); });
	thread.join();
	bool lost = false;
	while (!lost && std::chrono::steady_clock::now() < deadline + std::chrono::seconds(5)) {
		lost = !client.poll().has_value();
		std::this_thread::sleep_for(std::chrono::milliseconds(1));
	}
	ASSERT_TRUE(lost);

	// The engine keeps calling these every tick; none may fail before the host is dropped.
	uint32_t turns = 0;
	EXPECT_TRUE(client.SNetGetTurnsInTransit(&turns));
	int32_t turn = 1;
	EXPECT_TRUE(client.SNetSendTurn(reinterpret_cast<char *>(&turn), sizeof(turn)));
	EXPECT_TRUE(client.SNetSendMessage(0, &marker, 1));
	EXPECT_TRUE(client.SNetDropPlayer(0, leaveinfo_t::LEAVE_DROP));
	EXPECT_EQ(LeftPlayers, std::vector<uint8_t> { 0 });
	EXPECT_TRUE(client.SNetLeaveGame(leaveinfo_t::LEAVE_EXIT));
}
#endif

} // namespace
} // namespace devilution::net
