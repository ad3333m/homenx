# HomeNX

Wii U-style home menu for modded Switch consoles, forked from
[SwitchU](https://github.com/PoloNX/SwitchU) by PoloNX (GPL-3.0). It ships two ways:

- **HomeNX.nro**: run it from Sphaira/hbmenu like any homebrew.
- **HomeNX-full.zip**: replaces the console's HOME menu (needs Atmosphere).
  (`HomeNX-frontend.zip` is the smaller update package the console downloads itself.)

| Left | Right |
|---|---|
| Sphaira, Discord, News, Custom button, Settings | Nintendo eShop, Controllers, Social, Theme Shop, Sleep |

Settings > HomeNX: the custom button's app and icon, the order of the side
buttons, and Glass effects (off = steady 60 FPS).

- **Games** are the ones installed on the console. **Profiles** are always chosen.
- **News** reads the console's own saved HOME-menu news.
- **Social** reads the console's friend list; Add Friend and Friend Requests open
  Nintendo's screens (they need the console to go online). The **Mii Editor** is in
  Social too: your Miis with a HomeNX portrait; create/edit opens Mii Maker.
- **Discord** slides in from the left: friends online, your DMs (read and reply).
  Sign in with a QR code, reuse a saved SwitchCord account, or enter an existing
  Discord user session token. OAuth2 access tokens and bot tokens are not
  accepted by this client. **When Discord requires an interactive CAPTCHA that
  HomeNX cannot display or complete, use the user-token login instead** — it
  lets you paste an existing Discord user session token directly and skips the
  CAPTCHA entirely. Tokens are saved in plaintext on the SD card at
  `sdmc:/config/SwitchU/discord.json`; protect that file. Discord may also
  restrict accounts using third-party clients.
- **Lock Screen** — a custom, single-press lock screen with clock, date, and an
  optional wallpaper. See [Lock Screen](#lock-screen) below.
- **Controllers** opens HomeNX's Controllers menu (Change Grip/Order, Find
  Controllers, Test Input Devices, Button Mapping).
- R opens Quick Settings.

## Lock Screen

This fork adds a custom lock screen that overlays the HOME menu:

- Large centered clock and date.
- Single-press unlock: press **A** once to unlock — no multi-press confirmation.
- Optional custom wallpaper (see below).

### Wallpaper

The lock screen loads `sdmc:/switch/SwitchU/lockscreen/wallpaper.png` first, then
falls back to `wallpaper.jpg` if the PNG isn't present. Any 16:9 image works;
**1280×720 or larger is recommended** for the best result (e.g. 1920×1080).

To replace it:

1. Prepare a 16:9 image (PNG or JPG).
2. Copy it to `sdmc:/switch/SwitchU/lockscreen/` as either `wallpaper.png` or
   `wallpaper.jpg`. If both exist, the PNG wins.
3. It's picked up on the next unlock — no reboot needed.

## Installing the frontend

1. Extract `HomeNX-full.zip` to the root of the SD card (it adds
   `atmosphere/` and `switch/` folders) and reboot. It's on straight away.
2. **HomeNX Toggle** (`switch/HomeNX-Toggle/HomeNX-Toggle.nro`, copied there
   by HomeNX on its first start) has buttons to
   turn the frontend off and on, and **Check for updates** downloads the newest
   frontend from this repo's `latest` release and installs it on the console.
   Settings > power tab > **Turn Off HomeNX** also turns it off.

**If the console won't reach the HOME menu**, put the SD card in a PC and delete
`atmosphere/contents/0100000000001000`. The normal HOME menu comes back.

In the frontend, HomeNX installs **HomeNX Loader** as `sdmc:/hbmenu.nro` (the
old one is kept as `hbmenu.nro.homenx-original`). The Album's homebrew loader
always starts that file, so the Loader opens Sphaira, or the .nro picked on the
Homebrew page. Settings > HomeNX checks for updates and turns HomeNX off.

HomeNX ships [Sphaira](https://github.com/ITotalJustice/sphaira) 1.0.8 by
ITotalJustice (GPL-3.0) unmodified, at `switch/SwitchU/bin/sphaira.nro`.

---

<div align="center">
    <h1>SwitchU</h1>
    <p>A Wii U-style custom home menu replacement for Nintendo Switch</p>
</div>

<p align="center">
  <a rel="LICENSE" href="https://github.com/PoloNX/SwitchU/blob/master/LICENSE">
    <img src="https://img.shields.io/static/v1?label=license&message=GPLV3&labelColor=111111&color=0057da&style=for-the-badge" alt="License">
  </a>
  <a rel="VERSION" href="https://github.com/PoloNX/SwitchU/releases">
    <img src="https://img.shields.io/static/v1?label=version&message=1.2.0&labelColor=111111&color=06f&style=for-the-badge" alt="Version">
  </a>
  <a rel="BUILD" href="https://github.com/PoloNX/SwitchU/actions">
      <img src="https://img.shields.io/github/actions/workflow/status/PoloNX/SwitchU/switch.yml?branch=master&labelColor=111111&color=06f&style=for-the-badge" alt=Build>
  </a>
</p>

---

- [Screenshots](#screenshots)
  [Installation](#installation)
- [How to build](#how-to-build)
- [SwitchU Manager](#switchu-manager)
- [Help me](#help-me)
- [Credits](#credits)
- [License](#license)


## Screenshots

![](./screenshots/1.jpg)

<details>
  <summary><b>More screenshots</b></summary>

![](./screenshots/2.jpg)
![](./screenshots/3.jpg)
![](./screenshots/4.jpg)
![](./screenshots/5.jpg)

</details>

## Installation

Download ``SwitchU.zip`` [here](https://github.com/PoloNX/SwitchU/releases/latest) and extract ``atmosphere`` and ``switch`` folders at the root of your sdcard

## How to build

### Requirements

- [devkitPro](https://devkitpro.org/wiki/Getting_Started)
- [Xmake](https://xmake.io/#/)

### Clone

```bash
git clone --recursive https://github.com/PoloNX/SwitchU
cd SwitchU
```

### Build (production daemon + external menu mode)

```bash
xmake f -p cross --toolchain=devkita64 --homebrew=n --backend=deko3d
xmake
```

### Build (homebrew .nro mode)

```bash
xmake f -p cross --toolchain=devkita64 --homebrew=y --backend=deko3d
xmake
```

### Clean

```bash
xmake clean
```

Build outputs are generated under `build/cross/aarch64/<mode>/`.

## SwitchU Manager

`SwitchU-Manager.nro` enables or disables SwitchU

## Know issues
- Some settings are not implemented yet
- Current icons are very ugly, feel free to replace them with better ones
- You may experience some crash when using a lot of sysmodules. I tried to reduce the ram usage but I still get some instability when launching a game.

## Unsorted roadmap
- [x] Add a pannel when pressing + on a game to show more information about it    
- [x] Add controller/screen test window
- [x] Add Wii U-style folders
- [ ] Improve theme shop UI and metadata
- [ ] Add eshop and news applet  
- [ ] Add a setting to switch the theme based on the time of day  
- [x] Add a steamgriddb integration to get game banners and backgrounds  
- [ ] Replace temporary icons with polished assets
- [ ] Add more animations inspired by Wii U
- [ ] Improve localization coverage

## Help me

If you want to help, open an issue when you find a bug and open a pull request if you have a fix.

## Credits

- Thanks to [Xortroll](https://github.com/Xortroll) for the help and for [uLaunch](https://github.com/Xortroll/uLaunch) which inspired this project a lot

- Thanks to [ad3333m](https://github.com/ad3333m) for [HomeNX](https://github.com/ad3333m/homenx) which was the original Project I forked to add more features and improve independently.

## License

This project is licensed under the GNU General Public License v3.0. See the [LICENSE](https://github.com/PoloNX/SwitchU/blob/master/LICENSE) file for details.

---

<div align="center">
  <a href="https://buymeacoffee.com/cyanes">
    <img src="https://cdn.buymeacoffee.com/buttons/v2/default-yellow.png" width="180" alt="Buy Me A Coffee">
  </a>
</div>
```
