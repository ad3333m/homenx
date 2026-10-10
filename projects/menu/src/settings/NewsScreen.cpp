#include "NewsScreen.hpp"
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

// Greedy word wrap into at most maxLines lines; the last one is ellipsized.
std::vector<std::string> wrap(nxui::Font* font, const std::string& text, float maxWidth,
                              float scale, int maxLines) {
    std::vector<std::string> lines;
    std::istringstream words(text);
    std::string word, line;
    while (words >> word) {
        std::string candidate = line.empty() ? word : line + " " + word;
        if (line.empty() || font->measure(candidate).x * scale <= maxWidth) {
            line = std::move(candidate);
            continue;
        }
        lines.push_back(line);
        line = word;
        if ((int)lines.size() == maxLines - 1) {
            std::string rest;
            std::getline(words, rest);
            line += rest;
            break;
        }
    }
    if (!line.empty()) lines.push_back(ellipsize(font, line, maxWidth, scale));
    return lines;
}

} // namespace

NewsScreen::NewsScreen()
    : TabbedOverlayScreen(ScreenMode::GameOptions) {
    constexpr float width = 1120.f;
    setRect({(1280.f - width) * 0.5f, 32.f, width, 656.f});
}

void NewsScreen::setArticles(std::vector<Article> articles, std::string emptyMessage) {
    m_articles = std::move(articles);
    m_emptyMessage = std::move(emptyMessage);
    m_tabIndex = 0;
    m_tabs.clear();
    m_cachedTabContentWidgets.clear();
    warmup();
}

void NewsScreen::buildTabs() {
    auto& i18n = nxui::I18n::instance();
    m_tabs.clear();

    auto paragraph = [](const std::string& text) {
        SettingItem item;
        item.label = text;
        item.type = ItemType::Info;
        item.wrapLabel = true;
        return item;
    };

    if (m_articles.empty()) {
        Tab tab;
        tab.name = i18n.tr("sidebar.news", "News");
        tab.items.push_back(paragraph(m_emptyMessage));
        m_tabs.push_back(std::move(tab));
    }

    for (const Article& a : m_articles) {
        Tab tab;
        tab.name = ellipsize(m_font, a.subject.empty() ? a.topic : a.subject, 230.f, 0.9f);

        std::istringstream body(a.body);
        std::string line;
        while (std::getline(body, line)) {
            line.erase(0, line.find_first_not_of(" \t\r"));
            if (!line.empty()) tab.items.push_back(paragraph(line));
        }
        if (!a.footer.empty()) {
            SettingItem section;
            section.type = ItemType::Section;
            tab.items.push_back(std::move(section));
            tab.items.push_back(paragraph(a.footer));
        }
        if (!a.linkUrl.empty()) {
            SettingItem link;
            link.label = i18n.tr("news.open_link", "Open the link in the browser");
            link.buttonLabel = i18n.tr("button.open", "Open");
            link.type = ItemType::Action;
            link.onChange = [this, url = a.linkUrl](SettingItem&) {
                if (m_openLinkCb) m_openLinkCb(url);
            };
            tab.items.push_back(std::move(link));
        }
        if (tab.items.empty()) tab.items.push_back(paragraph(a.subject));
        m_tabs.push_back(std::move(tab));
    }

    m_cachedTabContentWidgets.clear();
    m_cachedTabContentWidgets.resize(m_tabs.size());
    rebuildTabBar();
    rebuildContentItems();
}

void NewsScreen::drawOverlayHeader(nxui::Renderer& ren, const nxui::Rect& panel, float opacity) {
    if (!m_theme || !m_font || !m_smallFont)
        return;

    const float pad = 30.f;
    const float imageH = 150.f;
    float textX = panel.x + pad;

    const Article* a = (m_tabIndex >= 0 && m_tabIndex < (int)m_articles.size())
        ? &m_articles[(size_t)m_tabIndex] : nullptr;

    if (a && a->image && a->image->valid()) {
        const float aspect = a->image->height() > 0
            ? (float)a->image->width() / (float)a->image->height() : 16.f / 9.f;
        const nxui::Rect img{panel.x + pad, panel.y + pad, std::min(imageH * aspect, 300.f), imageH};
        ren.drawTextureRounded(a->image.get(), img, 16.f, nxui::Color::white().withAlpha(opacity));
        textX = img.right() + 26.f;
    }

    const float textW = std::max(0.f, panel.right() - pad - textX);
    const std::string topic = a ? a->topic : nxui::I18n::instance().tr("sidebar.news", "News");
    ren.drawText(ellipsize(m_smallFont, topic, textW, 0.8f), {textX, panel.y + pad + 2.f},
                 m_smallFont, m_theme->textSecondary.withAlpha(opacity), 0.8f);

    if (a) {
        const float scale = 1.f;
        const float lineH = m_font->measure("Ag").y * scale + 4.f;
        float y = panel.y + pad + 34.f;
        for (const std::string& line : wrap(m_font, a->subject, textW, scale, 3)) {
            ren.drawText(line, {textX, y}, m_font, m_theme->textPrimary.withAlpha(opacity), scale);
            y += lineH;
        }
    }

    const float dividerY = panel.y + pad + overlayHeaderHeight() - 14.f;
    ren.drawRoundedRect({panel.x + pad, dividerY, panel.width - pad * 2.f, 1.5f},
                        m_theme->panelBorder.withAlpha(0.25f * opacity), 0.75f);
}
