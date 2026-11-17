# Privacy

HomeNX has no servers and no accounts, and it collects nothing. It has no analytics, no tracking and no ads.

## What stays on your SD card

Everything HomeNX saves is on your console's SD card, readable by anyone who has the card:

- Settings: `config/SwitchU/`, `config/CardsNX/` and `config/HomeNX/`
- Your Discord sign-in token: `config/SwitchU/discord.json`
- Your SteamGridDB key: `config/SwitchU/settings.json`
- The lock screen password, as a salted hash (never the presses themselves): `config/HomeNX/lock_password`
- Downloaded game pictures, and the copies "Save every picture" makes

Deleting those files removes them. Anyone who has your SD card can use the Discord sign-in, so
keep the card safe, or sign out first.

## What leaves your console

HomeNX only connects to the internet for what you use, straight from your console to these services:

| Service | When | What it gets |
|---|---|---|
| GitHub | checking for and downloading updates | the request, your IP address |
| Discord | when you sign in and use Discord | your token and messages, as with any Discord client |
| YouTube Music (through the separately installed Switube service) | when you browse, search or play Switube music | searches and playback requests |
| SteamGridDB, GameTDB, nlib.cc | game pictures | game names or IDs, your SteamGridDB key |
| Nextendo | the green "playing online" dots | the request |
| ip-api, Open-Meteo | the weather widget | your IP address (for your town), then that town's location |
| Nintendo's Mii Studio | Mii pictures | the Mii's look |
| jsDelivr | emoji in Discord | which emoji |
| Cloudflare | the connection test | test traffic |

Each of those services has its own privacy policy.

## Switube

HomeNX controls Switube's background service locally through IPC. Switube makes the music network requests
and keeps its own settings and downloads in `config/Switube/`. HomeNX does not use Spotify credentials;
an old `config/CardsNX/spotify.json` from a previous version can be deleted.

## The website

[homenx.pages.dev](https://homenx.pages.dev) is a static site on Cloudflare Pages, with no cookies, no
trackers and no analytics. Cloudflare may log requests as it does for any website.
