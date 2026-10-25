#include "CardsActivity.hpp"

#include "social/NotificationLog.hpp"

#include <nxui/Application.hpp>
#include <nxui/core/GpuDevice.hpp>
#include <nxui/core/Renderer.hpp>
#include <nxui/core/ThreadPool.hpp>
#include <nxui/third_party/stb/stb_image.h>
#include <nxui/widgets/Widget.hpp>
#include <switch.h>
#include <SDL2/SDL_mixer.h>

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <unordered_map>

namespace cards {
namespace {

// The layout: the reference screenshot (2000x920) on 1280x720. Sizes follow
// its height (x0.78), positions its width (x0.64).
constexpr float kTopY = 47.f, kCircle = 77.f;
// Cards are the shape of a Switch case cover without its red band (352x499),
// so its whole picture fits them exactly.
constexpr float kCardW = 240.f, kCardH = 340.f, kCardGrowW = 10.f, kCardGrowH = 14.f;
constexpr float kCardMidY = 291.f, kCardPitch = 250.f, kCardFirstX = 136.f, kCardRadius = 26.f;
const nxui::Rect kPanel{186.f, 503.f, 907.f, 207.f};
constexpr float kPanelRadius = 35.f;
// The top bar's right side: Nextendo, notifications, power, settings.
constexpr float kFriendsX = 896.f, kBellX = 992.f, kPowerX = 1061.f, kGearX = 1146.f;

const nxui::Color kWhite(1.f, 1.f, 1.f, 1.f);
const nxui::Color kMuted(1.f, 1.f, 1.f, 0.82f);
const nxui::Color kGlass(0.17f, 0.20f, 0.27f, 0.30f); // over the blurred frame
const nxui::Color kGlassEdge(1.f, 1.f, 1.f, 0.20f);
const nxui::Color kGreen(0.204f, 0.78f, 0.349f, 1.f); // #34C759
const nxui::Color kRed(1.f, 0.231f, 0.188f, 1.f);     // #FF3B30

const char* const kPageNames[] = {"Games", "Homebrew", "Nintendo"};
const char* const kSortNames[] = {"Last Played", "Title", "Most Played"};

class Painter final : public nxui::Widget {
public:
    explicit Painter(std::function<void(nxui::Renderer&)> fn) : m_fn(std::move(fn)) { setRect({0.f, 0.f, 1280.f, 720.f}); }

protected:
    void onRender(nxui::Renderer& r) override {
        if (m_fn) m_fn(r);
    }

private:
    std::function<void(nxui::Renderer&)> m_fn;
};

float clamp01(float v) { return std::clamp(v, 0.f, 1.f); }
float easeOut(float t) { t = clamp01(t); return 1.f - (1.f - t) * (1.f - t) * (1.f - t); }
float easeOutBack(float t) { // a small overshoot, then settles
    t = clamp01(t);
    const float c1 = 1.4f, c3 = c1 + 1.f;
    return 1.f + c3 * std::pow(t - 1.f, 3.f) + c1 * std::pow(t - 1.f, 2.f);
}

// Text sizes, measured once: measuring is shaping the text again, and the
// Games page measured every long name letter by letter every frame (it lagged).
nxui::Vec2 measured(nxui::Font& font, const std::string& s) {
    static std::unordered_map<std::string, nxui::Vec2> cache;
    std::string key = std::to_string(reinterpret_cast<std::uintptr_t>(&font)) + '|' + s;
    auto it = cache.find(key);
    if (it != cache.end()) return it->second;
    if (cache.size() > 6000) cache.clear();
    return cache[key] = font.measure(s);
}

// As much of `text` as fits in maxW (cut between letters, "..." added), remembered.
std::string fit(nxui::Font& font, const std::string& text, float maxW, float scale = 1.f) {
    static std::unordered_map<std::string, std::string> cache;
    std::string key = std::to_string(reinterpret_cast<std::uintptr_t>(&font)) + '|' + std::to_string(int(maxW)) + '|'
                    + std::to_string(int(scale * 100.f)) + '|' + text;
    auto it = cache.find(key);
    if (it != cache.end()) return it->second;
    if (cache.size() > 3000) cache.clear();
    if (measured(font, text).x * scale <= maxW) return cache[key] = text;
    std::vector<size_t> cuts; // where letters start
    for (size_t i = 1; i < text.size(); ++i)
        if ((static_cast<unsigned char>(text[i]) & 0xC0) != 0x80) cuts.push_back(i);
    size_t lo = 0, hi = cuts.size(); // the longest start that fits
    while (lo < hi) {
        const size_t mid = (lo + hi + 1) / 2;
        if (measured(font, text.substr(0, cuts[mid - 1]) + "...").x * scale <= maxW) lo = mid;
        else hi = mid - 1;
    }
    return cache[key] = (lo == 0 ? std::string() : text.substr(0, cuts[lo - 1])) + "...";
}

// The scale (down to minScale) that fits `text` in maxW: long names shrink a
// little before they're cut.
float fitScale(nxui::Font& font, const std::string& text, float maxW, float minScale = 0.78f) {
    const float w = measured(font, text).x;
    return w <= maxW ? 1.f : std::max(minScale, maxW / w);
}

// Text drawn with its middle at y.
void textMid(nxui::Renderer& r, nxui::Font& font, const std::string& s, float x, float y, const nxui::Color& c,
             float scale = 1.f) {
    r.drawText(s, {x, y - measured(font, s.empty() ? std::string("A") : s).y * scale * 0.5f}, &font, c, scale);
}

std::string playTime(std::uint64_t seconds) {
    const std::uint64_t m = seconds / 60, h = m / 60;
    if (m == 0) return seconds ? "<1m" : "0m";
    if (h == 0) return std::to_string(m) + "m";
    if (h >= 100 || m % 60 == 0) return std::to_string(h) + "h";
    return std::to_string(h) + "h " + std::to_string(m % 60) + "m";
}

std::string lastPlayed(std::uint64_t at) {
    if (at == 0) return "Never played";
    const std::uint64_t now = static_cast<std::uint64_t>(std::time(nullptr));
    const std::uint64_t s = now > at ? now - at : 0;
    auto unit = [](std::uint64_t n, const char* one) {
        return std::to_string(n) + " " + one + (n == 1 ? "" : "s") + " ago";
    };
    if (s < 60) return "Last played just now";
    if (s < 3600) return "Last played " + unit(s / 60, "minute");
    if (s < 86400) return "Last played " + unit(s / 3600, "hour");
    if (s < 30 * 86400) return "Last played " + unit(s / 86400, "day");
    TimeCalendarTime t{};
    TimeCalendarAdditionalInfo info{};
    if (R_FAILED(timeToCalendarTimeWithMyRule(at, &t, &info))) return "Last played a while ago";
    static const char* const kMonths[] = {"Jan", "Feb", "Mar", "Apr", "May", "Jun",
                                          "Jul", "Aug", "Sep", "Oct", "Nov", "Dec"};
    return "Last played " + std::to_string(t.day) + " " + kMonths[(t.month + 11) % 12] + " " + std::to_string(t.year);
}

// Halves a picture until it's at most maxW wide (a softer, cheaper texture).
void shrink(std::vector<std::uint8_t>& rgba, int& w, int& h, int maxW) {
    while (w > maxW && w >= 2 && h >= 2) {
        const int nw = w / 2, nh = h / 2;
        std::vector<std::uint8_t> out(static_cast<size_t>(nw) * nh * 4);
        for (int y = 0; y < nh; ++y)
            for (int x = 0; x < nw; ++x)
                for (int c = 0; c < 4; ++c) {
                    const auto at = [&](int xx, int yy) { return rgba[(static_cast<size_t>(yy) * w + xx) * 4 + c]; };
                    out[(static_cast<size_t>(y) * nw + x) * 4 + c] = static_cast<std::uint8_t>(
                        (at(2 * x, 2 * y) + at(2 * x + 1, 2 * y) + at(2 * x, 2 * y + 1) + at(2 * x + 1, 2 * y + 1)) / 4);
                }
        rgba.swap(out);
        w = nw;
        h = nh;
    }
}

// A picture drawn to fill `dst`, cropped to its shape. trimTop / trimBottom:
// fractions left out first; zoom / panX / panY: a slow drift (the background).
void drawCover(nxui::Renderer& r, const nxui::Texture* tex, const nxui::Rect& dst, float radius, const nxui::Color& tint,
               float trimTop = 0.f, float trimBottom = 0.f, float zoom = 1.f, float panX = 0.f, float panY = 0.f) {
    const float tw = static_cast<float>(tex->width());
    const float top = static_cast<float>(tex->height()) * trimTop;
    const float th = static_cast<float>(tex->height()) * (1.f - trimTop - trimBottom);
    const float scale = std::max(dst.width / tw, dst.height / th) * zoom;
    const float sw = dst.width / scale, sh = dst.height / scale;
    const nxui::Rect src{(tw - sw) * (0.5f + panX), top + (th - sh) * (0.5f + panY), sw, sh};
    if (radius > 0.f) r.drawTextureSubRounded(tex, src, dst, radius, tint);
    else r.drawTextureSub(tex, src, dst, tint);
}

nxui::Rect scaled(const nxui::Rect& r, float s) {
    const float w = r.width * s, h = r.height * s;
    return {r.x + (r.width - w) * 0.5f, r.y + (r.height - h) * 0.5f, w, h};
}

} // namespace

// ---------------------------------------------------------------- set up

bool CardsActivity::onCreate() {
    auto& gpu = app().gpu();
    auto& ren = app().renderer();
    ren.setBoxWireframeEnabled(false);
    app().setIdleThrottle(false); // it keeps moving: always 60 Hz

    // More room for pictures than HOME has, when this process has it.
    u64 total = 0;
    svcGetInfo(&total, InfoType_TotalMemorySize, CUR_PROCESS_HANDLE, 0);
    nxui::GpuDevice::setImageBudget(std::clamp<u64>(total / 4, 64ull << 20, 160ull << 20));

    m_settings.load();
    m_boxArt = m_settings.boxArt;

    // Names in other scripts (Japanese, Chinese, Korean) from the console's own fonts.
    if (R_SUCCEEDED(plInitialize(PlServiceType_User))) {
        for (PlSharedFontType type : {PlSharedFontType_Standard, PlSharedFontType_ChineseSimplified,
                                      PlSharedFontType_ChineseTraditional, PlSharedFontType_KO,
                                      PlSharedFontType_NintendoExt}) {
            PlFontData font{};
            if (R_SUCCEEDED(plGetSharedFontByType(&font, type))) nxui::Font::addFallback(font.address, font.size);
        }
    }
    const char* f5 = "romfs:/fonts/Inter-500.ttf";
    const char* f6 = "romfs:/fonts/Inter-600.ttf";
    const char* f7 = "romfs:/fonts/Inter-700.ttf";
    if (!m_fTitle.load(gpu, ren, f7, 30) || !m_fPlay.load(gpu, ren, f7, 24) || !m_fLabel.load(gpu, ren, f6, 22)
        || !m_fPill.load(gpu, ren, f6, 19) || !m_fBody.load(gpu, ren, f5, 17) || !m_fBadge.load(gpu, ren, f7, 15)
        || !m_fCount.load(gpu, ren, f7, 21))
        return false;
    for (const char* name : {"dots", "sort", "friends", "bell", "gear", "thermo", "tag", "play", "copy", "home",
                             "power"}) {
        auto tex = std::make_unique<nxui::Texture>();
        nxui::GpuDevice::EssentialScope essential; // the buttons always draw
        if (tex->loadFromFile(gpu, ren, std::string("romfs:/icons/") + name + ".png", 96)) m_icons[name] = std::move(tex);
    }

    // The pages: games; homebrew (.nro apps, then NRO forwarders); Nintendo.
    for (Game& g : listGames()) (g.homebrew ? m_pages[kHomebrew] : m_pages[kGames]).push_back(std::move(g));
    std::vector<Game> nros = listHomebrew();
    m_pages[kHomebrew].insert(m_pages[kHomebrew].begin(), std::make_move_iterator(nros.begin()),
                              std::make_move_iterator(nros.end()));
    for (size_t i = 0; i < m_pages[kHomebrew].size(); ++i) m_pages[kHomebrew][i].order = static_cast<int>(i);
    m_pages[kNintendo] = listSystem();

    // You: the last account that played.
    AccountUid uid{};
    if (R_SUCCEEDED(accountGetLastOpenedUser(&uid)) && accountUidIsValid(&uid)) {
        m_user.uid[0] = uid.uid[0];
        m_user.uid[1] = uid.uid[1];
        AccountProfile profile{};
        if (R_SUCCEEDED(accountGetProfile(&profile, uid))) {
            u32 size = 0;
            if (R_SUCCEEDED(accountProfileGetImageSize(&profile, &size)) && size > 0) {
                std::vector<std::uint8_t> jpeg(size);
                u32 got = 0;
                if (R_SUCCEEDED(accountProfileLoadImage(&profile, jpeg.data(), size, &got)) && got > 0) {
                    m_avatar = std::make_unique<nxui::Texture>();
                    if (!m_avatar->loadFromMemory(gpu, ren, jpeg.data(), got, 96)) m_avatar.reset();
                }
            }
            accountProfileClose(&profile);
        }
    }
    refreshStatus();
    m_unread = notifications::unreadCount();
    m_key = steamGridDbKey();

    m_audio.start();
    m_audio.setSounds(m_settings.sounds);
    m_audio.setMusic(m_settings.music, m_settings.musicVolume);

    m_painter = std::make_shared<Painter>([this](nxui::Renderer& r) { draw(r); });
    rootBox().addChild(m_painter);

    m_page = std::clamp(m_settings.startPage, 0, kPageCount - 1);
    sortGames(m_settings.sort, false); // opens on the first game
    m_netUp = startNetwork();
    for (int i = 0; i < 3; ++i) m_threads.emplace_back([this, i]() { worker(i); });
    wantArt();
    m_intro = m_settings.motion ? 0.f : 1.f;
    return true;
}

void CardsActivity::onDestroy() {
    {
        std::lock_guard<std::mutex> lk(m_mu);
        m_quit = true;
        m_jobs.clear();
    }
    m_cv.notify_all();
    for (std::thread& t : m_threads)
        if (t.joinable()) t.join();
    if (m_netUp) stopNetwork();
    m_audio.stop();
}

const Game* CardsActivity::current() {
    auto& g = games();
    return g.empty() ? nullptr : &g[(size_t)std::clamp(sel(), 0, static_cast<int>(g.size()) - 1)];
}

// ---------------------------------------------------------------- the workers

void CardsActivity::worker(int index) {
    // Off the screen's core and below it: decoding a picture used to take the
    // frames' time (the Games page stuttered).
    nxui::moveToWorkerCores();
    svcSetThreadPriority(CUR_THREAD_HANDLE, 0x3B);
    std::uint64_t countsAt = 0;
    for (;;) {
        Job job;
        bool haveJob = false;
        {
            std::unique_lock<std::mutex> lk(m_mu);
            m_cv.wait_for(lk, std::chrono::seconds(1), [this]() { return m_quit || !m_jobs.empty(); });
            if (m_quit) break;
            if (!m_jobs.empty()) {
                job = std::move(m_jobs.front());
                m_jobs.pop_front();
                haveJob = true;
            }
        }
        if (!haveJob) { // pictures first; Nextendo's players every minute
            const std::uint64_t now = armTicksToNs(armGetSystemTick()) / 1000000ULL;
            if (index == 0 && m_netUp && now >= countsAt) {
                countsAt = now + 60000;
                std::map<std::uint64_t, int> counts;
                if (nextendoOnline(counts)) {
                    std::lock_guard<std::mutex> lk(m_mu);
                    m_playersIn = std::move(counts);
                    m_playersFresh = true;
                }
            }
            continue;
        }
        Done done;
        done.id = job.game.id;
        done.what = job.what;
        std::vector<std::uint8_t> data;
        if (job.what == kBackground) {
            data = backgroundArt(job.game, m_key, done.kind);
        } else if (job.what == kCard) {
            data = cardArt(job.game, m_key, m_boxArt.load(), done.kind);
        } else if (!job.game.action.empty()) {
            std::ifstream in("romfs:/icons/sys_" + job.game.action + ".png", std::ios::binary);
            data.assign(std::istreambuf_iterator<char>(in), {});
        } else {
            data = gameIcon(job.game);
        }
        if (!data.empty()) {
            int w = 0, h = 0, ch = 0;
            if (stbi_uc* px = stbi_load_from_memory(data.data(), static_cast<int>(data.size()), &w, &h, &ch, 4)) {
                done.rgba.assign(px, px + static_cast<size_t>(w) * h * 4);
                stbi_image_free(px);
                // Backgrounds sharp (a box cover softer, an icon a wash of its colours); cards as drawn.
                const int maxW = job.what == kIcon ? 160
                               : job.what == kCard ? 400
                               : done.kind == ArtKind::Icon ? 32
                               : done.kind == ArtKind::Box ? 400
                                                           : 960;
                shrink(done.rgba, w, h, maxW);
                done.w = w;
                done.h = h;
            }
        }
        std::lock_guard<std::mutex> lk(m_mu);
        m_done.push_back(std::move(done));
    }
    threadDone();
}

void CardsActivity::wantArt() {
    auto& page = games();
    std::vector<Job> jobs;
    auto add = [&](int i, int what) {
        if (i < 0 || i >= static_cast<int>(page.size())) return;
        Art& a = m_art[page[(size_t)i].id];
        bool& asked = what == kBackground ? a.bgAsked : what == kIcon ? a.iconAsked : a.cardAsked;
        if (asked) return;
        asked = true;
        jobs.push_back({page[(size_t)i], what});
    };
    const int s = sel();
    add(s, kIcon);
    add(s, kCard);
    add(s, kBackground);
    for (int d = 1; d <= 6; ++d) {
        add(s + d, kCard);
        add(s - d, kCard);
        if (d <= 2) {
            add(s + d, kIcon);
            add(s - d, kIcon);
        }
        if (d == 1) {
            add(s + 1, kBackground);
            add(s - 1, kBackground);
        }
    }
    for (int d = 7; d <= 9; ++d) add(s + d, kCard);
    if (jobs.empty()) return;
    {
        std::lock_guard<std::mutex> lk(m_mu); // what's near the selection now goes first
        for (auto it = jobs.rbegin(); it != jobs.rend(); ++it) m_jobs.push_front(std::move(*it));
    }
    m_cv.notify_all();
}

void CardsActivity::takeArt() {
    std::vector<Done> done;
    {
        std::lock_guard<std::mutex> lk(m_mu);
        const size_t n = std::min<size_t>(1, m_done.size()); // one upload a frame
        for (size_t i = 0; i < n; ++i) done.push_back(std::move(m_done[i]));
        m_done.erase(m_done.begin(), m_done.begin() + static_cast<std::ptrdiff_t>(n));
        if (m_playersFresh) {
            m_players = m_playersIn;
            m_playersFresh = false;
            m_playersTotal = 0;
            for (const auto& p : m_players) m_playersTotal += p.second;
        }
    }
    const Game* cur = current();
    const std::uint64_t selected = cur ? cur->id : 0;
    for (Done& d : done) {
        auto it = m_art.find(d.id);
        if (it == m_art.end() || d.rgba.empty()) continue;
        auto tex = std::make_unique<nxui::Texture>();
        if (!tex->loadFromPixels(app().gpu(), app().renderer(), d.rgba.data(), d.w, d.h)) {
            (d.what == kBackground ? it->second.bgAsked : d.what == kIcon ? it->second.iconAsked : it->second.cardAsked) =
                false; // no room now: asked again later
            continue;
        }
        if (d.what == kIcon) {
            it->second.icon = std::move(tex);
        } else if (d.what == kBackground) {
            it->second.bg = std::move(tex);
            it->second.bgKind = d.kind;
            if (d.id == selected && m_bgId != d.id) {
                m_bgPrevId = m_bgId;
                m_bgId = d.id;
                m_bgFade = 0.f;
            }
        } else {
            it->second.card = std::move(tex);
            it->second.kind = d.kind;
            it->second.cardIn = m_time;
        }
    }

    // Far from the selection, on any page: let the pictures go, now and then and
    // all together (freeing one waits for the GPU; the rest are then free).
    if ((m_evictIn -= 1.f / 60.f) > 0.f) return;
    m_evictIn = 2.f;
    int loaded = 0;
    for (const auto& [id, a] : m_art) loaded += (a.card ? 1 : 0) + (a.bg ? 3 : 0);
    if (loaded < 56) return;
    std::map<std::uint64_t, int> distance;
    for (int p = 0; p < kPageCount; ++p) {
        const auto& page = m_pages[p];
        const int s = m_sels[p];
        for (size_t i = 0; i < page.size(); ++i) // another page's: as if 8 further away
            distance[page[i].id] = std::abs(static_cast<int>(i) - s) + (p == m_page ? 0 : 8);
    }
    for (auto& [id, a] : m_art) {
        auto d = distance.find(id);
        const int dist = d == distance.end() ? 1000 : d->second;
        if (a.card && dist > 24) {
            a.card.reset();
            a.cardAsked = false;
        }
        if (a.icon && dist > 24) {
            a.icon.reset();
            a.iconAsked = false;
        }
        if (a.bg && dist > 4 && id != m_bgId && id != m_bgPrevId) {
            a.bg.reset();
            a.bgAsked = false;
        }
    }
}

void CardsActivity::forgetAllArt() {
    {
        std::lock_guard<std::mutex> lk(m_mu);
        m_jobs.clear();
        m_done.clear();
    }
    app().gpu().waitIdle(); // they may still be drawing
    for (auto& page : m_pages)
        for (const Game& g : page) forgetArt(g.id);
    m_art.clear();
    m_bgId = m_bgPrevId = 0;
    wantArt();
}

// ---------------------------------------------------------------- actions

void CardsActivity::select(int index) {
    auto& page = games();
    if (page.empty()) return;
    index = std::clamp(index, 0, static_cast<int>(page.size()) - 1);
    if (index == sel()) return;
    sel() = index;
    m_audio.play("navigation");
    m_detailIn = m_settings.motion ? 0.f : 1.f;
    m_sheen = m_settings.motion ? 0.f : 1.f;
    const std::uint64_t id = page[(size_t)index].id;
    auto it = m_art.find(id);
    if (it != m_art.end() && it->second.bg && m_bgId != id) {
        m_bgPrevId = m_bgId;
        m_bgId = id;
        m_bgFade = 0.f;
    }
    wantArt();
}

void CardsActivity::switchPage(int dir) {
    m_page = (m_page + dir + kPageCount) % kPageCount;
    m_pageDir = dir;
    m_pageIn = m_settings.motion ? 0.f : 1.f;
    m_detailIn = m_pageIn;
    m_scroll = static_cast<float>(sel());
    m_zone = Zone::Cards;
    m_audio.play("tab_transition");
    m_bgPrevId = m_bgId; // the new page's art fades in over the old
    m_bgFade = 0.f;
    const Game* cur = current();
    m_bgId = cur ? cur->id : 0;
    {
        std::lock_guard<std::mutex> lk(m_mu);
        m_jobs.clear(); // what's near has changed
    }
    for (auto& [id, art] : m_art) {
        if (!art.card) art.cardAsked = false;
        if (!art.bg) art.bgAsked = false;
        if (!art.icon) art.iconAsked = false;
    }
    wantArt();
}

void CardsActivity::sortGames(int sort, bool keepChoice) {
    auto& page = m_pages[kGames];
    const std::uint64_t keep = page.empty() ? 0 : page[(size_t)std::clamp(m_sels[kGames], 0, (int)page.size() - 1)].id;
    m_settings.sort = std::clamp(sort, 0, 2);
    auto lower = [](std::string s) {
        for (char& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        return s;
    };
    std::stable_sort(page.begin(), page.end(), [&](const Game& a, const Game& b) {
        switch (m_settings.sort) {
            case 1: return lower(a.name) < lower(b.name);
            case 2: return a.playSeconds != b.playSeconds ? a.playSeconds > b.playSeconds : a.order < b.order;
            default: return a.lastPlayed != b.lastPlayed ? a.lastPlayed > b.lastPlayed : a.order < b.order;
        }
    });
    m_sels[kGames] = 0;
    for (size_t i = 0; keepChoice && i < page.size(); ++i)
        if (page[i].id == keep) m_sels[kGames] = static_cast<int>(i);
    if (m_page == kGames) m_scroll = static_cast<float>(m_sels[kGames]);
    for (auto& [id, art] : m_art) {
        if (!art.card) art.cardAsked = false;
        if (!art.bg) art.bgAsked = false;
    }
    {
        std::lock_guard<std::mutex> lk(m_mu);
        m_jobs.clear();
    }
    wantArt();
}

void CardsActivity::play() {
    const Game* g = current();
    if (!g) return;
    if (!g->action.empty()) { // the Nintendo page
        if (g->action == "settings") {
            openSettings();
            return;
        }
        m_audio.play("activation");
        Mix_PauseMusic();
        std::string error;
        const bool ok = openSystem(g->action, m_user, error);
        Mix_ResumeMusic();
        if (!ok) say(error);
        return;
    }
    if (!g->path.empty()) { // a .nro: it replaces this app, as hbmenu starts it
        if (!envHasNextLoad()) {
            say("Open CardsNX from HOME to start apps.");
            return;
        }
        m_audio.play("launch_game");
        m_audio.fadeOut(300);
        envSetNextLoad(g->path.c_str(), ("\"" + g->path + "\"").c_str());
        m_settings.save();
        app().requestExit();
        return;
    }
    if (launch(g->id, m_user)) {
        m_audio.play("launch_game");
        m_audio.fadeOut(300);
        m_settings.save();
        app().requestExit(); // the game starts once this has closed
        return;
    }
    say("Couldn't start it: update HomeNX, then try again.");
}

void CardsActivity::openMenu(std::string title, std::vector<MenuItem> items) {
    m_menuTitle = std::move(title);
    m_menu = std::move(items);
    m_menuSel = 0;
    m_menuIn = m_settings.motion ? 0.f : 1.f;
    m_audio.play("show_modal");
}

void CardsActivity::openSettings() {
    auto onOff = [](bool v) { return std::string(v ? "On" : "Off"); };
    auto saveAnd = [this](std::function<void()> fn) {
        return [this, fn](int) {
            fn();
            m_settings.save();
            m_audio.play("toggle_on");
        };
    };
    openMenu("CardsNX Settings", {
        {"Music", nullptr, [this, onOff]() { return onOff(m_settings.music); },
         saveAnd([this]() {
             m_settings.music = !m_settings.music;
             m_audio.setMusic(m_settings.music, m_settings.musicVolume);
         })},
        {"Music volume", nullptr, [this]() { return std::to_string(m_settings.musicVolume) + "%"; },
         [this](int d) {
             m_settings.musicVolume = std::clamp(m_settings.musicVolume + d * 10, 0, 100);
             m_audio.setMusic(m_settings.music, m_settings.musicVolume);
             m_settings.save();
         }},
        {"Sounds", nullptr, [this, onOff]() { return onOff(m_settings.sounds); },
         saveAnd([this]() {
             m_settings.sounds = !m_settings.sounds;
             m_audio.setSounds(m_settings.sounds);
         })},
        {"Animations", nullptr, [this, onOff]() { return onOff(m_settings.motion); },
         saveAnd([this]() { m_settings.motion = !m_settings.motion; })},
        {"Nextendo players", nullptr, [this, onOff]() { return onOff(m_settings.players); },
         saveAnd([this]() { m_settings.players = !m_settings.players; })},
        {"Background art", nullptr,
         [this]() { return m_settings.haze <= 20 ? "Sharp" : m_settings.haze <= 50 ? "Soft" : "Softer"; },
         [this](int d) {
             m_settings.haze = std::clamp(m_settings.haze + d * 10, 0, 80);
             m_settings.save();
         }},
        {"Card pictures", nullptr, [this]() { return std::string(m_settings.boxArt ? "Box covers" : "eShop banners"); },
         saveAnd([this]() {
             m_settings.boxArt = !m_settings.boxArt;
             m_boxArt = m_settings.boxArt;
             forgetAllArt();
         })},
        {"Start on", nullptr, [this]() { return std::string(kPageNames[m_settings.startPage]); },
         [this](int d) {
             m_settings.startPage = (m_settings.startPage + d + kPageCount) % kPageCount;
             m_settings.save();
         }},
        {"Download all pictures again", [this]() {
             forgetAllArt();
             say("Pictures will download again.");
         }, nullptr, nullptr},
        {std::string("About: CardsNX ") + HOMENX_BUILD, nullptr, nullptr, nullptr},
    });
}

void CardsActivity::openPower() {
    auto doPower = [this](Power p) {
        return [this, p]() {
            m_settings.save();
            m_audio.fadeOut(200);
            std::string error;
            if (!power(p, error)) say(error);
            else if (p != Power::Sleep) app().requestExit();
        };
    };
    std::vector<MenuItem> items = {
        {"Sleep", doPower(Power::Sleep), nullptr, nullptr},
        {"Restart", doPower(Power::Restart), nullptr, nullptr},
        {"Shut down", doPower(Power::Shutdown), nullptr, nullptr},
    };
    items.push_back({canRebootToPayload() ? "Reboot to Hekate" : "Hekate: not on this model",
                     doPower(Power::Hekate), nullptr, nullptr});
    items.push_back({"Cancel", [] {}, nullptr, nullptr});
    openMenu("Power", std::move(items));
}

void CardsActivity::openNotifications() {
    m_notifRows.clear();
    for (const auto& it : notifications::recent(6)) {
        std::string title = it.title;
        for (size_t i = 0; i < title.size(); ++i) // Discord's emoji marks aren't letters here
            if (static_cast<unsigned char>(title[i]) >= 0xF3) title.erase(i--, 4);
        m_notifRows.push_back({(it.kind.rfind("discord", 0) == 0 ? "Discord - " : "") + title, it.text});
    }
    notifications::markSeen();
    m_unread = 0;
    m_notifOpen = true;
    m_menuIn = m_settings.motion ? 0.f : 1.f;
    m_audio.play("show_modal");
}

void CardsActivity::say(const std::string& message) {
    m_message = message;
    m_messageFor = 4.5f;
}

void CardsActivity::refreshStatus() {
    TimeCalendarTime t{};
    TimeCalendarAdditionalInfo info{};
    u64 now = 0;
    if (R_SUCCEEDED(timeGetCurrentTime(TimeType_Default, &now)) && R_SUCCEEDED(timeToCalendarTimeWithMyRule(now, &t, &info))) {
        char buf[8];
        std::snprintf(buf, sizeof(buf), "%02d:%02d", t.hour, t.minute);
        m_clock = buf;
    }
    u32 charge = 0;
    if (R_SUCCEEDED(psmGetBatteryChargePercentage(&charge))) m_battery = std::to_string(charge) + "%";
    NifmInternetConnectionType type{};
    u32 strength = 0;
    NifmInternetConnectionStatus status{};
    m_online = R_SUCCEEDED(nifmGetInternetConnectionStatus(&type, &strength, &status))
            && status == NifmInternetConnectionStatus_Connected;
    if (FsFileSystem* fs = fsdevGetDeviceFileSystem("sdmc")) {
        s64 free = 0;
        if (R_SUCCEEDED(fsFsGetFreeSpace(fs, "/", &free))) {
            char buf[32];
            std::snprintf(buf, sizeof(buf), "%.2f GB free", static_cast<double>(free) / 1e9);
            m_free = buf;
        }
    }
}

void CardsActivity::press(int zone, int index) {
    m_pressZone = zone;
    m_pressIndex = index;
    m_pressT = 0.f;
}

void CardsActivity::activateTop(int index) {
    press(0, index);
    switch (index) {
        case 0:
            openMenu("CardsNX", {
                {"Games", [this]() { if (m_page != kGames) switchPage(kGames - m_page); }, nullptr, nullptr},
                {"Homebrew", [this]() { if (m_page != kHomebrew) switchPage(kHomebrew - m_page); }, nullptr, nullptr},
                {"Nintendo", [this]() { if (m_page != kNintendo) switchPage(kNintendo - m_page); }, nullptr, nullptr},
                {"Settings", [this]() { openSettings(); }, nullptr, nullptr},
                {"Close CardsNX", [this]() { app().requestExit(); }, nullptr, nullptr},
            });
            break;
        case 1:
            if (m_page == kGames) {
                sortGames((m_settings.sort + 1) % 3);
                m_settings.save();
                m_audio.play("toggle_on");
            } else {
                switchPage(1);
            }
            break;
        case 2:
            say(m_playersTotal < 0 ? "Counting Nextendo players..."
                                   : std::to_string(m_playersTotal) + " players online in Nextendo.");
            break;
        case 3: openNotifications(); break;
        case 4: openPower(); break;
        case 5: openSettings(); break;
        default: break;
    }
}

void CardsActivity::activateBottom(int index) {
    press(2, index);
    const Game* g = current();
    switch (index) {
        case 0: play(); break;
        case 1: {
            if (!g) break;
            const std::uint64_t id = g->id;
            openMenu(g->name, {
                {g->path.empty() && g->action.empty() ? "Play" : "Open", [this]() { play(); }, nullptr, nullptr},
                {"Download new pictures", [this, id]() {
                     forgetArt(id);
                     auto it = m_art.find(id);
                     if (it != m_art.end()) {
                         it->second.cardAsked = it->second.bgAsked = false;
                         app().gpu().waitIdle(); // they may still be drawing
                         it->second.card.reset();
                         it->second.bg.reset();
                     }
                     wantArt();
                 }, nullptr, nullptr},
                {"Cancel", [] {}, nullptr, nullptr},
            });
            break;
        }
        case 2: switchPage(1); break;
        case 3: m_settings.save(); app().requestExit(); break;
        default: break;
    }
}

// Text is cached per font as pictures. When a font's cache is full it stops
// making new ones (text went missing): half of it goes, once the GPU is done.
void CardsActivity::keepTextCachesSmall() {
    bool waited = false;
    for (nxui::Font* f : {&m_fTitle, &m_fPlay, &m_fLabel, &m_fPill, &m_fBody, &m_fBadge, &m_fCount}) {
        if (!f->maintenanceRequested()) continue;
        if (!waited) app().gpu().waitIdle();
        waited = true;
        f->trimCache(f->cacheEntryCount() / 2, f->cacheBytes() / 2);
    }
    if (waited) app().renderer().reclaimReleasedTextureSlotsAfterIdle();
}

// ---------------------------------------------------------------- touch

bool CardsActivity::handleTouch() {
    auto& in = app().input();
    const float x = in.touchX(), y = in.touchY();
    auto inCircle = [&](float cx, float cy, float r) { return (x - cx) * (x - cx) + (y - cy) * (y - cy) <= r * r; };

    // A popup: a row runs (a setting changes: left half down, right half up); beside it closes.
    if (m_notifOpen) {
        if (in.touchUp()) {
            m_notifOpen = false;
            m_audio.play("hide_modal");
        }
        return in.isTouching() || in.touchUp();
    }
    if (!m_menu.empty()) {
        if (!in.touchUp()) return in.isTouching();
        const float w = 560.f, rowH = 54.f, h = 82.f + rowH * static_cast<float>(m_menu.size());
        const nxui::Rect box{(1280.f - w) * 0.5f, (720.f - h) * 0.5f, w, h};
        if (!box.contains(x, y)) {
            m_menu.clear();
            m_audio.play("hide_modal");
            return true;
        }
        const int row = static_cast<int>((y - box.y - 66.f) / rowH);
        if (y < box.y + 66.f || row < 0 || row >= static_cast<int>(m_menu.size())) return true;
        m_menuSel = row;
        MenuItem& item = m_menu[(size_t)row];
        if (item.change) {
            item.change(x < box.x + box.width * 0.5f ? -1 : 1);
        } else if (item.run) {
            auto run = item.run;
            m_menu.clear();
            run();
        }
        return true;
    }

    if (in.touchDown()) {
        m_touchOnCards = y > 100.f && y < 490.f;
        m_touchScroll = m_scroll;
        m_dragging = false;
    }
    // Dragging the row: it follows the finger, the card under the middle is chosen.
    if (in.isTouching() && m_touchOnCards) {
        const float dx = x - in.touchStartX();
        if (!m_dragging && std::abs(dx) > 14.f) m_dragging = true;
        if (m_dragging && !games().empty()) {
            const float last = static_cast<float>(games().size() - 1);
            m_scroll = std::clamp(m_touchScroll - dx / kCardPitch, -0.35f, last + 0.35f);
            const int near = std::clamp(static_cast<int>(std::lround(m_scroll)), 0, static_cast<int>(last));
            if (near != sel()) select(near);
        }
        return true;
    }
    if (!in.touchUp()) return false;
    if (m_dragging) { // let go: it settles on the nearest card
        m_dragging = false;
        return true;
    }

    // A tap.
    if (m_touchOnCards) {
        for (int i = 0; i < static_cast<int>(games().size()); ++i) {
            nxui::Rect card;
            float t = 0.f;
            if (!cardRect(i, card, t) || !card.contains(x, y)) continue;
            m_zone = Zone::Cards;
            if (i == sel()) {
                press(1, i);
                play();
            } else {
                select(i);
            }
            return true;
        }
        return true;
    }
    if (y < 95.f) { // the top bar
        int hit = -1;
        if (inCircle(134.f, kTopY, 40.f)) hit = 0;
        else if (inCircle(219.f, kTopY, 40.f) || (x > 260.f && x < 420.f)) hit = 1;
        else if (x > kFriendsX - 30.f && x < kFriendsX + 60.f) hit = 2;
        else if (inCircle(kBellX, kTopY, 32.f)) hit = 3;
        else if (inCircle(kPowerX, kTopY, 40.f)) hit = 4;
        else if (inCircle(kGearX, kTopY, 40.f)) hit = 5;
        if (hit >= 0) {
            m_zone = Zone::Top;
            m_topSel = hit;
            activateTop(hit);
        }
        return true;
    }
    if (kPanel.contains(x, y)) { // the panel's buttons
        const float by = kPanel.y + 108.f, cy = by + 38.5f, right = kPanel.x + kPanel.width - 22.f;
        int hit = -1;
        if (nxui::Rect{kPanel.x + 22.f, by, 262.f, 77.f}.contains(x, y)) hit = 0;
        else if (inCircle(kPanel.x + 22.f + 262.f + 8.f + 38.5f, cy, 40.f)) hit = 1;
        else if (inCircle(right - 77.f - 8.f - 38.5f, cy, 40.f)) hit = 2;
        else if (inCircle(right - 38.5f, cy, 40.f)) hit = 3;
        if (hit >= 0) {
            m_zone = Zone::Bottom;
            m_bottomSel = hit;
            activateBottom(hit);
        }
        return true;
    }
    return true;
}

// ---------------------------------------------------------------- frame

void CardsActivity::onUpdate(float dt) {
    m_time += dt;
    keepTextCachesSmall();
    takeArt();
    m_audio.update();
    const bool motion = m_settings.motion;
    auto step = [&](float& v, float seconds) { v = motion ? std::min(1.f, v + dt / seconds) : 1.f; };
    step(m_intro, 1.1f);
    step(m_pageIn, 0.38f);
    step(m_detailIn, 0.25f);
    step(m_sheen, 0.8f);
    step(m_menuIn, 0.2f);
    step(m_pressT, 0.22f);
    step(m_bgFade, 0.45f);
    const float target = static_cast<float>(sel());
    if (!m_dragging) {
        m_scroll = motion ? m_scroll + (target - m_scroll) * (1.f - std::exp(-dt * 13.f)) : target;
        if (std::abs(m_scroll - target) < 0.001f) m_scroll = target;
    }
    if (m_messageFor > 0.f) m_messageFor -= dt;
    if ((m_statusIn -= dt) <= 0.f) {
        m_statusIn = 1.f;
        refreshStatus();
    }

    auto& in = app().input();
    using B = nxui::Button;
    const bool aDown = in.isDown(B::A), bDown = in.isDown(B::B);
    const bool upDown = in.isDown(B::DUp) || in.isDown(B::LStickU);
    const bool downDown = in.isDown(B::DDown) || in.isDown(B::LStickD);
    const bool leftDown = in.isDown(B::DLeft) || in.isDown(B::LStickL);
    const bool rightDown = in.isDown(B::DRight) || in.isDown(B::LStickR);

    if (handleTouch()) return;

    if (m_notifOpen) {
        if (bDown || aDown || in.isDown(B::Minus)) {
            m_notifOpen = false;
            m_audio.play("hide_modal");
        }
        if (in.isDown(B::X)) {
            notifications::clear();
            m_notifRows.clear();
            m_audio.play("toggle_off");
        }
        return;
    }
    if (!m_menu.empty()) {
        const int n = static_cast<int>(m_menu.size());
        if (upDown && m_menuSel > 0) {
            --m_menuSel;
            m_audio.play("navigation");
        }
        if (downDown && m_menuSel < n - 1) {
            ++m_menuSel;
            m_audio.play("navigation");
        }
        MenuItem& item = m_menu[(size_t)m_menuSel];
        if ((leftDown || rightDown) && item.change) item.change(leftDown ? -1 : 1);
        if (bDown) {
            m_menu.clear();
            m_audio.play("hide_modal");
        } else if (aDown) {
            if (item.change) {
                item.change(1);
            } else {
                auto run = item.run;
                m_menu.clear();
                if (run) run();
            }
        }
        return;
    }

    // Left / right: repeating while held.
    const bool left = in.isHeld(B::DLeft) || in.isHeld(B::LStickL);
    const bool right = in.isHeld(B::DRight) || in.isHeld(B::LStickR);
    int move = 0;
    if (leftDown) {
        move = -1;
        m_repeat = 0.32f;
    } else if (rightDown) {
        move = 1;
        m_repeat = 0.32f;
    } else if (left || right) {
        if ((m_repeat -= dt) <= 0.f) {
            move = left ? -1 : 1;
            m_repeat = 0.075f;
        }
    }
    if (move) {
        if (m_zone == Zone::Cards) {
            select(sel() + move);
        } else {
            int& s = m_zone == Zone::Top ? m_topSel : m_bottomSel;
            const int next = std::clamp(s + move, 0, m_zone == Zone::Top ? 5 : 3);
            if (next != s) m_audio.play("navigation");
            s = next;
        }
    }
    if (upDown && m_zone != Zone::Top) {
        m_zone = m_zone == Zone::Bottom ? Zone::Cards : Zone::Top;
        m_audio.play("navigation");
    }
    if (downDown && m_zone != Zone::Bottom) {
        m_zone = m_zone == Zone::Top ? Zone::Cards : Zone::Bottom;
        m_audio.play("navigation");
    }
    if (aDown) {
        if (m_zone == Zone::Cards) {
            press(1, sel());
            play();
        } else if (m_zone == Zone::Top) {
            activateTop(m_topSel);
        } else {
            activateBottom(m_bottomSel);
        }
    }
    if (bDown && m_zone != Zone::Cards) m_zone = Zone::Cards;
    if (in.isDown(B::X)) activateBottom(1);
    if (in.isDown(B::Y)) activateTop(1);
    if (in.isDown(B::Minus)) openNotifications();
    if (in.isDown(B::L)) select(sel() - 5);
    if (in.isDown(B::R)) select(sel() + 5);
    if (in.isDown(B::ZL)) switchPage(-1);
    if (in.isDown(B::ZR)) switchPage(1);
    if (in.isDown(B::Plus)) {
        m_settings.save();
        app().requestExit();
    }
}

void CardsActivity::draw(nxui::Renderer& r) {
    drawBackground(r);
    drawCards(r);
    // One blurred copy of the frame for every glass shape on top of it.
    m_frosted = r.gpu().offscreenReady();
    if (m_frosted) {
        r.captureToOffscreen(false);
        r.applyBlur(2.5f, 2);
    }
    drawCardExtras(r);
    const float barIn = easeOut(m_intro * 2.2f);
    drawTopBar(r, barIn);
    drawPanel(r, easeOut((m_intro - 0.25f) * 2.f));
    if (m_messageFor > 0.f && !m_message.empty()) {
        const float a = std::min(1.f, m_messageFor * 3.f) * easeOut((4.5f - m_messageFor) * 5.f);
        // Two lines when it's long: words up to the width, then the rest.
        std::string first = m_message, second;
        if (measured(m_fBody, m_message).x > 820.f) {
            size_t cut = m_message.size();
            while (cut != std::string::npos && cut > 0 && measured(m_fBody, m_message.substr(0, cut)).x > 820.f)
                cut = m_message.rfind(' ', cut - 1);
            if (cut != std::string::npos && cut > 0) {
                first = m_message.substr(0, cut);
                second = fit(m_fBody, m_message.substr(cut + 1), 820.f);
            } else {
                first = fit(m_fBody, m_message, 820.f);
            }
        }
        const float w = std::max(measured(m_fBody, first).x, measured(m_fBody, second).x) + 44.f;
        const float h = second.empty() ? 42.f : 68.f;
        const nxui::Rect pill{640.f - w * 0.5f, 450.f + 10.f * (1.f - a) - (h - 42.f) * 0.5f, w, h};
        frost(r, pill, 21.f, nxui::Color(0.06f, 0.07f, 0.10f, 0.75f * a));
        r.drawRoundedRectOutline(pill, nxui::Color(1.f, 1.f, 1.f, 0.15f * a), 21.f, 1.5f);
        textMid(r, m_fBody, first, pill.x + 22.f, pill.y + (second.empty() ? 21.f : 22.f), kWhite.withAlpha(a));
        if (!second.empty()) textMid(r, m_fBody, second, pill.x + 22.f, pill.y + 46.f, kWhite.withAlpha(a));
    }
    drawPopup(r);
}

// ---------------------------------------------------------------- drawing

void CardsActivity::icon(nxui::Renderer& r, const char* name, float cx, float cy, float size, const nxui::Color& tint) {
    auto it = m_icons.find(name);
    if (it == m_icons.end() || !it->second->valid()) return;
    r.drawTexture(it->second.get(), {cx - size * 0.5f, cy - size * 0.5f, size, size}, tint);
}

void CardsActivity::frost(nxui::Renderer& r, const nxui::Rect& rect, float radius, const nxui::Color& tint) {
    if (m_frosted) r.drawOffscreenRounded(0, rect, radius, kWhite.withAlpha(std::min(1.f, tint.a * 3.f)));
    r.drawRoundedRect(rect, tint, radius);
}

void CardsActivity::glassCircle(nxui::Renderer& r, float cx, float cy, float d, bool focused, const char* name,
                                float iconSize, float a, float pressed) {
    const float s = 1.f - 0.08f * pressed;
    d *= s;
    const nxui::Rect box{cx - d * 0.5f, cy - d * 0.5f, d, d};
    frost(r, box, d * 0.5f, focused ? nxui::Color(1.f, 1.f, 1.f, 0.22f * a) : kGlass.withAlpha(kGlass.a * a));
    const float pulse = 0.8f + 0.2f * std::sin(m_time * 5.f);
    r.drawRoundedRectOutline(box, focused ? kWhite.withAlpha(pulse * a) : kGlassEdge.withAlpha(kGlassEdge.a * a), d * 0.5f,
                             focused ? 3.f : 1.5f);
    icon(r, name, cx, cy, iconSize * s, kWhite.withAlpha(a));
}

void CardsActivity::drawBackground(nxui::Renderer& r) {
    r.drawGradientRect({0.f, 0.f, 1280.f, 720.f}, nxui::Color(0.36f, 0.41f, 0.55f, 1.f),
                       nxui::Color(0.22f, 0.25f, 0.34f, 1.f));
    auto bgOf = [this](std::uint64_t id) -> const nxui::Texture* {
        auto it = m_art.find(id);
        return it != m_art.end() && it->second.bg && it->second.bg->valid() ? it->second.bg.get() : nullptr;
    };
    const nxui::Rect screen{0.f, 0.f, 1280.f, 720.f};
    // A slow drift (zoom and pan), so the art feels alive.
    const bool motion = m_settings.motion;
    const float zoom = motion ? 1.06f + 0.03f * std::sin(m_time * 0.11f) : 1.04f;
    const float panX = motion ? 0.25f * std::sin(m_time * 0.07f) : 0.f;
    const float panY = motion ? 0.2f * std::cos(m_time * 0.05f) : 0.f;
    if (m_bgFade < 1.f)
        if (const nxui::Texture* prev = bgOf(m_bgPrevId)) drawCover(r, prev, screen, 0.f, kWhite, 0.f, 0.f, zoom, panX, panY);
    if (const nxui::Texture* cur = bgOf(m_bgId))
        drawCover(r, cur, screen, 0.f, kWhite.withAlpha(easeOut(m_bgFade)), 0.f, 0.f, zoom, panX, panY);
    // The haze over it (Settings > Background art), and a little depth at the edges.
    const float haze = static_cast<float>(m_settings.haze) / 100.f;
    r.drawRect(screen, nxui::Color(0.38f, 0.43f, 0.56f, haze));
    r.drawGradientRect({0.f, 0.f, 1280.f, 140.f}, nxui::Color(0.05f, 0.06f, 0.10f, 0.35f), nxui::Color(0.05f, 0.06f, 0.10f, 0.f));
    r.drawGradientRect({0.f, 560.f, 1280.f, 160.f}, nxui::Color(0.05f, 0.06f, 0.10f, 0.f), nxui::Color(0.05f, 0.06f, 0.10f, 0.40f));
}

void CardsActivity::drawTopBar(nxui::Renderer& r, float a) {
    if (a <= 0.001f) return;
    const bool top = m_zone == Zone::Top;
    const float y = kTopY - 18.f * (1.f - a);
    auto pressedOf = [this](int index) {
        return m_pressZone == 0 && m_pressIndex == index ? std::sin(clamp01(m_pressT) * 3.14159f) : 0.f;
    };
    glassCircle(r, 134.f, y, kCircle, top && m_topSel == 0, "dots", 34.f, a, pressedOf(0));
    glassCircle(r, 219.f, y, kCircle, top && m_topSel == 1, "sort", 34.f, a, pressedOf(1));
    const std::string label = (m_page == kGames ? std::string(kSortNames[m_settings.sort]) : std::string(kPageNames[m_page]))
                            + "  " + std::to_string(games().size());
    textMid(r, m_fLabel, label, 268.f, y, kWhite.withAlpha(a));

    // The pill: you, online, the time, battery, free space.
    const std::string word = m_online ? "Online" : "Offline";
    const float avatar = 32.f, gap = 16.f;
    const float wWord = measured(m_fPill, word).x, wClock = measured(m_fPill, m_clock).x;
    const float wBattery = measured(m_fPill, m_battery).x, wFree = measured(m_fPill, m_free).x;
    const float width = 12.f + avatar + 12.f + wWord + gap + wClock + gap + wBattery + gap + 18.f + 8.f + wFree + 18.f;
    const nxui::Rect pill{600.f - width * 0.5f, y - 26.f, width, 52.f};
    frost(r, pill, 26.f, kGlass.withAlpha(kGlass.a * a));
    r.drawRoundedRectOutline(pill, kGlassEdge.withAlpha(kGlassEdge.a * a), 26.f, 1.5f);
    float x = pill.x + 12.f;
    const nxui::Rect face{x, y - avatar * 0.5f, avatar, avatar};
    if (m_avatar && m_avatar->valid()) r.drawTextureRounded(m_avatar.get(), face, avatar * 0.5f, kWhite.withAlpha(a));
    else r.drawCircle({face.x + avatar * 0.5f, y}, avatar * 0.5f, nxui::Color(1.f, 1.f, 1.f, 0.3f * a), 32);
    x += avatar + 12.f;
    textMid(r, m_fPill, word, x, y, (m_online ? kGreen : nxui::Color(0.75f, 0.77f, 0.8f, 1.f)).withAlpha(a));
    x += wWord + gap;
    textMid(r, m_fPill, m_clock, x, y, kWhite.withAlpha(a));
    x += wClock + gap;
    textMid(r, m_fPill, m_battery, x, y, kWhite.withAlpha(a));
    x += wBattery + gap;
    icon(r, "thermo", x + 9.f, y, 24.f, nxui::Color(0.39f, 0.82f, 1.f, a));
    x += 18.f + 8.f;
    textMid(r, m_fPill, m_free, x, y, kWhite.withAlpha(a));

    // Nextendo's players, notifications, power, settings.
    const float ring = 0.8f + 0.2f * std::sin(m_time * 5.f);
    icon(r, "friends", kFriendsX, y, 34.f, kWhite.withAlpha(a));
    textMid(r, m_fLabel, m_playersTotal < 0 ? "?" : std::to_string(m_playersTotal), kFriendsX + 21.f, y,
            kWhite.withAlpha(a));
    if (top && m_topSel == 2)
        r.drawRoundedRectOutline({kFriendsX - 26.f, y - 26.f, 90.f, 52.f}, kWhite.withAlpha(ring * a), 26.f, 3.f);
    icon(r, "bell", kBellX, y, 36.f, kWhite.withAlpha(a));
    if (top && m_topSel == 3) r.drawRoundedRectOutline({kBellX - 26.f, y - 26.f, 52.f, 52.f}, kWhite.withAlpha(ring * a), 26.f, 3.f);
    if (m_unread > 0) {
        const std::string n = m_unread > 99 ? "99+" : std::to_string(m_unread);
        const float bw = std::max(24.f, measured(m_fBadge, n).x + 14.f);
        const float s = 1.f + (m_settings.motion ? 0.06f * std::sin(m_time * 4.f) : 0.f);
        const nxui::Rect badge = scaled({kBellX + 6.f, y - 37.f, bw, 24.f}, s);
        r.drawRoundedRect(badge, kRed.withAlpha(a), badge.height * 0.5f);
        textMid(r, m_fBadge, n, badge.x + (badge.width - measured(m_fBadge, n).x) * 0.5f, badge.y + badge.height * 0.5f,
                kWhite.withAlpha(a));
    }
    glassCircle(r, kPowerX, y, kCircle, top && m_topSel == 4, "power", 34.f, a, pressedOf(4));
    glassCircle(r, kGearX, y, kCircle, top && m_topSel == 5, "gear", 36.f, a, pressedOf(5));
}

bool CardsActivity::cardRect(int index, nxui::Rect& card, float& chosen) {
    const float rel = static_cast<float>(index) - m_scroll;
    // A page switch slides the row in from the side.
    const float slide = (1.f - easeOut(m_pageIn)) * 160.f * static_cast<float>(m_pageDir);
    const float x0 = kCardFirstX + rel * kCardPitch + slide;
    if (x0 > 1290.f || x0 + kCardPitch < -10.f) return false;
    chosen = clamp01(1.f - std::abs(rel));
    const float grow = easeOutBack(chosen);
    const float w = kCardW + kCardGrowW * grow, h = kCardH + kCardGrowH * grow;
    // Opening: the cards rise in one after another.
    const float k = std::max(0.f, rel + 1.f);
    const float rise = 1.f - easeOut((m_intro - 0.1f - 0.07f * k) / 0.5f);
    card = {x0 + kCardW * 0.5f - w * 0.5f, kCardMidY - h * 0.5f - 7.f * chosen + 60.f * rise, w, h};
    return true;
}

void CardsActivity::drawCards(nxui::Renderer& r) {
    auto& page = games();
    if (page.empty()) {
        textMid(r, m_fLabel, m_page == kHomebrew ? "No homebrew in sdmc:/switch" : "No games installed", 480.f, kCardMidY,
                kWhite);
        return;
    }
    const bool cardsFocused = m_zone == Zone::Cards;
    const float pageAlpha = easeOut(m_pageIn);
    for (int i = 0; i < static_cast<int>(page.size()); ++i) {
        nxui::Rect card;
        float t = 0.f;
        if (!cardRect(i, card, t)) continue;
        const Game& g = page[(size_t)i];
        if (g.homebrew || !g.action.empty()) continue; // glass: drawn over the blur (drawCardExtras)
        const float k = std::max(0.f, static_cast<float>(i) - m_scroll + 1.f);
        const float alpha = pageAlpha * easeOut((m_intro - 0.1f - 0.07f * k) / 0.5f);
        if (alpha <= 0.01f) continue;
        const float pressed = m_pressZone == 1 && m_pressIndex == i ? std::sin(clamp01(m_pressT) * 3.14159f) : 0.f;
        if (pressed > 0.f) card = scaled(card, 1.f - 0.05f * pressed);

        const float shadow = 0.18f + 0.18f * t;
        r.drawRoundedRect({card.x - 3.f, card.y + 6.f + 6.f * t, card.width + 6.f, card.height + 6.f},
                          nxui::Color(0.f, 0.f, 0.f, shadow * alpha), kCardRadius + 3.f);
        auto it = m_art.find(g.id);
        const nxui::Texture* tex = it != m_art.end() && it->second.card && it->second.card->valid() ? it->second.card.get()
                                                                                                    : nullptr;
        const float artIn = tex ? easeOut((m_time - it->second.cardIn) / 0.35f) : 0.f;
        // Under the art (and while it's coming): the name on glass.
        if (artIn < 1.f) {
            r.drawRoundedRect(card, nxui::Color(0.28f, 0.31f, 0.40f, 0.85f * alpha), kCardRadius);
            const std::string name = fit(m_fLabel, g.name, card.width - 28.f, 0.9f);
            textMid(r, m_fLabel, name, card.x + (card.width - measured(m_fLabel, name).x * 0.9f) * 0.5f,
                    card.y + card.height * 0.5f, kMuted.withAlpha(alpha * (1.f - artIn)), 0.9f);
        }
        if (tex) {
            // A Switch case cover without the red Nintendo Switch band on top:
            // the rest is the card's own shape, so all of it shows.
            const bool box = it->second.kind == ArtKind::Box;
            drawCover(r, tex, card, kCardRadius, kWhite.withAlpha(alpha * artIn), box ? 0.125f : 0.f, 0.f);
        }
        if (i < sel()) // the ones already passed sit back a little
            r.drawRoundedRect(card, nxui::Color(0.f, 0.f, 0.f, 0.30f * (1.f - t) * alpha), kCardRadius);
        if (t > 0.01f) {
            const float ring = cardsFocused ? 0.85f + 0.15f * std::sin(m_time * 4.f) : 0.55f;
            r.drawRoundedRectOutline(card, kWhite.withAlpha(ring * t * alpha), kCardRadius, 3.5f);
        }
    }
}

// Nextendo: players online in each game now (a glass pill, its dot breathing);
// and the light that crosses the card just chosen.
void CardsActivity::drawCardExtras(nxui::Renderer& r) {
    auto& page = games();
    for (int i = 0; i < static_cast<int>(page.size()); ++i) {
        nxui::Rect card;
        float t = 0.f;
        if (!cardRect(i, card, t)) continue;
        const float k = std::max(0.f, static_cast<float>(i) - m_scroll + 1.f);
        const float alpha = easeOut(m_pageIn) * easeOut((m_intro - 0.1f - 0.07f * k) / 0.5f);
        if (alpha <= 0.01f) continue;

        const Game& g = page[(size_t)i];
        if (g.homebrew || !g.action.empty()) {
            // An app, in the glass of the rest of the screen: the blurred
            // background through it, its icon floating on a soft glow of its
            // own colours, its name and what it is below.
            const float pressed = m_pressZone == 1 && m_pressIndex == i ? std::sin(clamp01(m_pressT) * 3.14159f) : 0.f;
            if (pressed > 0.f) card = scaled(card, 1.f - 0.05f * pressed);
            r.drawRoundedRect({card.x - 3.f, card.y + 6.f + 6.f * t, card.width + 6.f, card.height + 6.f},
                              nxui::Color(0.f, 0.f, 0.f, (0.10f + 0.10f * t) * alpha), kCardRadius + 3.f);
            frost(r, card, kCardRadius, nxui::Color(0.17f, 0.20f, 0.27f, (0.30f + 0.08f * t) * alpha));
            r.drawGradientRect({card.x + 2.f, card.y + 2.f, card.width - 4.f, 70.f},
                               nxui::Color(1.f, 1.f, 1.f, 0.07f * alpha), nxui::Color(1.f, 1.f, 1.f, 0.f)); // a sheen of glass
            auto it = m_art.find(g.id);
            const nxui::Texture* tex = it != m_art.end() && it->second.card && it->second.card->valid() ? it->second.card.get()
                                                                                                        : nullptr;
            const float artIn = tex ? easeOut((m_time - it->second.cardIn) / 0.35f) : 0.f;
            const float side = 150.f + 8.f * t;
            const nxui::Rect iconRect{card.x + (card.width - side) * 0.5f, card.y + 40.f - 4.f * t, side, side};
            if (tex) {
                const float a = alpha * artIn;
                // Its glow: the icon itself, bigger and faint, behind it.
                r.drawTextureRounded(tex, scaled(iconRect, 1.22f), 52.f, kWhite.withAlpha(0.22f * a * (0.6f + 0.4f * t)));
                r.drawRoundedRect({iconRect.x, iconRect.y + 8.f, iconRect.width, iconRect.height},
                                  nxui::Color(0.f, 0.f, 0.f, 0.25f * a), 34.f); // its shadow
                r.drawTextureRounded(tex, iconRect, 34.f, kWhite.withAlpha(a));
            } else {
                r.drawRoundedRect(iconRect, nxui::Color(1.f, 1.f, 1.f, 0.08f * alpha), 34.f);
            }
            const float ns = fitScale(m_fLabel, g.name, card.width - 24.f);
            const std::string name = fit(m_fLabel, g.name, card.width - 24.f, ns);
            r.drawText(name, {card.x + (card.width - measured(m_fLabel, name).x * ns) * 0.5f, iconRect.y + side + 26.f},
                       &m_fLabel, kWhite.withAlpha(alpha), ns);
            const std::string about = !g.author.empty() ? g.author : (g.path.empty() ? "NRO forwarder" : "Homebrew");
            const std::string by = fit(m_fBody, about, card.width - 28.f);
            r.drawText(by, {card.x + (card.width - measured(m_fBody, by).x) * 0.5f, iconRect.y + side + 58.f}, &m_fBody,
                       kMuted.withAlpha(alpha));
            r.drawRoundedRectOutline(card, nxui::Color(1.f, 1.f, 1.f, 0.20f * alpha), kCardRadius, 1.5f);
            if (i < sel()) r.drawRoundedRect(card, nxui::Color(0.f, 0.f, 0.f, 0.22f * (1.f - t) * alpha), kCardRadius);
            if (t > 0.01f) {
                const float ring = m_zone == Zone::Cards ? 0.85f + 0.15f * std::sin(m_time * 4.f) : 0.55f;
                r.drawRoundedRectOutline(card, kWhite.withAlpha(ring * t * alpha), kCardRadius, 3.5f);
            }
        }

        if (i == sel() && m_sheen < 1.f && t > 0.5f) {
            const float e = easeOut(m_sheen);
            const float x = card.x - 120.f + (card.width + 240.f) * e;
            const float fade = std::sin(e * 3.14159f) * 0.28f * alpha;
            r.pushClipRect(card);
            for (int band = 0; band < 2; ++band) { // a wide soft band and a narrow bright one
                const float w = band ? 26.f : 80.f, al = band ? fade : fade * 0.45f;
                const nxui::Color c(1.f, 1.f, 1.f, al);
                r.drawTriangle({x, card.y}, {x + w, card.y}, {x - 90.f, card.y + card.height}, c);
                r.drawTriangle({x + w, card.y}, {x + w - 90.f, card.y + card.height}, {x - 90.f, card.y + card.height}, c);
            }
            r.popClipRect();
        }

        if (!m_settings.players) continue;
        auto p = m_players.find(page[(size_t)i].id);
        if (p == m_players.end() || p->second <= 0) continue;
        const std::string n = std::to_string(p->second);
        const nxui::Rect badge{card.x + 14.f, card.y + 14.f, 34.f + measured(m_fCount, n).x + 14.f, 34.f};
        frost(r, badge, 17.f, nxui::Color(0.05f, 0.07f, 0.09f, 0.42f * alpha));
        r.drawRoundedRectOutline(badge, nxui::Color(1.f, 1.f, 1.f, 0.16f * alpha), 17.f, 1.5f);
        const float breathe = m_settings.motion ? 0.5f + 0.5f * std::sin(m_time * 3.f) : 0.5f;
        r.drawCircle({badge.x + 18.f, badge.y + 17.f}, 8.f + 4.f * breathe, kGreen.withAlpha(0.25f * (1.f - breathe) * alpha), 24);
        r.drawCircle({badge.x + 18.f, badge.y + 17.f}, 8.f, kGreen.withAlpha(alpha), 24);
        textMid(r, m_fCount, n, badge.x + 33.f, badge.y + 17.f, kWhite.withAlpha(alpha));
        if (i < sel()) r.drawRoundedRect(badge, nxui::Color(0.f, 0.f, 0.f, 0.30f * (1.f - t) * alpha), 17.f);
    }
}

const nxui::Texture* CardsActivity::gameIconTexture() {
    const Game* g = current();
    if (!g) return nullptr;
    auto it = m_art.find(g->id);
    return it != m_art.end() && it->second.icon && it->second.icon->valid() ? it->second.icon.get() : nullptr;
}

void CardsActivity::drawPanel(nxui::Renderer& r, float a) {
    if (a <= 0.001f) return;
    const nxui::Rect panel{kPanel.x, kPanel.y + 40.f * (1.f - a), kPanel.width, kPanel.height};
    frost(r, panel, kPanelRadius, nxui::Color(0.16f, 0.19f, 0.26f, 0.34f * a));
    r.drawRoundedRectOutline(panel, nxui::Color(1.f, 1.f, 1.f, 0.22f * a), kPanelRadius, 1.5f);
    const Game* gp = current();
    if (!gp) return;
    const Game& g = *gp;
    // Its details fade and slide in when the chosen game changes.
    const float d = easeOut(m_detailIn);
    const float da = a * d, dx = 14.f * (1.f - d);

    const nxui::Rect iconRect{panel.x + 25.f + dx, panel.y + 20.f, 78.f, 78.f};
    if (const nxui::Texture* ic = gameIconTexture()) r.drawTextureRounded(ic, iconRect, 18.f, kWhite.withAlpha(da));
    const float tx = iconRect.x + iconRect.width + 15.f;
    const float maxW = panel.x + panel.width - 30.f - tx;
    const float ts = fitScale(m_fTitle, g.name, maxW);
    r.drawText(fit(m_fTitle, g.name, maxW, ts), {tx, panel.y + 20.f + (1.f - ts) * 14.f}, &m_fTitle, kWhite.withAlpha(da), ts);
    std::string about;
    if (!g.action.empty()) about = g.author;
    else if (g.homebrew) about = g.path.empty() ? "NRO forwarder" : (g.author.empty() ? "Homebrew" : "Homebrew by " + g.author);
    else about = playTime(g.playSeconds) + "    " + lastPlayed(g.lastPlayed);
    r.drawText(fit(m_fBody, about, maxW), {tx, panel.y + 60.f}, &m_fBody, kMuted.withAlpha(da));
    if (!g.version.empty()) {
        icon(r, "tag", tx + 8.f, panel.y + 95.f, 17.f, kMuted.withAlpha(da));
        const std::string v = (g.version[0] == 'v' || g.version[0] == 'V') ? g.version : "v" + g.version;
        textMid(r, m_fBody, v, tx + 22.f, panel.y + 95.f, kMuted.withAlpha(da), 0.95f);
    }

    // Play, its options, the page button, back.
    const bool bottom = m_zone == Zone::Bottom;
    auto pressedOf = [this](int index) {
        return m_pressZone == 2 && m_pressIndex == index ? std::sin(clamp01(m_pressT) * 3.14159f) : 0.f;
    };
    const float by = panel.y + 108.f;
    const nxui::Rect playRect = scaled({panel.x + 22.f, by, 262.f, 77.f}, 1.f - 0.06f * pressedOf(0));
    r.drawRoundedRect(playRect, kWhite.withAlpha(a), playRect.height * 0.5f);
    if (bottom && m_bottomSel == 0) {
        const float ring = 0.8f + 0.2f * std::sin(m_time * 5.f);
        r.drawRoundedRectOutline({playRect.x - 4.f, playRect.y - 4.f, playRect.width + 8.f, playRect.height + 8.f},
                                 kWhite.withAlpha(ring * a), playRect.height * 0.5f + 4.f, 3.f);
    }
    const nxui::Color ink(0.f, 0.f, 0.f, a);
    const char* word = g.path.empty() && g.action.empty() && !g.homebrew ? "Play" : "Open";
    const float playW = 24.f + 10.f + measured(m_fPlay, word).x;
    const float px = playRect.x + (playRect.width - playW) * 0.5f;
    icon(r, "play", px + 12.f, playRect.y + playRect.height * 0.5f, 24.f, ink);
    textMid(r, m_fPlay, word, px + 34.f, playRect.y + playRect.height * 0.5f, ink);
    glassCircle(r, panel.x + 22.f + 262.f + 8.f + 38.5f, by + 38.5f, 77.f, bottom && m_bottomSel == 1, "dots", 32.f, a,
                pressedOf(1));
    const float right = panel.x + panel.width - 22.f;
    glassCircle(r, right - 77.f - 8.f - 38.5f, by + 38.5f, 77.f, bottom && m_bottomSel == 2, "copy", 32.f, a, pressedOf(2));
    glassCircle(r, right - 38.5f, by + 38.5f, 77.f, bottom && m_bottomSel == 3, "home", 34.f, a, pressedOf(3));
    // Which page this is: three dots under the page button.
    for (int p = 0; p < kPageCount; ++p) {
        const float cx = right - 77.f - 8.f - 38.5f + (static_cast<float>(p) - 1.f) * 12.f;
        r.drawCircle({cx, by + 77.f + 9.f}, p == m_page ? 3.5f : 2.5f, kWhite.withAlpha((p == m_page ? 0.95f : 0.4f) * a), 12);
    }
}

void CardsActivity::drawPopup(nxui::Renderer& r) {
    const float a = easeOut(m_menuIn);
    if (m_notifOpen) {
        const float rows = static_cast<float>(std::max<size_t>(1, m_notifRows.size()));
        nxui::Rect box{kGearX + 38.f - 460.f, 84.f - 12.f * (1.f - a), 460.f, 60.f + rows * 64.f + 40.f};
        frost(r, box, 24.f, nxui::Color(0.10f, 0.11f, 0.15f, 0.82f * a));
        r.drawRoundedRectOutline(box, nxui::Color(1.f, 1.f, 1.f, 0.14f * a), 24.f, 1.5f);
        r.drawText("Notifications", {box.x + 22.f, box.y + 16.f}, &m_fLabel, kWhite.withAlpha(a));
        if (m_notifRows.empty())
            r.drawText("You're all caught up", {box.x + 22.f, box.y + 70.f}, &m_fBody, kMuted.withAlpha(a));
        for (size_t i = 0; i < m_notifRows.size(); ++i) {
            const float y = box.y + 60.f + static_cast<float>(i) * 64.f;
            r.drawText(fit(m_fPill, m_notifRows[i].first, box.width - 44.f), {box.x + 22.f, y + 6.f}, &m_fPill,
                       kWhite.withAlpha(a));
            r.drawText(fit(m_fBody, m_notifRows[i].second, box.width - 44.f), {box.x + 22.f, y + 32.f}, &m_fBody,
                       kMuted.withAlpha(a));
        }
        r.drawText(m_notifRows.empty() ? "B  Close" : "X  Clear all      B  Close",
                   {box.x + 22.f, box.y + box.height - 32.f}, &m_fBody, kMuted.withAlpha(a));
        return;
    }
    if (m_menu.empty()) return;
    r.drawRect({0.f, 0.f, 1280.f, 720.f}, nxui::Color(0.f, 0.f, 0.f, 0.42f * a));
    const float w = 560.f, rowH = 54.f, h = 82.f + rowH * static_cast<float>(m_menu.size());
    const nxui::Rect box = scaled({(1280.f - w) * 0.5f, (720.f - h) * 0.5f, w, h}, 0.94f + 0.06f * a);
    frost(r, box, 28.f, nxui::Color(0.10f, 0.11f, 0.15f, 0.85f * a));
    r.drawRoundedRectOutline(box, nxui::Color(1.f, 1.f, 1.f, 0.14f * a), 28.f, 1.5f);
    r.drawText(fit(m_fLabel, m_menuTitle, w - 48.f), {box.x + 26.f, box.y + 22.f}, &m_fLabel, kWhite.withAlpha(a));
    for (size_t i = 0; i < m_menu.size(); ++i) {
        const nxui::Rect row{box.x + 14.f, box.y + 66.f + rowH * static_cast<float>(i), box.width - 28.f, rowH - 6.f};
        const MenuItem& item = m_menu[i];
        const bool chosen = static_cast<int>(i) == m_menuSel;
        if (chosen) {
            r.drawRoundedRect(row, nxui::Color(1.f, 1.f, 1.f, 0.14f * a), 16.f);
            r.drawRoundedRectOutline(row, kWhite.withAlpha((0.8f + 0.2f * std::sin(m_time * 5.f)) * a), 16.f, 2.f);
        }
        const bool info = !item.run && !item.change; // a line to read (About)
        textMid(r, m_fPill, fit(m_fPill, item.label, row.width - 220.f), row.x + 18.f, row.y + row.height * 0.5f,
                (info ? kMuted : kWhite).withAlpha(a));
        if (item.value) {
            const std::string v = item.change ? "<  " + item.value() + "  >" : item.value();
            textMid(r, m_fPill, v, row.x + row.width - 18.f - measured(m_fPill, v).x, row.y + row.height * 0.5f,
                    nxui::Color(0.55f, 0.75f, 1.f, a));
        }
    }
}

} // namespace cards
