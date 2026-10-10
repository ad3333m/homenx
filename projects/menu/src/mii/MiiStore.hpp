#pragma once
#include <switch.h>
#include <nxui/core/Types.hpp>
#include <string>
#include <vector>

namespace nxui { class Renderer; }

// The console's Miis (mii service database), plus a simple HomeNX-drawn
// portrait from each Mii's own skin, hair, eye and favourite colours.
std::vector<MiiCharInfo> loadMiis(Result& rc);
std::string miiName(const MiiCharInfo& mii);
std::string miiFavouriteColourName(const MiiCharInfo& mii);
void drawMiiPortrait(nxui::Renderer& ren, const nxui::Rect& r, const MiiCharInfo& mii, float opacity);
