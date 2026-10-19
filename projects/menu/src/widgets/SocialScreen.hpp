#pragma once
#include "friends/FriendStore.hpp"
#include "social/DiscordService.hpp"
#include "social/MiiStore.hpp"

#include <nxui/Theme.hpp>
#include <nxui/core/Font.hpp>
#include <nxui/core/Texture.hpp>
#include <nxui/widgets/Widget.hpp>

#include <atomic>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <set>
#include <string>
#include <utility>
#include <vector>

namespace nxui { class GpuDevice; class Input; class Renderer; }

// HomeNX Social: you and your Nintendo friends (profile pictures, online dots,
// last online), everyone's play history (games, hours, launches), friend
// requests and adding friends by friend code, all in HomeNX's own screens.
// Everything that talks to the system runs on its own thread, so the screen
// opens straight away.
class SocialScreen : public nxui::Widget {
public:
    struct Callbacks {
        std::function<void()> onClosed;
        std::function<void()> onMiiEditor;
        std::function<void()> onDiscord;
        // HomeNX's keyboard: title, hint, starting text, max length, then what was typed.
        std::function<void(const std::string&, const std::string&, const std::string&, int,
                           std::function<void(const std::string&)>)> onEnterText;
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
    // The installed games: their names, and whose play history to read for you.
    void setTitles(std::vector<std::pair<std::uint64_t, std::string>> titles);

    // uid: whose friends; name/avatar: shown as "you".
    void show(AccountUid uid, std::string name, std::shared_ptr<nxui::Texture> avatar);
    void hide();
    // Straight to a page: your Miis (the Mii Maker), or your profile.
    void showMiisPage() { openMiis(); }
    void showProfilePage();
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
        int requests = -1;
    };
    // One person's play history, read on its own thread. Key 0 is you.
    struct History {
        std::mutex mu;
        bool done = false;
        Result rc = 0;
        std::string friendCode; // yours only
        std::vector<PlayRecord> records;
        std::vector<std::pair<std::uint64_t, std::vector<std::uint8_t>>> icons; // JPEGs
    };
    struct HistoryView {
        bool loaded = false;
        Result rc = 0;
        std::vector<PlayRecord> records;
    };
    // The requests list, answering one, a friend-code lookup or sending a request.
    struct Job {
        std::mutex mu;
        bool done = false;
        Result rc = 0;
        std::vector<FriendRequestEntry> list;
        std::uint64_t networkId = 0;
        std::string name;
        std::vector<MiiEntry> miis;
        MiiEntry mii;
    };
    enum class JobKind { None, Requests, Accept, Reject, Lookup, Send,
                         MiiLoad, MiiRandom, MiiAdd, MiiRename, MiiDelete, MiiSave };
    struct FaceJob { // Mii faces from Mii Studio, a few at a time
        std::mutex mu;
        bool done = false;
        std::vector<std::pair<std::string, std::vector<std::uint8_t>>> pngs;
        std::vector<std::string> errors; // why each empty one failed (for menu.log)
    };
    enum class Zone { Tiles, List, Detail };
    enum class Mode { Friends, Requests, Miis };
    enum class Add { Idle, Looking, Found, Sending, Sent, Failed };

    void bindInput();
    void move(int dx, int dy);
    void activateItem();
    void back();
    void answer(bool accept);
    void pressX();
    void pressY();
    void openMiis();
    void startMiiJob(JobKind kind, std::function<void(Job&)> work);
    void ensureFaces();
    const nxui::Texture* face(const std::string& url) const;
    void drawMii(nxui::Renderer& ren, const MiiEntry* mii, const nxui::Rect& r, float a);
    void drawMiis(nxui::Renderer& ren, const nxui::Rect& r, float a);
    // HomeNX's own Mii Maker: one Mii's looks, feature by feature.
    void startEdit(int index);
    void endEdit();
    void editChange(int delta);
    void editRandom();
    void updateEditFace();
    void drawMiiEditor(nxui::Renderer& ren, const nxui::Rect& r, float a);
    void startLoad();
    void startJob(JobKind kind, std::function<void(Job&)> work);
    void finishJob();
    void startAddFriend();
    void openRequests();
    void ensureHistory();
    std::uint64_t selectedKey(bool& valid) const; // whose history the selection shows
    int listCount() const;
    void notice(std::string text);
    std::string titleName(std::uint64_t id) const;
    std::string playTime(const PlayRecord& r) const;
    std::string errorText(Result rc) const;
    nxui::Rect panelRect() const;
    nxui::Rect listRect() const;
    nxui::Rect tileRect(int i) const;
    nxui::Rect detailRect() const;
    const nxui::Texture* icon(int i);
    std::string statusLine(const FriendEntry& f) const;
    nxui::Color statusColor(const FriendEntry& f, float a) const;
    void drawAvatar(nxui::Renderer& ren, const nxui::Texture* tex, const std::string& name,
                    const nxui::Rect& r, float a);
    void drawTileIcon(nxui::Renderer& ren, int i, const nxui::Rect& r, float a);
    void drawDetail(nxui::Renderer& ren, const nxui::Rect& r, float a);
    void drawPerson(nxui::Renderer& ren, const nxui::Rect& r, float a);
    void drawHistory(nxui::Renderer& ren, const nxui::Rect& r, float a, const HistoryView* hv, bool mine);
    void drawRequests(nxui::Renderer& ren, const nxui::Rect& r, float a);
    void drawAddFriend(nxui::Renderer& ren, const nxui::Rect& r, float a);
    void drawMessage(nxui::Renderer& ren, const nxui::Rect& r, float a, const std::string& title,
                     const std::string& body);

