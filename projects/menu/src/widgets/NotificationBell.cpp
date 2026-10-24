#include "NotificationBell.hpp"

#include <nxui/core/I18n.hpp>

#include <algorithm>
#include <string>

NotificationBell::NotificationBell() {
    setTag("notificationBell");
    setFocusable(true);
    setCornerRadius(26.f);
    setPadding(6.f);
    setLiquidGlassEnabled(true);
    setForceLiquidGlass(true);
    setBlurEnabled(false);
    setPanelOpacity(0.90f);
    setBorderWidth(1.8f);
    setBaseColor(nxui::Color(0.20f, 0.22f, 0.28f, 0.94f));
    setBorderColor(nxui::Color::white().withAlpha(0.24f));
    setHighlightColor(nxui::Color::white().withAlpha(0.08f));
    setUnread(0);
}

void NotificationBell::setTheme(const nxui::Theme* theme) {
    m_theme = theme;
    if (!theme)
        return;
    setBaseColor(theme->iconDefault.withAlpha(theme->mode == nxui::ThemeMode::Dark ? 0.92f : 0.94f));
    setBorderColor(theme->panelBorder);
    setHighlightColor(theme->panelHighlight);
}

void NotificationBell::setUnread(int count) {
    m_unread = std::max(0, count);
    auto& i18n = nxui::I18n::instance();
    setAccessibilityLabel(i18n.tr("notifications.title", "Notifications")
                          + (m_unread ? ", " + std::to_string(m_unread) + " " + i18n.tr("notifications.new", "new")
                                      : std::string()));
    setAccessibilityRole(i18n.tr("accessibility.roles.button", "button"));
}

void NotificationBell::onContentRender(nxui::Renderer& ren) {
    const float a = opacity();
    const nxui::Rect r = glassContentRect();
    const float s = std::min(r.width, r.height);
    const float cx = r.x + r.width * 0.5f, top = r.y + (r.height - s) * 0.5f;
    const nxui::Color ink = m_theme ? m_theme->textPrimary.withAlpha(0.85f * a) : nxui::Color::white().withAlpha(0.85f * a);

    // The bell: knob, dome, body, rim, clapper.
    ren.drawCircle({cx, top + s * 0.17f}, s * 0.05f, ink, 12);
    ren.drawCircle({cx, top + s * 0.42f}, s * 0.22f, ink, 28);
    ren.drawRect({cx - s * 0.22f, top + s * 0.42f, s * 0.44f, s * 0.24f}, ink);
    ren.drawRoundedRect({cx - s * 0.33f, top + s * 0.63f, s * 0.66f, s * 0.09f}, ink, s * 0.045f);
    ren.drawCircle({cx, top + s * 0.80f}, s * 0.07f, ink, 16);

    if (m_unread > 0) {
        const nxui::Vec2 c{r.x + r.width - s * 0.12f, r.y + s * 0.12f};
        ren.drawCircle(c, s * 0.2f, nxui::Color(0.93f, 0.26f, 0.27f, a), 24);
        if (m_font) {
            const std::string n = m_unread > 9 ? "9+" : std::to_string(m_unread);
            const float scale = 0.62f;
            const nxui::Vec2 size = m_font->measure(n);
            ren.drawText(n, {c.x - size.x * scale * 0.5f, c.y - size.y * scale * 0.5f}, m_font,
                         nxui::Color::white().withAlpha(a), scale);
        }
    }
}
