#include "widgets/SocialScreen.hpp"
#include "core/BackgroundThread.hpp"
#include "launcher/AppListLoader.hpp"
#include "net/http.h"
#include "themeshop/ThemeHttp.hpp"

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
constexpr float kHistRowH = 62.f;
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

}  // namespace

// Shared with the game Information page (declared in FriendStore.hpp).
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

std::string hoursText(std::uint64_t minutes) {
    auto& i18n = nxui::I18n::instance();
    char buf[48];
    const std::uint64_t h = minutes / 60, m = minutes % 60;
    if (h == 0)
        std::snprintf(buf, sizeof(buf), "%u %s", (unsigned)m, i18n.tr("social.min", "min").c_str());
    else if (h < 10 && m != 0)
        std::snprintf(buf, sizeof(buf), "%u h %u %s", (unsigned)h, (unsigned)m, i18n.tr("social.min", "min").c_str());
    else
        std::snprintf(buf, sizeof(buf), "%u h", (unsigned)h);
    return buf;
}

SocialScreen::SocialScreen() {
    setRect({0.f, 0.f, 1280.f, 720.f});
    setVisible(false);
}

void SocialScreen::setTitles(std::vector<std::pair<std::uint64_t, std::string>> titles) {
    m_titles.clear();
    for (auto& t : titles) m_titles[t.first] = std::move(t.second);
}

void SocialScreen::show(AccountUid uid, std::string name, std::shared_ptr<nxui::Texture> avatar) {
    const bool sameUser = std::memcmp(&uid, &m_uid, sizeof(uid)) == 0;
    m_uid = uid;
    m_myName = std::move(name);
    m_myAvatar = std::move(avatar);
    m_active = true;
    m_zone = Zone::Tiles;
    m_mode = Mode::Friends;
    m_add = Add::Idle;
    m_haveCandidate = false;
    m_deleteArmed = -1;
    m_tile = 0;
    m_sel = 0;
    m_scroll = 0.f;
    m_detailScroll = 0.f;
    m_followSel = true;
    m_notice.clear();
    if (!sameUser) {
        m_friends.clear();
        m_loaded = false;
        m_histories.clear();
        m_requests.clear();
        m_requestsLoaded = false;
        m_requestCount = -1;
        m_friendCode.clear();
    }
    // Statuses and play times change: read them again if more than a minute old.
    if (!m_loaded || armTicksToNs(armGetSystemTick() - m_loadedAt) > 60'000'000'000ULL) {
        startLoad();
        m_histories.clear();
    } else {
        m_histories.erase(0); // yours changes with every game
    }
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
    addAction(static_cast<uint64_t>(nxui::Button::B), [this]() { back(); });
    addAction(static_cast<uint64_t>(nxui::Button::X), [this]() { pressX(); });
    addAction(static_cast<uint64_t>(nxui::Button::Y), [this]() { pressY(); });
}

int SocialScreen::listCount() const {
    if (m_mode == Mode::Miis) return 1 + (int)m_miis.size(); // "New Mii", then yours
    return m_mode == Mode::Friends ? 1 + (int)m_friends.size() : (int)m_requests.size();
}

std::uint64_t SocialScreen::selectedKey(bool& valid) const {
    valid = false;
    if (m_mode != Mode::Friends || m_zone == Zone::Tiles) return 0;
    if (m_sel == 0) {
        valid = true;
        return 0;
    }
    const int i = m_sel - 1;
    if (i < 0 || i >= (int)m_friends.size() || m_friends[(size_t)i].data.networkId == 0) return 0;
    valid = true;
    return m_friends[(size_t)i].data.networkId;
}

void SocialScreen::notice(std::string text) {
    m_notice = std::move(text);
    m_noticeAt = armGetSystemTick();
}

std::string SocialScreen::titleName(std::uint64_t id) const {
    auto it = m_titles.find(id);
    if (it != m_titles.end() && !it->second.empty()) return it->second;
    char buf[48];
    std::snprintf(buf, sizeof(buf), "%s %016lX", nxui::I18n::instance().tr("social.game", "Game").c_str(),
                  (unsigned long)id);
    return buf;
}

std::string SocialScreen::playTime(const PlayRecord& r) const {
    if (!r.haveTime) return {};
    if (r.minutes == 0) return nxui::I18n::instance().tr("social.under_minute", "Less than a minute");
    return hoursText(r.minutes);
}

std::string SocialScreen::errorText(Result rc) const {
    char buf[64];
    std::snprintf(buf, sizeof(buf), "%s %04u-%04u", nxui::I18n::instance().tr("social.error", "Error").c_str(),
                  2000 + R_MODULE(rc), R_DESCRIPTION(rc));
    return buf;
}

void SocialScreen::startLoad() {
    if (m_loading || !accountUidIsValid(&m_uid)) return; // one at a time: the reader shares buffers
    auto job = std::make_shared<Loaded>();
    m_loading = job;
    const AccountUid uid = m_uid;
    runInBackground([job, uid]() {
        nxui::moveToWorkerCores();
        Result rc = 0;
        std::string diag;
        std::vector<FriendEntry> list = loadFriends(uid, rc, diag);
        const int requests = receivedFriendRequestCount(uid);
        std::lock_guard<std::mutex> lk(job->mu);
        job->list = std::move(list);
        job->rc = rc;
        job->diag = std::move(diag);
        job->requests = requests;
        job->done = true;
    });
}

// The selected person's play history, read once (one reader at a time).
void SocialScreen::ensureHistory() {
    if (m_historyJob || !accountUidIsValid(&m_uid)) return;
    bool valid = false;
    const std::uint64_t key = selectedKey(valid);
    if (!valid || m_histories.count(key)) return;
    auto job = std::make_shared<History>();
    m_historyJob = job;
    m_historyKey = key;
    const AccountUid uid = m_uid;
    std::vector<std::uint64_t> ids;
    if (key == 0)
        for (const auto& t : m_titles) ids.push_back(t.first);
    std::vector<std::uint64_t> have;
    for (const auto& t : m_titleIcons) have.push_back(t.first);
    for (const auto& t : m_iconJpegs) have.push_back(t.first);
    runInBackground([job, uid, key, ids = std::move(ids), have = std::move(have)]() {
        nxui::moveToWorkerCores();
        Result rc = 0;
        std::string code;
        std::vector<PlayRecord> records;
        if (key == 0) {
            code = loadFriendCode(uid);
            records = loadMyPlayHistory(uid, ids);
        } else {
            records = loadFriendPlayLog(uid, key, rc);
        }
        std::vector<std::pair<std::uint64_t, std::vector<std::uint8_t>>> icons;
        for (const auto& r : records) {
            if (icons.size() >= 40) break;
            if (std::find(have.begin(), have.end(), r.titleId) != have.end()) continue;
            std::vector<std::uint8_t> data = AppListLoader::loadIconData(r.titleId);
            if (!data.empty()) icons.emplace_back(r.titleId, std::move(data));
        }
        std::lock_guard<std::mutex> lk(job->mu);
        job->rc = rc;
        job->friendCode = std::move(code);
        job->records = std::move(records);
        job->icons = std::move(icons);
        job->done = true;
    });
}

void SocialScreen::startJob(JobKind kind, std::function<void(Job&)> work) {
    if (m_job) return;
    auto job = std::make_shared<Job>();
    m_job = job;
    m_jobKind = kind;
    runInBackground([job, work = std::move(work)]() {
        nxui::moveToWorkerCores();
        work(*job); // read by the menu only after `done`, under the lock
        std::lock_guard<std::mutex> lk(job->mu);
        job->done = true;
    });
}

