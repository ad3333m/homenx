#pragma once
#include "social/DiscordService.hpp"

#include <nxui/core/Font.hpp>
#include <nxui/core/Texture.hpp>
#include <nxui/widgets/Widget.hpp>

#include <functional>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

namespace nxui { class GpuDevice; class Input; class Renderer; }

// HomeNX Discord panel, in Discord's own dark grey look: slides in from the left
// like Quick Settings does from the right. A server bar on the left (DMs first,
// then your servers), your DMs with profile pictures and status dots or a
// server's channels, a chat view, and your account at the bottom (Y, or A on
// it, switches accounts). Signed out it offers the QR sign-in.
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
    void setGpu(nxui::GpuDevice* gpu, nxui::Renderer* ren) { m_gpu = gpu; m_ren = ren; }
    void setLogoPath(std::string path) { m_logoPath = std::move(path); }
    void setService(DiscordService* s) { m_service = s; }
    void setCallbacks(Callbacks cb) { m_cb = std::move(cb); }

    void show();
    void hide();
    bool isActive() const { return m_active || m_anim > 0.001f; }
    bool isOpen() const { return m_active; } // false as soon as it starts sliding away
    void handleTouch(const nxui::Input& in);
    // Straight to a chat (HOME's Discord alert); the panel must be open.
    void openChat(Snowflake channelId, Snowflake guildId, const std::string& name, const std::string& avatarUrl);
    const nxui::Texture* avatar(const std::string& url); // downloads it the first time
    std::string clean(const std::string& text) const; // drops letters the font can't draw

protected:
    void onUpdate(float dt) override;
    void onRender(nxui::Renderer& ren) override;

