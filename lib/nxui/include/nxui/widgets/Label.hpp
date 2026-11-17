#pragma once
#include <nxui/widgets/Box.hpp>
#include <nxui/core/I18n.hpp>
#include <cstdint>
#include <string>
#include <vector>

namespace nxui {

class Font;

/// A text label widget. Draws a single line of text.
class Label : public Box {
public:
    Label();
    explicit Label(const std::string& text);
    Label(const std::string& text, Font* font);
    ~Label() override;

    void setText(const std::string& t);
    void setTextKey(const std::string& key, const std::string& fallback = "");
    const std::string& text() const    { return m_text; }

    void setFont(Font* f)    { m_font = f; }
    Font* font() const       { return m_font; }

    void setTextColor(const Color& c) { m_textColor = c; }
    const Color& textColor() const    { return m_textColor; }

    void setScale(float s)   { m_textScale = s; }
    float scale() const      { return m_textScale; }

    /// Enable automatic line wrapping inside the widget content rect.
    /// Disabled by default to preserve previous behavior.
    void setMultiline(bool enabled) { m_multiline = enabled; }
    bool multiline() const          { return m_multiline; }

    /// Additional multiplier for wrapped-line spacing.
    void setLineSpacing(float spacing) { m_lineSpacing = spacing; }
    float lineSpacing() const          { return m_lineSpacing; }

    /// Horizontal alignment within the widget rect.
    enum class HAlign { Left, Center, Right };
    void setHAlign(HAlign a) { m_hAlign = a; }

    /// Vertical alignment within the widget rect.
    enum class VAlign { Top, Center, Bottom };
    void setVAlign(VAlign a) { m_vAlign = a; }

    /// Measure the text and resize the widget to fit.
    void sizeToFit();

    /// Measure text size (requires font to be set).
    Vec2 measureText() const;

    /// Measure wrapped text size for a constrained width.
    Vec2 measureWrappedText(float maxWidth) const;

protected:
    void onRender(Renderer& ren) override;

private:
    void refreshLocalizedText();
    std::vector<std::string> wrappedLines(float maxWidth) const;
    // wrappedLines, kept until the text, width, scale or font changes: a
    // multiline label is drawn every frame and wrapping measures every word.
    struct WrappedLine { std::string text; float width; };
    const std::vector<WrappedLine>& cachedWrappedLines(float maxWidth) const;

    std::string m_text;
    std::string m_textSource;
    std::string m_textKey;
    std::string m_textFallback;
    bool        m_useTextKey = false;
    int         m_i18nListenerId = -1;
    Font*       m_font = nullptr;
    Color       m_textColor = Color::white();
    float       m_textScale = 1.f;
    bool        m_multiline = false;
    float       m_lineSpacing = 1.15f;
    HAlign      m_hAlign = HAlign::Left;
    VAlign      m_vAlign = VAlign::Top;

    mutable std::vector<WrappedLine> m_wrapLines;
    mutable std::string   m_wrapText;
    mutable const Font*   m_wrapFont = nullptr;
    mutable std::uint64_t m_wrapFontRevision = 0;
    mutable float         m_wrapWidth = -1.f;
    mutable float         m_wrapScale = 0.f;
    mutable bool          m_wrapValid = false;
};

} // namespace nxui
