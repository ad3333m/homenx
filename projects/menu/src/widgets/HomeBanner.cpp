#include "HomeBanner.hpp"
#include <nxui/core/Renderer.hpp>
#include <algorithm>

namespace {

std::string fit(nxui::Font* font, const std::string& text, float maxWidth, float scale) {
    if (!font || font->measure(text).x * scale <= maxWidth) return text;
    std::string out = text.substr(0, 160); // a long message: no need to measure all of it
    while (!out.empty() && (static_cast<unsigned char>(out.back()) & 0xC0u) == 0x80u) out.pop_back();
    if (!out.empty() && (static_cast<unsigned char>(out.back()) & 0x80u)) out.pop_back(); // a cut lead byte
    while (!out.empty() && font->measure(out + "...").x * scale > maxWidth) {
        std::size_t start = out.size() - 1;
        while (start > 0 && (static_cast<unsigned char>(out[start]) & 0xC0u) == 0x80u) --start;
        out.erase(start);
    }
    return out + "...";
}

}

HomeBanner::HomeBanner() {
    setLiquidGlassEnabled(true);
    setForceLiquidGlass(true);
    setBlurEnabled(false);
    setPanelOpacity(0.98f);
    setBorderWidth(0.f);
    setPadding(9.f, 16.f, 9.f, 10.f);
    setVisible(false);
}

void HomeBanner::setContent(const std::string& title, const std::string& text) {
    m_title = title;
    m_text = text;
    m_fitted = false;
}

void HomeBanner::slideIn() {
    if (m_in) return;
    m_in = true;
    setVisible(true);
    m_t.set(1.f, 0.32f, nxui::Easing::outCubic);
}

void HomeBanner::slideOut() {
    if (!m_in) return;
    m_in = false;
    m_t.set(0.f, 0.24f, nxui::Easing::outCubic);
}

void HomeBanner::onRender(nxui::Renderer& ren) {
    const float t = std::clamp(m_t.value(), 0.f, 1.f);
    if (!m_in && t <= 0.01f) {
        setVisible(false);
        return;
    }
    m_rect = m_place;
    m_rect.y += (1.f - t) * m_fromDy;
    setCornerRadius(m_rect.height * 0.5f);
    setOpacity(t);
    nxui::GlassWidget::onRender(ren);
}

void HomeBanner::onContentRender(nxui::Renderer& ren) {
    const nxui::Rect cr = contentRect();
    const float a = m_opacity;
    const float pic = cr.height;
    if (m_picture && m_picture->valid()) {
        nxui::Rect pr{cr.x, cr.y, pic, pic};
        if (!m_round && m_picture->width() > 0 && m_picture->height() > 0) { // fitted, not stretched
            const float aspect = (float)m_picture->width() / (float)m_picture->height();
            if (aspect < 1.f) {
                pr.width = pic * aspect;
                pr.x += (pic - pr.width) * 0.5f;
            } else {
                pr.height = pic / aspect;
                pr.y += (pic - pr.height) * 0.5f;
            }
        }
        ren.drawTextureRounded(m_picture, pr, m_round ? pic * 0.5f : std::min(8.f, std::min(pr.width, pr.height) * 0.2f),
                               nxui::Color::white().withAlpha(a));
    } else {
        ren.drawCircle({cr.x + pic * 0.5f, cr.y + pic * 0.5f}, pic * 0.5f, m_fallbackColor.withAlpha(a));
        if (m_titleFont && !m_letter.empty()) {
            const nxui::Vec2 s = m_titleFont->measure(m_letter);
            ren.drawText(m_letter, {cr.x + (pic - s.x) * 0.5f, cr.y + (pic - s.y) * 0.5f}, m_titleFont,
                         nxui::Color::white().withAlpha(a));
        }
    }

    float right = cr.x + cr.width;
    if (m_hintShown && !m_hintGlyph.empty() && m_iconFont && m_smallFont) {
        const nxui::Vec2 ls = m_smallFont->measure(m_hintLabel);
        const nxui::Vec2 gs = m_iconFont->measure(m_hintGlyph);
        const float x = right - ls.x;
        ren.drawText(m_hintLabel, {x, cr.y + (cr.height - ls.y) * 0.5f}, m_smallFont, m_textColor.withAlpha(0.8f * a));
        ren.drawText(m_hintGlyph, {x - gs.x - 4.f, cr.y + (cr.height - gs.y) * 0.5f}, m_iconFont,
                     m_textColor.withAlpha(0.9f * a));
        right = x - gs.x - 16.f;
    }

    const float tx = cr.x + pic + 12.f;
    const float tw = std::max(0.f, right - tx);
    const float lineH = m_progress >= 0.f ? cr.height - 6.f : cr.height;
    if (!m_fitted) {
        m_fitted = true;
        m_shownTitle = fit(m_titleFont, m_title, tw, 0.82f);
        m_shownText = fit(m_smallFont, m_text, tw, 0.9f);
    }
    if (m_titleFont)
        ren.drawText(m_shownTitle, {tx, cr.y + 1.f}, m_titleFont, m_textColor.withAlpha(a), 0.82f);
    if (m_smallFont)
        ren.drawText(m_shownText, {tx, cr.y + lineH * 0.5f + 1.f}, m_smallFont, m_textColor.withAlpha(0.72f * a), 0.9f);
    if (m_progress >= 0.f) {
        const nxui::Rect track{tx, cr.y + cr.height - 3.f, tw, 3.f};
        ren.drawRoundedRect(track, m_textColor.withAlpha(0.18f * a), 1.5f);
        ren.drawRoundedRect({track.x, track.y, track.width * std::clamp(m_progress, 0.f, 1.f), track.height},
                            m_textColor.withAlpha(0.85f * a), 1.5f);
    }
}
