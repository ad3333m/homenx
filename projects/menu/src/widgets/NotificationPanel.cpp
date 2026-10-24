#include "NotificationPanel.hpp"

#include <nxui/core/Animation.hpp>
#include <nxui/core/I18n.hpp>
#include <nxui/core/Input.hpp>
#include <nxui/core/Renderer.hpp>

#include <algorithm>
#include <ctime>

namespace {

constexpr float kCardX = 24.f, kCardY = 84.f, kCardW = 560.f;
constexpr float kHeaderH = 62.f, kRowH = 78.f, kFooterH = 44.f;
constexpr int kMaxRows = 6;
constexpr float kAnimDur = 0.16f;

// Discord's emoji marks (U+F0000 and up: 4-byte UTF-8 from 0xF3) aren't letters here.
std::string plain(const std::string& s) {
    std::string out;
    for (std::size_t i = 0; i < s.size(); ++i) {
        if (static_cast<unsigned char>(s[i]) >= 0xF3) {
            i += 3;
            continue;
        }
        out += s[i];
    }
    return out;
}

// As much of `s` as fits in `maxW` at `scale`, cut between letters.
std::string fit(nxui::Font* font, std::string s, float maxW, float scale) {
    if (!font || font->measure(s).x * scale <= maxW) return s;
    while (!s.empty() && font->measure(s + "...").x * scale > maxW) {
        std::size_t cut = s.size() - 1;
        while (cut > 0 && (static_cast<unsigned char>(s[cut]) & 0xC0) == 0x80) --cut;
        s.erase(cut);
    }
    return s + "...";
}

std::string ago(std::uint64_t at) {
    auto& i18n = nxui::I18n::instance();
    const std::uint64_t now = static_cast<std::uint64_t>(std::time(nullptr));
    const std::uint64_t s = now > at ? now - at : 0;
    if (s < 60) return i18n.tr("notifications.now", "now");
    if (s < 3600) return std::to_string(s / 60) + i18n.tr("notifications.m", "m");
    if (s < 86400) return std::to_string(s / 3600) + i18n.tr("notifications.h", "h");
    return std::to_string(s / 86400) + i18n.tr("notifications.d", "d");
}

bool isDiscord(const notifications::Item& it) { return it.kind.rfind("discord", 0) == 0; }

} // namespace

NotificationPanel::NotificationPanel() {
    setRect({0.f, 0.f, 1280.f, 720.f});
    setVisible(false);
}

void NotificationPanel::show() {
    m_seenBefore = notifications::seenAt();
    m_items = notifications::recent(50);
    notifications::markSeen();
    m_sel = 0;
    m_top = 0;
    m_active = true;
    setVisible(true);
    setFocusable(true);
    clearActions();
    addDirectionAction(nxui::FocusDirection::UP, [this]() { move(-1); });
    addDirectionAction(nxui::FocusDirection::DOWN, [this]() { move(+1); });
    addDirectionAction(nxui::FocusDirection::LEFT, [] {});
    addDirectionAction(nxui::FocusDirection::RIGHT, [] {});
    addAction(static_cast<uint64_t>(nxui::Button::A), [this]() { openSelected(); });
    addAction(static_cast<uint64_t>(nxui::Button::X), [this]() { clearAll(); });
    addAction(static_cast<uint64_t>(nxui::Button::B), [this]() { hide(); });
}

void NotificationPanel::hide() {
    if (!m_active) return;
    m_active = false;
    clearActions();
    setFocusable(false); // a full-screen focus target left behind turns the screen blue
    if (m_cb.onClosed) m_cb.onClosed();
}

int NotificationPanel::visibleRows() const {
    return std::clamp(static_cast<int>(m_items.size()), 1, kMaxRows);
}

nxui::Rect NotificationPanel::cardRect() const {
    const float h = kHeaderH + kRowH * visibleRows() + kFooterH;
    const float a = nxui::Easing::outCubic(m_anim);
    return {kCardX, kCardY - 14.f * (1.f - a), kCardW, h};
}

