#include "widgets/SocialScreen.hpp"

#include <nxui/core/Animation.hpp>
#include <nxui/core/GpuDevice.hpp>
#include <nxui/core/I18n.hpp>
#include <nxui/core/Input.hpp>
#include <nxui/core/Renderer.hpp>
#include <nxui/core/ThreadPool.hpp>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <thread>

namespace {

constexpr float kPanelX = 80.f;
constexpr float kPanelY = 36.f;
constexpr float kPanelW = 1120.f;
constexpr float kPanelH = 648.f;
constexpr float kAnimDur = 0.22f;
constexpr float kHeaderH = 84.f;
constexpr float kLeftW = 470.f;
constexpr float kTile = 106.f;
constexpr float kRowH = 66.f;
constexpr float kFooterH = 48.f;
constexpr int kTiles = 4; // Mii Editor, Add Friend, Friend Requests, Discord

const nxui::Color kOnline(0.20f, 0.78f, 0.35f, 1.f);
const nxui::Color kPlaying(0.04f, 0.52f, 1.f, 1.f);
const nxui::Color kGold(1.f, 0.78f, 0.18f, 1.f);
const nxui::Color kRed(0.95f, 0.25f, 0.26f, 1.f);

// Drop whole UTF-8 characters until the text (plus "...") fits.
std::string fit(nxui::Font* font, std::string text, float maxW, float scale) {
    if (!font || font->measure(text).x * scale <= maxW) return text;
    while (!text.empty()) {
        while (!text.empty()) {
            const unsigned char c = static_cast<unsigned char>(text.back());
            text.pop_back();
            if ((c & 0xC0) != 0x80) break;
        }
        if (font->measure(text + "...").x * scale <= maxW) return text + "...";
    }
    return "...";
}

std::vector<std::string> wrap(nxui::Font* font, const std::string& text, float maxW, float scale) {
    std::vector<std::string> lines;
    if (!font) return lines;
    std::string line;
    size_t i = 0;
    while (i <= text.size()) {
        const size_t sp = text.find(' ', i);
        const std::string word = text.substr(i, sp == std::string::npos ? std::string::npos : sp - i);
        const std::string candidate = line.empty() ? word : line + " " + word;
        if (font->measure(candidate).x * scale <= maxW || line.empty()) {
            line = candidate;
        } else {
            lines.push_back(fit(font, line, maxW, scale));
            line = word;
        }
        if (sp == std::string::npos) break;
        i = sp + 1;
    }
    if (!line.empty()) lines.push_back(fit(font, line, maxW, scale));
    return lines;
}

bool inside(const nxui::Rect& r, float x, float y) {
    return x >= r.x && x < r.x + r.width && y >= r.y && y < r.y + r.height;
}

std::string ago(std::int64_t when) {
    auto& i18n = nxui::I18n::instance();
    const std::int64_t s = static_cast<std::int64_t>(std::time(nullptr)) - when;
    char buf[64];
    if (s < 60) return i18n.tr("social.just_now", "just now");
    if (s < 3600) {
        std::snprintf(buf, sizeof(buf), "%d %s", (int)(s / 60), i18n.tr("social.min_ago", "min ago").c_str());
    } else if (s < 86400) {
        const int h = (int)(s / 3600);
        std::snprintf(buf, sizeof(buf), "%d %s", h,
                      (h == 1 ? i18n.tr("social.hour_ago", "hour ago") : i18n.tr("social.hours_ago", "hours ago")).c_str());
    } else {
        const int d = (int)(s / 86400);
        std::snprintf(buf, sizeof(buf), "%d %s", d,
                      (d == 1 ? i18n.tr("social.day_ago", "day ago") : i18n.tr("social.days_ago", "days ago")).c_str());
    }
    return buf;
}

std::string dateOf(std::int64_t when) {
    TimeCalendarTime at{};
    TimeCalendarAdditionalInfo info{};
    if (R_FAILED(timeToCalendarTimeWithMyRule(static_cast<u64>(when), &at, &info))) return {};
    static const char* const kMonths[] = {"Jan", "Feb", "Mar", "Apr", "May", "Jun",
                                          "Jul", "Aug", "Sep", "Oct", "Nov", "Dec"};
    char buf[48];
    std::snprintf(buf, sizeof(buf), "%d %s %d, %02d:%02d", at.day, kMonths[(at.month + 11) % 12], at.year,
                  at.hour, at.minute);
    return buf;
}

}  // namespace

