#pragma once

#include <expected>
#include <string>

#include "engine/clx_sprite.hpp"
#include "engine/surface.hpp"
#include "utils/attributes.h"

namespace devilution {

extern DVL_API_FOR_TEST bool PartySidePanelOpen;
extern DVL_API_FOR_TEST bool InspectingFromPartyPanel;
extern DVL_API_FOR_TEST int PortraitIdUnderCursor;

std::expected<void, std::string> LoadPartyPanel();
void FreePartyPanel();
void DrawPartyMemberInfoPanel(const Surface &out);
bool DidRightClickPartyPortrait();

} // namespace devilution
