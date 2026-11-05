# Time initialization diagnostics

Build the menu with `xmake b -j2 SwitchU`. This branch instruments the installed
libnx through linker wrappers scoped to `timeInitialize()` on the calling thread.
It keeps the existing service selection (`time:a` in the menu, the libnx default
in homebrew), service guard, and initialization cleanup.

Messages prefixed `[time-debug]` go directly to `svcOutputDebugString` before the
SD card or wall clock is ready. They are also buffered and replayed to
`sdmc:/config/SwitchU/menu.log` once the menu logger opens. Homebrew replays to
`sdmc:/config/SwitchU/log.txt`. Replay timestamps reflect replay time, not the
original call time. A stall before the logger opens requires the debug output;
the final `begin` message identifies the operation that did not return.

The trace records:

- Service acquisition, including the actual service name and returned session.
- IPC session, HIPC type, CMIF command, kernel Result, and CMIF service Result.
  SM may use TIPC; its acquisition Result is logged at the SM wrapper instead
  of decoding TIPC as CMIF.
- Shared-memory handle, size, permissions, loading, mapping address, and both
  `shmemMap` and `svcMapSharedMemory` Results. `shmemLoadRemote` is a void API.
- The final raw `timeInitialize` Result and mapped address. A guarded repeat
  initialization can succeed without repeating acquisition or IPC.
- Entry/return from the void libc time initialization call, which is skipped
  if time initialization failed.

Every Result is printed as eight hexadecimal digits, with module and description
alongside it. A successful kernel IPC Result does not imply a successful service
Result. Initialization stops at its first failure; later initialization commands
will therefore be absent, while the independent probes below still run.

After initialization returns, read-only probes separately acquire `time:u` and
`time:a`. An acquisition failure skips only that service. For each accessible
service they independently request the user, network, steady, timezone, and local
clock sessions (commands 0–4), then test current-time reads, timezone lookup,
calendar conversion, and shared-memory acquisition/mapping (command 20 on
firmware 6+). Each temporary session and mapping is released. Calendar conversion
uses timestamp zero so it does not depend on a clock read succeeding.

If libnx initialization succeeded, a separate pass tests all three
`timeGetCurrentTime` clock types, `timeGetStandardSteadyClockTimePoint`,
`timeGetStandardSteadyClockInternalOffset`, `timeGetDeviceLocationName`, and
`timeToCalendarTimeWithMyRule`. Each call runs regardless of the previous API's
Result, allowing comparisons between direct IPC and libnx's shared-memory path.
Uninitialized libnx API calls are skipped after an initialization failure.

## On-device verification

1. Run the menu in its normal library-applet process and capture the debug output
   and `menu.log`. Find the acquisition, commands 0–4, command 20/mapping where
   supported, and final initialization Result.
2. Compare the independent `time:u` and `time:a` blocks. Record access failures
   as results for that process; do not interpret a result from homebrew or a
   daemon as proof of menu access.
3. Compare each direct clock IPC Result against the corresponding libnx API
   Result. Check timezone lookup and fixed-input calendar conversion separately.
4. If testing the homebrew build, capture `log.txt` as a separate run. Libnx may
   already have initialized time before `userAppInit`; the wrapper also traces
   that automatic initialization. Do not merge its access results with the menu's.

Cross-compilation validates the wrapper signatures and linkage. Actual service
access, IPC responses, and kernel mapping behavior require a Switch run; a host
build cannot establish those results. These probes add startup work on each
launch and are intended for this diagnostic branch.

The IPC decoder has a host test using the installed libnx headers:

```sh
g++ -std=gnu++20 -I/opt/devkitpro/libnx/include -Iprojects/menu/src \
    -fsanitize=address,undefined -g tests/time_ipc_diagnostics_test.cpp \
    -o /tmp/time-ipc-test
ASAN_OPTIONS=detect_leaks=0 /tmp/time-ipc-test
```

This checks raw Result preservation, descriptor alignment, unchanged IPC buffers,
and rejection of TIPC and missing/truncated CMIF headers. Leak detection is
disabled because the sandbox's tracing prevents LeakSanitizer from running.
