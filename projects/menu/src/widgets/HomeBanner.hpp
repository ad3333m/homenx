#pragma once
#include <nxui/widgets/GlassWidget.hpp>
#include <nxui/core/Animation.hpp>
#include <nxui/core/Font.hpp>
#include <nxui/core/Texture.hpp>
#include <string>

// HomeNX: a glass card with a picture, a title, a line of text, an optional
// button hint and progress line. HOME's Discord alerts and now-playing bar.
class HomeBanner : public nxui::GlassWidget {
public:
    HomeBanner();

    void setFonts(nxui::Font* title, nxui::Font* small, nxui::Font* icons) {
        m_titleFont = title;
        m_smallFont = small;
        m_iconFont = icons;
    }
    void setTextColor(const nxui::Color& c) { m_textColor = c; }
    // Where it sits when shown; it slides in from `fromDy` pixels away.
    void setPlace(const nxui::Rect& shown, float fromDy) {
        m_place = shown;
        m_fromDy = fromDy;
    }

    void setContent(const std::string& title, const std::string& text);
    // Not owned (null = the fallback circle with a letter); round or a rounded square.
    void setPicture(const nxui::Texture* tex, bool round) {
        m_picture = tex;
        m_round = round;
    }
    void setFallback(const nxui::Color& color, const std::string& letter) {
        m_fallbackColor = color;
        m_letter = letter;
    }
    void setHint(const std::string& glyph, const std::string& label) {
        m_hintGlyph = glyph;
        m_hintLabel = label;
    }
    void setHintShown(bool on) { m_hintShown = on; }
    void setProgress(float p) { m_progress = p; } // < 0: no line

    void slideIn();
    void slideOut();
    bool shown() const { return m_in; }

protected:
    void onRender(nxui::Renderer& ren) override;
    void onContentUpdate(float dt) override;
    void onContentRender(nxui::Renderer& ren) override;

private:
    nxui::Font* m_titleFont = nullptr;
    nxui::Font* m_smallFont = nullptr;
    nxui::Font* m_iconFont = nullptr;
    nxui::Color m_textColor{1.f, 1.f, 1.f, 1.f};
    nxui::Color m_fallbackColor{0.345f, 0.396f, 0.949f, 1.f};
    nxui::Rect m_place{};
    float m_fromDy = -80.f;
    std::string m_title, m_text, m_letter, m_hintGlyph, m_hintLabel;
    std::string m_shownTitle, m_shownText; // cut to fit, once per content
    bool m_fitted = false;
    const nxui::Texture* m_picture = nullptr;
    bool m_round = true;
    bool m_hintShown = true;
    float m_progress = -1.f;
    bool m_in = false;
    nxui::AnimatedFloat m_t{0.f};
};
