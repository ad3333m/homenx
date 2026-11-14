#pragma once
#include <nxui/widgets/GlassWidget.hpp>
#include <nxui/core/Font.hpp>
#include <nxui/core/Types.hpp>


class BatteryWidget : public nxui::GlassWidget {
public:
    BatteryWidget() = default;
    void setFont(nxui::Font* f) { m_font = f; }
    void setTextColor(const nxui::Color& c) { m_textColor = c; }
    void setBatteryStatus(uint32_t percentage, bool charging);
    // Deep idle: the charging pulse stands still (drawn about once a second).
    void setMotionPaused(bool paused) { m_motionPaused = paused; }

protected:
    void onContentUpdate(float dt) override;
    void onContentRender(nxui::Renderer& ren) override;
    nxui::Vec2 computeContentSize() const override;

private:
    nxui::Font* m_font = nullptr;
    float m_level   = -1.f;
    bool  m_charging = false;
    bool  m_motionPaused = false;
    float m_timer    = 0.f;
    float m_chargeAnim = 0.f;
    nxui::Color m_textColor {1.f, 1.f, 1.f, 1.f};
};