void SocialScreen::finishJob() {
    auto& i18n = nxui::I18n::instance();
    Job& j = *m_job;
    switch (m_jobKind) {
        case JobKind::Requests:
            m_requests = std::move(j.list);
            m_requestsRc = j.rc;
            m_requestsLoaded = true;
            if (R_SUCCEEDED(j.rc)) m_requestCount = (int)m_requests.size();
            m_sel = std::clamp(m_sel, 0, std::max(0, (int)m_requests.size() - 1));
            break;
        case JobKind::Accept:
        case JobKind::Reject: {
            const bool accepted = m_jobKind == JobKind::Accept;
            auto it = std::find_if(m_requests.begin(), m_requests.end(),
                                   [this](const FriendRequestEntry& q) { return q.requestId == m_answering; });
            const std::string who = it != m_requests.end() && !it->nickname.empty() ? it->nickname
                                  : i18n.tr("social.them", "them");
            if (R_SUCCEEDED(j.rc)) {
                if (it != m_requests.end()) m_requests.erase(it);
                m_requestCount = (int)m_requests.size();
                m_sel = std::clamp(m_sel, 0, std::max(0, (int)m_requests.size() - 1));
                if (accepted) {
                    notice(i18n.tr("social.now_friends", "You're now friends with") + " " + who + ".");
                    m_loadedAt = 0;
                    startLoad();
                } else {
                    notice(i18n.tr("social.declined", "Declined the request from") + " " + who + ".");
                }
            } else {
                notice(i18n.tr("social.answer_failed", "That didn't go through.") + " (" + errorText(j.rc) + ")");
            }
            m_answering = 0;
            break;
        }
        case JobKind::Lookup:
            if (R_SUCCEEDED(j.rc) && j.networkId != 0) {
                m_add = Add::Found;
                m_addNetworkId = j.networkId;
                m_addName = j.name.empty() ? m_addCode : j.name;
            } else {
                m_add = Add::Failed;
                m_addRc = R_FAILED(j.rc) ? j.rc : MAKERESULT(Module_Libnx, LibnxError_NotFound);
            }
            break;
        case JobKind::Send:
            m_add = R_SUCCEEDED(j.rc) ? Add::Sent : Add::Failed;
            m_addRc = j.rc;
            break;
        case JobKind::MiiLoad:
            m_miis = std::move(j.miis);
            m_miisRc = j.rc;
            m_miisLoaded = true;
            m_sel = std::clamp(m_sel, 0, std::max(0, listCount() - 1));
            break;
        case JobKind::MiiRandom:
            if (R_SUCCEEDED(j.rc)) {
                m_candidate = std::move(j.mii);
                m_haveCandidate = true;
            } else {
                notice(i18n.tr("mii.random_failed", "Couldn't make a Mii.") + " (" + errorText(j.rc) + ")");
            }
            break;
        case JobKind::MiiAdd:
        case JobKind::MiiRename:
        case JobKind::MiiDelete:
        case JobKind::MiiSave:
            if (R_SUCCEEDED(j.rc)) {
                notice(m_jobKind == JobKind::MiiAdd ? i18n.tr("mii.added", "Your new Mii is saved.")
                       : m_jobKind == JobKind::MiiRename ? i18n.tr("mii.renamed", "Renamed.")
                       : m_jobKind == JobKind::MiiSave ? i18n.tr("mii.looks_saved", "New looks saved.")
                                                       : i18n.tr("mii.deleted", "Deleted."));
                if (m_jobKind == JobKind::MiiAdd) m_haveCandidate = false;
            } else {
                notice(i18n.tr("mii.failed", "That didn't work.") + " (" + errorText(j.rc) + ")");
            }
            m_miisLoaded = false; // read them again
            break;
        case JobKind::None:
            break;
    }
    m_job.reset();
    m_jobKind = JobKind::None;
}

void SocialScreen::startAddFriend() {
    auto& i18n = nxui::I18n::instance();
    if (!m_cb.onEnterText) return;
    if (!accountUidIsValid(&m_uid)) {
        notice(i18n.tr("friends.no_profile", "Choose a profile with the profile button first."));
        return;
    }
    m_cb.onEnterText(i18n.tr("friends.add", "Add Friend"),
                     i18n.tr("social.code_guide", "Their friend code, like SW-1234-5678-9012"), std::string(), 17,
                     [this](const std::string& typed) {
        if (typed.empty() || !m_active) return;
        if (m_job) {
            notice(nxui::I18n::instance().tr("social.busy", "Still busy with the last thing. Try again in a moment."));
            return;
        }
        m_addCode = typed;
        m_addName.clear();
        m_addRc = 0;
        m_add = Add::Looking;
        m_zone = Zone::Tiles;
        m_tile = 1;
        const AccountUid uid = m_uid;
        startJob(JobKind::Lookup, [uid, typed](Job& j) { j.rc = findByFriendCode(uid, typed, j.networkId, j.name); });
    });
}

void SocialScreen::openRequests() {
    m_mode = Mode::Requests;
    m_zone = Zone::List;
    m_sel = 0;
    m_scroll = 0.f;
    m_followSel = true;
    m_requestsLoaded = false; // read again every time: they arrive from Nintendo's servers
}

void SocialScreen::answer(bool accept) {
    if (m_add != Add::Idle || m_mode != Mode::Requests || m_zone != Zone::List || m_job) return;
    if (m_sel < 0 || m_sel >= (int)m_requests.size()) return;
    const std::uint64_t id = m_requests[(size_t)m_sel].requestId;
    m_answering = id;
    if (m_cb.onActivate) m_cb.onActivate();
    const AccountUid uid = m_uid;
    startJob(accept ? JobKind::Accept : JobKind::Reject,
             [uid, id, accept](Job& j) { j.rc = answerFriendRequest(uid, id, accept); });
}

void SocialScreen::move(int dx, int dy) {
    if (m_editing) { // the Mii Maker: up/down choose a feature, left/right change it
        if (dy != 0) {
            const int row = std::clamp(m_editRow + dy, 0, kMiiFeatureCount - 1);
            if (row != m_editRow && m_cb.onNavigate) m_cb.onNavigate();
            m_editRow = row;
        } else if (dx != 0) {
            editChange(dx);
        }
        return;
    }
    if (m_add != Add::Idle) return; // the Add Friend card has the buttons until it's closed
    const int n = listCount();
    bool moved = false;
    switch (m_zone) {
        case Zone::Tiles:
            if (dx != 0) {
                const int t = std::clamp(m_tile + dx, 0, kTiles - 1);
                moved = t != m_tile;
                m_tile = t;
            } else if (dy > 0 && n > 0) {
                m_zone = Zone::List;
                m_sel = std::clamp(m_sel, 0, n - 1);
                moved = true;
            }
            break;
        case Zone::List:
            if (dx > 0 && m_mode == Mode::Friends) {
                bool valid = false;
                auto it = m_histories.find(selectedKey(valid));
                if (valid && it != m_histories.end() && !it->second.records.empty()) {
                    m_zone = Zone::Detail;
                    moved = true;
                }
            } else if (dy != 0) {
                if (m_sel + dy < 0) {
                    m_zone = Zone::Tiles;
                    moved = true;
                } else if (m_sel + dy < n) {
                    m_sel += dy;
                    m_detailScroll = 0.f;
                    moved = true;
                }
            }
            break;
        case Zone::Detail:
            if (dx < 0) {
                m_zone = Zone::List;
                moved = true;
            } else if (dy != 0) {
                m_detailScroll = std::max(0.f, m_detailScroll + dy * kHistRowH);
                moved = true;
            }
            break;
    }
    if (moved) {
        m_followSel = true;
        if (m_cb.onNavigate) m_cb.onNavigate();
    }
}

void SocialScreen::activateItem() {
    if (m_editing) { // save the new looks
        if (m_job) return;
        const MiiEntry mii = m_editOriginal;
        const int index = m_editIndex;
        const MiiCharInfo info = m_edit.info;
        endEdit();
        startMiiJob(JobKind::MiiSave, [mii, index, info](Job& j) { j.rc = updateMii(mii, index, info); });
        return;
    }
    switch (m_add) {
        case Add::Found: {
            if (m_job) return;
            if (m_cb.onActivate) m_cb.onActivate();
            m_add = Add::Sending;
            const AccountUid uid = m_uid;
            const std::uint64_t id = m_addNetworkId;
            startJob(JobKind::Send, [uid, id](Job& j) { j.rc = sendFriendRequest(uid, id); });
            return;
        }
        case Add::Sent:
        case Add::Failed:
            m_add = Add::Idle;
            return;
        case Add::Looking:
        case Add::Sending:
            return;
        case Add::Idle:
            break;
    }
    if (m_zone == Zone::Tiles) {
        if (m_cb.onActivate) m_cb.onActivate();
        switch (m_tile) {
            case 0: openMiis(); break;
            case 1: startAddFriend(); break;
            case 2: openRequests(); break;
            case 3: if (m_cb.onDiscord) m_cb.onDiscord(); break;
        }
        return;
    }
    if (m_mode == Mode::Requests) {
        answer(true);
        return;
    }
    if (m_mode == Mode::Miis) {
        if (m_zone != Zone::List || m_job || !m_cb.onEnterText) return;
        auto& i18n = nxui::I18n::instance();
        if (m_sel == 0 && !m_haveCandidate) { // roll a new one to look at
            const int gender = m_candidateGender;
            startMiiJob(JobKind::MiiRandom, [gender](Job& j) { j.rc = buildRandomMii(gender, j.mii); });
            return;
        }
        if (m_sel == 0) { // keep it: name it, then save it
            m_cb.onEnterText(i18n.tr("mii.name_title", "Name your Mii"),
                             i18n.tr("mii.name_guide", "Up to 10 letters"), std::string(), 10,
                             [this](const std::string& name) {
                if (name.empty() || !m_active || m_job || !m_haveCandidate) return;
                const MiiCharInfo info = m_candidate.info;
                startMiiJob(JobKind::MiiAdd, [info, name](Job& j) { j.rc = addMii(info, name); });
            });
            return;
        }
        if (m_sel - 1 < (int)m_miis.size()) { // rename
            const MiiEntry mii = m_miis[(size_t)(m_sel - 1)];
            const int index = m_sel - 1;
            m_cb.onEnterText(i18n.tr("mii.rename_title", "Rename"), i18n.tr("mii.name_guide", "Up to 10 letters"),
                             mii.name, 10, [this, mii, index](const std::string& name) {
                if (name.empty() || !m_active || m_job) return;
                startMiiJob(JobKind::MiiRename, [mii, index, name](Job& j) { j.rc = renameMii(mii, index, name); });
            });
        }
        return;
    }
    if (m_zone == Zone::List) move(+1, 0); // into the play history
}

