# CardsNX artwork performance investigation

Status: implementation requires cross compilation and hardware validation. No claim
is made that the OLED stalls are fixed. SD performance, clocks and GPU scheduling
remain hypotheses.

## Pipeline and findings

The relevant frontend is projects/cards, not projects/menu's separate
SteamGridDbBackdrop cache. CardsActivity::select/wantArt mark requests using
per-title asked flags. A mutex/condition-variable queue feeds the existing workers.
Library cardArt/backgroundArt/titleArt first use saved artwork (including compressed
CNX2 shown-size images), with network/service fallback. Workers read, decode,
resize and save images; takeArt uploads completed pixels on the rendering thread.
Textures live in m_art, keyed by stable game ID; drawBackground/drawCards consume
them. Texture destruction transfers GPU images/descriptors to Renderer retirement,
which keeps device bytes charged until frames in flight are safe.

Before: two-second distance/count-based reclamation, potentially accumulating
pending/completed work, up to two large decoders, duplicate background uploads,
and a misleading GPU/system label for time outside update/draw. Main artwork
reads/decoding were already asynchronous. Uploads, presentation, explicit GPU
waits, resource freeing and unrelated synchronous UI image paths can block frames.
The existing watchdog and takeWaitStats/takeFreeStats remain in place.

## Changes

- CardsActivity.hpp/.cpp: byte admission before main artwork uploads, using exact
  deko image layout plus 4096-byte alignment. CARDS_ARTWORK_GPU_MB defaults to 72;
  override through compiler defines. Admission conservatively counts *all* charged
  device images, including deferred retirement and other UI images. This is not
  a replacement for the device's existing 114 MB picture-pool limit.
- LRU per-title accesses; selected title and both background transition IDs pinned.
  One unpinned entry retires per failed admission. No fence wait is added; retry
  is delayed while retired bytes remain charged. Existing distance purge removed.
- Nearby requests retain existing priority and prefetch behavior. Distant queued
  requests are cancelled, queue capped at 40, obsolete distant completions dropped.
  Main cache hits keep existing textures, with duplicate background uploads skipped.
- Completed queue stops workers at six entries or 12 MiB (capacity-based threshold).
  This is a backpressure threshold, not a hard 12 MiB peak: running workers can
  each publish a bounded result. Existing quit notification wakes waiting workers.
- Serialized artwork preparation limits concurrent temporary decode buffers.
  File payloads capped at 16 MiB, decoded/CNX2 output capped at 16 MiB,
  large stb decodes serialized, allocation exceptions in the worker pipeline
  yield a missing-result retry. Temporary buffers are bounded separately rather
  than included in the GPU budget. Compressed input, stb internals, scaling copies,
  small copies and other subsystems still contribute to total process memory.
- Library.hpp/.cpp: per-thread file-read counters drained by each artwork worker;
  duration, bytes, effective MiB/s, failures and >100 ms aggregate reads reported.
  Source time includes service/network/CNX2 decompression; decode_scale_save includes
  resizing and disk cache writes and must not be interpreted as pure decoding.
- Existing buffered perfLine path receives ART cache hit/miss, eviction, read,
  upload/accounting and selection-to-upload-visible timing. Upload visibility is
  CPU submission time, not measured scanout. Log buffer capped at 128 KiB;
  existing worker writes it every five seconds. Unmeasured frame time now says
  outside update/draw, unattributed; explicit frame/upload GPU waits remain separate.

## Validation and build

Commands attempted from /home/cyan/Projects/homenx:

```
xmake f --yes -p cross -m release -a aarch64 --toolchain=devkita64 --backend=deko3d --homebrew=y
xmake b -j2 CardsNX
```

Both dependency setup/build attempts failed fetching
https://github.com/PoloNX/switch-repo.git because github.com could not be resolved.
No cross compilation or new NRO was produced. Retry these commands with dependencies
available. Full packaging uses the existing build script, but build.sh is an
untracked pre-existing file and currently sets build 183; do not mistake it for
build 185. This change does not modify release numbering.

Passed:

```
g++ -std=c++20 -Iprojects/menu/src tests/grid_navigation_test.cpp projects/menu/src/widgets/GridNavigation.cpp -o /tmp/homenx-tests/grid_navigation
/tmp/homenx-tests/grid_navigation
git diff --check
```

The navigation test is an existing regression check, not a cache concurrency test.
No Switch hardware, rendering test or memory-pressure test was available.

## OLED diagnostic procedure / acceptance tests

1. Keep a copy of the installed cardsnx.nro and perf.log. Compile successfully
   first. Close HOME safely and replace switch/SwitchU/bin/cardsnx.nro with
   build/cross/aarch64/release/CardsNX.nro. Retain the matching installed assets.
   Reboot through the normal HomeNX loader; enable Settings performance logging.
2. Use the same artwork, layout, resolution, background/motion settings and SD
   contents for every comparison. Record firmware, Atmosphere, library sizes,
   suspended game, SD model, clock profile and GPU MHz. Do not change clocks
   automatically. Test build 185 and the candidate with identical conditions.
3. Small library (15–20): navigate back/forth for two minutes, pause on titles,
   verify immediate cached artwork, correct fades, no missing text and ~60 FPS.
4. Large library (40–60): traverse all titles three times, pause one second each.
   Verify cache hits, LRU evictions, bounded charged bytes at admission and no
   repeated whole-cache purges. Inspect deferred retirement and process free RAM.
5. Rapid navigation: hold each direction for 30 seconds, reverse repeatedly,
   switch pages, then settle. Verify selected artwork and transition correctness;
   no stale distant uploads, crashes or ever-growing memory. Trace queue size in
   debugger if needed; the implemented hard pending cap is 40.
6. Slow storage: on a diagnostic build insert a bounded 200 ms delay immediately
   before worker readShown, never in takeArt/onRender. Repeat step 5, remove the
   delay afterward. UI input must remain responsive while placeholders display.
   Compare measured read MiB/s and failures; do not infer SD fault from source time
   containing network work. No destructive SD benchmark is required.
7. Memory pressure: lower CARDS_ARTWORK_GPU_MB through build compiler defines,
   repeat steps 4–5. Selected/transition resources should stay pinned; refused
   loads should retry without blocking. Check that retirement frees bytes before
   another admission. Very low budgets may legitimately retain placeholders.
8. Suspended game: repeat with no game, then the same game suspended. Compare
   free RAM, read timings, measured waits, worst frames and watchdog output.
9. Stock/overclock: manually select known stock settings, reboot and repeat the
   same two-minute navigation trace three times; repeat under the original OC.
   Keep temperatures and power mode comparable. Preserve logs per run after
   allowing five seconds idle for buffered flushing.

Remaining work before release: successful cross build, cache-specific automated
concurrency/accounting tests, hardware validation of GPU retirement and transitions,
separate queue-latency/presentation/scheduler instrumentation, and audit remaining
UI/thumbnail/launch paths which do not all share the main cache admission policy.
This patch does not establish a strict combined CPU+GPU artwork peak across those
paths. Read counters do not expose device retries below the filesystem API.
