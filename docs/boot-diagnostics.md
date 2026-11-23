# HOME boot investigation

The Atmosphère daemon owns HOME and application lifecycle. Its event worker receives
Horizon notifications, and its main loop launches the HOME library applet and drains
commands through applet storage (SMI). Sleep and wake cross this same channel.
A responsive external overlay provides no evidence of progress in either process.

The SwitchU executable uses one nxui thread for HID polling, activity dispatch, updates,
and deko3d frame acquisition and presentation. It can host HomeNX, CardsNX or the
tutorial. The initial submitted frame precedes activity construction in onCreate.
The loader, manager/NXToggle, updater, standalone CardsNX, video applet and achievements
overlay are separate executables. Media workers run in the daemon.

The menu uses one FS service session. Both processes explicitly initialize Horizon
services and mount the SD card. Existing FileLog and DebugLog sinks batch output,
and CardsNX already monitors nxui's phase markers after reaching its activity.
The resolution probe and CardsNX startup fallback cover specific failure paths.
HomeNX settings use the existing Settings activity and SD-backed JSON with backup
and legacy fallbacks under config/SwitchU.

A prior applet-pool problem motivated the current heap ladder. The new report does
not establish a cause. Early service initialization, daemon-to-menu launch, GPU
submission, activity creation and input dispatch are independently observable stages.

## After a black screen

Power off and mount the SD card on a computer. Copy
`config/SwitchU/diagnostic-menu.log` and `diagnostic-daemon.log` and their
`.old` files before additional boot attempts. Run
`python3 tools/export_boot_diagnostics.py /path/to/sd > homenx-report.txt`.
The export reads only the dedicated diagnostic files and Atmosphère boot-flag
presence. It omits raw settings, network logs, title IDs of launched games and
account data. Review the report before sharing. No menu log from the affected
attempt can indicate a failure before the SD mount or before launch. Check the
daemon launch result and collect Horizon debug output if available.

Each process has a fixed 48-record queue, a low-priority writer and a 512 KiB
current file plus one rotated file. The writer polls every 250 ms and emits a
warning once per stall. The menu uses nxui phases for its combined UI/render
thread. The daemon monitors work on its main loop and suspends monitoring
during intentional event waits and sleep. Active event dispatch and cache jobs
also have progress monitoring. The watchdog shares the writer
thread, so a blocked SD write can also prevent watchdog output. Before SD mount
the records are buffered and mirrored to Horizon debug output. Failure before
mount cannot be persisted by that process. Ordinary logs retain their existing
behavior. Compile with `HOMENX_DIAGNOSTIC_LEVEL=3` to include controller action
dispatch records, or `4` for TRACE. The default threshold is 5000 ms
(`HOMENX_DIAGNOSTIC_TIMEOUT_MS`).

## Recovery without HOME

1. With the console off, create an empty `config/SwitchU/recovery.flag` on the SD
   card and boot Atmosphère. HOME uses default app settings without writing
   the settings JSON, skips the lock screen, CardsNX selection, the
   1080p preference and pending boot housekeeping, and disables Discord in HOME.
   This still requires the daemon, HOME applet, Horizon services and renderer
   to initialize. The flag does not change Atmosphère boot settings.
2. If recovery still fails, power off and move the HomeNX override directory
   `atmosphere/contents/0100000000001000` to a backup directory outside
   `atmosphere/contents`. Use a unique destination and preserve the entire
   directory. Reboot to use the stock HOME. Restore the directory to its exact
   path with the console powered off to re-enable HomeNX.
3. To restore the previous settings, delete only `recovery.flag` and reboot.
   To reset an invalid settings file instead, back up and move
   `settings.json`, `settings.json.bak`, `config.json` and `config.json.bak`
   out of `config/SwitchU` with the console powered off. Keep the logs.

## Sysmodule profiles

Settings → System → Sysmodules reports observed boot flags. The verified
allowlist currently contains sys-clk. Unknown packages and legacy boot flags
are read-only. Running status and package versions are unavailable.
A missing active flag alone is treated as unknown. HomeNX parks the original
flag by renaming it to `flags/boot2.homenx-disabled`. The parked flag preserves
the original Normal state across reboots. Minimal parks all supported active
flags. Normal restores parked flags. Custom applies a single confirmed change.
All changes require reboot. If active and parked flags coexist, inspect both
before resolving the conflict. Never replace an existing flag when manually
restoring a parked one. Profile rollback is best effort and the parked flags
remain inspectable after interruption. Alternate launch mechanisms may still
start a module.

References: [Atmosphère flags](https://github.com/Atmosphere-NX/Atmosphere/blob/master/docs/features/configurations.md#content-specific-flags)
and [sys-clk installation](https://github.com/retronx-team/sys-clk/blob/develop/README.md).

## Hardware validation checklist

- Clean Atmosphère boot. Confirm daemon launch END, initial frame, UI creation
  END and first activity frame. Verify logs remain readable across launches.
- Boot with optional sysmodules installed. Capture the export before changing
  any boot flags. Compare traces if the display is black and overlays respond.
- Launch a game, return to HOME and resume the game. Check operation Results.
- Sleep and wake with HOME open and then with a game suspended. Check for
  spurious stall warnings.
- Dock and undock, including with the 1080p preference enabled.
- Disable sys-clk and reboot. Confirm the parked flag remains and the binary
  and configuration are intact. Restore and reboot again.
- Apply Minimal twice, then Normal twice. Confirm original flags are restored
  and unrelated entries are unchanged. Add an external active flag while a
  parked copy exists and verify that HomeNX refuses to overwrite it.
- With a backed-up malformed settings file, boot with the recovery flag.
  Confirm defaults render and original settings and logs survive. Restore the
  settings, remove the flag and reboot.

## Host validation

Run these from the repository root.

```sh
c++ -std=c++20 -pthread -Itests/support/diagnostic -Iprojects/common/include tests/diagnostic_test.cpp -o /tmp/homenx-diagnostic-test
/tmp/homenx-diagnostic-test
/tmp/homenx-diagnostic-test missing
c++ -std=c++20 -Iprojects/common/include tests/boot_modules_test.cpp -o /tmp/homenx-boot-modules-test
/tmp/homenx-boot-modules-test
```

The logger test uses a host shim for libnx threads and timing. The Switch
filesystem, scheduler, applet lifecycle and display require hardware validation.
