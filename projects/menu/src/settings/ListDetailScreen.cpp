#include "ListDetailScreen.hpp"
#include <nxui/core/I18n.hpp>
#include <nxui/core/Renderer.hpp>
#include <algorithm>
#include <sstream>

namespace {

// Drop whole UTF-8 characters until the text (plus "...") fits.
std::string ellipsize(nxui::Font* font, std::string text, float maxWidth, float scale) {
    if (!font || font->measure(text).x * scale <= maxWidth)
        return text;
    while (!text.empty()) {
        // Pop continuation bytes until the character's lead byte is gone too.
        while (!text.empty()) {
            const unsigned char c = static_cast<unsigned char>(text.back());
            text.pop_back();
            if ((c & 0xC0) != 0x80) break;
        }
        if (font->measure(text + "...").x * scale <= maxWidth)
            return text + "...";
    }
    return "...";
}

// Greedy word wrap. Words wider than the line are ellipsized on their own line.
std::vector<std::string> wrap(nxui::Font* font, const std::string& text, float maxWidth, float scale) {
    std::vector<std::string> lines;
    std::istringstream words(text);
    std::string word, line;
    while (words >> word) {
        std::string candidate = line.empty() ? word : line + " " + word;
        if (font->measure(candidate).x * scale <= maxWidth) {
            line = std::move(candidate);
            continue;
        }
        if (!line.empty()) lines.push_back(line);
        line = ellipsize(font, word, maxWidth, scale);
    }
    if (!line.empty()) lines.push_back(line);
    return lines;
}

constexpr float kPad = 28.f;
constexpr float kScrollStep = 120.f;

} // namespace

ListDetailScreen::ListDetailScreen()
    : TabbedOverlayScreen(ScreenMode::GameOptions) {
    constexpr float width = 1160.f;
    setRect({(1280.f - width) * 0.5f, 32.f, width, 656.f});
}

void ListDetailScreen::setEntries(std::vector<Entry> entries, std::string emptyTitle,
                                  std::string emptyMessage) {
    m_entries = std::move(entries);
    m_empty = Entry{};
    m_empty.listName = emptyTitle;
    m_empty.title = std::move(emptyTitle);
    m_empty.body = std::move(emptyMessage);
    m_tabIndex = 0;
    m_layoutTab = -1;
    m_tabs.clear();
    m_cachedTabContentWidgets.clear();
    warmup();
}

const ListDetailScreen::Entry* ListDetailScreen::currentEntry() const {
    if (m_entries.empty()) return &m_empty;
    if (m_tabIndex < 0 || m_tabIndex >= (int)m_entries.size()) return nullptr;
    return &m_entries[(size_t)m_tabIndex];
}

void ListDetailScreen::buildTabs() {
    m_tabs.clear();
    const auto& list = m_entries.empty() ? std::vector<Entry>{m_empty} : m_entries;
    for (const Entry& e : list) {
        Tab tab;
        tab.name = ellipsize(m_font, e.listName.empty() ? e.title : e.listName, 232.f, 0.9f); // room for the unread dot
        m_tabs.push_back(std::move(tab));
    }
    m_cachedTabContentWidgets.clear();
    m_cachedTabContentWidgets.resize(m_tabs.size());
    rebuildTabBar();
    rebuildContentItems();
}

void ListDetailScreen::layout(float width) {
    m_lines.clear();
    m_imageRect = {};
    if (m_layoutTab != m_tabIndex && m_tabIndex >= 0 && m_tabIndex < (int)m_entries.size()) {
        Entry& shown = m_entries[(size_t)m_tabIndex];
        if (shown.unread) {
            shown.unread = false;
            if (shown.onShown) shown.onShown();
        }
    }
    m_layoutTab = m_tabIndex;
    m_layoutWidth = width;
    const Entry* e = currentEntry();
    if (!e || !m_font || !m_smallFont) return;

    const float inner = std::max(1.f, width - kPad * 2.f);
    float y = kPad;
    auto add = [&](const std::string& text, nxui::Font* font, float scale, int tone) {
        const float lineH = font->measure("Ag").y * scale + 5.f;
        for (std::string& l : wrap(font, text, inner, scale)) {
            m_lines.push_back({std::move(l), font, scale, y, tone});
            y += lineH;
        }
    };

    if (e->drawPortrait) {
        m_imageRect = {kPad, y, 170.f, 170.f};
        y += m_imageRect.height + 22.f;
    } else if (e->image && e->image->valid()) {
        if (e->roundImage) {
            m_imageRect = {kPad, y, 150.f, 150.f};
        } else {
            const float aspect = e->image->height() > 0
                ? (float)e->image->width() / (float)e->image->height() : 16.f / 9.f;
            const float h = std::min(inner / aspect, 320.f);
            const float w = h * aspect;
            m_imageRect = {kPad + (inner - w) * 0.5f, y, w, h};
        }
        y += m_imageRect.height + 22.f;
    }
    if (!e->kicker.empty()) { add(e->kicker, m_smallFont, 0.82f, 1); y += 4.f; }
    if (!e->title.empty()) { add(e->title, m_font, 1.15f, 0); y += 12.f; }

    std::istringstream body(e->body);
    std::string para;
    while (std::getline(body, para)) {
        para.erase(0, para.find_first_not_of(" \t\r"));
        if (para.empty()) continue;
        add(para, m_font, 0.86f, 0);
        y += 12.f;
    }
    if (!e->footer.empty()) {
        y += 6.f;
        std::istringstream footer(e->footer);
        for (std::string line; std::getline(footer, line);)
            if (!line.empty()) add(line, m_smallFont, 0.8f, 1);
    }
    if (e->onActivate && !e->actionHint.empty()) {
        y += 10.f;
        add(e->actionHint, m_smallFont, 0.86f, 2);
    }
    m_contentHeight = y + kPad;
}

