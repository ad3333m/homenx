#pragma once
#include <nlohmann/json.hpp>

#include <algorithm>
#include <map>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <map>
#include <string>
#include <vector>

namespace cards {

struct CustomPage {
    std::string name;
    std::vector<std::string> titles;
};

// Writes `data` to `path` on CardsNX's writer thread (Library.cpp): the
// screen's thread never waits on the SD card, which an install or the
// pictures can keep busy for seconds. The newest data for a path wins.
void writeLater(const std::string& path, std::string data);
void flushWrites(); // everything written (before HOME closes)

// CardsNX's settings (the gear, or the Nintendo page > Settings), in
// sdmc:/config/CardsNX/settings.json.
struct Settings {
    static constexpr int kBuiltinPages = 4;
    static constexpr int kMaxCustomPages = 8;
    static constexpr int kMaxPages = kBuiltinPages + kMaxCustomPages;
    std::vector<CustomPage> customPages;
    bool notificationCards = true;
    bool lockGameBanner = true;
    bool music = true;
    int musicVolume = 60;     // 0..100
    bool shuffle = false;
    bool repeat = false;      // the same song again
    bool sounds = true;
    int soundVolume = 80;     // 0..100
    bool players = true;      // Nextendo's green dots
    bool motion = true;       // animations (off: things move straight to place)
    bool bgMotion = true;     // the background art drifts
    int startPage = 0;        // (old: the page order's first is where it opens now)
    std::vector<int> pageOrder = {0, 1, 2, 3}; // the pages in order: 0 games, 1 homebrew, 2 Nintendo, 3 media, then custom pages
    int lastPage = -1; // the custom page something was started from: back there afterwards
    int sort = 1;             // 0 last played, 1 title, 2 most played, 3 my order (Plus > Move)
    int haze = 45;            // how much the background art is softened, 0..80
    bool boxArt = true;       // box covers on cards (off: the eShop banner)
    int accent = 0;           // the focus colour (see kAccents)
    int glass = 1;            // 0 clear, 1 frosted, 2 dark
    int launchStyle = 0;      // starting a game: 0 simple fade, 1 splash, 2 card zoom, 3 card flip, 4 dive in, 5 off
    bool clock24 = true;
    bool showFree = true;     // free space in the status pill
    bool showTemp = true;     // the chip's temperature in the status pill (red and said when hot)
    bool titlePics = true;    // a 3D picture by the game's name in the panel
    // Homebrew moved by hand (+ > Move to Media / Move to Homebrew): .nro paths,
    // NRO forwarders' title IDs. Out: the ones Media takes by itself, kept out.
    std::vector<std::string> mediaIn, mediaOut;
    std::vector<std::string> gamesIn, gamesOut; // moved by hand onto / off the Games page (ports, ...)
    std::vector<std::string> fullCards;         // homebrew drawn as games' cards (+ > Show as a full card)
    std::uint64_t nroHash = 0;  // the .nro files at the last full read (homebrewHash)
    int titles = -1;            // the installed titles then (installedTitleCount)
    bool askUser = false;       // starting a game asks who's playing, as Nintendo's HOME does
    std::string panelApp = "news"; // the panel's right button: "news", "" nothing, else a card's id (hex)
    bool lockScreen = false;    // a lock screen when the console wakes
    bool lockMusic = false;     // the menu's music keeps playing on the lock screen
    bool keepHomebrew = true;   // HOME leaves a homebrew app open to resume, as a game (off: HOME closes it)
    std::string hbPath, hbHost; // the homebrew app left open and the game it runs as (HOME only)
    std::map<std::string, std::string> hiddenCards; // cards hidden with + (their ids and names): still installed
    int sheetBack = -1, sheetBackRow = 0; // Settings' tab and row while one of Nintendo's screens is open (HOME only)
    std::vector<int> hiddenPages; // pages not shown (ZL / ZR skip them; one always stays)
    bool spotify = false;       // the music player shows Spotify, not HOME music (HOME only)
    long long picturesAt = 0;   // when every game's pictures were last made sure of (seconds)
    long long achSeenAt = 0;    // the newest achievement HOME has said (seconds; 0: none said yet)
    bool introDone = false;     // the first-time intro was shown
    std::string wallpaper;      // one picture behind everything ("" = each game's art)
    std::map<std::string, std::string> names; // + > Rename: by title ID (hex) or .nro path
    std::vector<std::string> sheetOrder; // Settings' tabs by name, in his order (new ones after)
    std::string user;         // who games start as (hex), "" = the last one who played
    std::string lastGame;     // the last game (or app) started from here, hex
    long long listsAt = 0;    // when the games and apps were last read in full (seconds since 1970)
    bool perfLog = true;      // sdmc:/config/CardsNX/perf.log: start-up times and slow frames
    std::string language;     // "es", "ja", ... ("" = the console's)
    std::vector<std::string> order[kMaxPages]; // My order, per page: ids (hex)

