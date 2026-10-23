#include "DateTimeWidget.hpp"
#include "core/DebugLog.hpp"
#include <nxui/core/GpuDevice.hpp>
#include <switch.h>
#include "services/ClockService.hpp"
#include <nxui/core/Renderer.hpp>
#include <cstdio>

void DateTimeWidget::setUse12HourClock(bool enabled) {
    if (m_use12HourClock == enabled)
        return;

    m_use12HourClock = enabled;
    m_timeStr.clear();
    m_timer = 1.f;
}

void DateTimeWidget::onContentUpdate(float dt) {
    m_timer += dt;
    if (m_timer < 1.f && !m_timeStr.empty()) return;
    m_timer = 0.f;
    refreshTime();
}

void DateTimeWidget::refreshTime() {
    if (!m_clockService)
        return;
    const auto snapshot = m_clockService->refresh();
    if (!snapshot.valid)
        return;
    const auto& calendar = snapshot.calendar;

    char buf[64];
    if (m_use12HourClock) {
        int hour = calendar.hour % 12;
        if (hour == 0)
            hour = 12;
        std::snprintf(buf, sizeof(buf), "%d:%02d %s", hour, calendar.minute,
                      calendar.hour >= 12 ? "PM" : "AM");
    } else {
        std::snprintf(buf, sizeof(buf), "%02u:%02u",
                      static_cast<unsigned>(calendar.hour),
                      static_cast<unsigned>(calendar.minute));
    }
    m_timeStr = buf;
    std::snprintf(buf, sizeof(buf), "%02u/%02u/%04u",
                  static_cast<unsigned>(calendar.day),
                  static_cast<unsigned>(calendar.month),
                  static_cast<unsigned>(calendar.year));
    m_dateStr = buf;
}

void DateTimeWidget::warmGlyphs(nxui::Renderer& ren) {
    if (!m_font) return;
    nxui::Font* sf = m_smallFont ? m_smallFont : m_font;
    const nxui::Color none{1.f, 1.f, 1.f, 0.f};
    drawGlyphs(ren, m_font, m_bigGlyphs, "0123456789:APM", -1000.f, -1000.f, none, 1.f, true);
    drawGlyphs(ren, sf, m_smallGlyphs, "0123456789/", -1000.f, -1000.f, none, 0.7f, true);
}

void DateTimeWidget::onContentRender(nxui::Renderer& ren) {
    if (!m_font) return;
    if (m_timeStr.empty()) refreshTime(); // not waiting for the next update

    nxui::Rect cr = contentRect();
    nxui::Font* sf = m_smallFont ? m_smallFont : m_font;

    // HomeNX: one letter at a time, from the clock's own textures (see Glyphs).
    const nxui::Color timeColor = m_textColor.withAlpha(m_opacity);
    const nxui::Color dateColor = m_secondaryColor.withAlpha(m_opacity);
    const float timeW = drawGlyphs(ren, m_font, m_bigGlyphs, m_timeStr, 0.f, 0.f, timeColor, 1.f, false);
    const float dateW = drawGlyphs(ren, sf, m_smallGlyphs, m_dateStr, 0.f, 0.f, dateColor, 0.7f, false);
    const float timeH = m_font->measure("0").y, dateH = sf->measure("0").y;
    float contentH = timeH + 2.f + dateH * 0.7f;
    float tx = cr.x + (cr.width - timeW) * 0.5f;
    float ty = cr.y + (cr.height - contentH) * 0.5f;
    drawGlyphs(ren, m_font, m_bigGlyphs, m_timeStr, tx, ty, timeColor, 1.f, true);
    drawGlyphs(ren, sf, m_smallGlyphs, m_dateStr, cr.x + (cr.width - dateW) * 0.5f, ty + timeH + 2.f, dateColor, 0.7f,
               true);
}

// Draws (or only measures) `text` from the clock's own letter textures,
// making any missing one; returns its width.
float DateTimeWidget::drawGlyphs(nxui::Renderer& ren, nxui::Font* font, Glyphs& glyphs, const std::string& text,
                                 float x, float y, const nxui::Color& color, float scale, bool draw) {
    if (glyphs.revision != font->revision()) { // another font (a theme): its own letters
        glyphs.tex.clear();
        glyphs.revision = font->revision();
    }
    const float start = x;
    for (char c : text) {
        const std::string one(1, c);
        const float advance = font->measure(one).x * scale;
        if (draw && c != ' ') {
            auto& tex = glyphs.tex[c];
            if (!tex || !tex->valid()) {
                tex = std::make_unique<nxui::Texture>();
                if (!font->renderTo(ren, one, *tex)) tex.reset(); // no room now: next frame
            }
            if (tex) {
                ren.drawTexture(tex.get(), {x, y, tex->width() * scale, tex->height() * scale}, color);
            } else if (!font->draw(ren, one, {x, y}, color, scale)) { // no room for our own yet: the shared cache's
                const std::uint64_t now = armGetSystemTick();
                if (now - m_lastMissLog > armNsToTicks(10'000'000'000ULL)) { // why the clock is blank, in menu.log
                    m_lastMissLog = now;
                    u64 total = 0, used = 0;
                    svcGetInfo(&total, InfoType_TotalMemorySize, CUR_PROCESS_HANDLE, 0);
                    svcGetInfo(&used, InfoType_UsedMemorySize, CUR_PROCESS_HANDLE, 0);
                    DebugLog::log("[clock] can't draw '%c': gpu %lluK of %lluK, process free %lluK", c,
                                  (unsigned long long)(ren.gpu().imageMemoryUsed() >> 10),
                                  (unsigned long long)(ren.gpu().imageMemoryBudget() >> 10),
                                  (unsigned long long)((total - used) >> 10));
                }
            }
        }
        x += advance;
    }
    return x - start;
}

nxui::Vec2 DateTimeWidget::computeContentSize() const {
    if (!m_font) return {130.f, 46.f};
    nxui::Vec2 timeSz = m_font->measure(m_use12HourClock ? "12:00 PM" : "00:00");
    nxui::Font* sf = m_smallFont ? m_smallFont : m_font;
    nxui::Vec2 dateSz = sf->measure("00/00/0000");
    float w = std::max(timeSz.x, dateSz.x * 0.7f);
    float h = timeSz.y + 2.f + dateSz.y * 0.7f;
    return {w, h};
}