void NotificationPanel::move(int delta) {
    if (m_items.empty()) return;
    const int next = std::clamp(m_sel + delta, 0, static_cast<int>(m_items.size()) - 1);
    if (next == m_sel) return;
    m_sel = next;
    if (m_sel < m_top) m_top = m_sel;
    if (m_sel >= m_top + kMaxRows) m_top = m_sel - kMaxRows + 1;
    if (m_cb.onNavigate) m_cb.onNavigate();
}

void NotificationPanel::openSelected() {
    if (m_sel < 0 || m_sel >= static_cast<int>(m_items.size())) return;
    const notifications::Item item = m_items[(std::size_t)m_sel];
    hide();
    if (m_cb.onOpen) m_cb.onOpen(item);
}

void NotificationPanel::clearAll() {
    if (m_items.empty()) return;
    notifications::clear();
    m_items.clear();
    m_sel = m_top = 0;
    if (m_cb.onCleared) m_cb.onCleared();
}

void NotificationPanel::handleTouch(const nxui::Input& in) {
    if (!m_active || !in.touchUp()) return;
    const float x = in.touchX(), y = in.touchY();
    const nxui::Rect card = cardRect();
    if (!card.contains(x, y)) { // a tap beside it closes it
        hide();
        return;
    }
    const int row = static_cast<int>((y - card.y - kHeaderH) / kRowH);
    if (y < card.y + kHeaderH || row < 0 || m_top + row >= static_cast<int>(m_items.size())) return;
    m_sel = m_top + row;
    openSelected();
}

void NotificationPanel::onUpdate(float dt) {
    const float target = m_active ? 1.f : 0.f;
    if (m_anim != target) {
        const float step = dt / kAnimDur;
        m_anim = m_anim < target ? std::min(target, m_anim + step) : std::max(target, m_anim - step);
        if (m_anim <= 0.f && !m_active) setVisible(false);
    }
}

