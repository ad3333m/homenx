#pragma once
#include <cstdint>
#include <map>
#include <string>
#include <vector>

namespace cards {

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

// HomeNX's SteamGridDB key (Settings > SteamGridDB), "" = none.
std::string steamGridDbKey();

// Pictures, from the SD card cache (sdmc:/config/SwitchU/cards) or downloaded:
// the card's box art (SteamGridDB 600x900 with a key, else GameTDB's box cover
// matched by name, else the eShop banner from nlib.cc) and the background
// (SteamGridDB hero or the banner).
enum class ArtKind { Grid, Box, Banner, Icon }; // Icon: a homebrew app's own icon
std::vector<std::uint8_t> cardArt(const Game& game, const std::string& key, bool boxArt, ArtKind& kind);
// The background: SteamGridDB hero, the eShop banner, the box cover, the icon.
std::vector<std::uint8_t> backgroundArt(const Game& game, const std::string& key, ArtKind& kind);
void forgetArt(std::uint64_t id); // the next ask downloads them again

// Players online in Nextendo (nextendo.network) by title, as its site counts them.
bool nextendoOnline(std::map<std::uint64_t, int>& out);

// Starts a game once this app has closed: as an application, through the
// system; as an applet (from the homebrew menu), HomeNX's HOME starts it
// (asked on the general channel, the way the system's own HOME is asked).
bool launch(std::uint64_t id, AccountUidLike uid);

// The Nintendo page: what's on it, and opening one (blocks while it's open).
std::vector<Game> listSystem();
bool openSystem(const std::string& action, AccountUidLike uid, std::string& error);

// Power. Hekate: the payload is handed to Atmosphere and this process (hbl)
// reboots, which Atmosphere sends to it. Only the first Switch model can.
enum class Power { Sleep, Shutdown, Restart, Hekate };
bool power(Power what, std::string& error);
bool canRebootToPayload();

} // namespace cards