private:
    enum class View { Home, Chat, Accounts };
    struct Row {
        enum Kind { Dm, Account, Button, Info, Error, Me, Channel, Category } kind = Info;
        std::string text, detail, avatarUrl, status, accountId;
        std::vector<std::string> lines; // Info / Error, wrapped
        Snowflake channelId = 0;
        bool unread = false, current = false, primary = false, danger = false, voice = false;
        int mentions = 0;
        int action = 0; // Button: see activateRow()
    };
    struct ChatItem {
        bool header = false; // avatar + name + time above the text
        std::string author, avatarUrl, time;
        std::vector<std::string> lines;
        int tone = 0; // 0 normal, 1 sending, 2 failed
        uint32_t color = 0; // the author's role colour, 0 = default
        float height = 0.f;
        std::vector<std::string> emojis; // pictures for the U+F0000+i marks in `lines`
        std::vector<DiscordService::Line::Media> media;
        bool bigEmoji = false; // only emoji: drawn large, like Discord
    };
    // A downloaded picture: a still image, or a GIF's frames in one texture.
    struct Picture {
        nxui::Texture tex;
        std::vector<nxui::Rect> frames; // where each frame is in tex; empty = all of it
        std::vector<int> delays;        // ms per frame
        int totalMs = 0;
        uint64_t start = 0;
        std::size_t bytes = 0;
    };

    void bindInput();
    void rebuildRows();
    void rebuildChat();
    void move(int dir);
    void activateRow();
    void back();
    void openAccounts();
    bool selectable(const Row& r) const {
        return r.kind == Row::Dm || r.kind == Row::Account || r.kind == Row::Button || r.kind == Row::Me
            || r.kind == Row::Channel;
    }
    // The server bar shows on the home view once signed in.
    bool railShown() const;
    bool listFocused() const { return !railShown() || m_column == 1; }
    void moveRail(int dir);
    void drawRail(nxui::Renderer& ren, const nxui::Rect& area, float a);
    std::string rowKey(const Row& r) const;
    nxui::Rect panelRect() const;
    void drawAvatar(nxui::Renderer& ren, const std::string& url, const std::string& name,
                    float x, float y, float size, float alpha);
    void drawStatus(nxui::Renderer& ren, const std::string& status, float cx, float cy, float r,
                    const nxui::Color& ring, float alpha);
    void drawQr(nxui::Renderer& ren, const std::string& text, const nxui::Rect& area, float alpha);
    float rowHeight(const Row& r) const;
    void drawTopBar(nxui::Renderer& ren, const nxui::Rect& p, float a);
    void drawRows(nxui::Renderer& ren, const nxui::Rect& area, float a);
    void drawChat(nxui::Renderer& ren, const nxui::Rect& area, float a);
    void drawMe(nxui::Renderer& ren, const nxui::Rect& area, float a);
    void drawQrView(nxui::Renderer& ren, const nxui::Rect& area, float a);

    DiscordService* m_service = nullptr;
    nxui::Font* m_font = nullptr;
    nxui::Font* m_smallFont = nullptr;
    nxui::GpuDevice* m_gpu = nullptr;
    nxui::Renderer* m_ren = nullptr;
    std::string m_logoPath;
    Callbacks m_cb;

    bool m_active = false;
    float m_anim = 0.f;
    View m_view = View::Home;
    std::vector<Row> m_rows;
    int m_sel = -1;
    float m_scroll = 0.f;
    bool m_followSel = true; // D-pad keeps the selection in view; a finger drag doesn't
    uint64_t m_builtVersion = ~0ull;
    int m_builtState = -1;
    View m_builtView = View::Home;
    Snowflake m_chat = 0;
    std::string m_chatName, m_chatAvatar, m_chatStatus;
    bool m_chatIsChannel = false; // a server channel ("#name") rather than a DM

    // The server bar: 0 = Direct Messages, else a server's id.
    std::vector<DiscordService::Server> m_servers;
    Snowflake m_guild = 0;
    Snowflake m_builtGuild = ~0ull;
    int m_column = 1; // 0 = the server bar, 1 = the list
    float m_railScroll = 0.f;
    std::vector<std::pair<int, nxui::Rect>> m_railHits; // index 0 = DMs, i = m_servers[i - 1]
    nxui::Texture m_mark; // Discord's logo in white
    bool m_markTried = false;
    std::vector<ChatItem> m_chatItems;
    uint64_t m_chatVersion = ~0ull;
    Snowflake m_chatBuiltFor = 0;
    float m_chatScroll = 0.f;

    // Touch: where each row was drawn last frame, and a drag in progress.
    std::vector<std::pair<int, nxui::Rect>> m_hits;
    nxui::Rect m_backRect{}, m_inputRect{};
    float m_touchScrollStart = 0.f;
    bool m_touchDragging = false;

    const Picture* picture(const std::string& url, const std::string& fallback, bool animated, int maxW, int maxH);
    void drawPicture(nxui::Renderer& ren, const Picture& pic, const nxui::Rect& r, float a, float radius);
    float richWidth(nxui::Font* font, const std::string& s, float scale, float emoji) const;
    std::vector<std::string> wrapRich(nxui::Font* font, const std::string& text, float maxW, float scale,
                                      int maxLines, float emoji) const;
    void drawRich(nxui::Renderer& ren, const std::string& s, float x, float y, nxui::Font* font,
                  const nxui::Color& color, float scale, const std::vector<std::string>& emojis, float emoji);

    // Images, GIFs and emoji by URL (nullptr while loading or after a failure).
    std::unordered_map<std::string, std::unique_ptr<Picture>> m_pictures;
    std::vector<std::string> m_pictureOrder; // oldest first, for dropping when over budget
    std::size_t m_pictureBytes = 0;

    // Profile pictures by URL (nullptr while loading or after a failure).
    std::unordered_map<std::string, std::unique_ptr<nxui::Texture>> m_avatars;
    std::shared_ptr<bool> m_alive = std::make_shared<bool>(true);
    nxui::Texture m_logo;
    bool m_logoTried = false;

    // The QR code for the current sign-in URL (modules, row-major).
    std::string m_qrText;
    std::vector<bool> m_qrModules;
    int m_qrSize = 0;

    mutable std::unordered_map<uint32_t, bool> m_glyphs;
};
