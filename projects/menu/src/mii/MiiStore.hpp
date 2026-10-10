#pragma once
#include <switch.h>
#include <nxui/core/Types.hpp>
#include <cstdint>
#include <string>
#include <vector>

namespace nxui { class Renderer; }

// The console's Miis (mii service database), plus a simple HomeNX-drawn
// portrait from each Mii's own skin, hair, eye and favourite colours.
std::vector<MiiCharInfo> loadMiis(Result& rc);
std::string miiName(const MiiCharInfo& mii);
std::string miiFavouriteColourName(const MiiCharInfo& mii);
// The Mii's real face as a PNG, rendered by the Mii Studio service from the
// Mii's own data (Nintendo's server, then a community mirror) and cached on
// the SD card. Empty when offline and not cached yet.
std::vector<std::uint8_t> miiFaceImage(const MiiCharInfo& mii);
// Fallback when there's no face image: a simple drawing in the Mii's colours.
void drawMiiPortrait(nxui::Renderer& ren, const nxui::Rect& r, const MiiCharInfo& mii, float opacity);
