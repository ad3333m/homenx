#pragma once
#include <cstdint>
#include <map>
#include <string>
#include <vector>

namespace cards {

// One installed game, or a homebrew app: an NRO forwarder (an installed title,
// ID 05...) or a .nro on the SD card (`path`; its `id` is made from the path).
struct Game {
    std::uint64_t id = 0;
    std::string name, version, author;
    std::string path;              // a .nro (else it's an installed title)
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

// The network, for the worker thread.
bool startNetwork();
void stopNetwork();

// HomeNX's SteamGridDB key (Settings > SteamGridDB), "" = none.
std::string steamGridDbKey();

// Pictures, from the SD card cache (sdmc:/config/SwitchU/cards) or downloaded:
// the card's box art (SteamGridDB 600x900 with a key, else GameTDB's box cover
// matched by name, else the eShop banner from nlib.cc) and the background
// (SteamGridDB hero or the banner).
enum class ArtKind { Grid, Box, Banner, Icon }; // Icon: a homebrew app's own icon
std::vector<std::uint8_t> cardArt(const Game& game, const std::string& key, ArtKind& kind);
std::vector<std::uint8_t> backgroundArt(const Game& game, const std::string& key);
void forgetArt(std::uint64_t id); // the next ask downloads them again

// Players online in Nextendo (nextendo.network) by title, as its site counts them.
bool nextendoOnline(std::map<std::uint64_t, int>& out);

// Starts a game: the system starts it once this app has closed.
bool launch(std::uint64_t id);

} // namespace cards
