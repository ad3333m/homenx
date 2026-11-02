# Achievement packs

A pack gives one game its own achievements, like "Finish the Cap Kingdom". While the game runs, HomeNX reads its
memory once a second (through Atmosphère's cheat service, read-only). An achievement unlocks the moment its
condition holds. With Ultrahand, a notification pops up over the game.

Anyone can write a pack. Put it on the SD card, or send it to this repository so everyone gets it.

## Where packs go

- Your own: `sdmc:/config/HomeNX/achievements/<title id>.txt`. These win over the ones HomeNX ships.
- Shipped with HomeNX: `projects/achievements/packs/<title id>.txt` in this repository. They're installed to
  `sdmc:/switch/SwitchU/achievements/`.

The file name is the game's title ID in 16 hex digits, for example `0100000000010000.txt` for Super Mario Odyssey.
A collection (several games in one, like Super Mario 3D All-Stars) has one pack under its own title ID: each game
in it has its own build IDs, so its `build` sections keep them apart.

## The format

```
# Lines starting with # are notes.
game 0100000000010000 Super Mario Odyssey

# a <id> | <name> | <what to do>
a moons100 | Moon hunter | Collect 100 Power Moons
a capclear | Hat trick | Finish the Cap Kingdom

# Each version of the game has its own addresses: "build" and that version's build id,
# then a "c" line per achievement.
build 3CA12DFAAB9C82DA
# c <id> | <type> <address> <comparison> <number>
c moons100 | u32 [[main+2D21A90]+20]+1C0 >= 100
c capclear | u8 [main+2E5F000]+12 == 1
```

(The addresses above only show the shape. They are not real.)

- **id:** letters, digits, `_`, `-` or `.`. Never change an id once a pack is shared, or people lose what they
  earned.
- **build id:** the start of the game's build ID, in hex. It's the name of that version's cheat file
  (`atmosphere/contents/<title id>/cheats/<build id>.txt`). HomeNX also writes it to `config/SwitchU/daemon.log`
  ("[achievements] <game>: build ...") when you play a game that has a pack.
- **type:** `u8`, `u16`, `u32`, `u64` (unsigned), `s8`, `s16`, `s32`, `s64` (signed), `f32` or `f64` (decimals).
- **address:** where the value is.
  - `main`, `heap` and `alias` are the game's regions, as in cheat codes. `main+2D21A90` is 0x2D21A90 into the main
    program.
  - Numbers in addresses are hex, with or without `0x`.
  - `[...]` reads the pointer stored there: `[[main+2D21A90]+20]+1C0` follows two pointers, then adds 0x1C0.
- **comparison:** `==`, `!=`, `>=`, `>`, `<=`, `<`. `&` means "any of these bits are set" (`& 0x4`).
- **number:** decimal, or hex with `0x`.
- **a second comparison** (optional): `u32 [main+1234]+10 >= 10000 <= 9999999`. Both must hold. Use it to rule out
  nonsense: a pointer read during a loading screen can briefly point somewhere else.

A condition has to hold for 3 seconds in a row before the achievement unlocks.

An achievement can have several `c` lines for one version: any of them unlocks it. Use this for games with save
slots, where each slot keeps its values somewhere else. An achievement with no condition for the running version
stays locked on that version.

## Finding addresses

The values achievements need (moons, coins, levels, story flags) are the same values cheat codes change.

- **From a cheat code.** Atmosphère codes that start `580F0000 <offset>` load the pointer at `main+<offset>`.
  `580F1000 <offset>` reads the next pointer, and `780F0000 <offset>` adds an offset. Together they give a chain like
  `[[main+offset]+offset]+offset`. The type comes from the code's write width.
- **By searching memory.** Use EdiZon SE or Breeze to find a value as it changes (collect a moon, search again),
  then find a pointer chain to it that survives a restart.

Test a pack: play until the condition is met and watch for the pop-up. Check `config/SwitchU/daemon.log` if
nothing happens.
