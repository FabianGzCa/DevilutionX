#include <algorithm>
#include <cstddef>
#include <cstring>
#include <memory>
#include <string>
#include <string_view>

#include <gtest/gtest.h>

#include "dvlnet/base.h"
#include "dvlnet/base_protocol.h"
#include "dvlnet/limits.hpp"
#include "dvlnet/protocol_zt.h"
#include "dvlnet/session.hpp"

namespace devilution::net {
namespace {

GameData Game(bool extended = false)
{
	GameData data {};
	data.size = sizeof(data);
	data.versionMajor = 1;
	data.versionMinor = 6;
	data.nTickRate = 20;
	data.rules = extended ? ExtendedSessionRules : ClassicSessionRules;
	data.gameSeed[0] = 0x12345678;
	return data;
}

buffer_t Encode(GameData data)
{
	SwapGameDataLE(data);
	const auto *bytes = reinterpret_cast<const unsigned char *>(&data);
	return { bytes, bytes + sizeof(data) };
}

buffer_t Advertisement(const GameData &data, std::string_view name, size_t players)
{
	buffer_t info = Encode(data);
	info.resize(sizeof(GameData) + PlayerNameLength * SessionPlayerCapacity(data.rules));
	for (size_t i = 0; i < players; ++i)
		info[sizeof(GameData) + i * PlayerNameLength] = static_cast<unsigned char>('a' + i);
	info.insert(info.end(), name.begin(), name.end());
	return info;
}

class session_net : public base {
public:
	int create(std::string_view) override { return -1; }
	int join(std::string_view) override { return -1; }
	std::string make_default_gamename() override { return {}; }
	std::expected<void, PacketError> poll() override { return {}; }
	std::expected<void, PacketError> send(packet &) override { return {}; }
	std::expected<void, PacketError> Receive(packet &pkt) { return RecvLocal(pkt); }
	plr_t PlayerId() const { return plr_self; }
	void Prepare(const GameData &data)
	{
		clear_password();
		setup_gameinfo(Encode(data));
		cookie_self = 42;
	}
	const buffer_t &Info() const { return game_init_info; }

protected:
	bool IsGameHost() override { return false; }
};

// Exercise the peer protocol without relying on a public ZeroTier network.
struct memory_protocol {
	using endpoint = protocol_zt::endpoint;
	static inline std::deque<std::pair<endpoint, buffer_t>> incoming;
	static inline std::vector<buffer_t> outgoing;
	static inline std::vector<endpoint> disconnected;
	void disconnect(const endpoint &peer) { disconnected.push_back(peer); }
	std::expected<void, PacketError> send(const endpoint &, const buffer_t &data)
	{
		outgoing.push_back(data);
		return {};
	}
	bool send_oob(const endpoint &, const buffer_t &data)
	{
		outgoing.push_back(data);
		return true;
	}
	bool send_oob_mc(const buffer_t &data)
	{
		outgoing.push_back(data);
		return true;
	}
	bool recv(endpoint &peer, buffer_t &data)
	{
		if (incoming.empty())
			return false;
		peer = incoming.front().first;
		data = std::move(incoming.front().second);
		incoming.pop_front();
		return true;
	}
	bool recv_oob(endpoint &, buffer_t &) { return false; }
	bool get_disconnected(endpoint &) { return false; }
	std::expected<bool, PacketError> network_online() { return true; }
	std::expected<bool, PacketError> peers_ready() { return true; }
	bool is_peer_connected(const endpoint &) { return true; }
	std::optional<bool> is_peer_relayed(const endpoint &) { return false; }
	std::optional<int> get_latency_to(const endpoint &) { return 0; }
	std::string make_default_gamename() { return "test"; }
};

class PeerSessionCompatibility : public ::testing::Test {
protected:
	void SetUp() override
	{
		Players.resize(ClassicPlayerLimit);
		memory_protocol::incoming.clear();
		memory_protocol::outgoing.clear();
		memory_protocol::disconnected.clear();
		host_.clear_password();
		host_.setup_gameinfo(Encode(Game(true)));
		ASSERT_EQ(host_.create("test"), 0);
	}
	void TearDown() override { Players.clear(); }
	void Receive(packet &pkt, uint8_t address = 1)
	{
		memory_protocol::endpoint peer;
		peer.addr[0] = address;
		memory_protocol::incoming.emplace_back(peer, pkt.Data());
		ASSERT_TRUE(host_.poll());
	}
	base_protocol<memory_protocol> host_;
	packet_factory factory_;
};

TEST_F(PeerSessionCompatibility, RejectionDoesNotConsumePlayerSlot)
{
	auto request = factory_.make_packet<PT_JOIN_REQUEST>(PLR_BROADCAST, PLR_MASTER, cookie_t { 42 }, buffer_t {});
	ASSERT_TRUE(request.has_value());
	Receive(**request);
	ASSERT_EQ(memory_protocol::outgoing.size(), 1);
	auto reply = factory_.make_packet(memory_protocol::outgoing.back());
	ASSERT_TRUE(reply.has_value());
	EXPECT_EQ((*reply)->Type(), PT_JOIN_REJECT);
	memory_protocol::outgoing.clear();
	auto compatibleRequest = MakeJoinRequest(factory_, 43, Encode(Game(true)));
	ASSERT_TRUE(compatibleRequest.has_value());
	Receive(**compatibleRequest, 2);
	ASSERT_EQ(memory_protocol::outgoing.size(), 1);
	auto acceptance = factory_.make_packet(memory_protocol::outgoing.back());
	ASSERT_TRUE(acceptance.has_value());
	EXPECT_EQ((*acceptance)->Type(), PT_JOIN_ACCEPT_EXTENDED);
	EXPECT_EQ(*(*acceptance)->NewPlayer(), 1);
	EXPECT_EQ(*(*acceptance)->Cookie(), 43);
}

TEST_F(PeerSessionCompatibility, ExtendedHostDoesNotAdvertiseToClassicDiscovery)
{
	auto request = factory_.make_packet<PT_INFO_REQUEST>(PLR_BROADCAST, PLR_MASTER);
	ASSERT_TRUE(request.has_value());
	Receive(**request);
	EXPECT_TRUE(memory_protocol::outgoing.empty());
	auto extendedRequest = factory_.make_packet<PT_INFO_REQUEST_EXTENDED>(PLR_BROADCAST, PLR_MASTER);
	ASSERT_TRUE(extendedRequest.has_value());
	Receive(**extendedRequest);
	ASSERT_EQ(memory_protocol::outgoing.size(), 1);
	auto reply = factory_.make_packet(memory_protocol::outgoing.back());
	ASSERT_TRUE(reply.has_value());
	EXPECT_EQ((*reply)->Type(), PT_INFO_REPLY_EXTENDED);
}

TEST_F(PeerSessionCompatibility, InvalidDiscoveryDataIsNotListed)
{
	for (size_t length : { size_t { 0 }, sizeof(GameData) - 1, sizeof(GameData) + PlayerNameLength * ClassicPlayerLimit }) {
		auto reply = factory_.make_packet<PT_INFO_REPLY_EXTENDED>(PLR_BROADCAST, PLR_MASTER, buffer_t(length));
		ASSERT_TRUE(reply.has_value());
		Receive(**reply);
		EXPECT_TRUE(host_.get_gamelist().empty());
	}
	buffer_t info = Encode(Game(true));
	info.resize(sizeof(GameData) + PlayerNameLength * ExtendedSessionRules.maxPlayers);
	info.push_back('x');
	auto wrongKind = factory_.make_packet<PT_INFO_REPLY>(PLR_BROADCAST, PLR_MASTER, info);
	ASSERT_TRUE(wrongKind.has_value());
	Receive(**wrongKind);
	EXPECT_TRUE(host_.get_gamelist().empty());
	auto extendedReply = factory_.make_packet<PT_INFO_REPLY_EXTENDED>(PLR_BROADCAST, PLR_MASTER, info);
	ASSERT_TRUE(extendedReply.has_value());
	Receive(**extendedReply);
	ASSERT_EQ(host_.get_gamelist().size(), 1);
	EXPECT_EQ(host_.get_gamelist()[0].name, "x");
}

TEST_F(PeerSessionCompatibility, ClassicHostAcceptsLegacyRequest)
{
	host_.setup_gameinfo(Encode(Game()));
	auto request = factory_.make_packet<PT_JOIN_REQUEST>(PLR_BROADCAST, PLR_MASTER, cookie_t { 42 }, buffer_t {});
	ASSERT_TRUE(request.has_value());
	Receive(**request);
	ASSERT_EQ(memory_protocol::outgoing.size(), 1);
	auto reply = factory_.make_packet(memory_protocol::outgoing.back());
	ASSERT_TRUE(reply.has_value());
	EXPECT_EQ((*reply)->Type(), PT_JOIN_ACCEPT);
	EXPECT_EQ(*(*reply)->NewPlayer(), 1);
}

TEST_F(PeerSessionCompatibility, IncompatibleIdentityNeverAnnouncesPlayer)
{
	for (unsigned scenario = 0; scenario < 6; ++scenario) {
		GameData client = Game(true);
		switch (scenario) {
		case 0: client.rules.protocolRevision = ExtendedSessionRules.protocolRevision + 1; break;
		case 1: client.rules.maxPlayers = 4; break;
		case 2: client.rules.balanceRevision = 2; break;
		case 3: client.versionMajor = 9; break;
		case 4: client.isSpawn = 1; break;
		case 5: client.isHellfire = 1; break;
		}
		auto request = factory_.make_packet<PT_JOIN_REQUEST_EXTENDED>(PLR_BROADCAST, PLR_MASTER, cookie_t { 42 }, Encode(client));
		ASSERT_TRUE(request.has_value());
		Receive(**request);
		ASSERT_EQ(memory_protocol::outgoing.size(), 1);
		auto reply = factory_.make_packet(memory_protocol::outgoing.back());
		ASSERT_TRUE(reply.has_value());
		EXPECT_EQ((*reply)->Type(), PT_JOIN_REJECT);
		memory_protocol::outgoing.clear();
	}
	auto request = MakeJoinRequest(factory_, 43, Encode(Game(true)));
	ASSERT_TRUE(request.has_value());
	Receive(**request);
	ASSERT_EQ(memory_protocol::outgoing.size(), 1);
	auto reply = factory_.make_packet(memory_protocol::outgoing.back());
	ASSERT_TRUE(reply.has_value());
	EXPECT_EQ((*reply)->Type(), PT_JOIN_ACCEPT_EXTENDED);
	EXPECT_EQ(*(*reply)->NewPlayer(), 1);
}

TEST_F(PeerSessionCompatibility, BrowserListsBothModesAndReportsDirectJoinMismatch)
{
	base_protocol<memory_protocol> client;
	client.clear_password();
	client.setup_gameinfo(Encode(Game()));
	memory_protocol::outgoing.clear();
	ASSERT_TRUE(client.send_info_request());
	ASSERT_EQ(memory_protocol::outgoing.size(), 2);
	for (size_t i = 0; i < 2; ++i) {
		auto request = factory_.make_packet(memory_protocol::outgoing[i]);
		ASSERT_TRUE(request.has_value());
		EXPECT_EQ((*request)->Type(), i == 0 ? PT_INFO_REQUEST : PT_INFO_REQUEST_EXTENDED);
	}

	auto classicReply = factory_.make_packet<PT_INFO_REPLY>(PLR_BROADCAST, PLR_MASTER, Advertisement(Game(), "classic", 2));
	auto extendedReply = factory_.make_packet<PT_INFO_REPLY_EXTENDED>(PLR_BROADCAST, PLR_MASTER, Advertisement(Game(true), "extended", 5));
	ASSERT_TRUE(classicReply.has_value());
	ASSERT_TRUE(extendedReply.has_value());
	memory_protocol::endpoint classicHost;
	classicHost.addr[0] = 3;
	memory_protocol::endpoint extendedHost;
	extendedHost.addr[0] = 4;
	memory_protocol::incoming.emplace_back(classicHost, (*classicReply)->Data());
	memory_protocol::incoming.emplace_back(extendedHost, (*extendedReply)->Data());
	const std::vector<GameInfo> games = client.get_gamelist();
	ASSERT_EQ(games.size(), 2);
	const auto find = [&](std::string_view name) { return std::find_if(games.begin(), games.end(), [&](const GameInfo &game) { return game.name == name; }); };
	const auto classic = find("classic");
	const auto extended = find("extended");
	ASSERT_NE(classic, games.end());
	ASSERT_NE(extended, games.end());
	EXPECT_EQ(classic->gameData.rules, ClassicSessionRules);
	EXPECT_EQ(classic->players.size(), 2);
	EXPECT_EQ(extended->gameData.rules, ExtendedSessionRules);
	EXPECT_EQ(extended->players.size(), 5);

	// A classic direct join learns the advertised mode without sending an incompatible request.
	memory_protocol::outgoing.clear();
	EXPECT_EQ(client.join("extended"), -1);
	EXPECT_EQ(client.get_join_rejection(), GameDataError::SessionMismatch);
	EXPECT_TRUE(memory_protocol::outgoing.empty());
}

TEST_F(PeerSessionCompatibility, FullDiscoveryCacheRefreshesExistingGamesWithoutGrowing)
{
	for (const bool extended : { false, true }) {
		SCOPED_TRACE(extended);
		host_.clear_gamelist();
		const auto advertise = [&](std::string_view name, const GameData &data, size_t players) {
			auto reply = data.isExtended()
			    ? factory_.make_packet<PT_INFO_REPLY_EXTENDED>(PLR_BROADCAST, PLR_MASTER, Advertisement(data, name, players))
			    : factory_.make_packet<PT_INFO_REPLY>(PLR_BROADCAST, PLR_MASTER, Advertisement(data, name, players));
			ASSERT_TRUE(reply.has_value());
			Receive(**reply);
		};
		for (size_t index = 0; index < MaxDiscoveredGames; ++index)
			advertise("room" + std::to_string(index), Game(extended), 1);
		ASSERT_EQ(host_.get_gamelist().size(), MaxDiscoveredGames);

		GameData updated = Game(!extended);
		updated.nTickRate = 30;
		++updated.gameSeed[0];
		advertise("room0", updated, 3);
		advertise("overflow", Game(extended), 1);
		const auto games = host_.get_gamelist();
		ASSERT_EQ(games.size(), MaxDiscoveredGames);
		const auto refreshed = std::find_if(games.begin(), games.end(), [](const GameInfo &game) { return game.name == "room0"; });
		ASSERT_NE(refreshed, games.end());
		EXPECT_EQ(refreshed->players.size(), 3U);
		EXPECT_EQ(refreshed->gameData.rules, updated.rules);
		EXPECT_EQ(refreshed->gameData.gameSeed[0], updated.gameSeed[0]);
		EXPECT_EQ(refreshed->gameData.nTickRate, updated.nTickRate);
		EXPECT_EQ(std::find_if(games.begin(), games.end(), [](const GameInfo &game) { return game.name == "overflow"; }), games.end());
		host_.clear_gamelist();
		advertise("overflow", Game(extended), 1);
		ASSERT_EQ(host_.get_gamelist().size(), 1U);
		EXPECT_EQ(host_.get_gamelist().front().name, "overflow");
	}
}

TEST(SessionCompatibility, TruncatedPacketHeadersAreRejected)
{
	packet_factory factory;
	for (packet_type type : { PT_JOIN_REQUEST_EXTENDED, PT_JOIN_ACCEPT_EXTENDED, PT_JOIN_REJECT }) {
		const size_t headerSize = type == PT_JOIN_ACCEPT_EXTENDED ? 8 : 7;
		buffer_t bytes(headerSize);
		bytes[0] = type;
		bytes[1] = PLR_BROADCAST;
		bytes[2] = PLR_MASTER;
		for (size_t size = 0; size < headerSize; ++size) {
			SCOPED_TRACE(size);
			EXPECT_FALSE(factory.make_packet(buffer_t(bytes.begin(), bytes.begin() + size)));
		}
	}
}

TEST(SessionCompatibility, ClassicWireLayoutAndLittleEndianSeeds)
{
	const buffer_t info = Encode(Game());
	ASSERT_EQ(info.size(), 40);
	EXPECT_EQ(info[0], 40);
	for (size_t i = 5; i != 8; ++i)
		EXPECT_EQ(info[i], 0);
	EXPECT_EQ(info[24], 0x78);
	EXPECT_EQ(info[25], 0x56);
	EXPECT_EQ(info[26], 0x34);
	EXPECT_EQ(info[27], 0x12);
	const auto decoded = DecodeGameData(info);
	ASSERT_TRUE(decoded.has_value());
	EXPECT_EQ(decoded->gameSeed[0], 0x12345678);
}

TEST(SessionCompatibility, RejectsAllTruncatedAndOversizedGameData)
{
	buffer_t info = Encode(Game());
	for (size_t size = 0; size < info.size(); ++size) {
		SCOPED_TRACE(size);
		EXPECT_EQ(DecodeGameData(std::span(info).first(size)).error(), GameDataError::InvalidSize);
	}
	info.push_back(0);
	EXPECT_EQ(DecodeGameData(info).error(), GameDataError::InvalidSize);
	GameData data = Game();
	data.size = 39;
	EXPECT_EQ(DecodeGameData(Encode(data)).error(), GameDataError::InvalidSize);
}

TEST(SessionCompatibility, RejectsInvalidSettings)
{
	for (auto member : { &GameData::isSpawn, &GameData::bRunInTown, &GameData::bTheoQuest, &GameData::bCowQuest, &GameData::bFriendlyFire, &GameData::fullQuests }) {
		GameData data = Game();
		data.*member = 2;
		EXPECT_EQ(ValidateGameData(data), GameDataError::InvalidParameters);
	}
	GameData data = Game();
	data.nTickRate = 0;
	EXPECT_EQ(ValidateGameData(data), GameDataError::InvalidParameters);
	data = Game();
	data.nDifficulty = static_cast<_difficulty>(DIFF_LAST + 1);
	EXPECT_EQ(ValidateGameData(data), GameDataError::InvalidParameters);
	data = Game(true);
	data.isHellfire = 2;
	EXPECT_EQ(ValidateGameData(data), GameDataError::InvalidParameters);
	data.isHellfire = 1;
	data.isSpawn = 1;
	EXPECT_EQ(ValidateGameData(data), GameDataError::InvalidParameters);
	data = Game(true);
	data.reserved = 1;
	EXPECT_EQ(ValidateGameData(data), GameDataError::InvalidParameters);
}

TEST(SessionCompatibility, EveryUnknownRuleByteIsRejected)
{
	for (auto member : { &SessionRules::protocolRevision, &SessionRules::maxPlayers, &SessionRules::balanceRevision }) {
		for (unsigned value = 0; value <= 255; ++value) {
			GameData data = Game(true);
			if (value == data.rules.*member)
				continue;
			data.rules.*member = static_cast<uint8_t>(value);
			EXPECT_EQ(ValidateGameData(data), GameDataError::UnsupportedRules);
		}
	}
}

TEST(SessionCompatibility, IdentityChecksVersionsEditionsAndRules)
{
	const GameData host = Game(true);
	for (auto member : { &GameData::versionMajor, &GameData::versionMinor, &GameData::versionPatch }) {
		GameData client = host;
		++(client.*member);
		EXPECT_EQ(CheckGameCompatibility(host, client), GameDataError::VersionMismatch);
	}
	GameData client = host;
	client.isSpawn = 1;
	EXPECT_EQ(CheckGameCompatibility(host, client), GameDataError::EditionMismatch);
	client = host;
	client.isHellfire = 1;
	EXPECT_EQ(CheckGameCompatibility(host, client), GameDataError::EditionMismatch);
	client = host;
	client.rules = ClassicSessionRules;
	EXPECT_EQ(CheckGameCompatibility(host, client), GameDataError::SessionMismatch);
	client = host;
	client.programid = 0x12345678;
	EXPECT_FALSE(CheckGameCompatibility(host, client));
	client = Game();
	client.isHellfire = 255; // Padding in the original client is not an identity field.
	client.reserved = 255;
	EXPECT_FALSE(CheckGameCompatibility(Game(), client));
}

TEST(SessionCompatibility, SupportedModesHaveCapacityAndOneAlternative)
{
	EXPECT_TRUE(IsSupportedSessionRules(ClassicSessionRules));
	EXPECT_TRUE(IsSupportedSessionRules(ExtendedSessionRules));
	EXPECT_EQ(SessionPlayerCapacity(ClassicSessionRules), 4);
	EXPECT_EQ(SessionPlayerCapacity(ExtendedSessionRules), 8);
	EXPECT_EQ(AlternateSessionRules(ClassicSessionRules), ExtendedSessionRules);
	EXPECT_EQ(AlternateSessionRules(ExtendedSessionRules), ClassicSessionRules);
	for (const SessionRules rules : { SessionRules { 3, 8, 2 }, SessionRules { 2, 8, 1 }, SessionRules { 3, 4, 1 }, SessionRules { 0, 8, 0 } }) {
		EXPECT_FALSE(IsSupportedSessionRules(rules));
		EXPECT_EQ(SessionPlayerCapacity(rules), 0);
		EXPECT_FALSE(AlternateSessionRules(rules));
	}
}

TEST(SessionCompatibility, NewGameSettingsAreValidatedBeforeHosting)
{
	EXPECT_FALSE(ValidateNewGame(Game(), false));
	EXPECT_FALSE(ValidateNewGame(Game(), true));
	EXPECT_FALSE(ValidateNewGame(Game(true), false));
	EXPECT_EQ(ValidateNewGame(Game(true), true), GameDataError::UnsupportedRules);
	GameData data = Game(true);
	data.rules.maxPlayers = 16;
	EXPECT_EQ(ValidateNewGame(data, false), GameDataError::UnsupportedRules);
	data = Game(true);
	data.nTickRate = 0;
	EXPECT_EQ(ValidateNewGame(data, false), GameDataError::InvalidParameters);
	data = Game();
	data.nDifficulty = static_cast<_difficulty>(DIFF_LAST + 1);
	EXPECT_EQ(ValidateNewGame(data, false), GameDataError::InvalidParameters);
}

TEST(SessionCompatibility, LegacyEmptyJoinIsClassicOnly)
{
	packet_factory factory;
	auto request = factory.make_packet<PT_JOIN_REQUEST>(PLR_BROADCAST, PLR_MASTER, cookie_t { 42 }, buffer_t {});
	ASSERT_TRUE(request.has_value());
	EXPECT_FALSE(ValidateJoinRequest(Encode(Game()), **request));
	EXPECT_EQ(ValidateJoinRequest(Encode(Game(true)), **request), GameDataError::SessionMismatch);
	auto extendedRequest = factory.make_packet<PT_JOIN_REQUEST_EXTENDED>(PLR_BROADCAST, PLR_MASTER, cookie_t { 42 }, buffer_t {});
	ASSERT_TRUE(extendedRequest.has_value());
	EXPECT_EQ(ValidateJoinRequest(Encode(Game(true)), **extendedRequest), GameDataError::InvalidSize);
}

TEST(SessionCompatibility, JoinRequestRequiresCorrectKindIdentityAndAddress)
{
	packet_factory factory;
	for (bool extended : { false, true }) {
		const buffer_t info = Encode(Game(extended));
		auto request = MakeJoinRequest(factory, 42, info);
		ASSERT_TRUE(request.has_value());
		EXPECT_FALSE(ValidateJoinRequest(info, **request));
		EXPECT_EQ(ValidateJoinRequest(Encode(Game(!extended)), **request), GameDataError::SessionMismatch);
	}
	auto invalidSource = factory.make_packet<PT_JOIN_REQUEST>(plr_t { 0 }, PLR_MASTER, cookie_t { 42 }, Encode(Game()));
	ASSERT_TRUE(invalidSource.has_value());
	EXPECT_EQ(ValidateJoinRequest(Encode(Game()), **invalidSource), GameDataError::InvalidParameters);
	GameData invalid = Game(true);
	invalid.rules.balanceRevision = 2;
	auto invalidRules = factory.make_packet<PT_JOIN_REQUEST_EXTENDED>(PLR_BROADCAST, PLR_MASTER, cookie_t { 42 }, Encode(invalid));
	ASSERT_TRUE(invalidRules.has_value());
	EXPECT_EQ(ValidateJoinRequest(Encode(Game(true)), **invalidRules), GameDataError::UnsupportedRules);
}

TEST(SessionCompatibility, NewPacketKindsRoundTripWithAndWithoutEncryption)
{
	for (bool encrypted : { false, true }) {
		auto factory = encrypted ? std::make_unique<packet_factory>("session-test") : std::make_unique<packet_factory>();
		const buffer_t info = Encode(Game(true));
		auto request = MakeJoinRequest(*factory, 42, info);
		ASSERT_TRUE(request.has_value());
		auto parsed = factory->make_packet((*request)->Data());
		ASSERT_TRUE(parsed.has_value());
		EXPECT_EQ((*parsed)->Type(), PT_JOIN_REQUEST_EXTENDED);
		EXPECT_EQ(*(*parsed)->Cookie(), 42);
		EXPECT_FALSE(ValidateJoinRequest(info, **parsed));
		auto accept = MakeJoinAccept(*factory, PLR_MASTER, 42, 1, info);
		ASSERT_TRUE(accept.has_value());
		auto parsedAccept = factory->make_packet((*accept)->Data());
		ASSERT_TRUE(parsedAccept.has_value());
		EXPECT_EQ((*parsedAccept)->Type(), PT_JOIN_ACCEPT_EXTENDED);
		EXPECT_EQ(*(*parsedAccept)->NewPlayer(), 1);
		EXPECT_EQ(**(*parsedAccept)->Info(), info);
		auto reject = MakeJoinReject(*factory, PLR_MASTER, **request, GameDataError::SessionMismatch);
		ASSERT_TRUE(reject.has_value());
		auto parsedReject = factory->make_packet((*reject)->Data());
		ASSERT_TRUE(parsedReject.has_value());
		EXPECT_EQ((*parsedReject)->Type(), PT_JOIN_REJECT);
		EXPECT_EQ(*(*parsedReject)->Cookie(), 42);
		EXPECT_EQ(**(*parsedReject)->Info(), buffer_t { static_cast<unsigned char>(GameDataError::SessionMismatch) });
		auto discovery = factory->make_packet<PT_INFO_REQUEST_EXTENDED>(PLR_BROADCAST, PLR_MASTER);
		ASSERT_TRUE(discovery.has_value());
		auto parsedDiscovery = factory->make_packet((*discovery)->Data());
		ASSERT_TRUE(parsedDiscovery.has_value());
		EXPECT_EQ((*parsedDiscovery)->Type(), PT_INFO_REQUEST_EXTENDED);
		auto discoveryReply = factory->make_packet<PT_INFO_REPLY_EXTENDED>(PLR_BROADCAST, PLR_MASTER, info);
		ASSERT_TRUE(discoveryReply.has_value());
		auto parsedReply = factory->make_packet((*discoveryReply)->Data());
		ASSERT_TRUE(parsedReply.has_value());
		EXPECT_EQ((*parsedReply)->Type(), PT_INFO_REPLY_EXTENDED);
		EXPECT_EQ(**(*parsedReply)->Info(), info);
	}
}

TEST(SessionCompatibility, TruncatedAcceptNeverPublishesGameData)
{
	packet_factory factory;
	for (size_t size = 0; size < sizeof(GameData); ++size) {
		session_net client;
		client.Prepare(Game(true));
		buffer_t info = Encode(Game(true));
		info.resize(size);
		auto accept = factory.make_packet<PT_JOIN_ACCEPT_EXTENDED>(PLR_MASTER, PLR_BROADCAST, cookie_t { 42 }, plr_t { 1 }, info);
		ASSERT_TRUE(accept.has_value());
		EXPECT_FALSE(client.Receive(**accept));
		EXPECT_EQ(client.PlayerId(), PLR_BROADCAST);
		EXPECT_EQ(client.Info(), Encode(Game(true)));
	}
}

TEST(SessionCompatibility, AcceptanceValidatesRulesKindAndPlayerBeforeAdmission)
{
	packet_factory factory;
	for (bool extended : { false, true }) {
		session_net client;
		client.Prepare(Game(extended));
		auto accept = MakeJoinAccept(factory, PLR_MASTER, 41, 1, Encode(Game(extended)));
		ASSERT_TRUE(accept.has_value());
		EXPECT_TRUE(client.Receive(**accept));
		EXPECT_EQ(client.PlayerId(), PLR_BROADCAST);
		auto invalidPlayer = MakeJoinAccept(factory, PLR_MASTER, 42, 255, Encode(Game(extended)));
		ASSERT_TRUE(invalidPlayer.has_value());
		EXPECT_FALSE(client.Receive(**invalidPlayer));
		EXPECT_EQ(client.PlayerId(), PLR_BROADCAST);
		auto wrongMode = MakeJoinAccept(factory, PLR_MASTER, 42, 1, Encode(Game(!extended)));
		ASSERT_TRUE(wrongMode.has_value());
		EXPECT_FALSE(client.Receive(**wrongMode));
		EXPECT_EQ(client.PlayerId(), PLR_BROADCAST);
		GameData host = Game(extended);
		host.gameSeed[0] = 100;
		auto validAccept = MakeJoinAccept(factory, PLR_MASTER, 42, 1, Encode(host));
		ASSERT_TRUE(validAccept.has_value());
		EXPECT_TRUE(client.Receive(**validAccept));
		EXPECT_EQ(client.PlayerId(), 1);
		EXPECT_EQ(client.Info(), Encode(host));
	}
	session_net client;
	client.Prepare(Game(true));
	auto wrongKind = factory.make_packet<PT_JOIN_ACCEPT>(PLR_MASTER, PLR_BROADCAST, cookie_t { 42 }, plr_t { 1 }, Encode(Game(true)));
	ASSERT_TRUE(wrongKind.has_value());
	EXPECT_FALSE(client.Receive(**wrongKind));
	EXPECT_EQ(client.PlayerId(), PLR_BROADCAST);
}

TEST(SessionCompatibility, RejectionIsCorrelatedAndReportsReason)
{
	packet_factory factory;
	session_net client;
	client.Prepare(Game(true));
	const auto reason = static_cast<unsigned char>(GameDataError::UnsupportedRules);
	auto reject = factory.make_packet<PT_JOIN_REJECT>(PLR_MASTER, PLR_BROADCAST, cookie_t { 41 }, buffer_t { reason });
	ASSERT_TRUE(reject.has_value());
	EXPECT_TRUE(client.Receive(**reject));
	EXPECT_FALSE(client.get_join_rejection());
	auto matchingReject = factory.make_packet<PT_JOIN_REJECT>(PLR_MASTER, PLR_BROADCAST, cookie_t { 42 }, buffer_t { reason });
	ASSERT_TRUE(matchingReject.has_value());
	const auto result = client.Receive(**matchingReject);
	ASSERT_FALSE(result.has_value());
	EXPECT_EQ(result.error().what(), GameDataErrorMessage(GameDataError::UnsupportedRules));
	EXPECT_EQ(client.get_join_rejection(), GameDataError::UnsupportedRules);
	EXPECT_EQ(client.PlayerId(), PLR_BROADCAST);
	for (buffer_t invalid : { buffer_t {}, buffer_t { 255 }, buffer_t { reason, reason } }) {
		auto invalidReject = factory.make_packet<PT_JOIN_REJECT>(PLR_MASTER, PLR_BROADCAST, cookie_t { 42 }, invalid);
		ASSERT_TRUE(invalidReject.has_value());
		EXPECT_FALSE(client.Receive(**invalidReject));
	}
}

TEST(SessionCompatibility, GamePayloadBeforeAdmissionIsRejected)
{
	packet_factory factory;
	session_net client;
	client.Prepare(Game(true));
	auto message = factory.make_packet<PT_MESSAGE>(plr_t { 0 }, PLR_BROADCAST, buffer_t { 1, 2, 3 });
	ASSERT_TRUE(message.has_value());
	EXPECT_FALSE(client.Receive(**message));
	auto turn = factory.make_packet<PT_TURN>(plr_t { 0 }, PLR_BROADCAST, turn_t { 0, 123 });
	ASSERT_TRUE(turn.has_value());
	EXPECT_FALSE(client.Receive(**turn));
	EXPECT_EQ(client.PlayerId(), PLR_BROADCAST);
}

TEST(SessionCompatibility, ClassicDescriptorKeepsOriginalPaddingInHellfire)
{
	const bool hellfire = gbIsHellfire;
	gbIsHellfire = true;
	InitGameInfo();
	EXPECT_EQ(sgGameInitInfo.rules, ClassicSessionRules);
	EXPECT_EQ(sgGameInitInfo.isHellfire, 0);
	EXPECT_EQ(Encode(sgGameInitInfo)[offsetof(GameData, isHellfire)], 0);
	SetSessionRules(sgGameInitInfo, ExtendedSessionRules);
	EXPECT_EQ(sgGameInitInfo.isHellfire, 1);
	SetSessionRules(sgGameInitInfo, ClassicSessionRules);
	EXPECT_EQ(sgGameInitInfo.isHellfire, 0);
	gbIsHellfire = false;
	SetSessionRules(sgGameInitInfo, ExtendedSessionRules);
	EXPECT_EQ(sgGameInitInfo.isHellfire, 0);
	gbIsHellfire = hellfire;
	InitGameInfo();
}

} // namespace
} // namespace devilution::net