    nxui::Font* m_font = nullptr;
    nxui::Font* m_smallFont = nullptr;
    nxui::GpuDevice* m_gpu = nullptr;
    nxui::Renderer* m_ren = nullptr;
    const nxui::Theme* m_theme = nullptr;
    const DiscordService* m_discord = nullptr;
    std::string m_iconDir;
    Callbacks m_cb;
    std::map<std::uint64_t, std::string> m_titles;

    bool m_active = false;
    float m_anim = 0.f;
    Zone m_zone = Zone::Tiles;
    Mode m_mode = Mode::Friends;
    int m_tile = 0;
    int m_sel = 0; // Friends mode: 0 is you, 1.. your friends
    float m_scroll = 0.f;
    bool m_followSel = true;
    float m_touchScrollStart = 0.f;
    bool m_touchDragging = false;
    float m_detailScroll = 0.f;

    AccountUid m_uid{};
    std::string m_myName;
    std::shared_ptr<nxui::Texture> m_myAvatar;
    std::string m_friendCode;

    std::shared_ptr<Loaded> m_loading; // the load in flight, if any
    bool m_loaded = false;
    std::uint64_t m_loadedAt = 0;
    Result m_rc = 0;
    std::string m_diag;
    std::vector<Friend> m_friends;
    int m_requestCount = -1;

    std::map<std::uint64_t, HistoryView> m_histories;
    std::shared_ptr<History> m_historyJob;
    std::uint64_t m_historyKey = 0;
    std::map<std::uint64_t, std::unique_ptr<nxui::Texture>> m_titleIcons;
    std::map<std::uint64_t, std::vector<std::uint8_t>> m_iconJpegs; // waiting to be uploaded

    std::shared_ptr<Job> m_job;
    JobKind m_jobKind = JobKind::None;
    std::vector<FriendRequestEntry> m_requests;
    bool m_requestsLoaded = false;
    Result m_requestsRc = 0;
    std::uint64_t m_answering = 0; // request id being accepted or declined

    Add m_add = Add::Idle;
    std::uint64_t m_addNetworkId = 0;
    std::string m_addName;
    std::string m_addCode;
    Result m_addRc = 0;

    std::vector<MiiEntry> m_miis;
    bool m_miisLoaded = false;
    Result m_miisRc = 0;
    MiiEntry m_candidate;          // a random new Mii being looked at
    bool m_haveCandidate = false;
    int m_candidateGender = 2;     // 0 male, 1 female, 2 either
    int m_deleteArmed = -1;        // X once arms delete for this row, X again deletes
    std::uint64_t m_deleteArmedAt = 0;
    std::map<std::string, std::unique_ptr<nxui::Texture>> m_faces;
    std::map<std::string, std::vector<std::uint8_t>> m_facePngs; // waiting to be uploaded
    std::set<std::string> m_faceTried;
    std::uint64_t m_faceRetryAt = 0; // a failed face is asked for again from this tick
    std::shared_ptr<FaceJob> m_faceJob;

    bool m_editing = false;            // in the Mii Maker
    MiiEntry m_edit;                   // the Mii as it's being changed
    MiiEntry m_editOriginal;           // as it was (what gets replaced)
    int m_editIndex = -1;              // its place in the list
    int m_editRow = 0;                 // the feature chosen
    std::uint64_t m_editChangedAt = 0; // the face is asked for once changes settle
    std::shared_ptr<FaceJob> m_editFaceJob;
    std::string m_editFaceUrl;         // the face asked for last
    std::unique_ptr<nxui::Texture> m_editFace;
    bool m_historyByTime = false;      // your play history: most played first (Y)

    std::string m_notice;
    std::uint64_t m_noticeAt = 0;

    nxui::Texture m_icons[4];
    bool m_iconTried[4] = {};
};
