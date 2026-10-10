# HomeNX

Wii U-style home menu for modded Switch consoles, forked from
[SwitchU](https://github.com/PoloNX/SwitchU) by PoloNX (GPL-3.0). It ships two ways:

- **HomeNX.nro**: run it from Sphaira/hbmenu like any homebrew.
- **HomeNX-frontend.zip**: replaces the console's HOME menu (needs Atmosphere).

| Left | Right |
|---|---|
| Sphaira, Miis, News, Custom button, Settings | Nintendo eShop, Controllers, Friends, Theme Shop, Sleep |

Settings > HomeNX: the custom button's app and icon, the order of the side
buttons, and Glass effects (off = steady 60 FPS).

- **Games** are the ones installed on the console. **Profiles** are always chosen.
- **News** reads the console's own saved HOME-menu news.
- **Friends** reads the console's friend list; Add Friend and Friend Requests open
  Nintendo's screens (they need the console to go online).
- **Miis** lists your Miis with a HomeNX portrait; create/edit opens Mii Maker.
- **Controllers** opens HomeNX's Controllers menu (Change Grip/Order, Find
  Controllers, Test Input Devices, Button Mapping).
- R opens Quick Settings.

## Installing the frontend

1. Extract `HomeNX-frontend.zip` to the root of the SD card (it adds
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

## License

This project is licensed under the GNU General Public License v3.0. See the [LICENSE](https://github.com/PoloNX/SwitchU/blob/master/LICENSE) file for details.