    void normalizePages() {
        if (customPages.size() > kMaxCustomPages) customPages.resize(kMaxCustomPages);
        const int count = kBuiltinPages + static_cast<int>(customPages.size());
        std::vector<int> valid;
        for (int page : pageOrder)
            if (page >= 0 && page < count && std::find(valid.begin(), valid.end(), page) == valid.end()) valid.push_back(page);
        for (int page = 0; page < count; ++page)
            if (std::find(valid.begin(), valid.end(), page) == valid.end()) valid.push_back(page);
        pageOrder = std::move(valid);
        hiddenPages.erase(std::remove_if(hiddenPages.begin(), hiddenPages.end(), [count](int page) {
            return page < 0 || page >= count;
        }), hiddenPages.end());
        std::sort(hiddenPages.begin(), hiddenPages.end());
        hiddenPages.erase(std::unique(hiddenPages.begin(), hiddenPages.end()), hiddenPages.end());
        if (static_cast<int>(hiddenPages.size()) == count) hiddenPages.erase(hiddenPages.begin());
    }

    static constexpr const char* kPath = "sdmc:/config/CardsNX/settings.json";

    // The file as it was read: save() writes its own keys over it, so a key
    // this build doesn't know (one from a newer or another branch's CardsNX,
    // say spotifyEnabled) survives instead of going back to its default there.
    nlohmann::json loaded = nlohmann::json::object();

    void load() {
        try {
            std::ifstream in(kPath);
            if (!in) return;
            const auto j = nlohmann::json::parse(in);
            if (j.is_object()) loaded = j;
            music = j.value("music", music);
            musicVolume = j.value("musicVolume", musicVolume);
            shuffle = j.value("shuffle", shuffle);
            repeat = j.value("repeat", repeat);
            sounds = j.value("sounds", sounds);
            soundVolume = j.value("soundVolume", soundVolume);
            players = j.value("players", players);
            motion = j.value("motion", motion);
            bgMotion = j.value("bgMotion", bgMotion);
            startPage = j.value("startPage", startPage);
            pageOrder = j.value("pageOrder", pageOrder);
            customPages.clear();
            if (j.contains("customPages") && j["customPages"].is_array())
                for (const auto& page : j["customPages"]) {
                    if (!page.is_object() || !page.contains("name") || !page["name"].is_string()) continue;
                    CustomPage custom;
                    custom.name = page["name"].get<std::string>();
                    if (custom.name.empty()) continue;
                    if (page.contains("titles") && page["titles"].is_array())
                        for (const auto& id : page["titles"])
                            if (id.is_string() && std::find(custom.titles.begin(), custom.titles.end(), id.get<std::string>()) == custom.titles.end())
                                custom.titles.push_back(id.get<std::string>());
                    customPages.push_back(std::move(custom));
                    if (customPages.size() == kMaxCustomPages) break;
                }
            notificationCards = j.value("notificationCards", notificationCards);
            lastPage = j.value("lastPage", lastPage);
            lockGameBanner = j.value("lockGameBanner", lockGameBanner);
            sort = j.value("sortOrder", sort);
            haze = j.value("haze", haze);
            boxArt = j.value("boxArt", boxArt);
            accent = j.value("accent", accent);
            glass = j.value("glass", glass);
            launchStyle = j.value("launchStyle", launchStyle);
            clock24 = j.value("clock24", clock24);
            showFree = j.value("showFree", showFree);
            showTemp = j.value("showTemp", showTemp);
            titlePics = j.value("titlePics", titlePics);
            mediaIn = j.value("mediaIn", mediaIn);
            mediaOut = j.value("mediaOut", mediaOut);
            gamesIn = j.value("gamesIn", gamesIn);
            gamesOut = j.value("gamesOut", gamesOut);
            fullCards = j.value("fullCards", fullCards);
            nroHash = j.value("nroHash", nroHash);
            titles = j.value("titles", titles);
            askUser = j.value("askUser", askUser);
            panelApp = j.value("panelApp", panelApp);
            lockScreen = j.value("lockScreen", lockScreen);
            lockMusic = j.value("lockMusic", lockMusic);
            keepHomebrew = j.value("keepHomebrew", keepHomebrew);
            hbPath = j.value("hbPath", hbPath);
            hbHost = j.value("hbHost", hbHost);
            hiddenCards = j.value("hiddenCards", hiddenCards);
            sheetBack = j.value("sheetBack", sheetBack);
            sheetBackRow = j.value("sheetBackRow", sheetBackRow);
            hiddenPages = j.value("hiddenPages", hiddenPages);
            spotify = j.value("spotify", spotify);
            picturesAt = j.value("picturesAt", picturesAt);
            achSeenAt = j.value("achSeenAt", achSeenAt);
            introDone = j.value("introDone", introDone);
            wallpaper = j.value("wallpaper", wallpaper);
            names = j.value("names", names);
            sheetOrder = j.value("sheetOrder", sheetOrder);
            user = j.value("user", user);
            lastGame = j.value("lastGame", lastGame);
            listsAt = j.value("listsAt", listsAt);
            perfLog = j.value("perfLog", perfLog);
            language = j.value("language", language);
            const char* const keys[4] = {"orderGames", "orderHomebrew", "orderNintendo", "orderMedia"};
            for (int p = 0; p < 4; ++p) order[p] = j.value(keys[p], order[p]);
        } catch (...) {
        }
        normalizePages();
    }