namespace {

// What HomeNX's Mii Maker changes, with how many choices the console allows.
struct MiiFeature {
    const char* key;
    const char* label;
    u8 MiiCharInfo::*field;
    int count;
    int step; // per press
};
const MiiFeature kMiiFeatures[] = {
    {"mii.f.gender", "Boy or girl", &MiiCharInfo::mii_sex, 2, 1},
    {"mii.f.skin", "Skin colour", &MiiCharInfo::mii_face_color, 10, 1},
    {"mii.f.face", "Face shape", &MiiCharInfo::mii_face_shape, 12, 1},
    {"mii.f.makeup", "Make-up", &MiiCharInfo::mii_makeup_style, 12, 1},
    {"mii.f.wrinkles", "Wrinkles", &MiiCharInfo::mii_wrinkles_style, 12, 1},
    {"mii.f.hair", "Hairstyle", &MiiCharInfo::mii_hair_style, 132, 1},
    {"mii.f.hair_colour", "Hair colour", &MiiCharInfo::mii_hair_color, 100, 1},
    {"mii.f.flip", "Part the other way", &MiiCharInfo::mii_has_hair_flipped, 2, 1},
    {"mii.f.eyebrows", "Eyebrows", &MiiCharInfo::mii_eyebrow_style, 24, 1},
    {"mii.f.eyebrow_colour", "Eyebrow colour", &MiiCharInfo::mii_eyebrow_color, 100, 1},
    {"mii.f.eyes", "Eyes", &MiiCharInfo::mii_eye_style, 60, 1},
    {"mii.f.eye_colour", "Eye colour", &MiiCharInfo::mii_eye_color, 100, 1},
    {"mii.f.nose", "Nose", &MiiCharInfo::mii_nose_style, 18, 1},
    {"mii.f.mouth", "Mouth", &MiiCharInfo::mii_mouth_style, 36, 1},
    {"mii.f.lip_colour", "Lip colour", &MiiCharInfo::mii_mouth_color, 100, 1},
    {"mii.f.beard", "Beard", &MiiCharInfo::mii_beard_style, 6, 1},
    {"mii.f.mustache", "Mustache", &MiiCharInfo::mii_mustache_style, 6, 1},
    {"mii.f.facial_hair_colour", "Beard colour", &MiiCharInfo::mii_facial_hair_color, 100, 1},
    {"mii.f.glasses", "Glasses", &MiiCharInfo::mii_glasses_style, 20, 1},
    {"mii.f.glasses_colour", "Glasses colour", &MiiCharInfo::mii_glasses_color, 100, 1},
    {"mii.f.mole", "Mole", &MiiCharInfo::mii_has_mole, 2, 1},
    {"mii.f.height", "Height", &MiiCharInfo::mii_height, 128, 8},
    {"mii.f.build", "Build", &MiiCharInfo::mii_width, 128, 8},
    {"mii.f.colour", "Favourite colour", &MiiCharInfo::mii_color, 12, 1},
};
constexpr int kMiiFeatureCount = static_cast<int>(sizeof(kMiiFeatures) / sizeof(kMiiFeatures[0]));

} // namespace

void SocialScreen::showProfilePage() {
    m_mode = Mode::Friends;
    m_zone = Zone::List;
    m_sel = 0;
    m_scroll = 0.f;
    m_detailScroll = 0.f;
    m_followSel = true;
}

void SocialScreen::startEdit(int index) {
    if (index < 0 || index >= (int)m_miis.size()) return;
    if (m_editFace && m_gpu) m_gpu->waitIdle(); // the last preview may still be drawing
    m_editFace.reset();
    m_editing = true;
    m_edit = m_miis[(size_t)index];
    m_editOriginal = m_edit;
    m_editIndex = index;
    m_editRow = 5; // hairstyle
    m_editChangedAt = 0;
    m_editFaceUrl.clear();
    if (m_cb.onActivate) m_cb.onActivate();
}

void SocialScreen::endEdit() {
    m_editing = false;
    m_editFaceJob.reset();
}

void SocialScreen::editChange(int delta) {
    const MiiFeature& f = kMiiFeatures[m_editRow];
    const int v = m_edit.info.*f.field;
    m_edit.info.*f.field = static_cast<u8>((((v + delta * f.step) % f.count) + f.count) % f.count);
    m_edit.faceUrl = miiFaceUrl(m_edit.info);
    m_editChangedAt = armGetSystemTick();
    if (m_cb.onNavigate) m_cb.onNavigate();
}

void SocialScreen::editRandom() {
    const MiiFeature& f = kMiiFeatures[m_editRow];
    u32 r = 0;
    randomGet(&r, sizeof(r));
    m_edit.info.*f.field = static_cast<u8>(r % (u32)f.count);
    m_edit.faceUrl = miiFaceUrl(m_edit.info);
    m_editChangedAt = armGetSystemTick();
    if (m_cb.onNavigate) m_cb.onNavigate();
}

// The edited face from Mii Studio, asked for once the changes settle; one
// picture at a time (each is a GPU texture).
void SocialScreen::updateEditFace() {
    if (m_editFaceJob) {
        std::vector<std::pair<std::string, std::vector<std::uint8_t>>> pngs;
        {
            std::lock_guard<std::mutex> lk(m_editFaceJob->mu);
            if (!m_editFaceJob->done) return;
            pngs = std::move(m_editFaceJob->pngs);
        }
        m_editFaceJob.reset();
        if (!pngs.empty() && !pngs.front().second.empty() && m_gpu && m_ren) {
            auto tex = std::make_unique<nxui::Texture>();
            if (tex->loadFromMemory(*m_gpu, *m_ren, pngs.front().second.data(), pngs.front().second.size(), 256)) {
                if (m_editFace) m_gpu->waitIdle();
                m_editFace = std::move(tex);
            }
        }
    }
    const bool settled = armTicksToNs(armGetSystemTick() - m_editChangedAt) > 350'000'000ULL;
    if (!settled || m_edit.faceUrl == m_editFaceUrl || m_faces.count(m_edit.faceUrl)) return;
    if (!themeshop::http::initialize()) return;
    m_editFaceUrl = m_edit.faceUrl;
    auto job = std::make_shared<FaceJob>();
    m_editFaceJob = job;
    const std::string url = m_editFaceUrl;
    runInBackground([job, url]() {
        nxui::moveToWorkerCores();
        Http::Request req;
        req.url = url;
        req.timeoutMs = 10000;
        req.maxBytes = 1u << 20;
        const Http::Response res = Http::Perform(req);
        Http::ThreadCleanup();
        std::lock_guard<std::mutex> lk(job->mu);
        job->pngs.emplace_back(url, res.Ok() ? std::vector<std::uint8_t>(res.body.begin(), res.body.end())
                                             : std::vector<std::uint8_t>());
        job->done = true;
    });
}

