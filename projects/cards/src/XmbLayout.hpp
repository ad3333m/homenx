#pragma once
#include <nxui/core/Types.hpp>
#include <algorithm>
#include <cmath>

namespace cards::xmb {
// Shared by rendering, launch transitions, touch hit testing and navigation.
inline constexpr float axisX = 320.f, categoryY = 205.f;
inline constexpr float categoryPitch = 158.f, itemY = 365.f, itemPitch = 92.f;
inline constexpr float listTop = 275.f, listBottom = 628.f;

inline int step(int selected, int delta, int count) {
    return count > 0 ? std::clamp(selected + delta, 0, count - 1) : 0;
}

inline nxui::Rect itemRect(int index, float scroll) {
    const float rel = static_cast<float>(index) - scroll;
    const float chosen = std::max(0.f, 1.f - std::abs(rel));
    const float size = 44.f + 32.f * chosen;
    return {axisX - size * .5f, itemY + rel * itemPitch - size * .5f, size, size};
}

inline nxui::Rect itemHit(int index, float scroll) {
    const auto r = itemRect(index, scroll);
    return {axisX - 48.f, r.center().y - 43.f, 830.f, 86.f};
}

inline float categoryX(int visibleIndex, int selectedVisibleIndex) {
    return axisX + static_cast<float>(visibleIndex - selectedVisibleIndex) * categoryPitch;
}
} // namespace cards::xmb
