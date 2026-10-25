#include "Library.hpp"

#include <switch.h>
#include <switchu/control_cache.hpp>
#include "homebrew/HomebrewStore.hpp"
#include <curl/curl.h>
#include <nlohmann/json.hpp>

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
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

// GET, the body in `out`; the HTTP status (0: no answer).
long get(const std::string& url, std::string& out, const std::vector<std::string>& headers = {}) {
    out.clear();
    CURL* c = curl_easy_init();
    if (!c) return 0;
    curl_slist* list = nullptr;
    for (const std::string& h : headers) list = curl_slist_append(list, h.c_str());
    curl_easy_setopt(c, CURLOPT_URL, url.c_str());
    curl_easy_setopt(c, CURLOPT_USERAGENT, "HomeNX-Cards");
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
    curl_easy_cleanup(c);
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
        std::ifstream in("romfs:/gametdb.tsv");
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
    std::string body;
    for (const char* r : order[pick]) {
        if (get(std::string("https://art.gametdb.com/switch/coverM/") + r + "/" + it->second.first + ".jpg", body) == 200
            && isPicture(body))
            return body;
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
    std::vector<Game> games;
    std::vector<NsApplicationRecord> records(64);
    for (s32 offset = 0;;) {
        s32 got = 0;
        if (R_FAILED(nsListApplicationRecord(records.data(), static_cast<s32>(records.size()), offset, &got)) || got <= 0)
            break;
        for (s32 i = 0; i < got; ++i) {
            Game g;
            g.id = records[(size_t)i].application_id;
            g.order = static_cast<int>(games.size());
            games.push_back(std::move(g));
        }
        offset += got;
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
    for (HomebrewApp& hb : scanHomebrew("sdmc:/switch/HomeNX-Cards/HomeNX-Cards.nro", false)) {
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
    if (R_FAILED(socketInitializeDefault())) return false;
    return curl_global_init(CURL_GLOBAL_DEFAULT) == CURLE_OK;
}

void stopNetwork() {
    curl_global_cleanup();
    socketExit();
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

std::vector<std::uint8_t> cardArt(const Game& game, const std::string& key, ArtKind& kind) {
    if (game.homebrew) { // apps have no box art: their icon makes the card
        kind = ArtKind::Icon;
        return gameIcon(game);
    }
    const std::string base = std::string(kCacheDir) + "/" + hexId(game.id);
    const std::string grid = base + ".grid", box = base + ".box", banner = base + ".card";
    kind = ArtKind::Grid;
    if (std::filesystem::exists(grid)) return readFile(grid);
    if (!key.empty()) {
        const std::string got = steamGridDbPicture(game, key, "grids",
            "dimensions=600x900&types=static&mimes=image/png,image/jpeg&nsfw=false&humor=false");
        if (!got.empty()) {
            writeFile(grid, got);
            return std::vector<std::uint8_t>(got.begin(), got.end());
        }
    }
    kind = ArtKind::Box;
    if (std::filesystem::exists(box)) return readFile(box);
    {
        const std::string got = gametdbCover(game);
        if (!got.empty()) {
            writeFile(box, got);
            return std::vector<std::uint8_t>(got.begin(), got.end());
        }
    }
    kind = ArtKind::Banner;
    if (std::filesystem::exists(banner)) return readFile(banner);
    const std::string got = eshopBanner(game, 640, 360);
    if (!got.empty()) writeFile(banner, got);
    return std::vector<std::uint8_t>(got.begin(), got.end());
}

std::vector<std::uint8_t> backgroundArt(const Game& game, const std::string& key) {
    if (game.homebrew) return gameIcon(game); // drawn huge and soft: its colours
    const std::string path = std::string(kCacheDir) + "/" + hexId(game.id) + ".bg";
    if (std::filesystem::exists(path)) return readFile(path);
    std::string got;
    if (!key.empty())
        got = steamGridDbPicture(game, key, "heroes", "types=static&mimes=image/png,image/jpeg&nsfw=false&humor=false");
    if (got.empty()) got = eshopBanner(game, 1280, 720);
    if (!got.empty()) writeFile(path, got);
    return std::vector<std::uint8_t>(got.begin(), got.end());
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

bool launch(std::uint64_t id) {
    return R_SUCCEEDED(appletRequestLaunchApplication(id, nullptr));
}

} // namespace cards