void SocialScreen::drawMiiEditor(nxui::Renderer& ren, const nxui::Rect& r, float a) {
    auto& i18n = nxui::I18n::instance();
    const nxui::Color text = m_theme->textPrimary.withAlpha(a);
    const nxui::Color muted = m_theme->textSecondary.withAlpha(a);
    const nxui::Color accent = m_theme->cursorNormal;
    // The face: the latest picture (it follows each change after a moment).
    const nxui::Rect big{r.x + (r.width - 190.f) * 0.5f, r.y + 16.f, 190.f, 190.f};
    const nxui::Vec2 c{big.x + big.width * 0.5f, big.y + big.height * 0.5f};
    ren.drawCircle(c, big.width * 0.5f, nxui::Color(0.93f, 0.95f, 0.98f, 0.9f * a), 40);
    const nxui::Texture* t = face(m_edit.faceUrl);
    const bool current = t != nullptr || (m_editFace && m_editFaceUrl == m_edit.faceUrl && !m_editFaceJob);
    if (!t && m_editFace && m_editFace->valid()) t = m_editFace.get();
    if (!t) t = face(m_editOriginal.faceUrl);
    if (t) {
        ren.drawTexture(t, big.shrunk(big.width * 0.04f), nxui::Color::white().withAlpha(current ? a : 0.55f * a));
    } else {
        const nxui::Color shade(0.62f, 0.66f, 0.74f, a);
        ren.drawCircle({c.x, c.y - big.height * 0.1f}, big.width * 0.2f, shade, 32);
        ren.drawCircle({c.x, c.y + big.height * 0.36f}, big.width * 0.3f, shade, 32);
    }
    const std::string name = m_edit.name.empty() ? i18n.tr("mii.no_name", "No name") : m_edit.name;
    ren.drawText(fit(m_smallFont, name, r.width - 40.f, 0.8f),
                 {r.x + (r.width - m_smallFont->measure(name).x * 0.8f) * 0.5f, big.y + big.height + 6.f},
                 m_smallFont, muted, 0.8f);
    // The features: five around the chosen one.
    float y = big.y + big.height + 40.f;
    const int first = std::clamp(m_editRow - 2, 0, std::max(0, kMiiFeatureCount - 5));
    for (int i = first; i < first + 5 && i < kMiiFeatureCount; ++i) {
        const MiiFeature& f = kMiiFeatures[i];
        const bool sel = i == m_editRow;
        const nxui::Rect row{r.x + 22.f, y, r.width - 44.f, 42.f};
        if (sel) ren.drawRoundedRect(row, accent.withAlpha(0.2f * a), 12.f);
        ren.drawText(i18n.tr(f.key, f.label), {row.x + 16.f, row.y + 7.f}, m_font, sel ? text : muted, 0.74f);
        const int v = m_edit.info.*f.field;
        std::string value;
        if (f.field == &MiiCharInfo::mii_sex) value = v ? i18n.tr("mii.girl", "Girl") : i18n.tr("mii.boy", "Boy");
        else if (f.count == 2) value = v ? i18n.tr("mii.yes", "Yes") : i18n.tr("mii.no", "No");
        else value = std::to_string(v + 1) + " / " + std::to_string(f.count);
        if (sel) value = "<   " + value + "   >";
        const float vw = m_font->measure(value).x * 0.74f;
        ren.drawText(value, {row.x + row.width - 16.f - vw, row.y + 7.f}, m_font, sel ? accent.withAlpha(a) : muted,
                     0.74f);
        y += 46.f;
    }
}

void SocialScreen::openMiis() {
    m_mode = Mode::Miis;
    m_zone = Zone::List;
    m_sel = 0;
    m_scroll = 0.f;
    m_followSel = true;
    m_haveCandidate = false;
    m_deleteArmed = -1;
    m_miisLoaded = false; // read again: they change in the Mii Maker too
}

void SocialScreen::startMiiJob(JobKind kind, std::function<void(Job&)> work) {
    if (m_cb.onActivate) m_cb.onActivate();
    startJob(kind, std::move(work));
}

// X: decline a request, or in Miis: change the new Mii's gender / delete (twice).
void SocialScreen::pressX() {
    if (m_editing) return;
    if (m_mode != Mode::Miis) {
        answer(false);
        return;
    }
    if (m_zone != Zone::List || m_job) return;
    auto& i18n = nxui::I18n::instance();
    if (m_sel == 0) {
        if (!m_haveCandidate) return;
        m_candidateGender = (m_candidateGender + 1) % 3;
        const int gender = m_candidateGender;
        startMiiJob(JobKind::MiiRandom, [gender](Job& j) { j.rc = buildRandomMii(gender, j.mii); });
        return;
    }
    if (m_sel - 1 >= (int)m_miis.size()) return;
    const bool armed = m_deleteArmed == m_sel
        && armTicksToNs(armGetSystemTick() - m_deleteArmedAt) < 4'000'000'000ULL;
    if (!armed) {
        m_deleteArmed = m_sel;
        m_deleteArmedAt = armGetSystemTick();
        notice(i18n.tr("mii.delete_again", "Press X again to delete") + " " + m_miis[(size_t)(m_sel - 1)].name + ".");
        if (m_cb.onNavigate) m_cb.onNavigate();
        return;
    }
    m_deleteArmed = -1;
    const MiiCharInfo info = m_miis[(size_t)(m_sel - 1)].info;
    startMiiJob(JobKind::MiiDelete, [info](Job& j) { j.rc = deleteMii(info); });
}

// Y: another random Mii, a Mii's looks in HomeNX's Mii Maker (a surprise for
// the chosen feature there), or your play history most-played first.
void SocialScreen::pressY() {
    if (m_editing) {
        editRandom();
        return;
    }
    if (m_mode == Mode::Friends && m_zone != Zone::Tiles && m_sel == 0) {
        m_historyByTime = !m_historyByTime;
        m_detailScroll = 0.f;
        if (m_cb.onNavigate) m_cb.onNavigate();
        return;
    }
    if (m_mode != Mode::Miis || m_zone != Zone::List || m_job) return;
    if (m_sel == 0) {
        if (!m_haveCandidate) return;
        const int gender = m_candidateGender;
        startMiiJob(JobKind::MiiRandom, [gender](Job& j) { j.rc = buildRandomMii(gender, j.mii); });
        return;
    }
    startEdit(m_sel - 1);
}

// Faces for the Miis on show, downloaded a few at a time (Mii Studio).
void SocialScreen::ensureFaces() {
    if (m_faceJob) {
        bool done = false;
        {
            std::lock_guard<std::mutex> lk(m_faceJob->mu);
            done = m_faceJob->done;
        }
        if (!done) return;
        for (auto& png : m_faceJob->pngs)
            if (!png.second.empty()) m_facePngs[png.first] = std::move(png.second);
        m_faceJob.reset();
    }
    if (m_mode != Mode::Miis) return;
    std::vector<std::string> urls;
    if (m_haveCandidate && !m_faceTried.count(m_candidate.faceUrl)) urls.push_back(m_candidate.faceUrl);
    for (const MiiEntry& m : m_miis)
        if (urls.size() < 6 && !m_faceTried.count(m.faceUrl)) urls.push_back(m.faceUrl);
    if (urls.empty() || !themeshop::http::initialize()) return;
    for (const auto& u : urls) m_faceTried.insert(u);
    auto job = std::make_shared<FaceJob>();
    m_faceJob = job;
    runInBackground([job, urls]() {
        nxui::moveToWorkerCores();
        std::vector<std::pair<std::string, std::vector<std::uint8_t>>> pngs;
        for (const auto& u : urls) {
            Http::Request req;
            req.url = u;
            req.timeoutMs = 10000;
            req.maxBytes = 1u << 20;
            const Http::Response res = Http::Perform(req);
            pngs.emplace_back(u, res.Ok() ? std::vector<std::uint8_t>(res.body.begin(), res.body.end())
                                          : std::vector<std::uint8_t>());
        }
        Http::ThreadCleanup();
        std::lock_guard<std::mutex> lk(job->mu);
        job->pngs = std::move(pngs);
        job->done = true;
    });
}

const nxui::Texture* SocialScreen::face(const std::string& url) const {
    auto it = m_faces.find(url);
    return it != m_faces.end() && it->second && it->second->valid() ? it->second.get() : nullptr;
}