void ListDetailScreen::scrollBy(float dy) {
    const float maxScroll = std::max(0.f, m_contentHeight - m_viewHeight);
    m_scrollGoal = std::clamp(m_scrollGoal + dy, 0.f, maxScroll);
}

void ListDetailScreen::onContentUpdate(float dt) {
    TabbedOverlayScreen::onContentUpdate(dt);
    if (m_tabIndex >= 0 && m_tabIndex < (int)m_entries.size()) {
        Entry& e = m_entries[(size_t)m_tabIndex];
        if (!e.image && e.lazyImage) {
            auto load = std::move(e.lazyImage);
            e.lazyImage = nullptr;
            e.image = load();
            if (e.image) m_layoutTab = -2; // lay out again with the picture
        }
    }
    m_scroll += (m_scrollGoal - m_scroll) * std::min(1.f, dt * 14.f);
}

// Green dot at the end of unread rows (the tab rows are laid out by drawTabs).
void ListDetailScreen::onContentRender(nxui::Renderer& ren) {
    TabbedOverlayScreen::onContentRender(ren);
    if (!m_tabBar || m_entries.empty()) return;
    const float op = visibilityProgress();
    ren.pushClipRect(tabsRect(panelRect(scale())));
    const auto& rows = m_tabBar->children();
    for (std::size_t i = 0; i < rows.size() && i < m_entries.size(); ++i) {
        if (!m_entries[i].unread) continue;
        const nxui::Rect r = rows[i]->rect();
        ren.drawCircle({r.x + r.width - 18.f, r.y + r.height * 0.5f}, 6.5f,
                       nxui::Color(0x34C759FF).withAlpha(op), 24);
    }
    ren.popClipRect();
}

void ListDetailScreen::drawCustomContent(nxui::Renderer& ren, const nxui::Rect&,
                                         const nxui::Rect& cr, float opacity) {
    if (!m_theme) return;
    if (m_layoutTab != m_tabIndex || m_layoutWidth != cr.width) {
        if (m_layoutTab != m_tabIndex) m_scroll = m_scrollGoal = 0.f;
        layout(cr.width);
    }
    m_viewHeight = cr.height;

    const float top = cr.y - m_scroll;
    const Entry* e = currentEntry();
    if (e && m_imageRect.width > 0.f) {
        nxui::Rect r{cr.x + m_imageRect.x, top + m_imageRect.y, m_imageRect.width, m_imageRect.height};
        if (e->drawPortrait)
            e->drawPortrait(ren, r, opacity);
        else
            ren.drawTextureRounded(e->image.get(), r, e->roundImage ? r.width * 0.5f : 18.f,
                                   nxui::Color::white().withAlpha(opacity));
    }
    for (const Line& l : m_lines) {
        const float y = top + l.y;
        if (y > cr.y + cr.height || y < cr.y - 80.f) continue;
        const nxui::Color c = l.tone == 2 ? m_theme->cursorNormal
                            : l.tone == 1 ? m_theme->textSecondary : m_theme->textPrimary;
        ren.drawText(l.text, {cr.x + kPad, y}, l.font, c.withAlpha(opacity), l.scale);
    }

    if (m_contentHeight > cr.height) {
        const float trackH = cr.height - 32.f;
        const float thumbH = std::max(40.f, trackH * cr.height / m_contentHeight);
        const float t = std::clamp(m_scroll / (m_contentHeight - cr.height), 0.f, 1.f);
        ren.drawRoundedRect({cr.x + cr.width - 10.f, cr.y + 16.f + (trackH - thumbH) * t, 4.f, thumbH},
                            m_theme->textSecondary.withAlpha(0.45f * opacity), 2.f);
    }
    if (m_focusArea == FocusArea::Content)
        ren.drawRoundedRectOutline({cr.x + 3.f, cr.y + 3.f, cr.width - 6.f, cr.height - 6.f},
                                   m_theme->cursorNormal.withAlpha(0.85f * opacity), 24.f, 2.5f);
}

bool ListDetailScreen::handleCustomPressA() {
    const Entry* e = currentEntry();
    if (e && e->onActivate) {
        if (m_activateSfxCb) m_activateSfxCb();
        auto run = e->onActivate; // may replace the entries
        run();
    }
    return true;
}

// The preview isn't selectable: left/right do nothing, up/down stay in the list.
bool ListDetailScreen::handleCustomNavRight() { return true; }
bool ListDetailScreen::handleCustomNavLeft() { return true; }
bool ListDetailScreen::handleCustomNavUp() { return false; }
bool ListDetailScreen::handleCustomNavDown() { return false; }