SocialScreen::SocialScreen() {
    setRect({0.f, 0.f, 1280.f, 720.f});
    setVisible(false);
}

void SocialScreen::show(AccountUid uid, std::string name, std::shared_ptr<nxui::Texture> avatar) {
    const bool sameUser = std::memcmp(&uid, &m_uid, sizeof(uid)) == 0;
    m_uid = uid;
    m_myName = std::move(name);
    m_myAvatar = std::move(avatar);
    m_active = true;
    m_zone = Zone::Tiles;
    m_tile = 0;
    m_sel = 0;
    m_scroll = 0.f;
    m_followSel = true;
    if (!sameUser) {
        m_friends.clear();
        m_loaded = false;
    }
    // Statuses change: read the list again if it's more than a minute old.
    if (!m_loaded || armTicksToNs(armGetSystemTick() - m_loadedAt) > 60'000'000'000ULL)
        startLoad();
    setVisible(true);
    setFocusable(true);
    bindInput();
}

void SocialScreen::hide() {
    if (!m_active) return;
    m_active = false;
    clearActions();
    setFocusable(false); // never leave a full-screen focus target behind
    if (m_cb.onClosed) m_cb.onClosed();
}

void SocialScreen::bindInput() {
    clearActions();
    addDirectionAction(nxui::FocusDirection::UP, [this]() { move(0, -1); });
    addDirectionAction(nxui::FocusDirection::DOWN, [this]() { move(0, +1); });
    addDirectionAction(nxui::FocusDirection::LEFT, [this]() { move(-1, 0); });
    addDirectionAction(nxui::FocusDirection::RIGHT, [this]() { move(+1, 0); });
    addAction(static_cast<uint64_t>(nxui::Button::A), [this]() { activateItem(); });
    addAction(static_cast<uint64_t>(nxui::Button::B), [this]() { hide(); });
}

void SocialScreen::startLoad() {
    if (m_loading || !accountUidIsValid(&m_uid)) return; // one at a time: the reader shares buffers
    auto job = std::make_shared<Loaded>();
    m_loading = job;
    const AccountUid uid = m_uid;
    std::thread([job, uid]() {
        nxui::moveToWorkerCores();
        Result rc = 0;
        std::string diag;
        std::vector<FriendEntry> list = loadFriends(uid, rc, diag);
        std::lock_guard<std::mutex> lk(job->mu);
        job->list = std::move(list);
        job->rc = rc;
        job->diag = std::move(diag);
        job->done = true;
    }).detach();
}

void SocialScreen::move(int dx, int dy) {
    const int n = (int)m_friends.size();
    bool moved = false;
    if (m_zone == Zone::Tiles) {
        if (dx != 0) {
            const int t = std::clamp(m_tile + dx, 0, kTiles - 1);
            moved = t != m_tile;
            m_tile = t;
        } else if (dy > 0 && n > 0) {
            m_zone = Zone::Friends;
            m_sel = std::clamp(m_sel, 0, n - 1);
            moved = true;
        }
    } else if (dy != 0) {
        if (m_sel + dy < 0) {
            m_zone = Zone::Tiles;
            moved = true;
        } else if (m_sel + dy < n) {
            m_sel += dy;
            moved = true;
        }
    }
    if (moved) {
        m_followSel = true;
        if (m_cb.onNavigate) m_cb.onNavigate();
    }
}

void SocialScreen::activateItem() {
    if (m_zone != Zone::Tiles) return;
    if (m_cb.onActivate) m_cb.onActivate();
    switch (m_tile) {
        case 0: if (m_cb.onMiiEditor) m_cb.onMiiEditor(); break;
        case 1: if (m_cb.onAddFriend) m_cb.onAddFriend(); break;
        case 2: if (m_cb.onRequests) m_cb.onRequests(); break;
        case 3: if (m_cb.onDiscord) m_cb.onDiscord(); break;
    }
}

void SocialScreen::onUpdate(float dt) {
    const float target = m_active ? 1.f : 0.f;
    if (m_anim != target) {
        const float step = dt / kAnimDur;
        m_anim = m_anim < target ? std::min(target, m_anim + step) : std::max(target, m_anim - step);
        if (m_anim <= 0.f && !m_active) setVisible(false);
    }

    bool finished = false;
    if (m_loading) {
        std::lock_guard<std::mutex> lk(m_loading->mu);
        finished = m_loading->done;
        if (finished) {
            m_friends.clear();
            for (auto& f : m_loading->list) {
                Friend fr;
                fr.data = std::move(f);
                m_friends.push_back(std::move(fr));
            }
            m_rc = m_loading->rc;
            m_diag = m_loading->diag;
            m_loaded = true;
            m_loadedAt = armGetSystemTick();
            m_sel = std::clamp(m_sel, 0, std::max(0, (int)m_friends.size() - 1));
            if (m_friends.empty() && m_zone == Zone::Friends) m_zone = Zone::Tiles;
        }
    }
    if (finished) m_loading.reset();

    // Profile pictures: a couple per frame (each is a small JPEG to decode).
    if (m_gpu && m_ren) {
        int budget = 2;
        for (auto& f : m_friends) {
            if (budget == 0) break;
            if (f.avatarTried) continue;
            f.avatarTried = true;
            if (f.data.avatar.empty()) continue;
            auto tex = std::make_unique<nxui::Texture>();
            if (tex->loadFromMemory(*m_gpu, *m_ren, f.data.avatar.data(), f.data.avatar.size(), 192))
                f.avatar = std::move(tex);
            --budget;
        }
    }
}

nxui::Rect SocialScreen::panelRect() const {
    const float e = nxui::Easing::outCubic(m_anim);
    return {kPanelX, kPanelY + (1.f - e) * 24.f, kPanelW, kPanelH};
}

nxui::Rect SocialScreen::tileRect(int i) const {
    const nxui::Rect p = panelRect();
    const float gap = (kLeftW - kTiles * kTile) / (kTiles - 1);
    return {p.x + 28.f + i * (kTile + gap), p.y + kHeaderH + 10.f, kTile, kTile};
}

nxui::Rect SocialScreen::listRect() const {
    const nxui::Rect p = panelRect();
    const float top = p.y + kHeaderH + 10.f + kTile + 52.f;
    return {p.x + 20.f, top, kLeftW + 16.f, p.y + kPanelH - kFooterH - top};
}

const nxui::Texture* SocialScreen::icon(int i) {
    static const char* const kFiles[kTiles] = {"mii_editor.png", "friends.png", nullptr, "discord.png"};
    if (i < 0 || i >= kTiles || !kFiles[i] || !m_gpu || !m_ren) return nullptr;
    if (!m_iconTried[i]) {
        m_iconTried[i] = true;
        m_icons[i].loadFromFile(*m_gpu, *m_ren, m_iconDir + "/" + kFiles[i], 128);
    }
    return m_icons[i].valid() ? &m_icons[i] : nullptr;
}

std::string SocialScreen::statusLine(const FriendEntry& f) const {
    auto& i18n = nxui::I18n::instance();
    if (f.status == 2) return i18n.tr("social.playing", "Playing online");
    if (f.status == 1) return i18n.tr("social.online", "Online");
    if (f.lastOnline > 0) return i18n.tr("social.last_online", "Last online") + " " + ago(f.lastOnline);
    return i18n.tr("social.offline", "Offline");
}

nxui::Color SocialScreen::statusColor(const FriendEntry& f, float a) const {
    if (f.status == 2) return kPlaying.withAlpha(a);
    if (f.status == 1) return kOnline.withAlpha(a);
    return m_theme ? m_theme->textSecondary.withAlpha(a) : nxui::Color(0.5f, 0.5f, 0.5f, a);
}

void SocialScreen::drawAvatar(nxui::Renderer& ren, const nxui::Texture* tex, const std::string& name,
                              const nxui::Rect& r, float a) {
    if (tex && tex->valid()) {
        ren.drawTextureRounded(tex, r, r.width * 0.5f, nxui::Color::white().withAlpha(a));
        return;
    }
    static const nxui::Color kColors[] = {{0.36f, 0.55f, 0.95f, 1.f}, {0.95f, 0.55f, 0.25f, 1.f},
                                          {0.30f, 0.72f, 0.50f, 1.f}, {0.80f, 0.40f, 0.75f, 1.f}};
    const nxui::Color c = kColors[std::hash<std::string>{}(name) % 4];
    ren.drawCircle({r.x + r.width * 0.5f, r.y + r.height * 0.5f}, r.width * 0.5f, c.withAlpha(a), 40);
    if (name.empty() || !m_font) return;
    std::string letter(1, name[0]);
    if ((unsigned char)name[0] >= 0x80) { // a multi-byte letter
        size_t n = 1;
        while (n < name.size() && ((unsigned char)name[n] & 0xC0) == 0x80) ++n;
        letter = name.substr(0, n);
    } else if (letter[0] >= 'a' && letter[0] <= 'z') {
        letter[0] = (char)(letter[0] - 32);
    }
    const float s = r.width / 46.f;
    const nxui::Vec2 m = m_font->measure(letter);
    ren.drawText(letter, {r.x + (r.width - m.x * s) * 0.5f, r.y + (r.height - m.y * s) * 0.5f}, m_font,
                 nxui::Color(1.f, 1.f, 1.f, a), s);
}

void SocialScreen::drawTileIcon(nxui::Renderer& ren, int i, const nxui::Rect& r, float a) {
    const nxui::Color text = m_theme ? m_theme->textPrimary : nxui::Color(1.f, 1.f, 1.f, 1.f);
    if (const nxui::Texture* t = icon(i)) {
        ren.drawTexture(t, r, nxui::Color::white().withAlpha(a));
    } else if (i == 2) { // an envelope for Friend Requests
        const nxui::Rect e{r.x + r.width * 0.08f, r.y + r.height * 0.2f, r.width * 0.84f, r.height * 0.6f};
        ren.drawRoundedRect(e, nxui::Color(0.98f, 0.62f, 0.20f, a), r.width * 0.08f);
        const nxui::Color fold(1.f, 1.f, 1.f, 0.9f * a);
        ren.drawLine({e.x + 4.f, e.y + 4.f}, {e.x + e.width * 0.5f, e.y + e.height * 0.55f}, fold, 3.f);
        ren.drawLine({e.x + e.width - 4.f, e.y + 4.f}, {e.x + e.width * 0.5f, e.y + e.height * 0.55f}, fold, 3.f);
    } else {
        ren.drawCircle({r.x + r.width * 0.5f, r.y + r.height * 0.5f}, r.width * 0.45f, text.withAlpha(0.2f * a), 32);
    }
    if (i == 1) { // Add Friend: a green plus badge
        const nxui::Vec2 c{r.x + r.width * 0.84f, r.y + r.height * 0.82f};
        const float br = r.width * 0.2f;
        ren.drawCircle(c, br, kOnline.withAlpha(a), 24);
        ren.drawRect({c.x - br * 0.55f, c.y - br * 0.12f, br * 1.1f, br * 0.24f}, nxui::Color(1.f, 1.f, 1.f, a));
        ren.drawRect({c.x - br * 0.12f, c.y - br * 0.55f, br * 0.24f, br * 1.1f}, nxui::Color(1.f, 1.f, 1.f, a));
    }
}

void SocialScreen::drawDetail(nxui::Renderer& ren, const nxui::Rect& r, float a) {
    auto& i18n = nxui::I18n::instance();
    const nxui::Color text = m_theme->textPrimary.withAlpha(a);
    const nxui::Color muted = m_theme->textSecondary.withAlpha(a);
    const nxui::Color accent = m_theme->cursorNormal.withAlpha(a);
    ren.drawRoundedRect(r, m_theme->textPrimary.withAlpha(0.045f * a), 24.f);
    const float x = r.x + 30.f, w = r.width - 60.f;

    if (m_zone == Zone::Friends && m_sel >= 0 && m_sel < (int)m_friends.size()) {
        const Friend& f = m_friends[(size_t)m_sel];
        const nxui::Rect av{x, r.y + 30.f, 168.f, 168.f};
        drawAvatar(ren, f.avatar.get(), f.data.nickname, av, a);
        const nxui::Vec2 dot{av.x + av.width - 22.f, av.y + av.height - 22.f};
        ren.drawCircle(dot, 19.f, m_theme->panelBase.withAlpha(a), 32);
        ren.drawCircle(dot, 14.f, statusColor(f.data, a), 32);
        if (f.data.status == 0) ren.drawCircle(dot, 7.f, m_theme->panelBase.withAlpha(a), 24);

        const float tx = av.x + av.width + 26.f, tw = r.x + r.width - 30.f - tx;
        ren.drawText(fit(m_font, f.data.nickname, tw, 1.2f), {tx, av.y + 26.f}, m_font, text, 1.2f);
        // The status as a pill.
        const std::string st = f.data.status == 2 ? i18n.tr("social.playing", "Playing online")
                             : f.data.status == 1 ? i18n.tr("social.online", "Online")
                                                  : i18n.tr("social.offline", "Offline");
        const float pw = m_smallFont->measure(st).x * 0.8f + 28.f;
        const nxui::Color sc = statusColor(f.data, a);
        ren.drawRoundedRect({tx, av.y + 76.f, pw, 30.f}, sc.withAlpha(0.18f * a), 15.f);
        ren.drawText(st, {tx + 14.f, av.y + 81.f}, m_smallFont, sc, 0.8f);
        if (f.data.favourite) {
            const std::string best = i18n.tr("social.best_friend", "Best friend");
            const float bx = tx + pw + 10.f;
            const float bw = m_smallFont->measure(best).x * 0.8f + 28.f;
            ren.drawRoundedRect({bx, av.y + 76.f, bw, 30.f}, kGold.withAlpha(0.2f * a), 15.f);
            ren.drawText(best, {bx + 14.f, av.y + 81.f}, m_smallFont, kGold.withAlpha(a), 0.8f);
        }

        float y = av.y + av.height + 34.f;
        auto line = [&](const std::string& label, const std::string& value) {
            ren.drawText(label, {x, y}, m_smallFont, muted, 0.8f);
            ren.drawText(fit(m_font, value, w, 0.82f), {x, y + 24.f}, m_font, text, 0.82f);
            y += 66.f;
        };
        if (f.data.status != 0) {
            line(i18n.tr("social.now", "Right now"), f.data.status == 2
                     ? i18n.tr("social.now_playing", "Online and playing a game")
                     : i18n.tr("social.now_online", "Online on their console"));
        } else if (f.data.lastOnline > 0) {
            line(i18n.tr("social.last_online", "Last online"), dateOf(f.data.lastOnline) + "  (" + ago(f.data.lastOnline) + ")");
        } else {
            line(i18n.tr("social.last_online", "Last online"), i18n.tr("social.unknown", "Not known"));
        }
        return;
    }

    // A quick button: its picture, name and what it does.
    std::string title, body, hint = i18n.tr("social.press_a", "Press A to open");
    switch (m_tile) {
        case 0:
            title = i18n.tr("sidebar.mii_editor", "Mii Editor");
            body = i18n.tr("social.mii_body", "Make new Miis, change the ones you have, and see every Mii on this console.");
            break;
        case 1:
            title = i18n.tr("friends.add", "Add Friend");
            body = i18n.tr("social.add_body", "Search by friend code, send friend requests, or find users you've played with.")
                 + " " + i18n.tr("friends.needs_online", "This uses Nintendo's servers, so the console must be able to go online.");
            break;
        case 2:
            title = i18n.tr("friends.requests", "Friend Requests");
            body = i18n.tr("social.requests_body", "Friend requests you've received, to accept or decline.")
                 + " " + i18n.tr("friends.needs_online", "This uses Nintendo's servers, so the console must be able to go online.");
            break;
        default: {
            title = "Discord";
            const bool ready = m_discord && m_discord->started() && m_discord->state() == DiscordService::State::Ready;
            if (ready) {
                const int on = m_discord->friendsOnline(), unread = m_discord->unreadDms();
                body = i18n.tr("social.discord_signed_in", "Signed in as") + " " + m_discord->myName() + ". "
                     + std::to_string(on) + " " + i18n.tr("social.discord_online", "friends online") + ", "
                     + std::to_string(unread) + " " + i18n.tr("social.discord_unread", "unread conversations") + ".";
            } else {
                body = i18n.tr("social.discord_body",
                               "Your Discord messages and who's online, in a panel on the left. Sign in once with "
                               "the Discord app on your phone.");
            }
            hint = i18n.tr("social.discord_open", "Press A to open Discord");
            break;
        }
    }
    const nxui::Rect ic{x, r.y + 30.f, 112.f, 112.f};
    drawTileIcon(ren, m_tile, ic, a);
    ren.drawText(fit(m_font, title, w - 140.f, 1.2f), {ic.x + ic.width + 26.f, ic.y + 36.f}, m_font, text, 1.2f);
    float y = ic.y + ic.height + 32.f;
    for (const auto& l : wrap(m_font, body, w, 0.82f)) {
        ren.drawText(l, {x, y}, m_font, text, 0.82f);
        y += 30.f;
    }
    ren.drawText(hint, {x, y + 14.f}, m_smallFont, accent, 0.86f);
}

void SocialScreen::onRender(nxui::Renderer& ren) {
    if (m_anim <= 0.001f || !m_font || !m_smallFont || !m_theme) return;
    auto& i18n = nxui::I18n::instance();
    const float a = m_anim;
    const nxui::Color text = m_theme->textPrimary.withAlpha(a);
    const nxui::Color muted = m_theme->textSecondary.withAlpha(a);
    const nxui::Color accent = m_theme->cursorNormal;

    ren.drawRect({0.f, 0.f, 1280.f, 720.f}, nxui::Color(0.f, 0.f, 0.f, 0.4f * a));
    const nxui::Rect p = panelRect();
    ren.drawRoundedRect(p, m_theme->panelBase.withAlpha(0.98f * a), 28.f);
    ren.drawRoundedRectOutline(p, m_theme->panelBorder.withAlpha(a), 28.f, 1.5f);

    // Header: the title, and you in the corner.
    if (const nxui::Texture* t = icon(1))
        ren.drawTexture(t, {p.x + 28.f, p.y + 20.f, 44.f, 44.f}, nxui::Color::white().withAlpha(a));
    ren.drawText(i18n.tr("sidebar.social", "Social"), {p.x + 84.f, p.y + 22.f}, m_font, text, 1.15f);
    if (!m_myName.empty()) {
        const float nameW = std::min(260.f, m_font->measure(m_myName).x * 0.82f);
        const float right = p.x + kPanelW - 28.f;
        drawAvatar(ren, m_myAvatar.get(), m_myName, {right - 48.f, p.y + 18.f, 48.f, 48.f}, a);
        ren.drawText(fit(m_font, m_myName, 260.f, 0.82f), {right - 60.f - nameW, p.y + 20.f}, m_font, text, 0.82f);
        int online = 0;
        for (const auto& f : m_friends) online += f.data.status != 0;
        const std::string sub = std::to_string(online) + " " + i18n.tr("social.friends_online", "friends online");
        ren.drawText(sub, {right - 60.f - m_smallFont->measure(sub).x * 0.72f, p.y + 48.f}, m_smallFont, muted, 0.72f);
    }
    ren.drawRect({p.x + 28.f, p.y + kHeaderH - 6.f, kPanelW - 56.f, 1.f}, m_theme->panelBorder.withAlpha(a));

    // Quick buttons.
    for (int i = 0; i < kTiles; ++i) {
        const nxui::Rect t = tileRect(i);
        const bool sel = m_zone == Zone::Tiles && m_tile == i && m_active;
        ren.drawRoundedRect(t, sel ? accent.withAlpha(0.16f * a) : m_theme->textPrimary.withAlpha(0.06f * a), 20.f);
        if (sel) ren.drawRoundedRectOutline(t, accent.withAlpha(0.9f * a), 20.f, 2.5f);
        drawTileIcon(ren, i, {t.x + (t.width - 50.f) * 0.5f, t.y + 12.f, 50.f, 50.f}, a);
        static const char* const kKeys[kTiles][2] = {{"social.tile_mii", "Mii Editor"}, {"social.tile_add", "Add Friend"},
                                                     {"social.tile_requests", "Requests"}, {"social.tile_discord", "Discord"}};
        const std::string label = fit(m_smallFont, i18n.tr(kKeys[i][0], kKeys[i][1]), t.width - 10.f, 0.7f);
        ren.drawText(label, {t.x + (t.width - m_smallFont->measure(label).x * 0.7f) * 0.5f, t.y + 72.f},
                     m_smallFont, text, 0.7f);
        if (i == 3 && m_discord && m_discord->started()) {
            const int unread = m_discord->unreadDms();
            if (unread > 0) {
                const std::string n = unread > 9 ? "9+" : std::to_string(unread);
                const nxui::Vec2 c{t.x + t.width - 14.f, t.y + 14.f};
                ren.drawCircle(c, 12.f, kRed.withAlpha(a), 24);
                ren.drawText(n, {c.x - m_smallFont->measure(n).x * 0.34f, c.y - 9.f}, m_smallFont,
                             nxui::Color(1.f, 1.f, 1.f, a), 0.68f);
            } else if (m_discord->state() == DiscordService::State::Ready) {
                ren.drawCircle({t.x + t.width - 14.f, t.y + 14.f}, 6.f, kOnline.withAlpha(a), 20);
            }
        }
    }

    // Friends.
    const nxui::Rect list = listRect();
    int online = 0;
    for (const auto& f : m_friends) online += f.data.status != 0;
    std::string head = i18n.tr("social.friends", "Friends");
    if (!m_friends.empty())
        head += "  " + std::to_string(m_friends.size()) + "  \xC2\xB7  " + std::to_string(online) + " "
              + i18n.tr("social.online_lower", "online");
    ren.drawText(head, {list.x + 8.f, list.y - 36.f}, m_smallFont, muted, 0.82f);

    if (m_friends.empty()) {
        std::string msg;
        if (!accountUidIsValid(&m_uid))
            msg = i18n.tr("friends.no_profile", "Choose a profile with the profile button first.");
        else if (m_loading || !m_loaded)
            msg = i18n.tr("social.loading", "Loading your friends") + std::string((size_t)(1 + (int)(armTicksToNs(armGetSystemTick()) / 400'000'000ULL) % 3), '.');
        else
            msg = i18n.tr("social.no_friends", "No friends yet. Add some with Add Friend.")
                + (R_FAILED(m_rc) || !m_diag.empty() ? "  (" + m_diag + ")" : std::string());
        float y = list.y + 10.f;
        for (const auto& l : wrap(m_smallFont, msg, list.width - 20.f, 0.82f)) {
            ren.drawText(l, {list.x + 8.f, y}, m_smallFont, muted, 0.82f);
            y += 26.f;
        }
    } else {
        const float total = kRowH * m_friends.size();
        if (m_followSel && m_zone == Zone::Friends) {
            const float top = m_sel * kRowH;
            if (top < m_scroll) m_scroll = top;
            if (top + kRowH > m_scroll + list.height) m_scroll = top + kRowH - list.height;
        }
        m_scroll = std::clamp(m_scroll, 0.f, std::max(0.f, total - list.height));
        ren.pushClipRect(list);
        for (int i = 0; i < (int)m_friends.size(); ++i) {
            const float y = list.y + i * kRowH - m_scroll;
            if (y + kRowH < list.y || y > list.y + list.height) continue;
            const Friend& f = m_friends[(size_t)i];
            const nxui::Rect row{list.x, y + 2.f, list.width, kRowH - 4.f};
            const bool sel = m_zone == Zone::Friends && m_sel == i && m_active;
            const nxui::Color rowBg = sel ? accent.withAlpha(0.16f * a) : m_theme->panelBase.withAlpha(a);
            if (sel) {
                ren.drawRoundedRect(row, rowBg, 16.f);
                ren.drawRoundedRectOutline(row, accent.withAlpha(0.9f * a), 16.f, 2.5f);
            }
            const nxui::Rect av{row.x + 10.f, row.y + (row.height - 46.f) * 0.5f, 46.f, 46.f};
            drawAvatar(ren, f.avatar.get(), f.data.nickname, av, a);
            const nxui::Vec2 dot{av.x + av.width - 6.f, av.y + av.height - 6.f};
            ren.drawCircle(dot, 9.f, sel ? m_theme->panelBase.withAlpha(a) : rowBg, 20);
            ren.drawCircle(dot, 6.5f, statusColor(f.data, a), 20);
            if (f.data.status == 0) ren.drawCircle(dot, 3.f, m_theme->panelBase.withAlpha(a), 16);
            const float tx = av.x + av.width + 14.f;
            const float tw = row.x + row.width - tx - (f.data.favourite ? 40.f : 12.f);
            ren.drawText(fit(m_font, f.data.nickname, tw, 0.8f), {tx, row.y + 8.f}, m_font, text, 0.8f);
            ren.drawText(fit(m_smallFont, statusLine(f.data), tw, 0.72f), {tx, row.y + 35.f}, m_smallFont,
                         f.data.status != 0 ? statusColor(f.data, a) : muted, 0.72f);
            if (f.data.favourite) { // a gold star for best friends
                const nxui::Vec2 c{row.x + row.width - 22.f, row.y + row.height * 0.5f};
                const float R = 10.f, rr = 4.2f;
                nxui::Vec2 pts[10];
                for (int k = 0; k < 10; ++k) {
                    const float ang = -1.5707963f + k * 0.6283185f;
                    const float rad = (k % 2) ? rr : R;
                    pts[k] = {c.x + std::cos(ang) * rad, c.y + std::sin(ang) * rad};
                }
                for (int k = 0; k < 10; ++k)
                    ren.drawTriangle(c, pts[k], pts[(k + 1) % 10], kGold.withAlpha(a));
            }
        }
        ren.popClipRect();
    }

    // The selected friend or button, on the right.
    const nxui::Rect detail{p.x + 28.f + kLeftW + 28.f, p.y + kHeaderH + 10.f,
                            kPanelW - kLeftW - 84.f, kPanelH - kHeaderH - 10.f - kFooterH};
    drawDetail(ren, detail, a);

    const std::string hints = m_zone == Zone::Tiles ? i18n.tr("social.hint_tiles", "A  Open     B  Close")
                                                    : i18n.tr("social.hint_list", "B  Close");
    ren.drawText(hints, {p.x + kPanelW - 28.f - m_smallFont->measure(hints).x * 0.78f, p.y + kPanelH - 36.f},
                 m_smallFont, muted, 0.78f);
}

void SocialScreen::handleTouch(const nxui::Input& in) {
    if (!m_active) return;
    const float tx = in.touchX(), ty = in.touchY();
    const nxui::Rect p = panelRect();
    if (in.touchDown()) {
        if (!inside(p, tx, ty)) { // a tap outside closes it
            hide();
            return;
        }
        m_touchDragging = false;
        m_touchScrollStart = m_scroll;
    }
    if (!in.isTouching() && !in.touchUp()) return;
    if (!inside(p, in.touchStartX(), in.touchStartY())) return;
    const nxui::Rect list = listRect();
    if (inside(list, in.touchStartX(), in.touchStartY()) && std::abs(in.touchDeltaY()) > 14.f)
        m_touchDragging = true;
    if (m_touchDragging) {
        m_scroll = std::max(0.f, m_touchScrollStart - in.touchDeltaY());
        m_followSel = false;
        return;
    }
    if (!in.touchUp()) return;
    for (int i = 0; i < kTiles; ++i)
        if (inside(tileRect(i), tx, ty)) {
            m_zone = Zone::Tiles;
            m_tile = i;
            activateItem();
            return;
        }
    if (inside(list, tx, ty)) {
        const int i = (int)((ty - list.y + m_scroll) / kRowH);
        if (i >= 0 && i < (int)m_friends.size()) {
            m_zone = Zone::Friends;
            m_sel = i;
            if (m_cb.onNavigate) m_cb.onNavigate();
        }
    }
}
