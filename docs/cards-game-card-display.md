# CardsNX: Game Card Presentation

Settings > Appearance > Game card inserted offers four immediate, persistent choices:

| Value | Label | Pin first | Title bar |
| --- | --- | --- | --- |
| 0 | Nothing | No | No |
| 1 | Game in first position | Yes | No |
| 2 | Title bar | No | Yes |
| 3 | First position and title | Yes | Yes |

`gameCardDisplay` defaults to 0 when absent and is clamped to 0..3 when loaded. Existing saved values are preserved. The title bar uses the current Glass tint, blur, border and sheen.

The independent Settings > Appearance > Game card icons modal persists as
`gameCardIcons`, clamped to 0..2:

| Value | Label | Colored inserted badge | Gray required badge |
| --- | --- | --- | --- |
| 0 | Nothing (default) | No | No |
| 1 | Inserted game card only | Yes | No |
| 2 | Inserted and required game cards | Yes | Yes |

The previous `showGameCardIcons` boolean is ignored; without an explicit new mode,
icons start hidden even when upgrading from the test build. The selection never
changes pinning, the title bar or the separate Open now badge. Saving and restarting
retain both preferences independently.

Pinning is a presentation-only projection of each library page, including categories that do not normally include those titles. It adds confirmed cartridge titles at the front in NS record-reading order, except cards explicitly hidden by the user, without duplicates, retains the other titles' order and preserves the selected title by ID. Existing game metadata is retained. An empty page selects the first displayed title. The canonical lists and saved orders never contain the temporary placement.

Multi-game cartridges, including two or three games, are supported as a set of distinct application IDs, not an arbitrary single title. Each title is marked and its launch flags retained. The title bar concatenates the system names with ` + `, using the first title's icon and fitting the text to the available width. No invented collection name is used: the current services expose application names. Reads revalidate every member and physical presence before publishing the result; cached names are reused by ID and the first icon is refreshed if reading order changes.

Library edits translate projected selections back to canonical indices and rebuild the projection afterward. Manual movement temporarily uses canonical lists; an injected-only cartridge cannot be permanently moved. Removal or an unconfirmed read restores the normal presentation.

`GameCardPresentation.hpp` contains the host-testable order and selection helpers. `cards_personalization_test.cpp` covers defaults, persistence, invalid values, pin/unpin order, replacement, empty categories, multi-title groups, duplicates and selection preservation. `game_card_test.cpp` covers one, two and three titles, cache reuse, changes in reading order, batching, partial failures and removal independently.

The tester reported successful on-console verification of both Pikmin 1+2 titles,
translations and ordering, followed by the build integrated with upstream
`8756d38`. On 2026-11-06, the tester also confirmed the corrected cartridge icon
edges and reported the final three-level icon modal package as fully working.
These are user-reported results, not automated UI tests; individual checklist
results were not separately reported. Physical three-title cartridge testing and
exhaustive language testing have not been reported; three-title behavior is
covered by mocked tests. No software launch, close, or MIG control is added.
