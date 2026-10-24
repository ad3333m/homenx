#pragma once

#include <nxui/Theme.hpp>
#include <nxui/core/Font.hpp>
#include <nxui/core/Renderer.hpp>
#include <nxui/widgets/GlassWidget.hpp>

// HOME's notification centre button, next to the profile tile: a bell, with a
// red count of what came since it was last opened.
class NotificationBell : public nxui::GlassWidget {
public:
    NotificationBell();

    void setTheme(const nxui::Theme* theme);
    void setFont(nxui::Font* font) { m_font = font; }
    void setUnread(int count);
    int unread() const { return m_unread; }

protected:
    void onContentRender(nxui::Renderer& ren) override;

private:
    const nxui::Theme* m_theme = nullptr;
    nxui::Font* m_font = nullptr;
    int m_unread = 0;
};