    void save() const {
        nlohmann::json j = {{"music", music}, {"musicVolume", musicVolume}, {"shuffle", shuffle}, {"repeat", repeat},
                            {"sounds", sounds}, {"soundVolume", soundVolume}, {"players", players},
                            {"motion", motion}, {"bgMotion", bgMotion}, {"startPage", startPage}, {"pageOrder", pageOrder},
                            {"sortOrder", sort}, {"haze", haze}, {"boxArt", boxArt}, {"accent", accent},
                            {"glass", glass}, {"launchStyle", launchStyle}, {"clock24", clock24}, {"showFree", showFree}, {"showTemp", showTemp}, {"titlePics", titlePics}, {"mediaIn", mediaIn}, {"mediaOut", mediaOut}, {"gamesIn", gamesIn}, {"gamesOut", gamesOut}, {"fullCards", fullCards}, {"nroHash", nroHash}, {"titles", titles}, {"askUser", askUser}, {"panelApp", panelApp},
                            {"lockScreen", lockScreen}, {"lockMusic", lockMusic}, {"keepHomebrew", keepHomebrew}, {"hbPath", hbPath}, {"hbHost", hbHost}, {"hiddenCards", hiddenCards}, {"sheetBack", sheetBack}, {"sheetBackRow", sheetBackRow}, {"hiddenPages", hiddenPages}, {"spotify", spotify}, {"picturesAt", picturesAt}, {"achSeenAt", achSeenAt},{"introDone", introDone}, {"wallpaper", wallpaper}, {"names", names},
                            {"sheetOrder", sheetOrder}, {"user", user}, {"lastGame", lastGame}, {"listsAt", listsAt}, {"perfLog", perfLog}, {"language", language},
                            {"orderGames", order[0]}, {"orderHomebrew", order[1]}, {"orderNintendo", order[2]},
                            {"orderMedia", order[3]}};
        j["customPages"] = nlohmann::json::array();
        for (const auto& page : customPages) j["customPages"].push_back({{"name", page.name}, {"titles", page.titles}});
        j["notificationCards"] = notificationCards;
        j["lastPage"] = lastPage;
        j["lockGameBanner"] = lockGameBanner;
        nlohmann::json out = loaded;
        out.update(j); // this build's keys win; the others stay as they were
        writeLater(kPath, out.dump(2));
    }
};

} // namespace cards
