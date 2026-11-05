#pragma once

#include <nxui/core/Renderer.hpp>
#include <algorithm>

namespace switchu::battery_drawing {

inline void drawBattery(nxui::Renderer& ren, const nxui::Rect& rect, int percent, bool charging, float alpha = 1.f) {
    const nxui::Color ink(1.f, 1.f, 1.f, 0.9f * alpha);
    ren.drawRoundedRectOutline(rect, ink, 4.f, 2.f);
    ren.drawRoundedRect({rect.x + rect.width + 2.f, rect.y + rect.height * 0.3f, 3.f, rect.height * 0.4f}, ink, 1.5f);
    if (percent < 0) return;
    const float fraction = std::max(0.f, std::min(1.f, static_cast<float>(percent) / 100.f));
    const auto fill = charging ? nxui::Color(0.20f, 0.78f, 0.35f, alpha)
                    : percent <= 20 ? nxui::Color(1.f, 0.23f, 0.19f, alpha) : ink;
    if (fraction > 0.f) ren.drawRoundedRect({rect.x + 4.f, rect.y + 4.f, (rect.width - 8.f) * fraction, rect.height - 8.f}, fill, 2.f);
}

inline nxui::Vec2 boltPoint(const nxui::Rect& r, float x, float y) {
    return {r.x + (x / 16.f) * r.width, r.y + (y / 16.f) * r.height};
}

inline void drawLightningBolt(nxui::Renderer& ren, const nxui::Rect& r,
                              const nxui::Color& fill,
                              const nxui::Color& edge,
                              float outlineThickness) {
    const nxui::Vec2 p0 = boltPoint(r, 4.732f, 7.95335f);
    const nxui::Vec2 p1 = boltPoint(r, 6.90908f, 2.f);
    const nxui::Vec2 p2 = boltPoint(r, 10.54547f, 2.f);
    const nxui::Vec2 p3 = boltPoint(r, 8.36364f, 7.01316f);
    const nxui::Vec2 p4 = boltPoint(r, 11.27275f, 7.01316f);
    const nxui::Vec2 p5 = boltPoint(r, 4.72725f, 14.f);
    const nxui::Vec2 p6 = boltPoint(r, 6.93656f, 7.95135f);

    ren.drawTriangle(p0, p1, p2, fill);
    ren.drawTriangle(p0, p2, p3, fill);
    ren.drawTriangle(p0, p3, p6, fill);
    ren.drawTriangle(p6, p3, p4, fill);
    ren.drawTriangle(p6, p4, p5, fill);

    if (outlineThickness <= 0.f) return;
    ren.drawLine(p0, p1, edge, outlineThickness);
    ren.drawLine(p1, p2, edge, outlineThickness);
    ren.drawLine(p2, p3, edge, outlineThickness);
    ren.drawLine(p3, p4, edge, outlineThickness);
    ren.drawLine(p4, p5, edge, outlineThickness);
    ren.drawLine(p5, p6, edge, outlineThickness);
    ren.drawLine(p6, p0, edge, outlineThickness);
}

} // namespace switchu::battery_drawing
