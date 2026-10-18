#pragma once

#include <cstring>
#include <span>

#include "dvlnet/packet.h"
#include "game_mode.hpp"
#include "multi.h"

namespace devilution::net {

inline std::expected<GameData, GameDataError> DecodeGameData(std::span<const unsigned char> bytes)
{
	if (bytes.size() != sizeof(GameData))
		return std::unexpected(GameDataError::InvalidSize);
	GameData data;
	std::memcpy(&data, bytes.data(), sizeof(data));
	SwapGameDataLE(data);
	if (auto error = ValidateGameData(data))
		return std::unexpected(*error);
	return data;
}

/** Invalid or absent local setup cannot advertise extended slots. Joins still validate it. */
inline uint8_t SessionCapacity(const buffer_t &info)
{
	const auto data = DecodeGameData(info);
	return data.has_value() && data->isExtended() ? data->rules.maxPlayers : ClassicPlayerLimit;
}

/** Check addresses before any table is indexed, including control packet IDs. */
inline std::expected<void, PacketError> ValidatePacketAddresses(packet &pkt, uint8_t capacity)
{
	if (pkt.Destination() != PLR_BROADCAST && pkt.Destination() >= capacity)
		return std::unexpected("Invalid packet destination");
	if (pkt.Source() >= capacity && pkt.Source() != PLR_MASTER)
		return std::unexpected("Invalid packet source");
	if ((pkt.Type() == PT_MESSAGE || pkt.Type() == PT_TURN || pkt.Type() == PT_ECHO_REQUEST || pkt.Type() == PT_ECHO_REPLY)
	    && pkt.Source() >= capacity)
		return std::unexpected("Invalid gameplay packet source");
	if (pkt.Type() == PT_CONNECT || pkt.Type() == PT_DISCONNECT || pkt.Type() == PT_JOIN_ACCEPT || pkt.Type() == PT_JOIN_ACCEPT_EXTENDED) {
		const auto player = pkt.NewPlayer();
		if (!player.has_value())
			return std::unexpected(player.error());
		if (*player >= capacity)
			return std::unexpected("Invalid player ID in control packet");
	}
	return { };
}

inline bool IsExtendedSession(const buffer_t &info)
{
	const auto data = DecodeGameData(info);
	return data.has_value() && data->isExtended();
}

/** Legacy clients send no identity at all. Permit that only in classic rooms. */
inline std::optional<GameDataError> ValidateJoinRequest(const buffer_t &hostInfo, packet &request)
{
	const auto host = DecodeGameData(hostInfo);
	if (!host.has_value())
		return host.error();
	if (request.Source() != PLR_BROADCAST || request.Destination() != PLR_MASTER
	    || (request.Type() != PT_JOIN_REQUEST && request.Type() != PT_JOIN_REQUEST_EXTENDED))
		return GameDataError::InvalidParameters;
	if (host->isExtended() != (request.Type() == PT_JOIN_REQUEST_EXTENDED))
		return GameDataError::SessionMismatch;
	const auto info = request.Info();
	if (!info.has_value())
		return GameDataError::InvalidSize;
	if ((*info)->empty() && !host->isExtended())
		return std::nullopt;
	const auto client = DecodeGameData(**info);
	if (!client.has_value())
		return client.error();
	return CheckGameCompatibility(*host, *client);
}

inline std::expected<std::unique_ptr<packet>, PacketError> MakeJoinRequest(packet_factory &factory, cookie_t cookie, const buffer_t &info)
{
	const auto data = DecodeGameData(info);
	if (!data.has_value())
		return std::unexpected(GameDataErrorMessage(data.error()));
	if (data->isExtended())
		return factory.make_packet<PT_JOIN_REQUEST_EXTENDED>(PLR_BROADCAST, PLR_MASTER, cookie, info);
	return factory.make_packet<PT_JOIN_REQUEST>(PLR_BROADCAST, PLR_MASTER, cookie, info);
}

inline std::expected<std::unique_ptr<packet>, PacketError> MakeJoinAccept(packet_factory &factory, plr_t source, cookie_t cookie, plr_t player, const buffer_t &info)
{
	if (IsExtendedSession(info))
		return factory.make_packet<PT_JOIN_ACCEPT_EXTENDED>(source, PLR_BROADCAST, cookie, player, info);
	return factory.make_packet<PT_JOIN_ACCEPT>(source, PLR_BROADCAST, cookie, player, info);
}

inline std::expected<std::unique_ptr<packet>, PacketError> MakeJoinReject(packet_factory &factory, plr_t source, packet &request, GameDataError error)
{
	return request.Cookie().and_then([&](cookie_t cookie) {
		return factory.make_packet<PT_JOIN_REJECT>(source, PLR_BROADCAST, cookie, buffer_t { static_cast<unsigned char>(error) });
	});
}

} // namespace devilution::net
