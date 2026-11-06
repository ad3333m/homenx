# CardsNX: Inserted Game Card

CardsNX can show a small status row above the cards, with the inserted game's system
name and icon. Its existing library card has a generic cartridge icon.
This is separate from "Open now": inserted software is not necessarily running.

Settings > Appearance > Game card inserted selects Icon only, Game in first
position, Title bar (default), or First position and title. Pinning is temporary,
bypasses category membership while respecting explicitly hidden cards, removes
duplicates and preserves other titles' order and the selected title while it
remains available. The bar follows the current Glass style.
See `cards-game-card-display.md` for persistence and presentation details.

## Scope

- Reads only the cartridge and titles currently recognized by Nintendo services.
- Does not communicate with MIG hardware, switch its games, or enumerate its files.
- Does not launch or close software or persistently reorder the library.
- Preserves an existing selected title while it remains available. Empty pages or
   removal of an injected-only title use a bounded fallback selection. An explicit
   software scan retains its existing behavior.
- Previously used cartridge games remain in the library without the inserted marker.
- The status row is independent of library pages, filters, and artwork downloads.
- This change applies to CardsNX, both standalone and as the default HOME layout.

## Detection

The background reader first checks physical cartridge presence through FS. If a
cartridge is present, it scans NS application views for both game-card flags:
`IsGameCard` and `IsGameCardInserted`. All distinct matching titles are collected
in reading order, including multi-game cartridges. Names and the first title's icon
come from NS control data, without writing to the control cache. The reader checks
every selected view and physical presence again before publishing the result.

The UI normally polls every two seconds, or every half-second while checking.
Daemon notifications and wake-up request a fresh read. Results from before an
invalidation are discarded. Reads are asynchronous; actual latency also depends
on the system services. Matching ready titles reuse their decoded icon.

States are checking, absent, ready, and error. A cartridge with no confirmed title
stays checking rather than guessing. Multiple confirmed titles are ready together. After ten
seconds of unresolved checking, an error is shown. Service and mount errors show
their result code when available. Polling continues so removal or a successful
read can recover the display.

## Validation

`tests/game_card_test.cpp` exercises the production reader with mocked FS/NS
services and real libnx declarations. It covers title changes, historical entries,
record ordering and batching, absence, conflicting flags, removal during reads,
metadata failures, missing icons, resource cleanup, and recovery.

The test is included in `tests/run_cards_personalization.sh`. It needs a host C++20
compiler, libnx headers, zlib, and the runner's existing JSON dependency. Use LF
line endings when running the shell script from a Windows checkout.

All four host tests pass with ASan/UBSan, including the upstream wake/notification
test. Translation structure checks pass; three existing unused-key warnings remain.
Standalone CardsNX, embedded HOME and the achievements overlay built successfully
from a new feature-only checkout based on upstream commit `8756d38`, with exit 0
and no OOM on 2026-11-06. This verification reused the existing SDK, dependency
cache and Sphaira release asset; it is not a fresh dependency-bootstrap test.
The local libcurl override and eSpeak staging fix were excluded from that checkout.

The tester confirmed an earlier development package on a physical Switch:
Pikmin 1+2 is recognized as two cartridge titles, with working presentation,
translations and ordering. On 2026-11-06, the tester also reported that the
test package integrated with upstream `8756d38` worked correctly on the Switch.
These are user-reported hardware results, not automated UI tests. Individual
console checklist results were not separately reported. Three-title cartridges
are covered by mocked reader tests; physical three-title testing and exhaustive
testing of all translation languages have not been reported.

## Console Checks After Building

1. With no game running, verify the empty slot shows "No game card inserted".
2. Insert game A: check its name, icon, and marker on its existing card.
3. Change A to B and back to A, with no game running. Confirm only the currently
   recognized titles are marked, and library order and selection remain stable.
4. Remove and reinsert the cartridge. Checking should resolve without a manual scan.
5. Launch the inserted game and return to HOME without changing the cartridge.
   Confirm the inserted marker and "Open now" remain distinct; resume the game.
6. Sleep and wake without changing the cartridge; verify the status refreshes.
7. Insert a multi-game cartridge: verify all its titles are marked, shown in the
   title bar, and pinned in reading order only when the selected mode enables it.

Do not change or remove a cartridge while its game is running to provoke errors.
If a normal read fails, record the displayed result code and whether removing and
reinserting the cartridge while no game is running restores the status.