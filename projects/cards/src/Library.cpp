#include "Library.hpp"

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

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <atomic>
#include <memory>
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
    const std::string tmp = path + ".part";
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

// The first picture SteamGridDB has of `kind` ("grids" or "heroes") for the game.
std::string steamGridDbPicture(const Game& game, const std::string& key, const char* kind, const char* query) {
    const std::vector<std::string> headers = {"Authorization: Bearer " + key, "Accept: application/json"};
    std::string body;
    if (get(std::string(kSteamGridDb) + "/search/autocomplete/" + percentEncode(game.name), body, headers) != 200)
        return {};
    long long gameId = 0;
    try {
        const auto j = nlohmann::json::parse(body);
        if (j.contains("data") && j["data"].is_array() && !j["data"].empty())
            gameId = j["data"][0].value("id", 0LL);
    } catch (...) {
    }
    if (gameId <= 0) return {};
    if (get(std::string(kSteamGridDb) + "/" + kind + "/game/" + std::to_string(gameId) + "?" + query, body, headers) != 200)
        return {};
    std::string url;
    try {
        const auto j = nlohmann::json::parse(body);
        if (j.contains("data") && j["data"].is_array() && !j["data"].empty())
            url = j["data"][0].value("url", std::string());
    } catch (...) {
    }
    if (url.empty() || get(url, body) != 200 || !isPicture(body)) return {};
    return body;
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
    std::error_code ec;
    std::filesystem::create_directories("sdmc:/config/CardsNX", ec);
    const std::string tmp = std::string(kLists) + ".part";
    {
        std::ofstream out(tmp, std::ios::trunc);
        out << nlohmann::json{{"games", toJson(games)}, {"homebrew", toJson(homebrew)}}.dump();
        if (!out) return;
    }
    std::filesystem::remove(kLists, ec);
    std::filesystem::rename(tmp, kLists, ec);
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

void forgetArt(std::uint64_t id) {
    std::error_code ec;
    for (const char* ext : {".grid", ".box", ".card", ".bg"})
        std::filesystem::remove(std::string(kCacheDir) + "/" + hexId(id) + ext, ec);
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
