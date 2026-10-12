#pragma once
#include "social/DiscordService.hpp"

#include <nxui/Theme.hpp>
#include <nxui/core/Font.hpp>
#include <nxui/widgets/Widget.hpp>

#include <functional>
#include <string>
#include <vector>

// HomeNX Discord panel: slides in from the left like Quick Settings does from the
// right. Signed out it offers the QR sign-in; signed in it lists friends online and
// your DMs, and a DM opens its recent messages (X writes a reply).
class DiscordPanel : public nxui::Widget {
public:
    struct Callbacks {
        std::function<void()> onClosed;
        std::function<void(Snowflake channelId, const std::string& name)> onReply;
        std::function<void()> onNavigate;  // sounds
        std::function<void()> onActivate;
    };

    DiscordPanel();
    void setFont(nxui::Font* f) { m_font = f; }
    void setSmallFont(nxui::Font* f) { m_smallFont = f; }
    void setTheme(const nxui::Theme* t) { m_theme = t; }
    void setService(DiscordService* s) { m_service = s; }
    void setCallbacks(Callbacks cb) { m_cb = std::move(cb); }

    void show();
    void hide();
    bool isActive() const { return m_active || m_anim > 0.001f; }

protected:
    void onUpdate(float dt) override;
    void onRender(nxui::Renderer& ren) override;

private:
    enum class View { Home, Chat };
    struct Row {
        enum Kind { Header, Friend, Dm, Action, Info } kind = Info;
        std::string text, detail;
        Snowflake channelId = 0;
        bool unread = false;
        int mentions = 0;
        int action = 0; // 1 = QR sign-in, 2 = sign out, 3 = retry
    };

    void bindInput();
    void rebuildRows();
    void move(int dir);
    void activateRow();
    void back();
    bool selectable(const Row& r) const { return r.kind == Row::Dm || r.kind == Row::Action; }
    nxui::Rect panelRect() const;
    void drawQr(nxui::Renderer& ren, const std::string& text, const nxui::Rect& area, float alpha);

    DiscordService* m_service = nullptr;
    nxui::Font* m_font = nullptr;
    nxui::Font* m_smallFont = nullptr;
    const nxui::Theme* m_theme = nullptr;
    Callbacks m_cb;

    bool m_active = false;
    float m_anim = 0.f;
    View m_view = View::Home;
    std::vector<Row> m_rows;
    int m_sel = -1;
    float m_scroll = 0.f;
    uint64_t m_builtVersion = ~0ull;
    int m_builtState = -1;
    Snowflake m_chat = 0;
    std::string m_chatName;

    // The QR code for the current sign-in URL (modules, row-major).
    std::string m_qrText;
    std::vector<bool> m_qrModules;
    int m_qrSize = 0;
};
