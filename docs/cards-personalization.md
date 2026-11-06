# Pages, lock screen, achievements and storage

These features are in the Cards interface, both as HOME and as the standalone
CardsNX app. The shared HOME lock screen also shows a battery icon and uses the
same wallpaper rotation selection.

- **Custom pages:** Settings > Home > Custom pages creates, renames or deletes
  a named collection (up to eight). Use **+ > Add to custom page** on a game or
  app. Collections leave the original library entries intact. ZL/ZR, page order,
  page visibility, and the top-left **Pages** menu include the new pages. Deleting
  a page does not uninstall anything. Membership and ordering survive restarts.
- **Hidden cards:** **+ > Hide this card** takes a card off HOME without deleting
  anything; Settings > Home > Hidden cards brings it back.
- **Your own pictures:** JPG or PNG files in `config/CardsNX/my-pictures` (up to
  about 2000 x 2000) come first in the card, background and title pictures' choosers.
- **Lock screen:** the battery icon shows charge level, low charge and charging.
  With a game open, the lock screen uses the same hero art as that game's HOME
  background, with its title in a subtle centered caption below the date. Custom wallpaper is used
  when no game hero is available; disable Playing game background in Settings > Lock
  screen to always use custom wallpaper. Artwork loads on
  the existing workers and is kept while the screen is locked.
- **Wallpapers:** Settings > Lock screen > Choose wallpapers to shuffle lists
  images in `config/CardsNX/backgrounds`, `switch/SwitchU/lockscreen`, and the
  newest 100 screenshots, plus previously selected images. Toggle as many as
  desired (up to 256) and choose Done. Each new lock chooses a usable selected
  image, avoiding the previous image when alternatives exist. Missing files are
  skipped. Turn rotation off to retain a fixed image. The existing thumbnail
  picker also supports X to select images for rotation.
- **Notifications:** floating cards show battery warnings, controller changes,
  achievements, Discord alerts and completed transfers, with an icon, accent
  border, entrance/exit animation and timeout indicator. System notices also
  appear in notification history. Settings > Home can disable the
  cards; an active software transfer still shows its status. Controller banners
  are suppressed while locked/asleep, with no reconnect replay after unlocking.
- **Achievements:** **+ > Achievements** on a game. Every game has milestones
  (play time, a long session, coming back after months), and games with a pack
  have their own goals too. Settings > Home > Playtime milestones turns the
  milestones off; existing earned records are preserved. First-launch rewards are removed.
- **Sorting:** the sort button in the top bar, or Settings > Home > Sort games.
- **Storage:** Settings > Storage shows console/microSD capacity and software
  locations. Select installed software, a destination, and confirm the move.
  Transfers use the console's NS application-management service, on a worker.
  Save data remains on the console. Running software must be closed first, and
  launching software is blocked during a transfer. The interface reports the
  returned Result when transfer requests fail. No installed data is copied or
  deleted directly by the frontend.

Settings are in `config/CardsNX/settings.json`. The shared lock-screen selection
uses `config/SwitchU/lock_pictures.txt` and `lock_picture_shuffle`; milestones
turned off leave `config/SwitchU/achievement_milestones_off`.

When locking is enabled, waking during a game launches the menu in an explicit
wake-lock mode after foreground ownership is confirmed. Sleep does not create a
menu or lock foreground while AM is suspending processes: the pre-sleep handoff
caused a black display on hardware and was removed. The game remains suspended
rather than being resumed automatically. When sleep starts from a game with no menu process, the daemon uses VI manager
content visibility to cover the game before sleep. On wake the menu starts
normally, then acknowledges three presented lock frames; the daemon restores
visibility. An eight-second wake timeout restores visibility if startup or the
acknowledgement fails. Sleep failure and daemon exit also restore visibility.
The cover applies to the display compositor rather than only the handheld
backlight. VI Results are logged; hardware validation is required for service
access and wake ordering. Existing-menu wake keeps its immediate lock screen.
Both the **daemon and menu**
must be updated together for this wake handoff. With locking disabled, the
existing direct game-resume behavior is retained.

## Verification

Run `bash tests/run_cards_personalization.sh` for saved-settings migration,
custom-page membership/order, bounded page ids, wallpaper persistence, rotation
without immediate repeats, missing-file fallback, and milestone preferences.
Wake-policy tests cover active versus absent menus, running applications,
frontend-specific lock preferences, and silent controller baseline updates while
locked/asleep without replay on unlock.
It also exercises the real libnx CMIF serialization against a simulated NS/kernel
boundary: both transfer directions, permission denials, insufficient space,
allocation failures, request/completion failures, the legacy firmware path, and
transfer-memory lifetime/cleanup. This test never moves installed software.
It uses AddressSanitizer and UndefinedBehaviorSanitizer; LeakSanitizer is disabled
because tracing in the sandbox prevents it from running.

Build the HOME and standalone variants with `xmake b -j8 SwitchU CardsNX` and the
achievement overlay with `make -C projects/achievements -j8`. Console service
permissions, game-specific achievement conditions, actual storage migration,
controller events, and the rendered layout need verification on a Switch.

Storage protocol reference: NS command 1310, [RequestMoveApplicationEntity](https://switchbrew.org/wiki/NS_services#RequestMoveApplicationEntity)
on firmware 10+, with transfer memory retained until its async result completes;
older firmware uses libnx's legacy move command. Both paths request a native
application move rather than moving save data or manipulating content files.
