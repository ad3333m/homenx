# CardsNX: Game Card Presentation

Settings > Appearance > Game card inserted offers four immediate, persistent choices:

| Value | Label | Pin first | Title bar |
| --- | --- | --- | --- |
| 0 | Icon only | No | No |
| 1 | Game in first position | Yes | No |
| 2 | Title bar | No | Yes |
| 3 | First position and title | Yes | Yes |

`gameCardDisplay` defaults to 2 for existing configurations and is clamped to 0..3 when loaded. The title bar uses the current Glass tint, blur, border and sheen. The inserted card always has an original generic cartridge icon, independent of the title-bar setting and of the separate Open now badge.

Pinning is a presentation-only projection of each library page, including categories that do not normally include those titles. It adds confirmed cartridge titles at the front in NS record-reading order, except cards explicitly hidden by the user, without duplicates, retains the other titles' order and preserves the selected title by ID. Existing game metadata is retained. An empty page selects the first displayed title. The canonical lists and saved orders never contain the temporary placement.

Multi-game cartridges, including two or three games, are supported as a set of distinct application IDs, not an arbitrary single title. Each title is marked and its launch flags retained. The title bar concatenates the system names with ` + `, using the first title's icon and fitting the text to the available width. No invented collection name is used: the current services expose application names. Reads revalidate every member and physical presence before publishing the result; cached names are reused by ID and the first icon is refreshed if reading order changes.

Library edits translate projected selections back to canonical indices and rebuild the projection afterward. Manual movement temporarily uses canonical lists; an injected-only cartridge cannot be permanently moved. Removal or an unconfirmed read restores the normal presentation.

`GameCardPresentation.hpp` contains the host-testable order and selection helpers. `cards_personalization_test.cpp` covers defaults, persistence, invalid values, pin/unpin order, replacement, empty categories, multi-title groups, duplicates and selection preservation. `game_card_test.cpp` covers one, two and three titles, cache reuse, changes in reading order, batching, partial failures and removal independently.

The tester reported successful on-console verification of the final build, including both Pikmin 1+2 titles, translations and ordering. Physical three-title cartridge testing and exhaustive language testing have not been reported; three-title behavior is covered by mocked tests. No software launch, close, or MIG control is added.
This hardware confirmation applies to the development build before integration with upstream commit `8756d38`; the newly integrated revision has not been retested on hardware.
