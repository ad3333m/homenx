#pragma once
#include <atomic>
#include <cstdint>
#include <map>
#include <string>
#include <vector>

namespace cards {

// Where CardsNX's own files are (fonts/, icons/, certs/, gametdb.tsv): its
// romfs as an .nro; HomeNX's install as HOME's frontend.
#ifdef SWITCHU_MENU
inline constexpr const char* kAssets = "sdmc:/switch/SwitchU/cards/";
#else
inline constexpr const char* kAssets = "romfs:/";
#endif

// An account's id (AccountUid), without <switch.h> here.
struct AccountUidLike {
    std::uint64_t uid[2] = {0, 0};
};

// One installed game, or a homebrew app: an NRO forwarder (an installed title,
// ID 05...) or a .nro on the SD card (`path`; its `id` is made from the path).
struct Game {
    std::uint64_t id = 0;
    std::string name, version, author;
    std::string path;              // a .nro (else it's an installed title)
    std::string action;            // a Nintendo page entry: "eshop", "album", ... (see openSystem)
    bool homebrew = false;
    std::uint64_t lastPlayed = 0;  // seconds since 1970 (0: never)
    std::uint64_t playSeconds = 0;
    std::uint32_t launches = 0;
    std::uint32_t flags = 0;       // ns's view flags (game card, can launch, ...); 0 = not known
    int order = 0;                 // the system's order (most recent first)
};

// Installed games in the system's order, names and versions from HOME's cache
// (sdmc:/config/SwitchU/control_cache) or the system, with play time from pdm.
std::vector<Game> listGames();
// The .nro apps in sdmc:/switch (and one folder below), Sphaira, Discord
// (SwitchCord) and the Homebrew Manager first.
std::vector<Game> listHomebrew();
std::vector<std::uint8_t> gameIcon(const Game& game); // JPEG

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

std::string idText(std::uint64_t id); // 16 hex digits

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
void forgetArt(std::uint64_t id); // the next ask downloads them again
// The game's own picture with no background (the Wonder Flower for Super Mario
// Bros. Wonder): the first SteamGridDB icon with clear edges. Empty: none.
std::vector<std::uint8_t> titleArt(const Game& game, const std::string& key);
// + > Title picture: SteamGridDB's icons and logos for the game to choose from
// (no background first), with the one an older CardsNX picked. Slow: a thread.
struct TitleChoice {
    std::string url, file;          // the picture online, or on the SD card (the one before)
    std::vector<std::uint8_t> rgba; // a small copy to show, decoded
    int w = 0, h = 0;
    bool clear = false, before = false, own = false; // own: its own icon (the chooser adds it)
};
std::vector<TitleChoice> titleChoices(const Game& game, const std::string& key, size_t max = 19);
// Downloads it and keeps it as the game's title picture for good (kept through
// "Download new pictures"); nullptr: its own icon instead.
bool chooseTitleArt(const Game& game, const TitleChoice* choice);
// + > Card picture: SteamGridDB's covers (600x900) for the game to choose from.
std::vector<TitleChoice> coverChoices(const Game& game, const std::string& key, size_t max = 15);
// Downloads it and keeps it as the game's card for good; nullptr: back to the usual one.
bool chooseCoverArt(const Game& game, const TitleChoice* choice);
// Halves a picture until it's at most maxW wide.
void shrink(std::vector<std::uint8_t>& rgba, int& w, int& h, int maxW);
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
    bool wifi = true, airplane = false, charging = false;
    float brightness = -1.f; // 0..1, -1 = not known
    int battery = -1;        // %
    int temperature = -1;    // the chip, degrees C
};
QuickStatus readQuickStatus();
bool setWifi(bool on);         // turning it on also ends Airplane mode
bool setAirplaneMode(bool on);
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