void NotificationPanel::onRender(nxui::Renderer& ren) {
    if (m_anim <= 0.001f || !m_font) return;
    auto& i18n = nxui::I18n::instance();
    nxui::Font* small = m_small ? m_small : m_font;
    const float a = nxui::Easing::outCubic(m_anim);
    const nxui::Color text = nxui::Color(0.95f, 0.96f, 0.98f, a), muted = nxui::Color(0.65f, 0.68f, 0.74f, a);
    const nxui::Color accent = m_theme ? m_theme->cursorNormal.withAlpha(a) : nxui::Color(0.35f, 0.6f, 1.f, a);

    ren.drawRect({0.f, 0.f, 1280.f, 720.f}, nxui::Color(0.f, 0.f, 0.f, 0.35f * a));
    const nxui::Rect card = cardRect();
    ren.drawRoundedRect(card, nxui::Color(0.10f, 0.11f, 0.15f, 0.97f * a), 26.f);
    ren.drawRoundedRectOutline(card, nxui::Color(1.f, 1.f, 1.f, 0.10f * a), 26.f, 1.5f);

    // Header: the title and how many.
    ren.drawText(i18n.tr("notifications.title", "Notifications"), {card.x + 24.f, card.y + 16.f}, m_font, text);
    if (!m_items.empty()) {
        const std::string count = std::to_string(m_items.size());
        const float cw = small->measure(count).x * 0.8f;
        ren.drawRoundedRect({card.x + card.width - 40.f - cw, card.y + 20.f, cw + 20.f, 26.f},
                            nxui::Color(1.f, 1.f, 1.f, 0.10f * a), 13.f);
        ren.drawText(count, {card.x + card.width - 30.f - cw, card.y + 22.f}, small, muted, 0.8f);
    }
    ren.drawRect({card.x + 16.f, card.y + kHeaderH - 1.f, card.width - 32.f, 1.f}, nxui::Color(1.f, 1.f, 1.f, 0.08f * a));

    if (m_items.empty()) {
        const float cy = card.y + kHeaderH + kRowH * 0.5f;
        ren.drawText(i18n.tr("notifications.caught_up", "You're all caught up"), {card.x + 24.f, cy - 26.f}, m_font,
                     text, 0.85f);
        ren.drawText(fit(small, i18n.tr("notifications.empty_hint",
                                        "Discord messages and low controller batteries show up here."),
                         card.width - 48.f, 0.8f),
                     {card.x + 24.f, cy + 4.f}, small, muted, 0.8f);
    }

    const int end = std::min(static_cast<int>(m_items.size()), m_top + kMaxRows);
    for (int i = m_top; i < end; ++i) {
        const notifications::Item& it = m_items[(std::size_t)i];
        const nxui::Rect row{card.x + 10.f, card.y + kHeaderH + kRowH * (i - m_top) + 4.f, card.width - 20.f, kRowH - 8.f};
        if (i == m_sel && m_active) {
            ren.drawRoundedRect(row, accent.withAlpha(0.16f * a), 16.f);
            ren.drawRoundedRectOutline(row, accent, 16.f, 2.f);
        }
        // Its picture: Discord's blurple with the sender's letter, a red "!" for a battery.
        const bool discord = isDiscord(it), battery = it.kind == "battery";
        const nxui::Vec2 c{row.x + 38.f, row.y + row.height * 0.5f};
        ren.drawCircle(c, 22.f, discord ? nxui::Color(0.345f, 0.396f, 0.949f, a)
                               : battery ? nxui::Color(0.93f, 0.26f, 0.27f, a)
                                         : nxui::Color(0.4f, 0.42f, 0.48f, a), 28);
        const std::string title = plain(it.title);
        std::string letter = battery ? "!" : title.empty() ? "?" : title.substr(0, 1);
        if (!letter.empty() && static_cast<unsigned char>(letter[0]) >= 0x80) letter = "?";
        if (!letter.empty() && letter[0] >= 'a' && letter[0] <= 'z') letter[0] = static_cast<char>(letter[0] - 32);
        const nxui::Vec2 ls = m_font->measure(letter);
        ren.drawText(letter, {c.x - ls.x * 0.4f, c.y - ls.y * 0.4f}, m_font, nxui::Color(1.f, 1.f, 1.f, a), 0.8f);
        if (it.at > m_seenBefore) ren.drawCircle({row.x + 8.f, c.y}, 4.5f, accent, 12); // new

        const float tx = row.x + 72.f;
        const std::string when = ago(it.at);
        const float ww = small->measure(when).x * 0.75f;
        ren.drawText(when, {row.x + row.width - 16.f - ww, row.y + 13.f}, small, muted, 0.75f);
        ren.drawText(fit(m_font, title, row.x + row.width - 28.f - ww - tx, 0.8f), {tx, row.y + 8.f}, m_font, text,
                     0.8f);
        ren.drawText(fit(small, plain(it.text), row.x + row.width - 16.f - tx, 0.85f), {tx, row.y + 40.f}, small, muted, 0.85f);
    }
    if (static_cast<int>(m_items.size()) > kMaxRows) { // where in the list
        const float trackH = kRowH * kMaxRows - 16.f;
        const float h = trackH * kMaxRows / m_items.size();
        const float y = card.y + kHeaderH + 8.f + (trackH - h) * m_top / std::max(1, (int)m_items.size() - kMaxRows);
        ren.drawRoundedRect({card.x + card.width - 6.f, y, 3.f, h}, nxui::Color(1.f, 1.f, 1.f, 0.25f * a), 1.5f);
    }

    // Footer: the buttons.
    const std::string hints = m_items.empty()
        ? i18n.tr("notifications.hint_empty", "B  Close")
        : i18n.tr("notifications.hint", "A  Open      X  Clear all      B  Close");
    ren.drawText(hints, {card.x + 24.f, card.y + card.height - kFooterH + 10.f}, small, muted, 0.8f);
}
