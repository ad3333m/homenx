#pragma once
#include <array>
#include <atomic>
#include <cstdint>
#include <initializer_list>
#include <map>
#include <string>
#include <utility>
#include <vector>

namespace cards {

// HOME started CardsNX this many times without it running a moment (see menu/src/main.cpp).
inline constexpr const char* kCardsStarting = "sdmc:/config/SwitchU/cards_starting";
// After a start that crashed, a safe one: no music, nothing online, plain glass, English, no
// .nro scan, so what breaks on some consoles can't stop HOME. The file keeps the rest of this
// boot's starts safe too (one crash each time back from a game would be worse).
inline constexpr const char* kCardsSafe = "sdmc:/config/SwitchU/cards_safe";
inline bool g_safeStart = false;
// config/CardsNX/start.log: how far a start got, written at once (a crash loses what
// perf.log still has in memory). Only at a console start and after a crash.
void startLogBegin(bool fresh, const std::string& line);
void startStep(const std::string& what);
void startLogEnd();

// Where CardsNX's own files are (fonts/, icons/, certs/, gametdb.tsv): its
// romfs as an .nro; HomeNX's install as HOME's frontend.
#ifdef SWITCHU_MENU
inline constexpr const char* kAssets = "sdmc:/switch/SwitchU/cards/";
#else
inline constexpr const char* kAssets = "romfs:/";
#endif

// CardsNX in other languages. lang.tsv (with the fonts): the English, then a
// column per language. Text is translated where it's measured and drawn
// (CardsActivity's measured / fit / textMid / wrap, and say); text made of
// parts goes through trf.
void setLanguage(const std::string& tag); // "" = the console's language
std::vector<std::pair<std::string, std::string>> languages(); // {tag, its own name} (Settings)
int languageGeneration(); // changes with the language (text caches start again)
std::string tr(const std::string& english);
// tr(english) with its {placeholders} filled in by `parts`, in the order they
// come in the English ("Close {game}?"); a translation can move them.
std::string trf(const std::string& english, std::initializer_list<std::string> parts);

// An account's id (AccountUid), without <switch.h> here.
struct AccountUidLike {
    std::uint64_t uid[2] = {0, 0};
};

// One installed game, or a homebrew app: an NRO forwarder (an installed title,
// ID 05...) or a .nro on the SD card (`path`; its `id` is made from the path).
struct Game {
    std::uint64_t id = 0;
    std::string name, version, author;
    std::string original;          // renamed (+ > Rename): its real name, which finds its pictures
    std::string path;              // a .nro (else it's an installed title)
    std::string action;            // a Nintendo page entry: "eshop", "album", ... (see openSystem)
    bool homebrew = false;
    bool fullCard = false;         // a homebrew app drawn as a game's card (+ > Show as a full card)
    std::uint64_t lastPlayed = 0;  // seconds since 1970 (0: never)
    std::uint64_t playSeconds = 0;
    std::uint32_t launches = 0;
    std::uint32_t flags = 0;       // ns's view flags (game card, can launch, ...); 0 = not known
    int order = 0;                 // the system's order (most recent first)
};

// Pictures are found by a game's real name, not one it was renamed to.
inline Game forLookup(Game g) {
    if (!g.original.empty()) g.name = g.original;
    return g;
}

// Installed games in the system's order, names and versions from HOME's cache
// (sdmc:/config/SwitchU/control_cache) or the system, with play time from pdm.
std::vector<Game> listGames();
enum class GameCardStatus { Checking, Absent, Ready, Error };
struct GameCardState {
    GameCardStatus status = GameCardStatus::Checking;
    Game game;
    std::vector<Game> games;
    std::uint32_t error = 0;
    std::vector<std::uint8_t> icon;
    bool contains(std::uint64_t id) const {
        for (const Game& title : games)
            if (title.id == id) return true;
        return false;
    }
};
enum class GameCardMarker { None, Required, Inserted };
GameCardMarker gameCardMarker(const Game& game, const GameCardState& state);
GameCardState readGameCard(std::uint64_t knownTitle = 0, const std::vector<Game>& knownGames = {});
// The .nro apps in sdmc:/switch (and one folder below), Sphaira, Discord
// (SwitchCord) and the Homebrew Manager first.
std::vector<Game> listHomebrew();
std::vector<std::uint8_t> gameIcon(const Game& game); // JPEG
// Quick checks (no names or icons read): the .nro files' paths, and how many
// titles are installed. Different from the last full read: something new.
std::uint64_t homebrewHash();
int installedTitleCount();

// The network, for the worker threads. Each keeps its connections open between
// pictures; threadDone() lets them go when a worker ends.
bool startNetwork();
void stopNetwork();
void threadDone();

// The lists as they were last time (sdmc:/config/CardsNX/lists.json): HOME
// shows them at once and reads the real ones on a thread.
bool loadLists(std::vector<Game>& games, std::vector<Game>& homebrew);
void saveLists(const std::vector<Game>& games, const std::vector<Game>& homebrew);

// HomeNX's SteamGridDB key (Settings > SteamGridDB), "" = none.
std::string steamGridDbKey();
void setSteamGridDbKey(const std::string& key); // Settings > Pictures (HomeNX reads the same file)

std::string idText(std::uint64_t id); // 16 hex digits
// A .nro's card id: 0xFF.. and its path's hash (as listHomebrew makes them).
inline std::uint64_t nroId(const std::string& path) {
    return 0xFF00000000000000ULL | (std::hash<std::string>{}(path) & 0x00FFFFFFFFFFFFFFULL);
}

// A Nintendo page entry's picture: HomeNX's own icon where HOME has one
// (eShop, News, Controllers, Mii Maker, Friends, Settings), else CardsNX's.
std::string systemIconPath(const std::string& action);

// Saves `url` to `path` (made through a .part file); false if it didn't arrive.
bool download(const std::string& url, const std::string& path);

// Pictures, from the SD card cache (sdmc:/config/SwitchU/cards) or downloaded:
// the card's box art (SteamGridDB 600x900 with a key, else GameTDB's box cover
// matched by name, else the eShop banner from nlib.cc) and the background
// (SteamGridDB hero or the banner).
enum class ArtKind { Grid, Box, Banner, Icon, Title }; // Icon: a homebrew app's own icon; Title: see titleArt
std::vector<std::uint8_t> cardArt(const Game& game, const std::string& key, bool boxArt, ArtKind& kind);
// The background: SteamGridDB hero, the eShop banner, the box cover, the icon.
std::vector<std::uint8_t> backgroundArt(const Game& game, const std::string& key, ArtKind& kind);
// Its card, background (and title picture) are on the SD card already (only the names are looked at).
bool artCached(const Game& game, bool title);
// stb_image's decode (RGBA), refusing pictures too big to hold (over 4096 on a side or 9 MP)
// and letting only two big ones (2 MP and up) decode at once: crashes came from many at once.
struct ArtworkReadStats {
    std::uint64_t bytes = 0;
    double milliseconds = 0;
    unsigned failures = 0;
};
ArtworkReadStats takeArtworkReadStats(); // calling worker's reads only
// yours: a picture of the user's own, not art: much bigger ones are let through.
unsigned char* decodePicture(const unsigned char* data, int size, int& w, int& h, bool yours = false);
// One thing in the Album: a picture or a video, on the SD card, in console memory, or known to both.
struct AlbumShot {
    std::string key, label;              // key: YYYYMMDDhhmmssNN (a picture of the user's: its name; none: "no picture")
    std::string path;                    // its file (none: only the Album service can read it)
    std::array<std::uint8_t, 0x18> id{}; // its CapsAlbumFileId
    bool inService = false, video = false;
    std::uint64_t bytes = 0;
    std::uint64_t app = 0;               // the software it was taken in (the Album service's; 0: not known)
};
std::vector<AlbumShot> listAlbum();                                          // all of it, newest first
std::vector<std::uint8_t> albumBytes(const AlbumShot& shot, bool thumbnail); // the whole JPEG, or its thumbnail
std::string albumPictureFile(const AlbumShot& shot);                         // a file to use (console memory: copied to the SD card)
bool deleteAlbumShot(const AlbumShot& shot);
// The HTTP status SteamGridDB answers this key with (200: it works; 401: refused; 0: no answer).
long steamGridDbCheck(const std::string& key);
// The logs, the newest crash reports and `info` in one .zip on the SD card, for a GitHub issue.
// No settings files (they hold sign-ins and keys). Its path, or "" if it couldn't be written.
std::string makeBugReport(const std::string& info);
// The console's clock: automatic (from the internet), or set by hand (HOME only).
bool autoClock();
bool darkTheme();
bool setDarkTheme(bool dark);
// SteamGridDB's games for what was typed (part of a name is enough): id and "Name (year)".
std::vector<std::pair<long long, std::string>> steamGridDbFind(const std::string& query, const std::string& key, size_t n);
// When each was last played (POSIX seconds), from the console's play log.
std::map<std::uint64_t, std::uint64_t> lastPlayTimes(const std::vector<std::uint64_t>& ids);
// A game's play statistics for this profile (what NX Activity Log shows), else the console's
// (no profile, or the per-profile ask failed): the console-wide ones count every profile's play.
struct PlayStatistics {
    std::uint64_t last = 0, seconds = 0; // last played (POSIX seconds), total play time
    std::uint32_t launches = 0;
    bool perUser = false; // the profile's own, not the console's
};
bool playStatistics(std::uint64_t id, AccountUidLike user, PlayStatistics& out);
// This profile's play time and launches of every game, counted from pdm's play-event log the
// way NX Activity Log does (the statistics call's play time differs from it). One pass over the
// log, slow: off the screen's thread. false: no valid profile, or the log couldn't be read in time or
// completely (out is empty then), or `stop` was set.
struct PlayTotal {
    std::uint64_t seconds = 0;
    std::uint32_t launches = 0;
};
bool playTotals(AccountUidLike user, std::map<std::uint64_t, PlayTotal>& out, const std::atomic<bool>* stop = nullptr);
void forgetArt(std::uint64_t id); // the next ask downloads them again
// Covers and backgrounds as they're shown (opened and shrunk), kept compressed in cards/shown/:
// read as they are next time. Opening and shrinking a cover took 0.1 to 1 s at every
// start of HOME (and HOME starts again after every game).
std::string shownPath(std::uint64_t id, const std::string& what);
// A game's cover as a very small picture (a few KB): read for every card at
// once when HOME starts, so no card shows its name instead of its cover.
inline std::string thumbPath(std::uint64_t id) { return shownPath(id, "t"); }
bool readShown(const std::string& path, std::vector<std::uint8_t>& rgba, int& w, int& h, ArtKind& kind);
void writeShown(const std::string& path, const std::vector<std::uint8_t>& rgba, int w, int h, ArtKind kind);
void forgetShown(std::uint64_t id); // its picture changed (chosen, downloaded again, a full card)
// The game's own picture with no background (the Wonder Flower for Super Mario
// Bros. Wonder): the first SteamGridDB icon with clear edges. Empty: none.
// busy: another search was running (it's asked again a little later).
std::vector<std::uint8_t> titleArt(const Game& game, const std::string& key, bool* busy = nullptr);
std::vector<std::uint8_t> chosenTitleArt(const Game& game); // only + > Title picture's
// What's installed of a game (its update's version, else its own), and what was seen
// last (config/CardsNX/versions.txt): a change reads its name and version again.
std::uint32_t installedVersion(std::uint64_t id);
std::map<std::uint64_t, std::uint32_t> readVersions();
void writeVersions(const std::map<std::uint64_t, std::uint32_t>& versions);
// + > Title picture: SteamGridDB's icons and logos for the game to choose from
// (no background first), with the one an older CardsNX picked. Slow: a thread.
struct TitleChoice {
    std::string url, file;          // the picture online, or on the SD card (the one before)
    std::vector<std::uint8_t> rgba; // a small copy to show, decoded
    int w = 0, h = 0;
    bool video = false;             // the Album's: a video (marked; its picture is the thumbnail)
    bool clear = false, before = false, own = false; // own: its own icon (the chooser adds it)
    std::string label;              // under it (the pictures chooser's)
};
// query: what was typed in the picker's Search (else the game's own name).
std::vector<TitleChoice> titleChoices(const Game& game, const std::string& key, size_t max = 19,
                                      const std::string& query = {});
// Downloads it and keeps it as the game's title picture for good (kept through
// "Download new pictures"); nullptr: its own icon instead.
bool chooseTitleArt(const Game& game, const TitleChoice* choice);
// Your own pictures: JPG or PNG files put in this folder (up to about 2000 x 2000), first
// in the card, background and title pictures' choosers.
inline constexpr const char* kMyPictures = "sdmc:/config/CardsNX/my-pictures";
std::vector<TitleChoice> myPictures(size_t max);
// + > Card picture: SteamGridDB's covers (600x900) for the game to choose from.
std::vector<TitleChoice> coverChoices(const Game& game, const std::string& key, size_t max = 15,
                                      const std::string& query = {});
// Downloads it and keeps it as the game's card for good; nullptr: back to the usual one.
// tooBig (if given): it failed because the picture is one HOME can't open, not the connection.
bool chooseCoverArt(const Game& game, const TitleChoice* choice, bool* tooBig = nullptr);
// + > Background picture: SteamGridDB's heroes to choose; the one chosen is kept (.bgme).
std::vector<TitleChoice> heroChoices(const Game& game, const std::string& key, size_t max = 11,
                                     const std::string& query = {});
bool chooseHeroArt(const Game& game, const TitleChoice* choice, bool* tooBig = nullptr);
// Halves a picture until it's at most maxW wide.
void shrink(std::vector<std::uint8_t>& rgba, int& w, int& h, int maxW);
// Scales it to maxW wide exactly (averaging), when it's wider: backgrounds at the screen's size.
void scaleDown(std::vector<std::uint8_t>& rgba, int& w, int& h, int maxW);
std::vector<std::uint8_t> scaledDown(const std::uint8_t* rgba, int& w, int& h, int maxW); // a smaller copy (w > maxW)
// SteamGridDB swaps a picture taken down (DMCA) for a flat notice: one mostly
// of a single colour. Those are passed over for the next picture it has.
bool removedNotice(const std::uint8_t* rgba, int w, int h);
void forgetPicture(std::uint64_t id, bool background); // that one comes again
bool keptBack(std::uint64_t id, const char* ext);      // the one put aside by forgetArt, back in the cache


// Players online in Nextendo (nextendo.network) by title, as its site counts them.
bool nextendoOnline(std::map<std::uint64_t, int>& out);

// Starts a game once this app has closed: as an application, through the
// system; as an applet (from the homebrew menu), HomeNX's HOME starts it
// (asked on the general channel, the way the system's own HOME is asked).
bool launch(std::uint64_t id, AccountUidLike uid);

// The console's users, their pictures (JPEG) and names.
struct User {
    AccountUidLike uid;
    std::string name;
    std::vector<std::uint8_t> picture;
};
std::vector<User> listUsers();
// Removes a user from the console (as System Settings > Users does).
bool deleteUser(AccountUidLike uid, std::string& error);

// The Nintendo page: what's on it, and opening one (blocks while it's open).
std::vector<Game> listSystem();
bool openSystem(const std::string& action, AccountUidLike uid, std::string& error);

// Quick Settings (R): the console's switches, as HomeNX's Quick Settings reads them.
struct QuickStatus {
    bool wifi = true, airplane = false, charging = false, bluetooth = true;
    float brightness = -1.f; // 0..1, -1 = not known
    int battery = -1;        // %
    int temperature = -1;    // the chip, degrees C
};
QuickStatus readQuickStatus();
int chipTemperature(); // degrees C, -1 not known
// Settings > Console > Test the SD card: how fast it writes and reads (64 MB in
// 1 MB pieces, then 200 small files), the files removed after. The result, said.
std::string sdCardTest(std::atomic<int>& progress, const std::atomic<bool>& stop);
int gpuMhz();          // the GPU's clock now (the perf log: a hot chip is throttled), 0 not known
bool setWifi(bool on);         // turning it on also ends Airplane mode
bool setAirplaneMode(bool on);
bool readAirplaneMode(bool& airplane); // false = nifm didn't answer
bool setBluetooth(bool on);
int tvResolution();              // 0 automatic, 1 1080p, 2 720p, 3 480p
bool setTvResolution(int resolution);
void setBrightness(float level); // 0..1

// Power. Hekate: the payload is handed to Atmosphere and this process (hbl)
// reboots, which Atmosphere sends to it. Only the first Switch model can.
enum class Power { Sleep, Shutdown, Restart, Hekate };
bool power(Power what, std::string& error);
bool canRebootToPayload();

// Settings > Internet, Controllers, Console: the console's own settings.
struct ConsoleSettings {
    std::string network, ip;             // the Wi-Fi's name, this console's address ("" = none)
    float vibration = 1.f;               // 0..1
    bool proWired = false, autoBrightness = false;
    int handheldSleep = 1, tvSleep = 1;  // the system's plans, 0..5 (5 = never)
    std::string firmware, atmosphere;
};
ConsoleSettings readConsoleSettings();
// Which network the console loads, as Nextendo's Prelude decides it: 1 Nextendo
// (Atmosphère's hosts file in use sends Nintendo's servers to Nextendo's), 0 Nintendo.
int nextendoMode();
void writeConsoleSettings(const ConsoleSettings& s); // the switches and the sleep times

// Settings > Internet > Connection test, on a worker: the numbers fill in as it goes.
struct NetTest {
    std::atomic<int> stage{0}; // 0 the network, 1 the Internet, 2 download, 3 upload, 4 done
    std::atomic<bool> internet{false}, stop{false};
    std::atomic<int> signal{-1}, pingMs{-1}, downKbps{-1}, upKbps{-1};
    std::atomic<char> nat{0}; // the Switch's NAT types: 'A' open .. 'F' no UDP; 0 not tested
};
void testConnection(NetTest& t);

} // namespace cards
