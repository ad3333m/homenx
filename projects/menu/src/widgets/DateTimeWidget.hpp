#pragma once
#include <nxui/widgets/GlassWidget.hpp>
#include <nxui/core/Font.hpp>
#include <nxui/core/Texture.hpp>
#include <nxui/core/Types.hpp>
#include <map>
#include <memory>
#include <string>

namespace switchu::services {
class ClockService;
}

class DateTimeWidget : public nxui::GlassWidget {
public:
    DateTimeWidget() = default;
    void setFont(nxui::Font* f) { m_font = f; }
    void setSmallFont(nxui::Font* sf) { m_smallFont = sf; }
    void setTextColor(const nxui::Color& c) { m_textColor = c; }
    void setSecondaryTextColor(const nxui::Color& c) { m_secondaryColor = c; }
    void setClockService(switchu::services::ClockService* service) { m_clockService = service; }
    void setUse12HourClock(bool enabled);
    // HomeNX: makes the clock's letters now, while HOME's GPU memory is still
    // free (after a game it fills up and the clock's letters couldn't be made).
    void warmGlyphs(nxui::Renderer& ren);

protected:
    void onContentUpdate(float dt) override;
    void onContentRender(nxui::Renderer& ren) override;
    nxui::Vec2 computeContentSize() const override;

private:
    nxui::Font* m_font = nullptr;
    nxui::Font* m_smallFont = nullptr;
    float m_timer = 0.f;
    std::string m_timeStr;
    std::string m_dateStr;
    bool m_use12HourClock = false;
    switchu::services::ClockService* m_clockService = nullptr;
    nxui::Color m_textColor      {1.f, 1.f, 1.f, 1.f};
    nxui::Color m_secondaryColor {0.7f, 0.7f, 0.8f, 0.8f};
    // HomeNX: the clock's own letters, one texture each, outside the fonts'
    // caches: HOME short of GPU memory (after a game) trims those caches to
    // almost nothing and the clock vanished. Made again if the font changes.
    struct Glyphs {
        std::uint64_t revision = ~0ull;
        std::map<char, std::unique_ptr<nxui::Texture>> tex;
    };
    Glyphs m_bigGlyphs, m_smallGlyphs;
    float drawGlyphs(nxui::Renderer& ren, nxui::Font* font, Glyphs& glyphs, const std::string& text, float x, float y,
                     const nxui::Color& color, float scale, bool draw);
    void refreshTime();
    std::uint64_t m_lastMissLog = 0;
};