void SocialScreen::back() {
    if (m_editing) { // leave the Mii Maker without saving
        endEdit();
        if (m_cb.onNavigate) m_cb.onNavigate();
        return;
    }
    if (m_add == Add::Looking || m_add == Add::Sending) return;
    if (m_add != Add::Idle) {
        m_add = Add::Idle;
    } else if (m_zone == Zone::Detail) {
        m_zone = Zone::List;
    } else if (m_mode == Mode::Miis && m_haveCandidate && m_sel == 0) {
        m_haveCandidate = false; // not this one
    } else if (m_mode == Mode::Miis) {
        m_mode = Mode::Friends;
        m_zone = Zone::Tiles;
        m_tile = 0;
        m_sel = 0;
        m_scroll = 0.f;
    } else if (m_mode == Mode::Requests) {
        m_mode = Mode::Friends;
        m_zone = Zone::Tiles;
        m_tile = 2;
        m_sel = 0;
        m_scroll = 0.f;
    } else {
        hide();
        return;
    }
    if (m_cb.onNavigate) m_cb.onNavigate();
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
            if (m_loading->requests >= 0) m_requestCount = m_loading->requests;
            m_loaded = true;
            m_loadedAt = armGetSystemTick();
            m_sel = std::clamp(m_sel, 0, std::max(0, listCount() - 1));
        }
    }
    if (finished) m_loading.reset();

    bool historyDone = false;
    if (m_historyJob) {
        std::lock_guard<std::mutex> lk(m_historyJob->mu);
        historyDone = m_historyJob->done;
        if (historyDone) {
            HistoryView& v = m_histories[m_historyKey];
            v.loaded = true;
            v.rc = m_historyJob->rc;
            v.records = std::move(m_historyJob->records);
            if (m_historyKey == 0 && !m_historyJob->friendCode.empty()) m_friendCode = m_historyJob->friendCode;
            for (auto& icon : m_historyJob->icons)
                if (!m_titleIcons.count(icon.first)) m_iconJpegs[icon.first] = std::move(icon.second);
        }
    }
    if (historyDone) m_historyJob.reset();

    bool jobDone = false;
    if (m_job) {
        std::lock_guard<std::mutex> lk(m_job->mu);
        jobDone = m_job->done;
    }
    if (jobDone) finishJob();

    if (m_active) {
        ensureHistory();
        if (m_mode == Mode::Miis && !m_miisLoaded && !m_job)
            startJob(JobKind::MiiLoad, [](Job& j) { j.rc = loadMiis(j.miis); });
        ensureFaces();
        if (m_editing) updateEditFace();
        if (m_mode == Mode::Requests && !m_requestsLoaded && !m_job) {
            const AccountUid uid = m_uid;
            startJob(JobKind::Requests, [uid](Job& j) { j.list = loadFriendRequests(uid, j.rc); });
        }
    }

    // Pictures: a couple per frame (each is a small JPEG to decode).
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
        while (budget > 0 && !m_facePngs.empty()) {
            auto it = m_facePngs.begin();
            auto tex = std::make_unique<nxui::Texture>();
            if (tex->loadFromMemory(*m_gpu, *m_ren, it->second.data(), it->second.size(), 256))
                m_faces[it->first] = std::move(tex);
            m_facePngs.erase(it);
            --budget;
        }
        while (budget > 0 && !m_iconJpegs.empty()) {
            auto it = m_iconJpegs.begin();
            auto tex = std::make_unique<nxui::Texture>();
            if (tex->loadFromMemory(*m_gpu, *m_ren, it->second.data(), it->second.size(), 96))
                m_titleIcons[it->first] = std::move(tex);
            m_iconJpegs.erase(it);
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

nxui::Rect SocialScreen::detailRect() const {
    const nxui::Rect p = panelRect();
    return {p.x + 28.f + kLeftW + 28.f, p.y + kHeaderH + 10.f,
            kPanelW - kLeftW - 84.f, kPanelH - kHeaderH - 10.f - kFooterH};
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

void SocialScreen::drawMessage(nxui::Renderer& ren, const nxui::Rect& r, float a, const std::string& title,
                               const std::string& body) {
    const nxui::Color text = m_theme->textPrimary.withAlpha(a);
    const float x = r.x + 30.f, w = r.width - 60.f;
    ren.drawText(fit(m_font, title, w, 1.1f), {x, r.y + 30.f}, m_font, text, 1.1f);
    float y = r.y + 92.f;
    for (const auto& l : wrap(m_font, body, w, 0.82f)) {
        ren.drawText(l, {x, y}, m_font, text, 0.82f);
        y += 30.f;
    }
}

void SocialScreen::drawHistory(nxui::Renderer& ren, const nxui::Rect& r, float a, const HistoryView* hv,
                               bool mine) {
    auto& i18n = nxui::I18n::instance();
    const nxui::Color text = m_theme->textPrimary.withAlpha(a);
    const nxui::Color muted = m_theme->textSecondary.withAlpha(a);
    std::string head = mine ? i18n.tr("social.play_history", "Play history")
                            : i18n.tr("social.play_activity", "Play activity");
    if (hv && hv->loaded && !hv->records.empty()) {
        std::uint64_t total = 0;
        for (const auto& rec : hv->records) total += rec.minutes;
        head += "  \xC2\xB7  " + std::to_string(hv->records.size()) + " "
              + (hv->records.size() == 1 ? i18n.tr("social.game_lower", "game") : i18n.tr("social.games_lower", "games"));
        if (total > 0) head += "  \xC2\xB7  " + hoursText(total) + " " + i18n.tr("social.in_total", "in total");
        if (mine && m_historyByTime) head += "  \xC2\xB7  " + i18n.tr("social.most_played", "most played first");
    }
    ren.drawText(fit(m_smallFont, head, r.width - 28.f, 0.8f), {r.x + 14.f, r.y}, m_smallFont, muted, 0.8f);
    const nxui::Rect box{r.x, r.y + 32.f, r.width, r.height - 32.f};

    if (!hv || !hv->loaded) {
        const std::string dots((size_t)(1 + (int)(armTicksToNs(armGetSystemTick()) / 400'000'000ULL) % 3), '.');
        ren.drawText(i18n.tr("social.loading_history", "Loading") + dots, {box.x + 14.f, box.y + 6.f}, m_smallFont,
                     muted, 0.82f);
        return;
    }
    if (hv->records.empty()) {
        std::string msg = mine ? i18n.tr("social.no_history", "No games played on this profile yet.")
                               : i18n.tr("social.no_activity",
                                         "Nothing to show. They may keep their play activity private.");
        if (R_FAILED(hv->rc)) msg += "  (" + errorText(hv->rc) + ")";
        float y = box.y + 6.f;
        for (const auto& l : wrap(m_smallFont, msg, box.width - 28.f, 0.82f)) {
            ren.drawText(l, {box.x + 14.f, y}, m_smallFont, muted, 0.82f);
            y += 26.f;
        }
        return;
    }

    std::vector<PlayRecord> byTime;
    const std::vector<PlayRecord>* records = &hv->records;
    if (mine && m_historyByTime) {
        byTime = hv->records;
        std::stable_sort(byTime.begin(), byTime.end(),
                         [](const PlayRecord& x, const PlayRecord& y) { return x.minutes > y.minutes; });
        records = &byTime;
    }
    const float total = kHistRowH * records->size();
    m_detailScroll = std::clamp(m_detailScroll, 0.f, std::max(0.f, total - box.height));
    ren.pushClipRect(box);
    for (size_t i = 0; i < records->size(); ++i) {
        const float y = box.y + i * kHistRowH - m_detailScroll;
        if (y + kHistRowH < box.y || y > box.y + box.height) continue;
        const PlayRecord& rec = (*records)[i];
        const nxui::Rect row{box.x, y + 2.f, box.width, kHistRowH - 4.f};
        if (i % 2 == 0) ren.drawRoundedRect(row, m_theme->textPrimary.withAlpha(0.035f * a), 12.f);
        const nxui::Rect ic{row.x + 8.f, row.y + (row.height - 44.f) * 0.5f, 44.f, 44.f};
        auto ti = m_titleIcons.find(rec.titleId);
        if (ti != m_titleIcons.end() && ti->second && ti->second->valid())
            ren.drawTextureRounded(ti->second.get(), ic, 8.f, nxui::Color::white().withAlpha(a));
        else
            ren.drawRoundedRect(ic, m_theme->textPrimary.withAlpha(0.1f * a), 8.f);
        const std::string when = rec.lastPlayed > 0 ? ago(rec.lastPlayed) : std::string();
        const float whenW = when.empty() ? 0.f : m_smallFont->measure(when).x * 0.68f + 12.f;
        const float tx = ic.x + ic.width + 12.f;
        const float tw = row.x + row.width - tx - whenW - 8.f;
        ren.drawText(fit(m_font, titleName(rec.titleId), tw, 0.72f), {tx, row.y + 6.f}, m_font, text, 0.72f);
        std::string sub = playTime(rec);
        if (rec.launches > 0) {
            char buf[48];
            std::snprintf(buf, sizeof(buf), "%s %u %s", i18n.tr("social.played", "Played").c_str(),
                          (unsigned)rec.launches,
                          (rec.launches == 1 ? i18n.tr("social.time", "time") : i18n.tr("social.times", "times")).c_str());
            sub += (sub.empty() ? "" : "  \xC2\xB7  ") + std::string(buf);
        }
        ren.drawText(fit(m_smallFont, sub, row.x + row.width - tx - 8.f, 0.66f), {tx, row.y + 32.f}, m_smallFont,
                     muted, 0.66f);
        if (!when.empty())
            ren.drawText(when, {row.x + row.width - 8.f - m_smallFont->measure(when).x * 0.68f, row.y + 9.f},
                         m_smallFont, muted, 0.68f);
    }
    ren.popClipRect();
}

void SocialScreen::drawPerson(nxui::Renderer& ren, const nxui::Rect& r, float a) {
    auto& i18n = nxui::I18n::instance();
    const nxui::Color text = m_theme->textPrimary.withAlpha(a);
    const nxui::Color muted = m_theme->textSecondary.withAlpha(a);
    const bool mine = m_sel == 0;
    const Friend* f = !mine && m_sel - 1 < (int)m_friends.size() ? &m_friends[(size_t)(m_sel - 1)] : nullptr;
    if (!mine && !f) return;
    const std::string name = mine ? (m_myName.empty() ? i18n.tr("social.you", "You") : m_myName) : f->data.nickname;

    const float x = r.x + 26.f;
    const nxui::Rect av{x, r.y + 24.f, 104.f, 104.f};
    drawAvatar(ren, mine ? m_myAvatar.get() : f->avatar.get(), name, av, a);
    if (f) {
        const nxui::Vec2 dot{av.x + av.width - 14.f, av.y + av.height - 14.f};
        ren.drawCircle(dot, 15.f, m_theme->panelBase.withAlpha(a), 28);
        ren.drawCircle(dot, 11.f, statusColor(f->data, a), 28);
        if (f->data.status == 0) ren.drawCircle(dot, 5.f, m_theme->panelBase.withAlpha(a), 20);
    }
    const float tx = av.x + av.width + 22.f, tw = r.x + r.width - 26.f - tx;
    ren.drawText(fit(m_font, name, tw, 1.1f), {tx, av.y + 14.f}, m_font, text, 1.1f);
    float px = tx;
    auto pill = [&](const std::string& s, const nxui::Color& c) {
        const float pw = m_smallFont->measure(s).x * 0.78f + 26.f;
        ren.drawRoundedRect({px, av.y + 60.f, pw, 28.f}, c.withAlpha(0.18f * a), 14.f);
        ren.drawText(s, {px + 13.f, av.y + 64.f}, m_smallFont, c.withAlpha(a), 0.78f);
        px += pw + 8.f;
    };
    if (mine) {
        pill(i18n.tr("social.your_profile", "Your profile"), m_theme->cursorNormal);
    } else {
        pill(f->data.status == 2 ? i18n.tr("social.playing", "Playing online")
             : f->data.status == 1 ? i18n.tr("social.online", "Online") : i18n.tr("social.offline", "Offline"),
             statusColor(f->data, 1.f));
        if (f->data.favourite) pill(i18n.tr("social.best_friend", "Best friend"), kGold);
    }

    std::string label, value;
    if (mine) {
        label = i18n.tr("social.friend_code", "Friend code");
        value = m_friendCode.empty() ? i18n.tr("social.unknown", "Not known") : m_friendCode;
    } else if (f->data.status != 0) {
        label = i18n.tr("social.now", "Right now");
        value = f->data.status == 2 ? i18n.tr("social.now_playing", "Online and playing a game")
                                    : i18n.tr("social.now_online", "Online on their console");
    } else {
        label = i18n.tr("social.last_online", "Last online");
        value = f->data.lastOnline > 0 ? dateOf(f->data.lastOnline) + "  (" + ago(f->data.lastOnline) + ")"
                                       : i18n.tr("social.unknown", "Not known");
    }
    float y = av.y + av.height + 16.f;
    ren.drawText(label, {x, y}, m_smallFont, muted, 0.76f);
    ren.drawText(fit(m_font, value, r.width - 52.f, 0.8f), {x, y + 22.f}, m_font, text, 0.8f);
    y += 64.f;

    bool valid = false;
    auto it = m_histories.find(selectedKey(valid));
    drawHistory(ren, {r.x + 12.f, y, r.width - 24.f, r.y + r.height - 12.f - y}, a,
                valid && it != m_histories.end() ? &it->second : nullptr, mine);
}

void SocialScreen::drawRequests(nxui::Renderer& ren, const nxui::Rect& r, float a) {
    auto& i18n = nxui::I18n::instance();
    const nxui::Color text = m_theme->textPrimary.withAlpha(a);
    const nxui::Color muted = m_theme->textSecondary.withAlpha(a);
    const std::string title = i18n.tr("friends.requests", "Friend Requests");
    if (!m_requestsLoaded) {
        drawMessage(ren, r, a, title, i18n.tr("social.requests_loading", "Checking for friend requests..."));
        return;
    }
    if (m_requests.empty()) {
        std::string body = R_FAILED(m_requestsRc)
            ? i18n.tr("social.requests_failed", "Couldn't get your friend requests. Check the console is online.")
                  + "  (" + errorText(m_requestsRc) + ")"
            : i18n.tr("social.requests_none", "No friend requests right now.");
        drawMessage(ren, r, a, title, body);
        return;
    }
    const FriendRequestEntry& q = m_requests[(size_t)std::clamp(m_sel, 0, (int)m_requests.size() - 1)];
    const std::string name = q.nickname.empty() ? i18n.tr("social.someone", "Someone") : q.nickname;
    const float x = r.x + 30.f, w = r.width - 60.f;
    const nxui::Rect av{x, r.y + 30.f, 120.f, 120.f};
    drawAvatar(ren, nullptr, name, av, a);
    const float tx = av.x + av.width + 24.f;
    ren.drawText(fit(m_font, name, r.x + r.width - 30.f - tx, 1.1f), {tx, av.y + 26.f}, m_font, text, 1.1f);
    ren.drawText(i18n.tr("social.wants_friend", "Wants to be your friend"), {tx, av.y + 72.f}, m_smallFont, muted, 0.8f);
    float y = av.y + av.height + 36.f;
    const std::string hint = m_answering != 0 ? i18n.tr("social.answering", "Sending your answer...")
                                              : i18n.tr("social.accept_hint", "A  Accept      X  Decline");
    for (const auto& l : wrap(m_font, hint, w, 0.86f)) {
        ren.drawText(l, {x, y}, m_font, m_theme->cursorNormal.withAlpha(a), 0.86f);
        y += 32.f;
    }
}

void SocialScreen::drawAddFriend(nxui::Renderer& ren, const nxui::Rect& r, float a) {
    auto& i18n = nxui::I18n::instance();
    const std::string title = i18n.tr("friends.add", "Add Friend");
    std::string body;
    switch (m_add) {
        case Add::Looking:
            body = i18n.tr("social.looking_up", "Looking up") + " " + m_addCode + "...";
            break;
        case Add::Found:
        case Add::Sending: {
            const nxui::Color text = m_theme->textPrimary.withAlpha(a);
            const float x = r.x + 30.f;
            const nxui::Rect av{x, r.y + 30.f, 120.f, 120.f};
            drawAvatar(ren, nullptr, m_addName, av, a);
            const float tx = av.x + av.width + 24.f;
            ren.drawText(fit(m_font, m_addName, r.x + r.width - 30.f - tx, 1.1f), {tx, av.y + 26.f}, m_font, text, 1.1f);
            ren.drawText(m_addCode, {tx, av.y + 72.f}, m_smallFont, m_theme->textSecondary.withAlpha(a), 0.8f);
            float y = av.y + av.height + 36.f;
            const std::string line = m_add == Add::Found
                ? i18n.tr("social.send_to", "Send a friend request to") + " " + m_addName + "?"
                : i18n.tr("social.sending", "Sending your friend request...");
            for (const auto& l : wrap(m_font, line, r.width - 60.f, 0.86f)) {
                ren.drawText(l, {x, y}, m_font, text, 0.86f);
                y += 32.f;
            }
            if (m_add == Add::Found)
                ren.drawText(i18n.tr("social.send_hint", "A  Send request      B  Cancel"), {x, y + 14.f}, m_font,
                             m_theme->cursorNormal.withAlpha(a), 0.86f);
            return;
        }
        case Add::Sent:
            body = i18n.tr("social.sent", "Friend request sent to") + " " + m_addName + ". "
                 + i18n.tr("social.sent_more", "They'll show up in your friends once they accept.");
            break;
        case Add::Failed:
            if (m_addRc == MAKERESULT(Module_Libnx, LibnxError_BadInput))
                body = i18n.tr("social.bad_code", "That isn't a friend code. Friend codes have 12 numbers, like SW-1234-5678-9012.");
            else if (m_addRc == MAKERESULT(Module_Libnx, LibnxError_NotFound))
                body = i18n.tr("social.no_such_code", "Nobody has that friend code.");
            else
                body = i18n.tr("social.add_failed", "That didn't work. Check the code and that the console is online.")
                     + "  (" + errorText(m_addRc) + ")";
            break;
        case Add::Idle:
            return;
    }
    drawMessage(ren, r, a, title, body);
}

// A Mii's face (Mii Studio), or a soft placeholder until it arrives.
void SocialScreen::drawMii(nxui::Renderer& ren, const MiiEntry* mii, const nxui::Rect& r, float a) {
    const nxui::Vec2 c{r.x + r.width * 0.5f, r.y + r.height * 0.5f};
    const nxui::Color bg = mii ? nxui::Color(0.93f, 0.95f, 0.98f, 1.f) : nxui::Color(0.5f, 0.5f, 0.5f, 1.f);
    ren.drawCircle(c, r.width * 0.5f, bg.withAlpha(0.9f * a), 40);
    if (const nxui::Texture* t = mii ? face(mii->faceUrl) : nullptr) {
        ren.drawTexture(t, r.shrunk(r.width * 0.04f), nxui::Color::white().withAlpha(a));
        return;
    }
    // Placeholder: a head and shoulders.
    const nxui::Color shade(0.62f, 0.66f, 0.74f, a);
    ren.drawCircle({c.x, c.y - r.height * 0.1f}, r.width * 0.2f, shade, 32);
    ren.drawCircle({c.x, c.y + r.height * 0.36f}, r.width * 0.3f, shade, 32);
}

void SocialScreen::drawMiis(nxui::Renderer& ren, const nxui::Rect& r, float a) {
    auto& i18n = nxui::I18n::instance();
    const nxui::Color text = m_theme->textPrimary.withAlpha(a);
    const nxui::Color muted = m_theme->textSecondary.withAlpha(a);
    const float x = r.x + 30.f, w = r.width - 60.f;
    if (!m_miisLoaded && m_miis.empty()) {
        drawMessage(ren, r, a, i18n.tr("social.miis", "Miis"), i18n.tr("mii.loading", "Loading your Miis..."));
        return;
    }
    const MiiEntry* shown = nullptr;
    std::string title, line;
    if (m_sel == 0) {
        if (!m_haveCandidate) {
            std::string body = i18n.tr("mii.make_body", "Press A for a random Mii. Keep it and name it, or "
                                                        "press Y for another one.");
            if (R_FAILED(m_miisRc)) body += "  (" + errorText(m_miisRc) + ")";
            drawMessage(ren, r, a, i18n.tr("mii.new", "Make a new Mii"), body);
            return;
        }
        shown = &m_candidate;
        title = i18n.tr("mii.new_one", "A new Mii");
        line = m_candidateGender == 0 ? i18n.tr("mii.boys", "Boys only")
             : m_candidateGender == 1 ? i18n.tr("mii.girls", "Girls only") : i18n.tr("mii.anyone", "Boy or girl");
    } else if (m_sel - 1 < (int)m_miis.size()) {
        shown = &m_miis[(size_t)(m_sel - 1)];
        title = shown->name.empty() ? i18n.tr("mii.no_name", "No name") : shown->name;
        line = shown->info.mii_sex == 1 ? i18n.tr("mii.girl", "Girl") : i18n.tr("mii.boy", "Boy");
    }
    const nxui::Rect big{r.x + (r.width - 230.f) * 0.5f, r.y + 28.f, 230.f, 230.f};
    drawMii(ren, shown, big, a);
    float y = big.y + big.height + 22.f;
    const std::string t = fit(m_font, title, w, 1.1f);
    ren.drawText(t, {r.x + (r.width - m_font->measure(t).x * 1.1f) * 0.5f, y}, m_font, text, 1.1f);
    y += 46.f;
    ren.drawText(line, {r.x + (r.width - m_smallFont->measure(line).x * 0.8f) * 0.5f, y}, m_smallFont, muted, 0.8f);
    if (m_job && (m_jobKind == JobKind::MiiAdd || m_jobKind == JobKind::MiiRename
                  || m_jobKind == JobKind::MiiDelete || m_jobKind == JobKind::MiiRandom)) {
        const std::string busy = i18n.tr("mii.working", "One moment...");
        ren.drawText(busy, {r.x + (r.width - m_smallFont->measure(busy).x * 0.8f) * 0.5f, y + 34.f}, m_smallFont,
                     m_theme->cursorNormal.withAlpha(a), 0.8f);
    }
    (void)x;
}

void SocialScreen::drawDetail(nxui::Renderer& ren, const nxui::Rect& r, float a) {
    auto& i18n = nxui::I18n::instance();
    const nxui::Color text = m_theme->textPrimary.withAlpha(a);
    const nxui::Color accent = m_theme->cursorNormal.withAlpha(a);
    ren.drawRoundedRect(r, m_theme->textPrimary.withAlpha(0.045f * a), 24.f);
    if (m_add != Add::Idle) {
        drawAddFriend(ren, r, a);
        return;
    }
    if (m_mode == Mode::Requests) {
        drawRequests(ren, r, a);
        return;
    }
    if (m_mode == Mode::Miis) {
        if (m_editing) drawMiiEditor(ren, r, a);
        else drawMiis(ren, r, a);
        return;
    }
    if (m_zone != Zone::Tiles) {
        drawPerson(ren, r, a);
        return;
    }

    // A quick button: its picture, name and what it does.
    const float x = r.x + 30.f, w = r.width - 60.f;
    std::string title, body, hint = i18n.tr("social.press_a", "Press A to open");
    switch (m_tile) {
        case 0:
            title = i18n.tr("social.miis", "Miis");
            body = i18n.tr("social.miis_body2", "Make new Miis, change their looks, name them and delete them, "
                                                "all right here.");
            break;
        case 1:
            title = i18n.tr("friends.add", "Add Friend");
            body = i18n.tr("social.add_body2", "Type someone's friend code to send them a friend request.")
                 + " " + i18n.tr("friends.needs_online", "This uses Nintendo's servers, so the console must be able to go online.");
            break;
        case 2:
            title = i18n.tr("friends.requests", "Friend Requests");
            body = i18n.tr("social.requests_body2", "See who wants to be your friend, and accept or decline.")
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
    int online = 0;
    for (const auto& f : m_friends) online += f.data.status != 0;
    if (!m_myName.empty()) {
        const float nameW = std::min(260.f, m_font->measure(m_myName).x * 0.82f);
        const float right = p.x + kPanelW - 28.f;
        drawAvatar(ren, m_myAvatar.get(), m_myName, {right - 48.f, p.y + 18.f, 48.f, 48.f}, a);
        ren.drawText(fit(m_font, m_myName, 260.f, 0.82f), {right - 60.f - nameW, p.y + 20.f}, m_font, text, 0.82f);
        const std::string sub = std::to_string(online) + " " + i18n.tr("social.friends_online", "friends online");
        ren.drawText(sub, {right - 60.f - m_smallFont->measure(sub).x * 0.72f, p.y + 48.f}, m_smallFont, muted, 0.72f);
    }
    ren.drawRect({p.x + 28.f, p.y + kHeaderH - 6.f, kPanelW - 56.f, 1.f}, m_theme->panelBorder.withAlpha(a));

    // Quick buttons.
    auto badge = [&](const nxui::Rect& t, int count) {
        const std::string n = count > 9 ? "9+" : std::to_string(count);
        const nxui::Vec2 c{t.x + t.width - 14.f, t.y + 14.f};
        ren.drawCircle(c, 12.f, kRed.withAlpha(a), 24);
        ren.drawText(n, {c.x - m_smallFont->measure(n).x * 0.34f, c.y - 9.f}, m_smallFont,
                     nxui::Color(1.f, 1.f, 1.f, a), 0.68f);
    };
    for (int i = 0; i < kTiles; ++i) {
        const nxui::Rect t = tileRect(i);
        const bool sel = (m_zone == Zone::Tiles || (i == 2 && m_mode == Mode::Requests)
                          || (i == 0 && m_mode == Mode::Miis)) && m_tile == i && m_active;
        ren.drawRoundedRect(t, sel ? accent.withAlpha(0.16f * a) : m_theme->textPrimary.withAlpha(0.06f * a), 20.f);
        if (sel) ren.drawRoundedRectOutline(t, accent.withAlpha(0.9f * a), 20.f, 2.5f);
        drawTileIcon(ren, i, {t.x + (t.width - 50.f) * 0.5f, t.y + 12.f, 50.f, 50.f}, a);
        static const char* const kKeys[kTiles][2] = {{"social.tile_miis", "Miis"}, {"social.tile_add", "Add Friend"},
                                                     {"social.tile_requests", "Requests"}, {"social.tile_discord", "Discord"}};
        const std::string label = fit(m_smallFont, i18n.tr(kKeys[i][0], kKeys[i][1]), t.width - 10.f, 0.7f);
        ren.drawText(label, {t.x + (t.width - m_smallFont->measure(label).x * 0.7f) * 0.5f, t.y + 72.f},
                     m_smallFont, text, 0.7f);
        if (i == 2 && m_requestCount > 0) badge(t, m_requestCount);
        if (i == 3 && m_discord && m_discord->started()) {
            const int unread = m_discord->unreadDms();
            if (unread > 0)
                badge(t, unread);
            else if (m_discord->state() == DiscordService::State::Ready)
                ren.drawCircle({t.x + t.width - 14.f, t.y + 14.f}, 6.f, kOnline.withAlpha(a), 20);
        }
    }

    // The list: you and your friends, or friend requests.
    const nxui::Rect list = listRect();
    const int n = listCount();
    std::string head;
    if (m_mode == Mode::Miis) {
        head = i18n.tr("social.miis", "Miis");
        if (!m_miis.empty()) head += "  " + std::to_string(m_miis.size());
    } else if (m_mode == Mode::Requests) {
        head = i18n.tr("friends.requests", "Friend Requests");
        if (!m_requests.empty()) head += "  " + std::to_string(m_requests.size());
    } else {
        head = i18n.tr("social.friends", "Friends");
        if (!m_friends.empty())
            head += "  " + std::to_string(m_friends.size()) + "  \xC2\xB7  " + std::to_string(online) + " "
                  + i18n.tr("social.online_lower", "online");
    }
    ren.drawText(head, {list.x + 8.f, list.y - 36.f}, m_smallFont, muted, 0.82f);

    const float total = kRowH * n;
    if (m_followSel && m_zone != Zone::Tiles) {
        const float top = m_sel * kRowH;
        if (top < m_scroll) m_scroll = top;
        if (top + kRowH > m_scroll + list.height) m_scroll = top + kRowH - list.height;
    }
    m_scroll = std::clamp(m_scroll, 0.f, std::max(0.f, total - list.height));
    ren.pushClipRect(list);
    for (int i = 0; i < n; ++i) {
        const float y = list.y + i * kRowH - m_scroll;
        if (y + kRowH < list.y || y > list.y + list.height) continue;
        const nxui::Rect row{list.x, y + 2.f, list.width, kRowH - 4.f};
        const bool sel = m_zone != Zone::Tiles && m_sel == i && m_active;
        const nxui::Color rowBg = sel ? accent.withAlpha(0.16f * a) : m_theme->panelBase.withAlpha(a);
        if (sel) {
            ren.drawRoundedRect(row, rowBg, 16.f);
            ren.drawRoundedRectOutline(row, accent.withAlpha((m_zone == Zone::Detail ? 0.4f : 0.9f) * a), 16.f, 2.5f);
        }
        const nxui::Rect av{row.x + 10.f, row.y + (row.height - 46.f) * 0.5f, 46.f, 46.f};
        const float tx = av.x + av.width + 14.f;
        if (m_mode == Mode::Miis) {
            const float tw = row.x + row.width - tx - 12.f;
            if (i == 0) {
                ren.drawCircle({av.x + av.width * 0.5f, av.y + av.height * 0.5f}, av.width * 0.5f, kOnline.withAlpha(a), 32);
                const nxui::Vec2 c{av.x + av.width * 0.5f, av.y + av.height * 0.5f};
                ren.drawRect({c.x - 12.f, c.y - 2.5f, 24.f, 5.f}, nxui::Color(1.f, 1.f, 1.f, a));
                ren.drawRect({c.x - 2.5f, c.y - 12.f, 5.f, 24.f}, nxui::Color(1.f, 1.f, 1.f, a));
                ren.drawText(fit(m_font, i18n.tr("mii.new", "Make a new Mii"), tw, 0.8f), {tx, row.y + 18.f}, m_font, text, 0.8f);
            } else {
                const MiiEntry& m = m_miis[(size_t)(i - 1)];
                drawMii(ren, &m, av, a);
                ren.drawText(fit(m_font, m.name, tw, 0.8f), {tx, row.y + 18.f}, m_font, text, 0.8f);
            }
            continue;
        }
        if (m_mode == Mode::Requests) {
            const FriendRequestEntry& q = m_requests[(size_t)i];
            const std::string name = q.nickname.empty() ? i18n.tr("social.someone", "Someone") : q.nickname;
            drawAvatar(ren, nullptr, name, av, a);
            const float tw = row.x + row.width - tx - 12.f;
            ren.drawText(fit(m_font, name, tw, 0.8f), {tx, row.y + 8.f}, m_font, text, 0.8f);
            ren.drawText(fit(m_smallFont, i18n.tr("social.wants_friend", "Wants to be your friend"), tw, 0.72f),
                         {tx, row.y + 35.f}, m_smallFont, muted, 0.72f);
            continue;
        }
        if (i == 0) { // you
            const std::string name = m_myName.empty() ? i18n.tr("social.you", "You") : m_myName;
            drawAvatar(ren, m_myAvatar.get(), name, av, a);
            const float tw = row.x + row.width - tx - 12.f;
            ren.drawText(fit(m_font, name, tw, 0.8f), {tx, row.y + 8.f}, m_font, text, 0.8f);
            const std::string sub = i18n.tr("social.your_profile", "Your profile")
                + (m_friendCode.empty() ? std::string() : "  \xC2\xB7  " + m_friendCode);
            ren.drawText(fit(m_smallFont, sub, tw, 0.72f), {tx, row.y + 35.f}, m_smallFont, accent.withAlpha(a), 0.72f);
            continue;
        }
        const Friend& f = m_friends[(size_t)(i - 1)];
        drawAvatar(ren, f.avatar.get(), f.data.nickname, av, a);
        const nxui::Vec2 dot{av.x + av.width - 6.f, av.y + av.height - 6.f};
        ren.drawCircle(dot, 9.f, sel ? m_theme->panelBase.withAlpha(a) : rowBg, 20);
        ren.drawCircle(dot, 6.5f, statusColor(f.data, a), 20);
        if (f.data.status == 0) ren.drawCircle(dot, 3.f, m_theme->panelBase.withAlpha(a), 16);
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

    // Under you: what the list is waiting for.
    if (m_mode == Mode::Friends && m_friends.empty()) {
        std::string msg;
        if (!accountUidIsValid(&m_uid))
            msg = i18n.tr("friends.no_profile", "Choose a profile with the profile button first.");
        else if (m_loading || !m_loaded)
            msg = i18n.tr("social.loading", "Loading your friends")
                + std::string((size_t)(1 + (int)(armTicksToNs(armGetSystemTick()) / 400'000'000ULL) % 3), '.');
        else
            msg = i18n.tr("social.no_friends", "No friends yet. Add some with Add Friend.")
                + (R_FAILED(m_rc) || !m_diag.empty() ? "  (" + m_diag + ")" : std::string());
        float y = list.y + kRowH + 10.f;
        for (const auto& l : wrap(m_smallFont, msg, list.width - 20.f, 0.82f)) {
            ren.drawText(l, {list.x + 8.f, y}, m_smallFont, muted, 0.82f);
            y += 26.f;
        }
    } else if (m_mode == Mode::Requests && m_requests.empty()) {
        const std::string msg = !m_requestsLoaded ? i18n.tr("social.requests_loading", "Checking for friend requests...")
                                                  : i18n.tr("social.requests_none", "No friend requests right now.");
        ren.drawText(fit(m_smallFont, msg, list.width - 20.f, 0.82f), {list.x + 8.f, list.y + 10.f}, m_smallFont,
                     muted, 0.82f);
    }

    // The selected person, request or button, on the right.
    drawDetail(ren, detailRect(), a);

    // What the buttons do right now, and the last thing that happened.
    std::string hints;
    if (m_editing) {
        hints = i18n.tr("mii.hint_maker", "Up/Down  Feature     Left/Right  Change     Y  Surprise     A  Save     B  Cancel");
    } else if (m_add == Add::Found) {
        hints = i18n.tr("social.hint_send", "A  Send     B  Cancel");
    } else if (m_add == Add::Sent || m_add == Add::Failed) {
        hints = i18n.tr("social.hint_ok", "A  OK");
    } else if (m_add != Add::Idle) {
        hints.clear();
    } else if (m_mode == Mode::Miis) {
        hints = m_sel == 0 ? (m_haveCandidate ? i18n.tr("mii.hint_new", "A  Keep     Y  Another     X  Boy / Girl     B  Back")
                                              : i18n.tr("mii.hint_make", "A  Make one     B  Back"))
                           : i18n.tr("mii.hint_mii2", "A  Rename     X  Delete     Y  Change looks     B  Back");
    } else if (m_mode == Mode::Requests) {
        hints = m_requests.empty() ? i18n.tr("social.hint_back", "B  Back")
                                   : i18n.tr("social.hint_requests", "A  Accept     X  Decline     B  Back");
    } else if (m_zone == Zone::Tiles) {
        hints = i18n.tr("social.hint_tiles", "A  Open     B  Close");
    } else if (m_zone == Zone::Detail) {
        hints = i18n.tr("social.hint_detail", "Up/Down  Scroll     B  Back");
    } else {
        hints = m_sel == 0 ? i18n.tr("social.hint_you", "A  Play history     Y  Sort     B  Close")
                           : i18n.tr("social.hint_person", "A  Play history     B  Close");
    }
    if (!hints.empty())
        ren.drawText(hints, {p.x + kPanelW - 28.f - m_smallFont->measure(hints).x * 0.78f, p.y + kPanelH - 36.f},
                     m_smallFont, muted, 0.78f);
    if (!m_notice.empty() && armTicksToNs(armGetSystemTick() - m_noticeAt) < 6'000'000'000ULL)
        ren.drawText(fit(m_smallFont, m_notice, kPanelW * 0.55f, 0.78f), {p.x + 32.f, p.y + kPanelH - 36.f},
                     m_smallFont, accent.withAlpha(a), 0.78f);
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
    if (!in.touchUp() || m_add == Add::Looking || m_add == Add::Sending) return;
    for (int i = 0; i < kTiles; ++i)
        if (inside(tileRect(i), tx, ty)) {
            m_add = Add::Idle;
            if (m_mode == Mode::Requests && i != 2) m_mode = Mode::Friends;
            m_zone = Zone::Tiles;
            m_tile = i;
            activateItem();
            return;
        }
    if (inside(list, tx, ty)) {
        const int i = (int)((ty - list.y + m_scroll) / kRowH);
        if (i >= 0 && i < listCount()) {
            m_add = Add::Idle;
            m_zone = Zone::List;
            m_sel = i;
            m_detailScroll = 0.f;
            if (m_cb.onNavigate) m_cb.onNavigate();
        }
    }
}
