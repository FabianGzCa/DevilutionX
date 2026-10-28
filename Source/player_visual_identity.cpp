#include "player_visual_identity.hpp"

#include "engine/render/clx_render.hpp"
#include "engine/render/primitive_render.hpp"
#include "options.h"

namespace devilution {

const PlayerVisualIdentity &GetConfiguredPlayerVisualIdentity(int playerId)
{
	if (!*GetOptions().Gameplay.showPlayerIdentifiers && playerId >= ClassicPlayerLimit && playerId < PlayerStorageCapacity)
		playerId %= ClassicPlayerLimit;
	return GetPlayerVisualIdentity(playerId);
}

std::string_view GetPlayerNameLabel(int playerId)
{
	if (!*GetOptions().Gameplay.showPlayerIdentifiers)
		return {};
	return GetPlayerVisualIdentity(playerId).nameLabel;
}

void DrawPlayerNumber(const Surface &out, Point center, int playerId)
{
	if (!*GetOptions().Gameplay.showPlayerIdentifiers)
		return;
	const PlayerVisualIdentity &identity = GetPlayerVisualIdentity(playerId);
	constexpr int GlyphWidth = 3;
	constexpr int GlyphHeight = 5;
	constexpr uint8_t BackgroundColor = 239;
	constexpr uint8_t NumberColor = 255;
	FillRect(out, center.x - 2, center.y - 3, GlyphWidth + 2, GlyphHeight + 2, BackgroundColor);
	for (int y = 0; y < GlyphHeight; ++y) {
		for (int x = 0; x < GlyphWidth; ++x) {
			if ((identity.automapNumber[y] & (1 << (GlyphWidth - x - 1))) != 0)
				DrawHorizontalLine(out, { center.x - 1 + x, center.y - 2 + y }, 1, NumberColor);
		}
	}
}

void DrawPlayerTag(const Surface &out, ClxSpriteList tags, Point position, int playerId)
{
	if (playerId < 0 || playerId >= PlayerStorageCapacity)
		playerId = 0;
	const uint32_t tagIndex = static_cast<uint32_t>(playerId % ClassicPlayerLimit + 1);
	if (tagIndex >= tags.numSprites())
		return;
	const ClxSprite tag = tags[tagIndex];
	RenderClxSprite(out, tag, position);
	DrawPlayerNumber(out, position + Displacement { static_cast<int>(tag.width()) / 2, 6 }, playerId);
}

} // namespace devilution
