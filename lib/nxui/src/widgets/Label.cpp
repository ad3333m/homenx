#include <nxui/widgets/Label.hpp>
#include <nxui/core/Renderer.hpp>
#include <nxui/core/Font.hpp>
#include <algorithm>

namespace nxui {

Label::Label() {
    m_i18nListenerId = I18n::instance().addLanguageChangedListener([this]() {
        refreshLocalizedText();
    });
}

Label::Label(const std::string& text) : Label() {
    setText(text);
}

Label::Label(const std::string& text, Font* font) : Label(text) {
    m_font = font;
}

Label::~Label() {
    I18n::instance().removeLanguageChangedListener(m_i18nListenerId);
}

void Label::setText(const std::string& t) {
    m_useTextKey = false;
    m_textSource = t;
    refreshLocalizedText();
}

void Label::setTextKey(const std::string& key, const std::string& fallback) {
    m_useTextKey = true;
    m_textKey = key;
    m_textFallback = fallback.empty() ? key : fallback;
    refreshLocalizedText();
}

void Label::refreshLocalizedText() {
    if (m_useTextKey)
        m_text = I18n::instance().tr(m_textKey, m_textFallback);
    else
        m_text = I18n::instance().tr(m_textSource, m_textSource);
}

Vec2 Label::measureText() const {
    if (!m_font || m_text.empty()) return {0, 0};
    Vec2 sz = m_font->measure(m_text);
    return {sz.x * m_textScale, sz.y * m_textScale};
}

std::vector<std::string> Label::wrappedLines(float maxWidth) const {
    std::vector<std::string> lines;
    if (!m_font) return lines;
    if (m_text.empty()) {
        lines.push_back("");
        return lines;
    }

    float width = std::max(1.f, maxWidth);

    auto pushWrappedWord = [&](const std::string& word) {
        if (word.empty()) return;

        std::string chunk;
        for (char ch : word) {
            std::string test = chunk;
            test.push_back(ch);
            float w = m_font->measure(test).x * m_textScale;
            if (!chunk.empty() && w > width) {
                lines.push_back(chunk);
                chunk.clear();
            }
            chunk.push_back(ch);
        }
        if (!chunk.empty())
            lines.push_back(chunk);
    };

    size_t start = 0;
    while (start <= m_text.size()) {
        size_t end = m_text.find('\n', start);
        std::string paragraph = (end == std::string::npos)
            ? m_text.substr(start)
            : m_text.substr(start, end - start);

        if (paragraph.empty()) {
            lines.push_back("");
        } else {
            std::string current;
            size_t i = 0;
            while (i < paragraph.size()) {
                while (i < paragraph.size() && paragraph[i] == ' ') ++i;
                if (i >= paragraph.size()) break;

                size_t wordEnd = i;
                while (wordEnd < paragraph.size() && paragraph[wordEnd] != ' ') ++wordEnd;
                std::string word = paragraph.substr(i, wordEnd - i);

                std::string candidate = current.empty() ? word : current + " " + word;
                float candW = m_font->measure(candidate).x * m_textScale;

                if (candW <= width) {
                    current = std::move(candidate);
                } else {
                    if (!current.empty()) {
                        lines.push_back(current);
                        current.clear();
                    }

                    float wordW = m_font->measure(word).x * m_textScale;
                    if (wordW <= width) {
                        current = std::move(word);
                    } else {
                        pushWrappedWord(word);
                    }
                }

                i = wordEnd;
            }

            if (!current.empty())
                lines.push_back(current);
        }

        if (end == std::string::npos) break;
        start = end + 1;
    }

    if (lines.empty())
        lines.push_back("");
    return lines;
}

const std::vector<Label::WrappedLine>& Label::cachedWrappedLines(float maxWidth) const {
    if (m_wrapValid && m_wrapFont == m_font && m_wrapFontRevision == m_font->revision()
        && m_wrapWidth == maxWidth && m_wrapScale == m_textScale && m_wrapText == m_text)
        return m_wrapLines;

    m_wrapLines.clear();
    for (auto& line : wrappedLines(maxWidth)) {
        const float w = m_font->measure(line).x * m_textScale;
        m_wrapLines.push_back({std::move(line), w});
    }
    m_wrapText = m_text;
    m_wrapFont = m_font;
    m_wrapFontRevision = m_font->revision();
    m_wrapWidth = maxWidth;
    m_wrapScale = m_textScale;
    m_wrapValid = true;
    return m_wrapLines;
}

Vec2 Label::measureWrappedText(float maxWidth) const {
    if (!m_font || m_text.empty()) return {0, 0};

    const auto& lines = cachedWrappedLines(maxWidth);
    float maxLineW = 0.f;
    for (const auto& line : lines)
        maxLineW = std::max(maxLineW, line.width);

    float lineH = m_font->measure("Ag").y * m_textScale;
    float step  = lineH * std::max(0.1f, m_lineSpacing);
    float totalH = lineH;
    if (!lines.empty())
        totalH = lineH + step * (float)(lines.size() - 1);

    return {maxLineW, totalH};
}

void Label::sizeToFit() {
    Vec2 sz = m_multiline
        ? measureWrappedText(std::max(1.f, contentRect().width))
        : measureText();
    m_rect.width  = sz.x + m_padding.horizontal();
    m_rect.height = sz.y + m_padding.vertical();
}

void Label::onRender(Renderer& ren) {
    if (!m_font || m_text.empty()) return;

    Rect cr = contentRect();
    if (!m_multiline) {
        Vec2 sz = m_font->measure(m_text);
        float drawW = sz.x * m_textScale;
        float drawH = sz.y * m_textScale;

        float tx = cr.x;
        switch (m_hAlign) {
            case HAlign::Left:   tx = cr.x; break;
            case HAlign::Center: tx = cr.x + (cr.width - drawW) * 0.5f; break;
            case HAlign::Right:  tx = cr.x + cr.width - drawW; break;
        }

        float ty = cr.y;
        switch (m_vAlign) {
            case VAlign::Top:    ty = cr.y; break;
            case VAlign::Center: ty = cr.y + (cr.height - drawH) * 0.5f; break;
            case VAlign::Bottom: ty = cr.y + cr.height - drawH; break;
        }

        ren.drawText(m_text, {tx, ty}, m_font, m_textColor.withAlpha(m_opacity), m_textScale);
        return;
    }

    const auto& lines = cachedWrappedLines(cr.width);
    float lineH = m_font->measure("Ag").y * m_textScale;
    float step  = lineH * std::max(0.1f, m_lineSpacing);
    float totalH = lineH;
    if (!lines.empty())
        totalH = lineH + step * (float)(lines.size() - 1);

    float ty = cr.y;
    switch (m_vAlign) {
        case VAlign::Top:    ty = cr.y; break;
        case VAlign::Center: ty = cr.y + (cr.height - totalH) * 0.5f; break;
        case VAlign::Bottom: ty = cr.y + cr.height - totalH; break;
    }

    Color color = m_textColor.withAlpha(m_opacity);
    for (const auto& line : lines) {
        float drawW = line.width;
        float tx = cr.x;
        switch (m_hAlign) {
            case HAlign::Left:   tx = cr.x; break;
            case HAlign::Center: tx = cr.x + (cr.width - drawW) * 0.5f; break;
            case HAlign::Right:  tx = cr.x + cr.width - drawW; break;
        }

        ren.drawText(line.text, {tx, ty}, m_font, color, m_textScale);
        ty += step;
    }
}

} // namespace nxui
