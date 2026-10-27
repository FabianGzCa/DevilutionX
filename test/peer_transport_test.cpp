#include <algorithm>
#include <array>
#include <functional>
#include <iostream>
#include <map>
#include <memory>
#include <optional>
#include <vector>

#include <gtest/gtest.h>

#include "dvlnet/base_protocol.h"
#include "dvlnet/protocol_zt.h"
#include "dvlnet/session.hpp"
#include "player.h"

namespace devilution::net {
namespace {

// Only the byte transport is substituted. Admission, discovery, player tables,
// peer handshakes, message routing and turns run through base_protocol itself.
class memory_wire {
public:
	using endpoint = protocol_zt::endpoint;
	static inline std::map<uint8_t, memory_wire *> instances;
	static inline std::function<void(uint8_t)> pump;
	static inline std::function<void(const buffer_t &)> observe;
	static inline std::function<bool(const endpoint &, const buffer_t &)> intercept;
	static inline bool streamsConnected = true;
	static inline uint8_t nextId = 1;
	memory_wire()
	    : id_(nextId++)
	{ instances[id_] = this; }
	~memory_wire()
	{
		instances.erase(id_);
		for (auto &[id, wire] : instances)
			wire->disconnected_.push_back(Address());
	}
	endpoint Address() const
	{
		endpoint ep;
		ep.addr[0] = id_;
		return ep;
	}
	std::expected<void, PacketError> send(const endpoint &destination, const buffer_t &data)
	{
		const auto it = instances.find(destination.addr[0]);
		if (it == instances.end()) return std::unexpected("Disconnected memory peer");
		if (observe) observe(data);
		if (intercept && !intercept(destination, data)) return { };
		it->second->incoming_.emplace_back(Address(), data);
		return { };
	}
	bool send_oob(const endpoint &peer, const buffer_t &data) { return send(peer, data).has_value(); }
	bool send_oob_mc(const buffer_t &data)
	{
		for (auto &[id, wire] : instances)
			if (id != id_) wire->incoming_.emplace_back(Address(), data);
		return true;
	}
	bool recv(endpoint &sender, buffer_t &data)
	{
		if (incoming_.empty() && pump) pump(id_);
		if (incoming_.empty()) return false;
		sender = incoming_.front().first;
		data = std::move(incoming_.front().second);
		incoming_.pop_front();
		return true;
	}
	void disconnect(const endpoint &) { }
	bool get_disconnected(endpoint &sender)
	{
		if (disconnected_.empty()) return false;
		sender = disconnected_.front();
		disconnected_.pop_front();
		return true;
	}
	std::expected<bool, PacketError> network_online() { return true; }
	std::expected<bool, PacketError> peers_ready() { return true; }
	bool is_peer_connected(const endpoint &peer) { return streamsConnected && instances.contains(peer.addr[0]); }
	std::optional<bool> is_peer_relayed(const endpoint &) { return false; }
	std::optional<int> get_latency_to(const endpoint &) { return 0; }
	std::string make_default_gamename() { return "eight-peers"; }

private:
	uint8_t id_;
	std::deque<std::pair<endpoint, buffer_t>> incoming_;
	std::deque<endpoint> disconnected_;
};

class PeerTransport : public ::testing::TestWithParam<bool> {
protected:
	using Network = base_protocol<memory_wire>;
	void SetUp() override
	{
		memory_wire::nextId = 1;
		memory_wire::pump = [this](uint8_t caller) {
			if (pumping_) return;
			pumping_ = true;
			for (const auto &[id, net] : endpoints_) {
				if (id != caller) { EXPECT_TRUE(net->poll()); }
			}
			pumping_ = false;
		};
	}
	void TearDown() override
	{
		memory_wire::pump = { };
		memory_wire::observe = { };
		memory_wire::intercept = { };
		memory_wire::streamsConnected = true;
		nets_.clear();
		Players.clear();
	}
	void Start(bool extended)
	{
		GameData data { };
		data.size = sizeof(data);
		data.nTickRate = 20;
		data.rules = extended ? ExtendedSessionRules : ClassicSessionRules;
		SwapGameDataLE(data);
		const auto *bytes = reinterpret_cast<const unsigned char *>(&data);
		info_ = buffer_t(bytes, bytes + sizeof(data));
		ASSERT_EQ(Add(true), 0);
	}
	int Add(bool host = false)
	{
		auto net = std::make_unique<Network>();
		if (GetParam())
			net->setup_password("peer-test");
		else
			net->clear_password();
		net->setup_gameinfo(info_);
		endpoints_[memory_wire::nextId - 1] = net.get();
		nets_.push_back(std::move(net));
		const int id = host ? nets_.back()->create("eight-peers") : nets_.back()->join("eight-peers");
		return id;
	}
	void Pump()
	{
		for (auto &net : nets_)
			if (net) { ASSERT_TRUE(net->poll()); }
	}
	bool Receive(size_t player, uint8_t source, uint8_t value)
	{
		for (unsigned attempt = 0; attempt < 32; ++attempt) {
			Pump();
			uint8_t sender;
			void *data;
			size_t size;
			if (nets_[player]->SNetReceiveMessage(&sender, &data, &size))
				return sender == source && size == 1 && *static_cast<uint8_t *>(data) == value;
		}
		return false;
	}
	std::vector<std::unique_ptr<Network>> nets_;
	buffer_t info_;
	bool pumping_ = false;
	std::map<uint8_t, Network *> endpoints_;
	void Remove(size_t slot)
	{
		std::erase_if(endpoints_, [&](const auto &entry) { return entry.second == nets_[slot].get(); });
		nets_[slot].reset();
	}
};

TEST_P(PeerTransport, EightPeersRouteMessagesTurnsAndEightNames)
{
	Start(true);
	for (int id = 1; id < 8; ++id)
		ASSERT_EQ(Add(), id);
	Pump();
	for (uint8_t source = 0; source < 8; ++source) {
		for (uint8_t destination = 0; destination < 8; ++destination) {
			ASSERT_TRUE(nets_[source]->SNetSendMessage(destination, &source, 1));
			ASSERT_TRUE(Receive(destination, source, source));
		}
		ASSERT_TRUE(nets_[source]->SNetSendMessage(SNPLAYER_OTHERS, &source, 1));
		for (size_t destination = 0; destination < 8; ++destination)
			if (destination != source) {
				ASSERT_TRUE(Receive(destination, source, source));
			}
	}
	for (int tick = 0; tick < 4; ++tick) {
		for (int player = 0; player < 8; ++player) {
			int32_t value = 100 * player + tick;
			ASSERT_TRUE(nets_[player]->SNetSendTurn(reinterpret_cast<char *>(&value), sizeof(value)));
		}
		Pump();
		for (size_t player = 0; player < 8; ++player) {
			std::array<char *, 8> data { };
			std::array<size_t, 8> size { };
			std::array<uint32_t, 8> status { };
			ASSERT_TRUE(nets_[player]->SNetReceiveTurns(data.data(), size.data(), status.data()));
			for (int source = 0; source < 8; ++source) {
				EXPECT_NE(status[source] & PS_TURN_ARRIVED, 0);
				ASSERT_NE(data[source], nullptr);
				int32_t value;
				std::memcpy(&value, data[source], sizeof(value));
				EXPECT_EQ(value, source * 100 + tick);
			}
		}
	}
	Players.resize(8);
	for (size_t player = 0; player < 8; ++player) {
		Players[player].plractive = true;
		std::snprintf(Players[player]._pName, PlayerNameLength, "Player%zu", player);
	}
	ASSERT_TRUE(nets_[7]->send_info_request());
	Pump();
	const auto games = nets_[7]->get_gamelist();
	ASSERT_EQ(games.size(), 1);
	ASSERT_EQ(games[0].players.size(), 8);
	for (size_t player = 0; player < 8; ++player)
		EXPECT_EQ(games[0].players[player], "Player" + std::to_string(player));
	EXPECT_EQ(Add(), -1);
	std::cout << "peer transport: IDs 0-7, directed/broadcast/turns, eight names, ninth rejected\n";
}

TEST_P(PeerTransport, HigherSlotsReplyBeforeProbingTheLowerPeersDuringAdmission)
{
	Start(true);
	for (int id = 1; id < 7; ++id)
		ASSERT_EQ(Add(), id);
	Pump();
	auto factory = GetParam() ? std::make_unique<packet_factory>("peer-test") : std::make_unique<packet_factory>();
	std::array<std::optional<packet_type>, 8> first;
	std::array<unsigned, 8> probes { };
	std::vector<std::pair<plr_t, buffer_t>> delayed;
	memory_wire::streamsConnected = false;
	memory_wire::intercept = [&](const auto &, const buffer_t &bytes) {
		auto packet = factory->make_packet(bytes);
		EXPECT_TRUE(packet.has_value());
		if (!packet) return true;
		const plr_t source = (*packet)->Source();
		if ((*packet)->Type() == PT_ECHO_REQUEST && (*packet)->Destination() == 7 && source > 0 && source < 7) {
			delayed.emplace_back(source, bytes);
			return false; // The lower peer's initial handshake is still in flight.
		}
		return true;
	};
	memory_wire::observe = [&](const buffer_t &bytes) {
		auto packet = factory->make_packet(bytes);
		ASSERT_TRUE(packet.has_value());
		const auto type = (*packet)->Type();
		const auto destination = (*packet)->Destination();
		if ((*packet)->Source() != 7 || destination == 0 || destination >= 7
		    || (type != PT_ECHO_REQUEST && type != PT_ECHO_REPLY))
			return;
		if (!first[destination])
			first[destination] = type;
		if (type == PT_ECHO_REQUEST)
			++probes[destination];
	};
	ASSERT_EQ(Add(), 7);
	ASSERT_EQ(delayed.size(), 6U);
	uint8_t sender;
	void *data;
	size_t size;
	// The ordinary five-second probe path runs before the delayed handshakes arrive.
	nets_[7]->SNetReceiveMessage(&sender, &data, &size);
	for (size_t peer = 1; peer < 7; ++peer)
		EXPECT_FALSE(first[peer]) << "higher slot opened a competing stream to " << peer;
	memory_wire::intercept = { };
	memory_wire::streamsConnected = true;
	for (const auto &[source, bytes] : delayed)
		ASSERT_TRUE(memory_wire::instances.at(source + 1)->send(memory_wire::instances.at(8)->Address(), bytes));
	Pump();
	for (size_t peer = 1; peer < 7; ++peer) {
		ASSERT_TRUE(first[peer]);
		EXPECT_EQ(*first[peer], PT_ECHO_REPLY);
		auto probe = factory->make_packet<PT_ECHO_REQUEST>(plr_t { 7 }, static_cast<plr_t>(peer), timestamp_t { 0 });
		ASSERT_TRUE(probe.has_value());
		ASSERT_TRUE(nets_[7]->send(**probe));
	}
	Pump();
	for (size_t peer = 1; peer < 7; ++peer)
		EXPECT_GT(probes[peer], 0U); // Established streams still carry latency probes.
}

TEST_P(PeerTransport, ClassicRoomRejectsFifthPeer)
{
	Start(false);
	for (int id = 1; id < 4; ++id)
		ASSERT_EQ(Add(), id);
	EXPECT_EQ(Add(), -1);
	uint8_t value = 17;
	ASSERT_TRUE(nets_[3]->SNetSendMessage(0, &value, 1));
	ASSERT_TRUE(Receive(0, 3, value));
}

TEST_P(PeerTransport, LeavingEverySlotAllowsItsReuse)
{
	Start(true);
	for (int id = 1; id < 8; ++id)
		ASSERT_EQ(Add(), id);
	for (size_t slot = 0; slot < 8; ++slot) {
		ASSERT_TRUE(nets_[slot]->SNetLeaveGame(leaveinfo_t::LEAVE_EXIT));
		Pump();
		Remove(slot);
		Pump();
		ASSERT_EQ(Add(), slot);
		nets_[slot] = std::move(nets_.back());
		nets_.pop_back();
		uint8_t value = static_cast<uint8_t>(slot);
		const size_t destination = (slot + 1) % 8;
		ASSERT_TRUE(nets_[slot]->SNetSendMessage(destination, &value, 1));
		ASSERT_TRUE(Receive(destination, slot, value));
		std::cout << "peer reused-slot=" << slot << '\n';
	}
}

TEST_P(PeerTransport, DuplicateJoinKeepsItsExistingSlot)
{
	Start(true);
	ASSERT_EQ(Add(), 1);
	auto factory = GetParam() ? std::make_unique<packet_factory>("peer-test") : std::make_unique<packet_factory>();
	auto request = MakeJoinRequest(*factory, 42, info_);
	ASSERT_TRUE(request.has_value());
	ASSERT_TRUE(memory_wire::instances.at(2)->send(memory_wire::instances.at(1)->Address(), (*request)->Data()));
	Pump();
	ASSERT_EQ(Add(), 2);
}

TEST_P(PeerTransport, CorruptControlIDDoesNotIndexOrRetainItsSlot)
{
	Start(true);
	ASSERT_EQ(Add(), 1);
	auto factory = GetParam() ? std::make_unique<packet_factory>("peer-test") : std::make_unique<packet_factory>();
	auto control = factory->make_packet<PT_CONNECT>(PLR_MASTER, PLR_BROADCAST, plr_t { 255 }, buffer_t(16));
	ASSERT_TRUE(control.has_value());
	ASSERT_TRUE(memory_wire::instances.at(2)->send(memory_wire::instances.at(1)->Address(), (*control)->Data()));
	Pump();
	Remove(1);
	Pump();
	ASSERT_EQ(Add(), 1);
}

INSTANTIATE_TEST_SUITE_P(PlainAndEncrypted, PeerTransport, ::testing::Bool());

} // namespace
} // namespace devilution::net
