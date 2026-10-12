#pragma once
#include "friends/FriendStore.hpp"
#include "social/DiscordService.hpp"

#include <nxui/Theme.hpp>
#include <nxui/core/Font.hpp>
#include <nxui/core/Texture.hpp>
#include <nxui/widgets/Widget.hpp>

#include <atomic>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

namespace nxui { class GpuDevice; class Input; class Renderer; }

// HomeNX Social: your profile, quick buttons (Mii Editor, Add Friend, Friend
// Requests, Discord) and your Nintendo friends with their profile pictures,
// online dots and when they were last online. The friend list loads on its own
// thread, so the screen opens straight away.
class SocialScreen : public nxui::Widget {
public:
    struct Callbacks {
        std::function<void()> onClosed;
        std::function<void()> onMiiEditor;
        std::function<void()> onAddFriend;
        std::function<void()> onRequests;
        std::function<void()> onDiscord;
        std::function<void()> onNavigate; // sounds
        std::function<void()> onActivate;
    };

    SocialScreen();
    void setFonts(nxui::Font* font, nxui::Font* small) { m_font = font; m_smallFont = small; }
    void setGpu(nxui::GpuDevice* gpu, nxui::Renderer* ren) { m_gpu = gpu; m_ren = ren; }
    void setTheme(const nxui::Theme* t) { m_theme = t; }
    void setIconDir(std::string dir) { m_iconDir = std::move(dir); }
    void setDiscord(const DiscordService* d) { m_discord = d; }
    void setCallbacks(Callbacks cb) { m_cb = std::move(cb); }

    // uid: whose friends; name/avatar: shown as "you" in the corner.
    void show(AccountUid uid, std::string name, std::shared_ptr<nxui::Texture> avatar);
    void hide();
    bool isActive() const { return m_active || m_anim > 0.001f; }
    bool isOpen() const { return m_active; }
    void handleTouch(const nxui::Input& in);

protected:
    void onUpdate(float dt) override;
    void onRender(nxui::Renderer& ren) override;

private:
    struct Friend {
        FriendEntry data;
        std::unique_ptr<nxui::Texture> avatar;
        bool avatarTried = false;
    };
    struct Loaded { // filled by the loading thread
        std::mutex mu;
        bool done = false;
        std::vector<FriendEntry> list;
        Result rc = 0;
        std::string diag;
    };
    enum class Zone { Tiles, Friends };

    void bindInput();
    void move(int dx, int dy);
    void activate();
    void startLoad();
    nxui::Rect panelRect() const;
    nxui::Rect listRect() const;
    nxui::Rect tileRect(int i) const;
    const nxui::Texture* icon(int i);
    std::string statusLine(const FriendEntry& f) const;
    nxui::Color statusColor(const FriendEntry& f, float a) const;
    void drawAvatar(nxui::Renderer& ren, const nxui::Texture* tex, const std::string& name,
                    const nxui::Rect& r, float a);
    void drawTileIcon(nxui::Renderer& ren, int i, const nxui::Rect& r, float a);
    void drawDetail(nxui::Renderer& ren, const nxui::Rect& r, float a);

    nxui::Font* m_font = nullptr;
    nxui::Font* m_smallFont = nullptr;
    nxui::GpuDevice* m_gpu = nullptr;
    nxui::Renderer* m_ren = nullptr;
    const nxui::Theme* m_theme = nullptr;
    const DiscordService* m_discord = nullptr;
    std::string m_iconDir;
    Callbacks m_cb;

    bool m_active = false;
    float m_anim = 0.f;
    Zone m_zone = Zone::Tiles;
    int m_tile = 0;
    int m_sel = 0;
    float m_scroll = 0.f;
    bool m_followSel = true;
    float m_touchScrollStart = 0.f;
    bool m_touchDragging = false;

    AccountUid m_uid{};
    std::string m_myName;
    std::shared_ptr<nxui::Texture> m_myAvatar;

    std::shared_ptr<Loaded> m_loading; // the load in flight, if any
    bool m_loaded = false;
    std::uint64_t m_loadedAt = 0;
    AccountUid m_loadedUid{};
    Result m_rc = 0;
    std::string m_diag;
    std::vector<Friend> m_friends;

    nxui::Texture m_icons[4];
    bool m_iconTried[4] = {};
};
