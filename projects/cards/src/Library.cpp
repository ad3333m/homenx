#include "Library.hpp"
#include "Settings.hpp"

#include <switch.h>
#include <switchu/control_cache.hpp>
#include <switchu/hekate_rtc.hpp>
#include <switchu/ns_ext.hpp>
#include "homebrew/HomebrewStore.hpp"
#ifdef SWITCHU_MENU
#include "themeshop/ThemeHttp.hpp"
#endif
#include <curl/curl.h>
#include <nlohmann/json.hpp>
#include <nxui/third_party/stb/stb_image.h>

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <map>
#include <set>
#include <memory>
#include <mutex>
#include <thread>
#include <nxui/core/ThreadPool.hpp>
#include <new>
#include <unordered_map>

namespace cards {
namespace {

constexpr const char* kCacheDir = "sdmc:/config/SwitchU/cards";
constexpr const char* kSettings = "sdmc:/config/SwitchU/settings.json";
constexpr const char* kSteamGridDb = "https://www.steamgriddb.com/api/v2";
constexpr const char* kNextendo = "https://nextendo.network";

std::string hexId(std::uint64_t id) {
    char buf[17];
    std::snprintf(buf, sizeof(buf), "%016llX", static_cast<unsigned long long>(id));
    return buf;
}

size_t toString(char* p, size_t size, size_t n, void* user) {
    auto* out = static_cast<std::string*>(user);
    if (out->size() + size * n > (16u << 20)) return 0; // nothing here is that big
    out->append(p, size * n);
    return size * n;
}

thread_local CURL* t_curl = nullptr; // each worker's: its connections stay open

// GET, the body in `out`; the HTTP status (0: no answer).
long get(const std::string& url, std::string& out, const std::vector<std::string>& headers = {}) {
    out.clear();
    if (!t_curl) t_curl = curl_easy_init();
    CURL* c = t_curl;
    if (!c) return 0;
    curl_easy_reset(c); // the open connections stay
    curl_slist* list = nullptr;
    for (const std::string& h : headers) list = curl_slist_append(list, h.c_str());
    // art.gametdb.com is on Let's Encrypt's newest chain (ISRG Root X1), which
    // the console's own store may not have: its root ships with the app.
    const std::string ca = std::string(kAssets) + "certs/isrg-root-x1.pem";
    if (url.find("gametdb.com") != std::string::npos) curl_easy_setopt(c, CURLOPT_CAINFO, ca.c_str());
    curl_easy_setopt(c, CURLOPT_URL, url.c_str());
    curl_easy_setopt(c, CURLOPT_USERAGENT, "CardsNX");
    curl_easy_setopt(c, CURLOPT_FOLLOWLOCATION, 1L);
    curl_easy_setopt(c, CURLOPT_NOSIGNAL, 1L);
    curl_easy_setopt(c, CURLOPT_CONNECTTIMEOUT, 10L);
    curl_easy_setopt(c, CURLOPT_TIMEOUT, 30L);
    curl_easy_setopt(c, CURLOPT_ACCEPT_ENCODING, "");
    curl_easy_setopt(c, CURLOPT_HTTPHEADER, list);
    curl_easy_setopt(c, CURLOPT_WRITEFUNCTION, toString);
    curl_easy_setopt(c, CURLOPT_WRITEDATA, &out);
    long status = 0;
    if (curl_easy_perform(c) == CURLE_OK) curl_easy_getinfo(c, CURLINFO_RESPONSE_CODE, &status);
    curl_slist_free_all(list);
    return status;
}

std::vector<std::uint8_t> readFile(const std::string& path) {
    std::ifstream in(path, std::ios::binary);
    return std::vector<std::uint8_t>(std::istreambuf_iterator<char>(in), {});
}

void writeFile(const std::string& path, const std::string& data) {
    std::error_code ec;
    std::filesystem::create_directories(kCacheDir, ec);
    // Its own .part per thread: two can save the same picture at once.
    const std::string tmp = path + ".part" + std::to_string(std::hash<std::thread::id>{}(std::this_thread::get_id()) % 1000);
    {
        std::ofstream out(tmp, std::ios::binary | std::ios::trunc);
        out.write(data.data(), static_cast<std::streamsize>(data.size()));
        if (!out) return;
    }
    std::filesystem::remove(path, ec);
    std::filesystem::rename(tmp, path, ec);
}

bool isPicture(const std::string& body) {
    return body.size() > 64 && ((static_cast<unsigned char>(body[0]) == 0xFF && static_cast<unsigned char>(body[1]) == 0xD8)
                                || body.compare(1, 3, "PNG") == 0);
}

std::string percentEncode(const std::string& s) {
    std::string out;
    for (unsigned char c : s) {
        if (std::isalnum(c) || c == '-' || c == '_' || c == '.' || c == '~') {
            out += static_cast<char>(c);
        } else {
            char buf[4];
            std::snprintf(buf, sizeof(buf), "%%%02X", c);
            out += buf;
        }
    }
    return out;
}

// The game on SteamGridDB, found once per name (pictures ask several times).
// 0: no answer (asked again later), -1: no such game.
long long steamGridDbGame(const std::string& name, const std::string& key) {
    static std::mutex foundMu;
    static std::map<std::string, long long> found;
    {
        std::lock_guard<std::mutex> lk(foundMu);
        auto it = found.find(name);
        if (it != found.end()) return it->second;
    }
    const std::vector<std::string> headers = {"Authorization: Bearer " + key, "Accept: application/json"};
    std::string body;
    if (get(std::string(kSteamGridDb) + "/search/autocomplete/" + percentEncode(name), body, headers) != 200) return 0;
    long long gameId = -1;
    try {
        const auto j = nlohmann::json::parse(body);
        if (j.contains("data") && j["data"].is_array() && !j["data"].empty())
            gameId = j["data"][0].value("id", -1LL);
    } catch (...) {
    }
    std::lock_guard<std::mutex> lk(foundMu);
    found[name] = gameId;
    return gameId;
}

// Its pictures of `kind` ("grids", "heroes", "icons", "logos"): each one's
// url and thumbnail, best first. answered: the list came.
std::vector<std::pair<std::string, std::string>> steamGridDbAssets(long long gameId, const std::string& key, const char* kind,
                                                                   const std::string& query, size_t max, bool* answered) {
    std::vector<std::pair<std::string, std::string>> out;
    if (gameId <= 0) return out;
    const std::vector<std::string> headers = {"Authorization: Bearer " + key, "Accept: application/json"};
    std::string body;
    if (get(std::string(kSteamGridDb) + "/" + kind + "/game/" + std::to_string(gameId) + "?" + query, body, headers) != 200)
        return out;
    if (answered) *answered = true;
    try {
        const auto j = nlohmann::json::parse(body);
        if (j.contains("data") && j["data"].is_array())
            for (const auto& a : j["data"]) {
                if (a.is_object()) out.emplace_back(a.value("url", std::string()), a.value("thumb", std::string()));
                if (out.size() == max) break;
            }
    } catch (...) {
    }
    return out;
}

// Most points along its edges see-through (a subject may touch them; a
// square with a background, even with round corners, doesn't pass).
bool clearEdges(const unsigned char* px, int w, int h) {
    int seen = 0;
    for (int i = 0; i < 16; ++i) {
        const int x = i * (w - 1) / 15, y = i * (h - 1) / 15;
        for (const auto& [cx, cy] : {std::pair{x, 0}, std::pair{x, h - 1}, std::pair{0, y}, std::pair{w - 1, y}})
            seen += px[(static_cast<size_t>(cy) * w + cx) * 4 + 3] < 40;
    }
    return seen * 10 >= 64 * 6;
}

// The first picture SteamGridDB has of `kind` for the game that came and isn't
// the notice for one taken down. clear: only one with see-through edges (an
// icon with no background). answered: the list came.
std::string steamGridDbPicture(const Game& game, const std::string& key, const char* kind, const std::string& query,
                               bool clear = false, bool* answered = nullptr, size_t tries = 6) {
    std::string body;
    for (const auto& [url, thumb] : steamGridDbAssets(steamGridDbGame(game.name, key), key, kind, query, tries, answered)) {
        if (url.empty() || get(url, body) != 200 || !isPicture(body)) continue;
        int w = 0, h = 0, ch = 0;
        stbi_uc* px = stbi_load_from_memory(reinterpret_cast<const stbi_uc*>(body.data()), static_cast<int>(body.size()),
                                            &w, &h, &ch, 4);
        const bool good = px && (clear ? clearEdges(px, w, h) : !removedNotice(px, w, h));
        stbi_image_free(px);
        if (good) return body;
    }
    return {};
}

// Its name, then the part before a colon or dash ("Minecraft: Nintendo Switch
// Edition" is "Minecraft" on SteamGridDB, with far more pictures).
std::vector<Game> searchNames(const Game& game) {
    std::vector<Game> names{game};
    for (const char* cut : {": ", " - "}) {
        const size_t at = game.name.find(cut);
        if (at != std::string::npos && at > 2) {
            Game shorter = game;
            shorter.name = game.name.substr(0, at);
            names.push_back(std::move(shorter));
            break;
        }
    }
    return names;
}

// A game's name as GameTDB's index has it (tools/make_gametdb_index.py does the
// same): letters and digits, accents folded, lower case, single spaces.
std::string nameKey(const std::string& name) {
    static const char kFold[] = // U+00C0..U+017F
        "aaaaaa ceeeeiiii nooooo  uuuuy  aaaaaa ceeeeiiii nooooo  uuuuy yaaaaaaccccccccdd  eeeeeeeeeegggggggghh  "
        "iiiiiiiii iijjkk llllllll  nnnnnnn  oooooo  rrrrrrsssssssstttt  uuuuuuuuuuuuwwyyyzzzzzzs";
    std::string out;
    for (size_t i = 0; i < name.size();) {
        const unsigned char c = static_cast<unsigned char>(name[i]);
        std::uint32_t cp = c;
        size_t len = 1;
        if (c >= 0xF0) { cp = c & 0x07; len = 4; }
        else if (c >= 0xE0) { cp = c & 0x0F; len = 3; }
        else if (c >= 0xC0) { cp = c & 0x1F; len = 2; }
        for (size_t k = 1; k < len && i + k < name.size(); ++k)
            cp = (cp << 6) | (static_cast<unsigned char>(name[i + k]) & 0x3F);
        i += len;
        char ch = ' ';
        if (cp < 128 && std::isalnum(static_cast<int>(cp))) ch = static_cast<char>(std::tolower(static_cast<int>(cp)));
        else if (cp >= 0xC0 && cp < 0x180) ch = kFold[cp - 0xC0];
        if (ch == ' ' && (out.empty() || out.back() == ' ')) continue;
        out += ch;
    }
    while (!out.empty() && out.back() == ' ') out.pop_back();
    return out;
}

// GameTDB's box covers: the index CI builds (romfs:/gametdb.tsv), then the
// cover in the region that has it first. Only the worker thread asks.
std::string gametdbCover(const Game& game) {
    static std::unordered_map<std::string, std::pair<std::string, char>> index;
    static bool loaded = false;
    if (!loaded) {
        loaded = true;
        std::ifstream in(std::string(kAssets) + "gametdb.tsv");
        std::string line;
        while (std::getline(in, line)) {
            const size_t a = line.find('\t'), b = a == std::string::npos ? a : line.find('\t', a + 1);
            if (b == std::string::npos || b + 1 >= line.size()) continue;
            index[line.substr(0, a)] = {line.substr(a + 1, b - a - 1), line[b + 1]};
        }
    }
    auto it = index.find(nameKey(game.name));
    if (it == index.end()) return {};
    const char region = it->second.second;
    const char* const order[3][3] = {{"US", "EN", "JA"}, {"EN", "US", "JA"}, {"JA", "US", "EN"}};
    const int pick = region == 'E' ? 1 : region == 'J' ? 2 : 0;
    static std::atomic<int> unreachable{0}; // no answer at all, several times: rest a minute
    static std::atomic<std::uint64_t> restUntil{0};
    const std::uint64_t nowMs = armTicksToNs(armGetSystemTick()) / 1000000ULL;
    if (nowMs < restUntil) return {};
    if (unreachable >= 3) {
        unreachable = 0;
        restUntil = nowMs + 60000;
        return {};
    }
    std::string body;
    for (const char* r : order[pick]) {
        const long status = get(std::string("https://art.gametdb.com/switch/coverM/") + r + "/" + it->second.first + ".jpg", body);
        if (status == 200 && isPicture(body)) {
            unreachable = 0;
            return body;
        }
        if (status == 0) {
            ++unreachable;
            break;
        }
    }
    return {};
}

// nlib.cc's copy of the game's eShop banner (no key needed).
std::string eshopBanner(const Game& game, int w, int h) {
    std::string body;
    const std::string url = "https://api.nlib.cc/nx/" + hexId(game.id) + "/banner/" + std::to_string(w) + "/" + std::to_string(h);
    if (get(url, body) != 200 || !isPicture(body)) return {};
    return body;
}

} // namespace

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

std::vector<Game> listGames() {
    // As HOME reads it (launcher/AppListLoader.cpp): 30 at a time, up to 1024.
    std::vector<Game> games;
    NsApplicationRecord records[30] = {};
    for (s32 offset = 0; offset < 1024;) {
        s32 got = 0;
        if (R_FAILED(nsListApplicationRecord(records, 30, offset, &got)) || got <= 0) break;
        for (s32 i = 0; i < got; ++i) {
            const u64 id = records[i].application_id;
            if (!id || std::any_of(games.begin(), games.end(), [id](const Game& g) { return g.id == id; })) continue;
            Game g;
            g.id = id;
            g.order = static_cast<int>(games.size());
            games.push_back(std::move(g));
        }
        offset += got;
        if (got < 30) break;
    }

    // Game cards, what can start now (HOME's AppListLoader asks the same).
    if (!games.empty()) {
        std::vector<std::uint64_t> ids;
        for (const Game& g : games) ids.push_back(g.id);
        std::vector<switchu::ns::ExtApplicationView> views(ids.size());
        if (R_SUCCEEDED(switchu::ns::queryApplicationViews(ids.data(), static_cast<int>(ids.size()), views.data())))
            for (size_t i = 0; i < games.size(); ++i) games[i].flags = views[i].flags;
    }

    std::unique_ptr<NsApplicationControlData> control;
    for (Game& g : games) {
        switchu::control_cache::Meta meta;
        if (!switchu::control_cache::readMeta(g.id, meta)) { // not seen by HOME yet: ask the system, keep it for HOME too
            if (!control) control.reset(new (std::nothrow) NsApplicationControlData);
            u64 size = 0;
            if (control && R_SUCCEEDED(nsGetApplicationControlData(NsApplicationControlSource_Storage, g.id, control.get(),
                                                                   sizeof(NsApplicationControlData), &size))) {
                switchu::control_cache::writeFromControlData(g.id, *control, size);
                switchu::control_cache::readMeta(g.id, meta);
            }
        }
        g.name = meta.name[0] ? meta.name : meta.english_name;
        g.version = meta.display_version;
        g.author = meta.publisher;
        g.homebrew = (g.id >> 56) == 0x05; // NRO forwarders' title IDs
        if (g.name.empty()) g.name = hexId(g.id);

        PdmPlayStatistics st{};
        if (R_SUCCEEDED(pdmqryQueryPlayStatisticsByApplicationId(g.id, false, &st))) {
            g.lastPlayed = st.last_timestamp_user;
            g.playSeconds = st.playtime / 1000000000ULL;
            g.launches = st.total_launches;
        }
    }
    return games;
}

std::vector<Game> listHomebrew() {
    std::vector<Game> apps;
    for (HomebrewApp& hb : scanHomebrew("sdmc:/switch/CardsNX/CardsNX.nro", false)) {
        Game g;
        g.path = std::move(hb.path);
        g.name = hb.name.empty() ? std::filesystem::path(g.path).stem().string() : std::move(hb.name);
        g.author = std::move(hb.author);
        g.version = std::move(hb.version);
        g.homebrew = true;
        g.id = 0xFF00000000000000ULL | (std::hash<std::string>{}(g.path) & 0x00FFFFFFFFFFFFFFULL);
        apps.push_back(std::move(g));
    }
    auto pin = [](const Game& g) {
        std::string n = g.name;
        for (char& c : n) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        if (n.find("sphaira") != std::string::npos) return 0;
        if (n.find("switchcord") != std::string::npos || n.find("discord") != std::string::npos) return 1;
        if (n.find("homebrew manager") != std::string::npos) return 2;
        if (n.find("homenx") != std::string::npos) return 3;
        return 4;
    };
    std::stable_sort(apps.begin(), apps.end(), [&](const Game& a, const Game& b) { return pin(a) < pin(b); });
    return apps;
}

std::vector<std::uint8_t> gameIcon(const Game& game) {
    return game.path.empty() ? switchu::control_cache::readIcon(game.id) : readHomebrewIcon(game.path);
}

bool startNetwork() {
#ifdef SWITCHU_MENU
    return themeshop::http::initialize(); // HOME's sockets and curl (Discord and updates use them too)
#else
    if (R_FAILED(socketInitializeDefault())) return false;
    return curl_global_init(CURL_GLOBAL_DEFAULT) == CURLE_OK;
#endif
}

void stopNetwork() {
#ifndef SWITCHU_MENU
    curl_global_cleanup();
    socketExit();
#endif
}

void threadDone() {
    if (t_curl) curl_easy_cleanup(t_curl);
    t_curl = nullptr;
}

std::string steamGridDbKey() {
    try {
        std::ifstream in(kSettings);
        const auto j = nlohmann::json::parse(in);
        return j.value("steamGridDbApiKey", std::string());
    } catch (...) {
        return {};
    }
}

std::string idText(std::uint64_t id) { return hexId(id); }

namespace {
constexpr const char* kLists = "sdmc:/config/CardsNX/lists.json";

nlohmann::json toJson(const std::vector<Game>& list) {
    nlohmann::json a = nlohmann::json::array();
    for (const Game& g : list)
        a.push_back({{"id", g.id}, {"name", g.name}, {"version", g.version}, {"author", g.author}, {"path", g.path},
                     {"homebrew", g.homebrew}, {"lastPlayed", g.lastPlayed}, {"playSeconds", g.playSeconds},
                     {"launches", g.launches}, {"flags", g.flags}, {"order", g.order}});
    return a;
}

void fromJson(const nlohmann::json& a, std::vector<Game>& out) {
    out.clear();
    if (!a.is_array()) return;
    for (const auto& j : a) {
        Game g;
        g.id = j.value("id", std::uint64_t{0});
        g.name = j.value("name", std::string());
        g.version = j.value("version", std::string());
        g.author = j.value("author", std::string());
        g.path = j.value("path", std::string());
        g.homebrew = j.value("homebrew", false);
        g.lastPlayed = j.value("lastPlayed", std::uint64_t{0});
        g.playSeconds = j.value("playSeconds", std::uint64_t{0});
        g.launches = j.value("launches", std::uint32_t{0});
        g.flags = j.value("flags", std::uint32_t{0});
        g.order = j.value("order", 0);
        if (g.id) out.push_back(std::move(g));
    }
}
} // namespace

bool loadLists(std::vector<Game>& games, std::vector<Game>& homebrew) {
    try {
        std::ifstream in(kLists);
        if (!in) return false;
        const auto j = nlohmann::json::parse(in);
        fromJson(j.value("games", nlohmann::json::array()), games);
        fromJson(j.value("homebrew", nlohmann::json::array()), homebrew);
        return !games.empty() || !homebrew.empty();
    } catch (...) {
        return false;
    }
}

void saveLists(const std::vector<Game>& games, const std::vector<Game>& homebrew) {
    writeLater(kLists, nlohmann::json{{"games", toJson(games)}, {"homebrew", toJson(homebrew)}}.dump());
}

namespace {
std::mutex g_writeMu;
std::condition_variable g_writeCv;
std::map<std::string, std::string> g_writes; // path -> newest data
bool g_writing = false, g_writerStop = false;
std::thread g_writer;

void writerLoop() {
    nxui::moveToWorkerCores();
    svcSetThreadPriority(CUR_THREAD_HANDLE, 0x3B);
    std::unique_lock<std::mutex> lk(g_writeMu);
    for (;;) {
        g_writeCv.wait(lk, [] { return g_writerStop || !g_writes.empty(); });
        if (g_writes.empty()) break; // stopping, and nothing left
        auto next = g_writes.begin();
        const std::string path = next->first, data = std::move(next->second);
        g_writes.erase(next);
        g_writing = true;
        lk.unlock();
        std::error_code ec;
        std::filesystem::create_directories(std::filesystem::path(path).parent_path(), ec);
        const std::string tmp = path + ".part";
        bool ok;
        {
            std::ofstream out(tmp, std::ios::binary | std::ios::trunc);
            out.write(data.data(), static_cast<std::streamsize>(data.size()));
            ok = static_cast<bool>(out);
        }
        if (ok) {
            std::filesystem::remove(path, ec);
            std::filesystem::rename(tmp, path, ec);
        }
        lk.lock();
        g_writing = false;
        g_writeCv.notify_all();
    }
}
} // namespace

void writeLater(const std::string& path, std::string data) {
    std::lock_guard<std::mutex> lk(g_writeMu);
    g_writes[path] = std::move(data);
    if (!g_writer.joinable()) {
        g_writerStop = false;
        g_writer = std::thread(writerLoop);
    }
    g_writeCv.notify_all();
}

void flushWrites() {
    {
        std::lock_guard<std::mutex> lk(g_writeMu);
        if (!g_writer.joinable()) return;
        g_writerStop = true; // it writes what's left, then ends
    }
    g_writeCv.notify_all();
    g_writer.join();
}

std::string systemIconPath(const std::string& action) {
    static const std::pair<const char*, const char*> kHomeNX[] = {
        {"news", "news"}, {"eshop", "eshop"}, {"controllers", "controller"},
        {"mii", "mii_editor"}, {"friends", "friends"}, {"settings", "settings"},
    };
    for (const auto& [a, file] : kHomeNX)
        if (action == a)
#ifdef SWITCHU_MENU
            return std::string("sdmc:/switch/SwitchU/icons/") + file + ".png";
#else
            return std::string("romfs:/icons/hx/") + file + ".png"; // copied from HomeNX's at build
#endif
    if (action == "get") return std::string(kAssets) + "icons/download.png"; // a Media app to download
    return std::string(kAssets) + "icons/sys_" + action + ".png"; // tools/make_cards_icons.py
}

namespace {
size_t toFile(char* p, size_t size, size_t n, void* user) {
    return std::fwrite(p, size, n, static_cast<FILE*>(user)) * size;
}
} // namespace

bool download(const std::string& url, const std::string& path) {
    std::error_code ec;
    std::filesystem::create_directories(std::filesystem::path(path).parent_path(), ec);
    const std::string part = path + ".part";
    FILE* f = std::fopen(part.c_str(), "wb");
    if (!f) return false;
    if (!t_curl) t_curl = curl_easy_init();
    long status = 0;
    if (CURL* c = t_curl) {
        curl_easy_reset(c);
        curl_easy_setopt(c, CURLOPT_URL, url.c_str());
        curl_easy_setopt(c, CURLOPT_USERAGENT, "CardsNX");
        curl_easy_setopt(c, CURLOPT_FOLLOWLOCATION, 1L);
        curl_easy_setopt(c, CURLOPT_NOSIGNAL, 1L);
        curl_easy_setopt(c, CURLOPT_CONNECTTIMEOUT, 10L);
        curl_easy_setopt(c, CURLOPT_TIMEOUT, 300L);
        curl_easy_setopt(c, CURLOPT_WRITEFUNCTION, toFile);
        curl_easy_setopt(c, CURLOPT_WRITEDATA, f);
        if (curl_easy_perform(c) == CURLE_OK) curl_easy_getinfo(c, CURLINFO_RESPONSE_CODE, &status);
    }
    std::fclose(f);
    if (status != 200 || std::filesystem::file_size(part, ec) < 1024) {
        std::filesystem::remove(part, ec);
        return false;
    }
    std::filesystem::remove(path, ec);
    std::filesystem::rename(part, path, ec);
    return !ec;
}

std::vector<std::uint8_t> cardArt(const Game& game, const std::string& key, bool boxArt, ArtKind& kind) {
    if (!game.action.empty()) { // the Nintendo page
        kind = ArtKind::Icon;
        return readFile(systemIconPath(game.action));
    }
    if (game.homebrew) { // apps have no box art: their icon makes the card
        kind = ArtKind::Icon;
        return gameIcon(game);
    }
    const std::string base = std::string(kCacheDir) + "/" + hexId(game.id);
    const std::string grid = base + ".grid", box = base + ".box", banner = base + ".card";
    kind = ArtKind::Grid;
    if (boxArt && std::filesystem::exists(grid)) return readFile(grid);
    if (boxArt && !key.empty()) {
        const std::string got = steamGridDbPicture(game, key, "grids",
            "dimensions=600x900&types=static&mimes=image/png,image/jpeg&nsfw=false&humor=false");
        if (!got.empty()) {
            writeFile(grid, got);
            return std::vector<std::uint8_t>(got.begin(), got.end());
        }
        if (keptBack(game.id, ".grid")) return readFile(grid); // taken down since: the one saved
    }
    kind = ArtKind::Box;
    if (boxArt && std::filesystem::exists(box)) return readFile(box);
    if (boxArt) {
        const std::string got = gametdbCover(game);
        if (!got.empty()) {
            writeFile(box, got);
            return std::vector<std::uint8_t>(got.begin(), got.end());
        }
    }
    kind = ArtKind::Banner;
    if (std::filesystem::exists(banner)) return readFile(banner);
    const std::string got = eshopBanner(game, 640, 360);
    if (!got.empty()) {
        writeFile(banner, got);
        return std::vector<std::uint8_t>(got.begin(), got.end());
    }
    kind = ArtKind::Icon; // nothing online (yet): its icon, on glass
    return gameIcon(game);
}

std::vector<std::uint8_t> backgroundArt(const Game& game, const std::string& key, ArtKind& kind) {
    kind = ArtKind::Icon;
    if (!game.action.empty()) return readFile(systemIconPath(game.action));
    if (game.homebrew) return gameIcon(game); // drawn huge and soft: its colours
    kind = ArtKind::Banner;
    const std::string path = std::string(kCacheDir) + "/" + hexId(game.id) + ".bg";
    if (std::filesystem::exists(path)) return readFile(path);
    std::string got;
    if (!key.empty())
        got = steamGridDbPicture(game, key, "heroes", "types=static&mimes=image/png,image/jpeg&nsfw=false&humor=false");
    if (got.empty() && keptBack(game.id, ".bg")) return readFile(path); // taken down since: the one saved
    if (got.empty()) got = eshopBanner(game, 1280, 720);
    if (!got.empty()) {
        writeFile(path, got);
        return std::vector<std::uint8_t>(got.begin(), got.end());
    }
    // No banner anywhere: its box cover, else its icon.
    kind = ArtKind::Box;
    const std::string box = std::string(kCacheDir) + "/" + hexId(game.id) + ".box";
    if (std::filesystem::exists(box)) return readFile(box);
    got = gametdbCover(game);
    if (!got.empty()) {
        writeFile(box, got);
        return std::vector<std::uint8_t>(got.begin(), got.end());
    }
    kind = ArtKind::Icon;
    return gameIcon(game);
}

// "Download them again" puts the old ones aside (kept/): if the new one can't
// be had -- taken down for DMCA since -- the old one comes back (keptBack).
void forgetArt(std::uint64_t id) {
    std::error_code ec;
    const std::string kept = std::string(kCacheDir) + "/kept";
    std::filesystem::create_directories(kept, ec);
    for (const char* ext : {".grid", ".box", ".card", ".bg", ".title2"}) {
        const std::string path = std::string(kCacheDir) + "/" + hexId(id) + ext;
        if (!std::filesystem::exists(path, ec) || std::filesystem::file_size(path, ec) == 0) continue;
        const std::string aside = kept + "/" + hexId(id) + ext;
        std::filesystem::remove(aside, ec);
        std::filesystem::rename(path, aside, ec);
    }
}

bool keptBack(std::uint64_t id, const char* ext) {
    std::error_code ec;
    const std::string aside = std::string(kCacheDir) + "/kept/" + hexId(id) + ext;
    if (!std::filesystem::exists(aside, ec)) return false;
    const std::string path = std::string(kCacheDir) + "/" + hexId(id) + ext;
    std::filesystem::remove(path, ec);
    std::filesystem::rename(aside, path, ec);
    return !ec;
}

std::vector<std::uint8_t> titleArt(const Game& game, const std::string& key) {
    const std::string base = std::string(kCacheDir) + "/" + hexId(game.id);
    std::error_code ec;
    // Chosen by hand (+ > Title picture): kept for good; empty = its own icon.
    if (std::filesystem::exists(base + ".titleme", ec)) return readFile(base + ".titleme");
    // The ones Adam picked on SteamGridDB (2026-10-26), by name: downloaded once.
    static const std::pair<const char*, const char*> kPicks[] = {
        {"51 worldwide games", "https://cdn2.steamgriddb.com/icon/2abbb67d13de26bdb4b9487c072ce63f.png"},
        {"clubhouse games", "https://cdn2.steamgriddb.com/icon/2abbb67d13de26bdb4b9487c072ce63f.png"},
        {"mario tennis aces", "https://cdn2.steamgriddb.com/icon/6a57122a72d2bd5e8a156c678952f88b.png"},
        {"new super mario bros u deluxe", "https://cdn2.steamgriddb.com/icon/f3848b78b70702300c576fcd56a9addd.png"},
        {"minecraft", "https://cdn2.steamgriddb.com/icon/cfecdb276f634854f3ef915e2e980c31.png"},
        // (its logo, SteamGridDB's 512 px copy: the original is 3187 px, 31 MB to decode)
        {"1 2 switch", "https://cdn2.steamgriddb.com/logo_thumb/67f5f6bb5b8cbbc3593fda172965b132.png"},
    };
    const std::string name = nameKey(game.name);
    for (const auto& [words, url] : kPicks)
        if (name.find(words) != std::string::npos) {
            // Saved under its link's number: a changed pick is downloaded again.
            const std::string saved = base + ".pick" + std::to_string(std::hash<std::string>{}(url) % 1000000);
            if (std::filesystem::exists(saved, ec)) return readFile(saved);
            std::string body;
            if (get(url, body) == 200 && isPicture(body)) {
                writeFile(saved, body);
                return std::vector<std::uint8_t>(body.begin(), body.end());
            }
            break; // (no internet now: the others below meanwhile)
        }
    // The one the stricter search before picked: he liked those (the leaf for
    // Animal Crossing, Tears of the Kingdom's) more than what's found now.
    if (std::vector<std::uint8_t> old = readFile(base + ".title"); !old.empty()) return old;
    // (".title2": the ones "none" was saved for before are asked again with these.)
    const std::string path = base + ".title2";
    if (std::filesystem::exists(path, ec)) return readFile(path); // empty: SteamGridDB had none
    if (key.empty()) return {};
    const std::vector<Game> names = searchNames(game);
    // An icon with see-through edges: up to 512 px first (big ones are MBs),
    // then the big ones; then its logo (they all have no background).
    const std::string query = "types=static&mimes=image/png&nsfw=false&humor=false";
    bool answered = false;
    std::string got;
    for (const Game& g : names) {
        got = steamGridDbPicture(g, key, "icons", query + "&dimensions=512,256,192,180,160,152,150,144,128", true, &answered);
        if (got.empty()) got = steamGridDbPicture(g, key, "icons", query + "&dimensions=1024,768", true, &answered, 3);
        if (got.empty()) got = steamGridDbPicture(g, key, "logos", query, true, &answered, 4);
        if (!got.empty()) break;
    }
    if (got.empty() && keptBack(game.id, ".title2")) return readFile(path); // taken down since: the one saved
    if (answered) writeFile(path, got); // none: not asked again until its pictures are downloaded again
    return std::vector<std::uint8_t>(got.begin(), got.end());
}

std::vector<TitleChoice> titleChoices(const Game& game, const std::string& key, size_t max) {
    std::vector<TitleChoice> out;
    const std::string base = std::string(kCacheDir) + "/" + hexId(game.id);
    if (std::vector<std::uint8_t> old = readFile(base + ".title"); !old.empty()) { // the one picked before
        int w = 0, h = 0, ch = 0;
        if (stbi_uc* px = stbi_load_from_memory(old.data(), static_cast<int>(old.size()), &w, &h, &ch, 4)) {
            TitleChoice c;
            c.file = base + ".title";
            c.rgba.assign(px, px + static_cast<size_t>(w) * h * 4);
            stbi_image_free(px);
            shrink(c.rgba, w, h, 160);
            c.w = w;
            c.h = h;
            c.clear = true;
            c.before = true;
            out.push_back(std::move(c));
        }
    }
    if (key.empty()) return out;
    std::set<std::string> seen;
    std::string body;
    for (const Game& g : searchNames(game))
        for (const char* kind : {"icons", "logos"})
            for (const auto& [url, thumb] : steamGridDbAssets(steamGridDbGame(g.name, key), key, kind,
                                                              "types=static&mimes=image/png&nsfw=false&humor=false", 14, nullptr)) {
                if (out.size() >= max || url.empty() || !seen.insert(url).second) continue;
                // The thumbnail (small) when it's a picture stb reads, else the picture.
                if ((thumb.empty() || get(thumb, body) != 200 || !isPicture(body)) && (get(url, body) != 200 || !isPicture(body)))
                    continue;
                int w = 0, h = 0, ch = 0;
                stbi_uc* px = stbi_load_from_memory(reinterpret_cast<const stbi_uc*>(body.data()), static_cast<int>(body.size()),
                                                    &w, &h, &ch, 4);
                if (!px) continue;
                TitleChoice c;
                c.url = url;
                c.clear = clearEdges(px, w, h);
                c.rgba.assign(px, px + static_cast<size_t>(w) * h * 4);
                stbi_image_free(px);
                shrink(c.rgba, w, h, 160);
                c.w = w;
                c.h = h;
                out.push_back(std::move(c));
            }
    // No background first (what a title picture is), the rest after.
    std::stable_partition(out.begin(), out.end(), [](const TitleChoice& c) { return c.before || c.clear; });
    return out;
}

bool chooseTitleArt(const Game& game, const TitleChoice* choice) {
    const std::string base = std::string(kCacheDir) + "/" + hexId(game.id);
    std::string data;
    if (choice && !choice->file.empty()) {
        const std::vector<std::uint8_t> old = readFile(choice->file);
        data.assign(old.begin(), old.end());
    } else if (choice && (get(choice->url, data) != 200 || !isPicture(data))) {
        return false;
    }
    writeFile(base + ".titleme", data); // empty: its own icon
    return true;
}

void forgetPicture(std::uint64_t id, bool background) {
    std::error_code ec;
    std::filesystem::remove(std::string(kCacheDir) + "/" + hexId(id) + (background ? ".bg" : ".grid"), ec);
}

// Over 80% of it within a few steps of one colour: art has more going on.
bool removedNotice(const std::uint8_t* rgba, int w, int h) {
    if (!rgba || w < 16 || h < 16) return false;
    constexpr int kSteps = 48;
    static thread_local std::vector<std::uint16_t> bins(1u << 15);
    std::fill(bins.begin(), bins.end(), 0);
    auto at = [&](int i, int j) { return rgba + (static_cast<size_t>(j * h / kSteps) * w + i * w / kSteps) * 4; };
    int best = 0;
    for (int j = 0; j < kSteps; ++j)
        for (int i = 0; i < kSteps; ++i) {
            const std::uint8_t* p = at(i, j);
            const int bin = (p[0] >> 3) << 10 | (p[1] >> 3) << 5 | p[2] >> 3;
            if (++bins[(size_t)bin] > bins[(size_t)best]) best = bin;
        }
    const int r = (best >> 10 & 31) * 8 + 4, g = (best >> 5 & 31) * 8 + 4, b = (best & 31) * 8 + 4;
    int near = 0;
    for (int j = 0; j < kSteps; ++j)
        for (int i = 0; i < kSteps; ++i) {
            const std::uint8_t* p = at(i, j);
            near += std::abs(p[0] - r) <= 10 && std::abs(p[1] - g) <= 10 && std::abs(p[2] - b) <= 10;
        }
    return near * 10 > kSteps * kSteps * 8;
}

// As nextendo.network's own pages count them (assets/js/*-counts.js): the
// service's list, Nintendo Classics from its catalogue, and the games with
// their own servers.
bool nextendoOnline(std::map<std::uint64_t, int>& out) {
    std::string body;
    auto readCounts = [&out](const nlohmann::json& counts, bool onlyNew) {
        if (!counts.is_object()) return;
        for (auto it = counts.begin(); it != counts.end(); ++it) {
            if (!it.value().is_number_integer()) continue;
            const std::uint64_t id = std::strtoull(it.key().c_str(), nullptr, 16);
            if (id && (!onlyNew || !out.count(id))) out[id] = it.value().get<int>();
        }
    };
    out.clear();
    bool any = false;
    if (get(std::string(kNextendo) + "/api/online-counts", body) == 200) {
        try {
            readCounts(nlohmann::json::parse(body).value("counts", nlohmann::json::object()), false);
            any = true;
        } catch (...) {
        }
    }
    if (get(std::string(kNextendo) + "/service-catalog/v1/online-counts", body) == 200) {
        try {
            readCounts(nlohmann::json::parse(body).value("counts", nlohmann::json::object()), true);
            any = true;
        } catch (...) {
        }
    }
    static const struct { const char* path; std::uint64_t id; bool needsOnline; } kOwn[] = {
        {"/game-stats/sm3dw", 0x010028600EBDA000ULL, false},
        {"/game-stats/jamboree", 0x0100965017338000ULL, true},
        {"/game-stats/stardew", 0x0100E65002BB8000ULL, true},
        {"/game-stats/kirby", 0x0100A8E016236000ULL, true},
        {"/game-stats/justdance22", 0x0100EA6014BB8000ULL, true},
        {"/game-stats/uno-count", 0x0100B6E012EBE000ULL, false},
    };
    for (const auto& own : kOwn) {
        if (get(std::string(kNextendo) + own.path, body) != 200) continue;
        try {
            const auto j = nlohmann::json::parse(body);
            if (own.needsOnline && !j.value("online", false)) continue;
            if (j.contains("connected") && j["connected"].is_number_integer()) {
                out[own.id] = j["connected"].get<int>();
                any = true;
            }
        } catch (...) {
        }
    }
    return any;
}

namespace {

bool isApplication() {
    const AppletType t = appletGetAppletType();
    return t == AppletType_Application || t == AppletType_SystemApplication;
}

// A request to HOME on the general channel ("SAMS": the system's own HOME
// takes 2 home, 3 sleep, 5 shut down, 6 restart; HomeNX's also 100 launch).
bool askHome(u32 msg, const void* extra = nullptr, size_t extraSize = 0) {
    struct Header {
        u32 magic, version, msg, reserved;
    } hdr{0x534D4153, 1, msg, 0};
    AppletStorage st{};
    if (R_FAILED(appletCreateStorage(&st, static_cast<s64>(sizeof(hdr) + extraSize)))) return false;
    bool ok = R_SUCCEEDED(appletStorageWrite(&st, 0, &hdr, sizeof(hdr)));
    if (ok && extra) ok = R_SUCCEEDED(appletStorageWrite(&st, sizeof(hdr), extra, extraSize));
    if (!ok) {
        appletStorageClose(&st);
        return false;
    }
    return R_SUCCEEDED(appletPushToGeneralChannel(&st));
}

AccountUid toUid(AccountUidLike u) {
    AccountUid uid{};
    uid.uid[0] = u.uid[0];
    uid.uid[1] = u.uid[1];
    if (!accountUidIsValid(&uid)) accountGetLastOpenedUser(&uid);
    return uid;
}

// The web applet as Sphaira and HomeNX's .nro open it; `shop`: the eShop itself.
Result showWeb(const char* url, AccountUid uid, bool shop) {
    WebCommonConfig cfg{};
    Result rc = webPageCreate(&cfg, url);
    if (R_FAILED(rc)) return rc;
    if (accountUidIsValid(&uid)) webConfigSetUid(&cfg, uid);
    if (shop) {
        reinterpret_cast<WebArgHeader*>(cfg.arg.data)->shimKind = WebShimKind_Shop;
        cfg.appletid = AppletId_LibraryAppletShop;
        return webConfigShow(&cfg, nullptr);
    }
    webConfigSetWhitelist(&cfg, ".*");
    webConfigSetBootDisplayKind(&cfg, WebBootDisplayKind_Black);
    webConfigSetPointer(&cfg, true);
    webConfigSetLeftStickMode(&cfg, WebLeftStickMode_Pointer);
    webConfigSetTouchEnabledOnContents(&cfg, true);
    WebCommonReply reply{};
    return webConfigShow(&cfg, &reply);
}

} // namespace

bool launch(std::uint64_t id, AccountUidLike user) {
    if (isApplication()) return R_SUCCEEDED(appletRequestLaunchApplication(id, nullptr));
    const AccountUid uid = toUid(user);
    struct {
        u64 titleId;
        u64 uid[2];
    } args{id, {uid.uid[0], uid.uid[1]}};
    return askHome(100, &args, sizeof(args));
}

std::vector<Game> listSystem() {
    struct Entry { const char* action; const char* name; const char* about; };
    static const Entry kEntries[] = {
#ifdef SWITCHU_MENU
        {"news", "News", "What's new"}, // as HOME's frontend: News instead of the online page
#else
        {"nso", "Switch Online", "Online and classics"},
#endif
        {"eshop", "eShop", "Games and add-ons"},
        {"album", "Album", "Screenshots, videos"},
        {"controllers", "Controllers", "Pair controllers"},
        {"mii", "Mii Maker", "Make your Miis"},
        {"friends", "Friends", "Your friend list"},
        {"browser", "Browser", "BrowseNX"},
        {"settings", "Settings", "Look, sound, updates"},
    };
    std::vector<Game> out;
    for (const Entry& e : kEntries) {
        Game g;
        g.action = e.action;
        g.name = e.name;
        g.author = e.about;
        g.homebrew = true;
        g.id = 0xFE00000000000000ULL | (std::hash<std::string>{}(g.action) & 0x0000FFFFFFFFFFFFULL);
        g.order = static_cast<int>(out.size());
        out.push_back(std::move(g));
    }
    return out;
}

bool openSystem(const std::string& action, AccountUidLike user, std::string& error) {
    const AccountUid uid = toUid(user);
    Result rc = 0;
    if (action == "eshop") {
        rc = showWeb("https://bugyo.hac.lp1.eshop.nintendo.net/ashigaru/", uid, true);
        if (R_FAILED(rc)) rc = showWeb("https://www.nintendo.com/store/", uid, false);
    } else if (action == "nso") {
        rc = showWeb("https://www.nintendo.com/switch/online/", uid, false);
    } else if (action == "album") {
        rc = albumLaShowAllAlbumFiles();
    } else if (action == "mii") {
        rc = miiLaShowMiiEdit(MiiSpecialKeyCode_Normal);
    } else if (action == "friends") {
        rc = accountUidIsValid(&uid) ? friendsLaShowFriendList(uid) : MAKERESULT(Module_Libnx, LibnxError_BadInput);
    } else if (action == "controllers") {
        HidLaControllerSupportArg arg;
        hidLaCreateControllerSupportArg(&arg);
        rc = hidLaShowControllerSupportForSystem(nullptr, &arg, false);
    } else if (action == "remap") {
        HidLaControllerKeyRemappingArg arg;
        hidLaCreateControllerKeyRemappingArg(&arg);
        rc = hidLaShowControllerKeyRemappingForSystem(&arg, HidLaControllerSupportCaller_System);
    } else if (action == "browser") {
        rc = showWeb("https://duckduckgo.com/?kae=d", uid, false);
    }
    if (R_FAILED(rc)) {
        char buf[120];
        std::snprintf(buf, sizeof(buf), "Couldn't open that here (0x%X). Start CardsNX over a game.", rc);
        error = buf;
        return false;
    }
    return true;
}

namespace {
// nifm:s's general service: wireless communication on/off is Airplane mode
// (commands 16 set / 17 get), as HomeNX's Quick Settings does it.
struct NifmSystem {
    Service root{}, general{};
    bool open() {
        if (R_FAILED(smGetService(&root, "nifm:s")) || R_FAILED(serviceConvertToDomain(&root))) return false;
        const u64 reserved = 0;
        serviceAssumeDomain(&root);
        return R_SUCCEEDED(serviceDispatchIn(&root, 5, reserved, .in_send_pid = true, .out_num_objects = 1,
                                             .out_objects = &general));
    }
    ~NifmSystem() {
        serviceClose(&general);
        serviceClose(&root);
    }
};
} // namespace

QuickStatus readQuickStatus() {
    QuickStatus q;
    if (R_SUCCEEDED(setsysInitialize())) {
        bool on = true;
        if (R_SUCCEEDED(setsysGetWirelessLanEnableFlag(&on))) q.wifi = on;
        setsysExit();
    }
    {
        NifmSystem nifm;
        u8 wireless = 1;
        if (nifm.open()) {
            serviceAssumeDomain(&nifm.general);
            if (R_SUCCEEDED(serviceDispatchOut(&nifm.general, 17, wireless))) q.airplane = (wireless & 1) == 0;
        }
    }
    if (R_SUCCEEDED(lblInitialize())) {
        float level = 0.f;
        if (R_SUCCEEDED(lblGetCurrentBrightnessSetting(&level))) q.brightness = std::clamp(level, 0.f, 1.f);
        lblExit();
    }
    u32 charge = 0;
    if (R_SUCCEEDED(psmGetBatteryChargePercentage(&charge))) q.battery = static_cast<int>(charge);
    PsmChargerType charger = PsmChargerType_Unconnected;
    if (R_SUCCEEDED(psmGetChargerType(&charger))) q.charging = charger != PsmChargerType_Unconnected;
    if (R_SUCCEEDED(tsInitialize())) {
        s32 t = 0;
        if (R_SUCCEEDED(tsGetTemperature(TsLocation_External, &t))) q.temperature = t > 1000 ? t / 1000 : t;
        tsExit();
    }
    return q;
}

ConsoleSettings readConsoleSettings() {
    ConsoleSettings s;
    if (R_SUCCEEDED(nifmInitialize(NifmServiceType_User))) {
        u32 ip = 0;
        if (R_SUCCEEDED(nifmGetCurrentIpAddress(&ip)) && ip) {
            char buf[20];
            std::snprintf(buf, sizeof(buf), "%u.%u.%u.%u", ip & 0xFF, (ip >> 8) & 0xFF, (ip >> 16) & 0xFF, ip >> 24);
            s.ip = buf;
        }
        NifmNetworkProfileData profile{};
        if (R_SUCCEEDED(nifmGetCurrentNetworkProfile(&profile))) {
            char ssid[33] = {};
            std::memcpy(ssid, profile.wireless_setting_data.ssid, 32);
            s.network = ssid;
        }
        nifmExit();
    }
    if (R_SUCCEEDED(setsysInitialize())) {
        setsysGetVibrationMasterVolume(&s.vibration);
        setsysGetUsbFullKeyEnableFlag(&s.proWired);
        SetSysSleepSettings sleep{};
        if (R_SUCCEEDED(setsysGetSleepSettings(&sleep))) {
            s.handheldSleep = std::clamp(static_cast<int>(sleep.handheld_sleep_plan), 0, 5);
            s.tvSleep = std::clamp(static_cast<int>(sleep.console_sleep_plan), 0, 5);
        }
        SetSysFirmwareVersion fw{};
        if (R_SUCCEEDED(setsysGetFirmwareVersion(&fw))) s.firmware = fw.display_version;
        setsysExit();
    }
    if (R_SUCCEEDED(lblInitialize())) {
        bool on = false;
        if (R_SUCCEEDED(lblIsAutoBrightnessControlEnabled(&on))) s.autoBrightness = on;
        lblExit();
    }
    if (R_SUCCEEDED(splInitialize())) {
        u64 v = 0;
        if (R_SUCCEEDED(splGetConfig(static_cast<SplConfigItem>(65000), &v))) { // Atmosphere's version
            char buf[24];
            std::snprintf(buf, sizeof(buf), "%u.%u.%u", unsigned(v >> 56 & 0xFF), unsigned(v >> 48 & 0xFF),
                          unsigned(v >> 40 & 0xFF));
            s.atmosphere = buf;
        }
        splExit();
    }
    return s;
}

void writeConsoleSettings(const ConsoleSettings& s) {
    if (R_SUCCEEDED(setsysInitialize())) {
        setsysSetVibrationMasterVolume(std::clamp(s.vibration, 0.f, 1.f));
        setsysSetUsbFullKeyEnableFlag(s.proWired);
        SetSysSleepSettings sleep{};
        if (R_SUCCEEDED(setsysGetSleepSettings(&sleep))) {
            sleep.handheld_sleep_plan = std::clamp(s.handheldSleep, 0, 5);
            sleep.console_sleep_plan = std::clamp(s.tvSleep, 0, 5);
            setsysSetSleepSettings(&sleep);
        }
        setsysExit();
    }
    if (R_SUCCEEDED(lblInitialize())) {
        if (s.autoBrightness) lblEnableAutoBrightnessControl();
        else lblDisableAutoBrightnessControl();
        lblExit();
    }
}

namespace {
size_t testDiscard(char*, size_t size, size_t n, void* user) {
    return static_cast<NetTest*>(user)->stop ? 0 : size * n;
}

int testStopped(void* user, curl_off_t, curl_off_t, curl_off_t, curl_off_t) {
    return static_cast<NetTest*>(user)->stop ? 1 : 0;
}

struct Zeros {
    NetTest* test;
    curl_off_t left;
};

size_t testZeros(char* p, size_t size, size_t n, void* user) {
    auto* z = static_cast<Zeros*>(user);
    if (z->test->stop) return CURL_READFUNC_ABORT;
    const size_t give = static_cast<size_t>(std::min<curl_off_t>(z->left, static_cast<curl_off_t>(size * n)));
    std::memset(p, 0, give);
    z->left -= static_cast<curl_off_t>(give);
    return give;
}

CURL* testHandle(NetTest& t, const char* url, long seconds) {
    CURL* c = curl_easy_init();
    if (!c) return nullptr;
    curl_easy_setopt(c, CURLOPT_URL, url);
    curl_easy_setopt(c, CURLOPT_USERAGENT, "CardsNX");
    curl_easy_setopt(c, CURLOPT_NOSIGNAL, 1L);
    curl_easy_setopt(c, CURLOPT_CONNECTTIMEOUT, 8L);
    curl_easy_setopt(c, CURLOPT_TIMEOUT, seconds);
    curl_easy_setopt(c, CURLOPT_WRITEFUNCTION, testDiscard);
    curl_easy_setopt(c, CURLOPT_WRITEDATA, &t);
    curl_easy_setopt(c, CURLOPT_NOPROGRESS, 0L);
    curl_easy_setopt(c, CURLOPT_XFERINFOFUNCTION, testStopped);
    curl_easy_setopt(c, CURLOPT_XFERINFODATA, &t);
    return c;
}

// Kilobits a second over the time the bytes moved (a run cut short by its time limit counts).
int testSpeed(CURL* c, CURLcode rc, bool up) {
    curl_off_t bytes = 0, total = 0, start = 0;
    curl_easy_getinfo(c, up ? CURLINFO_SIZE_UPLOAD_T : CURLINFO_SIZE_DOWNLOAD_T, &bytes);
    curl_easy_getinfo(c, CURLINFO_TOTAL_TIME_T, &total);
    curl_easy_getinfo(c, CURLINFO_PRETRANSFER_TIME_T, &start);
    if ((rc != CURLE_OK && rc != CURLE_OPERATION_TIMEDOUT) || bytes < 65536 || total <= start) return -1;
    return static_cast<int>(bytes * 8 * 1000 / (total - start)); // bits per ms = kbit/s
}
} // namespace

void testConnection(NetTest& t) {
    if (R_SUCCEEDED(nifmInitialize(NifmServiceType_User))) {
        NifmInternetConnectionType type{};
        u32 strength = 0;
        NifmInternetConnectionStatus status{};
        if (R_SUCCEEDED(nifmGetInternetConnectionStatus(&type, &strength, &status)))
            t.signal = type == NifmInternetConnectionType_WiFi ? static_cast<int>(strength) : 3;
        nifmExit();
    }
    // The Internet: Cloudflare's "are you online" page, the quickest of three hellos.
    t.stage = 1;
    for (int i = 0; i < 3 && !t.stop; ++i) {
        CURL* c = testHandle(t, "http://cp.cloudflare.com/generate_204", 8L);
        if (!c) break;
        curl_easy_setopt(c, CURLOPT_FRESH_CONNECT, 1L);
        long code = 0;
        curl_off_t lookup = 0, connect = 0;
        if (curl_easy_perform(c) == CURLE_OK && curl_easy_getinfo(c, CURLINFO_RESPONSE_CODE, &code) == CURLE_OK
            && (code == 204 || code == 200)) {
            curl_easy_getinfo(c, CURLINFO_NAMELOOKUP_TIME_T, &lookup);
            curl_easy_getinfo(c, CURLINFO_CONNECT_TIME_T, &connect);
            const int ms = static_cast<int>(std::max<curl_off_t>(1000, connect - lookup) / 1000);
            t.internet = true;
            if (t.pingMs < 0 || ms < t.pingMs) t.pingMs = ms;
        }
        curl_easy_cleanup(c);
    }
    if (!t.internet || t.stop) {
        t.stage = 4;
        return;
    }
    // Download and upload: as much as moves in 8 s (Cloudflare's speed test, else Tele2's).
    t.stage = 2;
    for (const char* url : {"https://speed.cloudflare.com/__down?bytes=50000000", "http://speedtest.tele2.net/100MB.zip"}) {
        CURL* c = testHandle(t, url, 8L);
        if (!c) break;
        const CURLcode rc = curl_easy_perform(c);
        t.downKbps = testSpeed(c, rc, false);
        curl_easy_cleanup(c);
        if (t.downKbps > 0 || t.stop) break;
    }
    t.stage = 3;
    for (const char* url : {"https://speed.cloudflare.com/__up", "http://speedtest.tele2.net/upload.php"}) {
        CURL* c = testHandle(t, url, 8L);
        if (!c || t.stop) {
            if (c) curl_easy_cleanup(c);
            break;
        }
        Zeros z{&t, 25'000'000};
        curl_easy_setopt(c, CURLOPT_POST, 1L);
        curl_easy_setopt(c, CURLOPT_POSTFIELDSIZE_LARGE, z.left);
        curl_easy_setopt(c, CURLOPT_READFUNCTION, testZeros);
        curl_easy_setopt(c, CURLOPT_READDATA, &z);
        const CURLcode rc = curl_easy_perform(c);
        t.upKbps = testSpeed(c, rc, true);
        curl_easy_cleanup(c);
        if (t.upKbps > 0) break;
    }
    t.stage = 4;
}

bool setAirplaneMode(bool on) {
    NifmSystem nifm;
    if (!nifm.open()) return false;
    const u8 wireless = on ? 0 : 1;
    serviceAssumeDomain(&nifm.general);
    return R_SUCCEEDED(serviceDispatchIn(&nifm.general, 16, wireless));
}

bool setWifi(bool on) {
    if (on) setAirplaneMode(false);
    bool ok = false;
    if (R_SUCCEEDED(setsysInitialize())) {
        ok = R_SUCCEEDED(setsysSetWirelessLanEnableFlag(on));
        setsysExit();
    }
    return ok;
}

void setBrightness(float level) {
    if (R_SUCCEEDED(lblInitialize())) {
        lblSetCurrentBrightnessSetting(std::clamp(level, 0.05f, 1.f));
        lblExit();
    }
}

std::vector<User> listUsers() {
    std::vector<User> out;
    AccountUid uids[8] = {};
    s32 count = 0;
    if (R_FAILED(accountListAllUsers(uids, 8, &count))) return out;
    for (s32 i = 0; i < count; ++i) {
        User u;
        u.uid.uid[0] = uids[i].uid[0];
        u.uid.uid[1] = uids[i].uid[1];
        AccountProfile profile{};
        if (R_SUCCEEDED(accountGetProfile(&profile, uids[i]))) {
            AccountProfileBase base{};
            AccountUserData data{};
            if (R_SUCCEEDED(accountProfileGet(&profile, &data, &base))) u.name = base.nickname;
            u32 size = 0;
            if (R_SUCCEEDED(accountProfileGetImageSize(&profile, &size)) && size > 0 && size < (1u << 20)) {
                u.picture.resize(size);
                u32 got = 0;
                if (R_FAILED(accountProfileLoadImage(&profile, u.picture.data(), size, &got))) u.picture.clear();
                else u.picture.resize(got);
            }
            accountProfileClose(&profile);
        }
        if (u.name.empty()) u.name = "User " + std::to_string(i + 1);
        out.push_back(std::move(u));
    }
    return out;
}

bool deleteUser(AccountUidLike user, std::string& error) {
    AccountUid uid{};
    uid.uid[0] = user.uid[0];
    uid.uid[1] = user.uid[1];
    // acc:su (the administrator's account service, which System Settings uses):
    // 203 DeleteUser.
    Service su{};
    Result rc = smGetService(&su, "acc:su");
    if (R_SUCCEEDED(rc)) {
        rc = serviceDispatchIn(&su, 203, uid);
        serviceClose(&su);
    }
    if (R_SUCCEEDED(rc)) return true;
    char buf[96];
    std::snprintf(buf, sizeof(buf), "The console didn't remove that user (0x%X).", rc);
    error = buf;
    return false;
}

bool canRebootToPayload() {
    SetSysProductModel model = SetSysProductModel_Invalid;
    if (R_SUCCEEDED(setsysInitialize())) {
        setsysGetProductModel(&model);
        setsysExit();
    }
    return model == SetSysProductModel_Nx || model == SetSysProductModel_Copper || model == SetSysProductModel_Invalid;
}

bool power(Power what, std::string& error) {
    switch (what) {
        case Power::Sleep:
            if (askHome(3)) return true;
            error = "Use the POWER button to sleep.";
            return false;
        case Power::Shutdown:
        case Power::Restart: {
            const bool restart = what == Power::Restart;
            if (askHome(restart ? 6 : 5)) return true; // HOME saves first
            Result rc = spsmInitialize();
            if (R_SUCCEEDED(rc)) {
                rc = spsmShutdown(restart);
                spsmExit();
            }
            if (R_SUCCEEDED(rc)) return true;
            error = "The console didn't take that (0x" + std::to_string(rc) + ").";
            return false;
        }
        case Power::Hekate: {
            if (!canRebootToPayload()) { // newer models: through the power chip's clock (as Ultrahand)
                if (!switchu::hekate_rtc::requestMenuOnNextBoot()) {
                    error = "The power chip didn't take the request for Hekate's menu.";
                    return false;
                }
                if (askHome(6)) return true; // HOME restarts (saving first)
                Result rc = spsmInitialize();
                if (R_SUCCEEDED(rc)) {
                    rc = spsmShutdown(true);
                    spsmExit();
                }
                error = "The restart didn't start.";
                return R_SUCCEEDED(rc);
            }
            static constexpr size_t kPayloadMax = 0x24000; // what fits in IRAM
            alignas(0x1000) static u8 payload[kPayloadMax];
            const char* path = nullptr;
            for (const char* p : {"sdmc:/bootloader/update.bin", "sdmc:/atmosphere/reboot_payload.bin"})
                if (std::filesystem::exists(p)) {
                    path = p;
                    break;
                }
            if (!path) {
                error = "Hekate isn't on the SD card.";
                return false;
            }
            std::memset(payload, 0, sizeof(payload));
            size_t got = 0;
            if (FILE* f = std::fopen(path, "rb")) {
                got = std::fread(payload, 1, sizeof(payload), f);
                std::fclose(f);
            }
            if (got == 0) {
                error = "Couldn't read the Hekate payload.";
                return false;
            }
            // Hekate's boot storage: 0x94 boot_cfg bit 0 (force autoboot) with
            // 0x95 autoboot 0 opens its menu instead of booting straight back.
            if (got > 0x98 && !payload[0x94] && !payload[0x95] && !payload[0x96] && !payload[0x97]) {
                payload[0x94] = 0x01;
                payload[0x95] = 0x00;
            }
            Handle port = 0;
            Result rc = svcConnectToNamedPort(&port, "bpc:ams"); // Atmosphere's: 65001 takes the payload
            if (R_SUCCEEDED(rc)) {
                Service bpcAms;
                serviceCreate(&bpcAms, port);
                rc = serviceDispatch(&bpcAms, 65001,
                    .buffer_attrs = { SfBufferAttr_In | SfBufferAttr_HipcMapAlias },
                    .buffers = { { payload, sizeof(payload) } },
                );
                serviceClose(&bpcAms);
            }
            if (R_FAILED(rc)) {
                error = "Atmosphere didn't take the Hekate payload.";
                return false;
            }
            // From hbl (this .nro), Atmosphere sends the reboot to that payload.
            rc = bpcInitialize();
            if (R_SUCCEEDED(rc)) {
                rc = bpcRebootSystem();
                bpcExit();
            }
            error = "The reboot didn't start.";
            return R_SUCCEEDED(rc);
        }
    }
    return false;
}

} // namespace cards
