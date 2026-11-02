# Writes HomeNX achievement packs for the Mario games from HamletDuFromage/switch-cheats-db:
# each achievement watches the value one cheat changes (every version and save slot the
# database has a plain code for: any slot unlocks it).
# Usage: python tools/make_mario_packs.py <a clone of github.com/HamletDuFromage/switch-cheats-db>
import json, os, re, sys
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from cheat_decode import decode, slotless

HERE = os.path.dirname(os.path.abspath(__file__))
DB = os.path.join(sys.argv[1] if len(sys.argv) > 1 else "switch-cheats-db", "cheats")
OUT = os.path.join(HERE, "..", "projects", "achievements", "packs")

# (cheat names (any of them, slot number dropped), which of its writes, id, name, what to do, op, number)
SPEC = {
    "010036B0034E4000": ("Super Mario Party", [
        (["1 st p 999 coins mario party"], 0, "board.coins100", "Coin pile", "Hold 100 coins in a Mario Party board game", ">=", 100),
        (["999 coins partner party"], 0, "partner.coins100", "Team treasury", "Hold 100 coins in Partner Party", ">=", 100),
    ]),
    "01006FE013472000": ("Mario Party Superstars", [
        (["Inf. Coins"], 0, "coins.1k", "Pocket money", "Have 1,000 coins", ">=", 1000),
        (["Inf. Coins"], 0, "coins.10k", "Party saver", "Have 10,000 coins", ">=", 10000),
        (["Inf. Coins"], 0, "coins.50k", "Superstar savings", "Have 50,000 coins", ">=", 50000),
    ]),
    "010028600EBDA000": ("Super Mario 3D World + Bowser's Fury", [
        (["100 Cat Shines"], 0, "fury.shines10", "Cat's whiskers", "Collect 10 Cat Shines in Bowser's Fury", ">=", 10),
        (["100 Cat Shines"], 0, "fury.shines50", "Lake Lapcat regular", "Collect 50 Cat Shines in Bowser's Fury", ">=", 50),
        (["100 Cat Shines"], 0, "fury.shines100", "Fury tamed", "Collect all 100 Cat Shines in Bowser's Fury", ">=", 100),
        (["02# 999 Lives"], 0, "lives.99", "1-Up hoarder", "Have 99 lives", ">=", 99),
        (["02# 999 Lives"], 0, "lives.500", "Cat with 500 lives", "Have 500 lives", ">=", 500),
    ]),
    "0100EA80032EA000": ("New Super Mario Bros. U Deluxe", [
        (["Start with 99 Lives"], 0, "lives.99", "1-Up hoarder", "Have 99 lives", ">=", 99),
    ]),
    "01009B90006DC000": ("Super Mario Maker 2", [
        (["02# 999 coins all modes"], 0, "coins.999", "Coin hoard", "Have 999 coins", ">=", 999),
    ]),
    "010049900F546000": ("Super Mario 3D All-Stars", [
        (["o Super Mario 64 Infinite Coins"], 0, "sm64.coins100", "100 coins", "Collect 100 coins in a course in Super Mario 64", ">=", 100),
        (["o Super Mario 64 Infinite Lives"], 0, "sm64.lives99", "Extra Marios", "Have 99 lives in Super Mario 64", ">=", 99),
        (["Star"], 0, "galaxy.bits999", "Star Bit hoard", "Have 999 Star Bits in Super Mario Galaxy", ">=", 999),
    ]),
    "0100A3900C3E2000": ("Paper Mario: The Origami King", [
        (["02# max Coins", "max Coins"], 0, "coins.10k", "Paper money", "Have 10,000 coins", ">=", 10000),
        (["02# max Coins", "max Coins"], 0, "coins.50k", "Folded fortune", "Have 50,000 coins", ">=", 50000),
        (["02# max Coins", "max Coins"], 0, "coins.99999", "Origami millionaire", "Have 99,999 coins", ">=", 99999),
    ]),
    "0100ECD018EBE000": ("Paper Mario: The Thousand-Year Door", [
        (["coins 9999"], 0, "coins.999", "Full wallet", "Have 999 coins", ">=", 999),
        (["star pieces 10"], 0, "starpieces.30", "Star Piece pocket", "Have 30 Star Pieces at once", ">=", 30),
        (["shine sprites 10"], 0, "shines.10", "Shine on", "Have 10 Shine Sprites at once", ">=", 10),
    ]),
    "010067300059A000": ("Mario + Rabbids Kingdom Battle", [
        (["17# 99999 Coins and bananas"], 0, "coins.10k", "Coin raider", "Have 10,000 coins", ">=", 10000),
        (["17# 99999 Coins and bananas"], 0, "coins.50k", "Rabbid riches", "Have 50,000 coins", ">=", 50000),
    ]),
    "010019401051C000": ("Mario Strikers: Battle League", [
        (["Inf. Coins"], 0, "coins.10k", "Club funds", "Have 10,000 coins", ">=", 10000),
        (["Inf. Coins"], 0, "coins.100k", "Big transfer budget", "Have 100,000 coins", ">=", 100000),
    ]),
    "0100DCA0064A6000": ("Luigi's Mansion 3", [
        (["Money MAX", "01# 999,999 Money", "999999 Money", "07# 999999 Money"], 0, "money.100k", "Treasure hunter",
         "Have 100,000 in treasure", ">=", 100000),
        (["Money MAX", "01# 999,999 Money", "999999 Money", "07# 999999 Money"], 0, "money.500k", "Last Resort's richest guest",
         "Have 500,000 in treasure", ">=", 500000),
    ]),
}

# Super Mario Odyssey: purple coins per kingdom (each slot's 13 writes, in the game's kingdom
# order) and coins; total coins, jumps and Cappy throws follow the coins in the same record
# (+4, +8, +C: as EnterpriseFreak/nsw-atmos-cheats' codes have them).
KINGDOMS = [("cap", "Cap", 50), ("cascade", "Cascade", 50), ("sand", "Sand", 100), ("wooded", "Wooded", 100),
            ("lake", "Lake", 50), ("lost", "Lost", 50), ("metro", "Metro", 100), ("seaside", "Seaside", 100),
            ("snow", "Snow", 50), ("luncheon", "Luncheon", 100), ("bowser", "Bowser's", 100), ("moon", "Moon", 50),
            ("mushroom", "Mushroom", 100)]
SMO_EXTRA = [  # (id, name, about, offset from the coins, bytes, number)
    ("coins.hoard", "Piggy bank", "Have 9,999 coins at once", 0x0, 2, 9999),
    ("coins.10k", "Coin collector", "Collect 10,000 coins in all", 0x4, 4, 10000),
    ("coins.50k", "Coin tycoon", "Collect 50,000 coins in all", 0x4, 4, 50000),
    ("jumps.10k", "Spring in your step", "Jump 10,000 times", 0x8, 4, 10000),
    ("jumps.50k", "Moon bounce", "Jump 50,000 times", 0x8, 4, 50000),
    ("cappy.5k", "Hat trick", "Throw Cappy 5,000 times", 0xC, 4, 5000),
    ("cappy.25k", "Cap master", "Throw Cappy 25,000 times", 0xC, 4, 25000),
]

HEADER = ("# {game}: its own achievements (docs/achievement-packs.md). Made from the cheats in\n"
          "# HamletDuFromage/switch-cheats-db (by their authors): each one watches the value a cheat changes,\n"
          "# for every version and save slot with a plain code (any slot unlocks it). Not checked on a console.\n")


def load(tid):
    return json.load(open(os.path.join(DB, f"{tid}.json"), encoding="utf-8"))


def writes_by_build(d, names):
    """build -> list of decoded write lists (one per matching cheat: a slot each)."""
    want = {n.lower() for n in names}
    out = {}
    for build, cheats in d.items():
        if build == "attribution":
            continue
        for name, code in cheats.items():
            if slotless(name).lower() in want:
                w, _ = decode(code)
                if w:
                    out.setdefault(build.upper(), []).append(w)
    return out


def cond(width, address, op, number):
    return f"u{8 * width} {address} {op} {number}"


def write_pack(tid, game, achievements, builds):
    lines = [HEADER.format(game=game), f"game {tid} {game}", ""]
    lines += [f"a {i} | {n} | {about}" for i, n, about in achievements]
    for build in sorted(builds):
        cs = builds[build]
        if not cs:
            continue
        lines += ["", f"build {build}"] + [f"c {i} | {c}" for i, c in cs]
    open(os.path.join(OUT, f"{tid}.txt"), "w", encoding="utf-8", newline="\n").write("\n".join(lines) + "\n")
    return sum(len(v) for v in builds.values())


def main():
    sys.stdout.reconfigure(encoding="utf-8")
    os.makedirs(OUT, exist_ok=True)
    for tid, (game, items) in SPEC.items():
        d = load(tid)
        achievements, builds = [], {}
        for names, index, aid, name, about, op, number in items:
            got = writes_by_build(d, names)
            if not got:
                print(f"!! {game}: no plain code for {names}")
                continue
            achievements.append((aid, name, about))
            for build, slots in got.items():
                for w in slots:
                    if index < len(w):
                        width, address, _ = w[index]
                        builds.setdefault(build, []).append((aid, cond(width, address, op, number)))
        n = write_pack(tid, game, achievements, builds)
        print(f"{tid} {game}: {len(achievements)} achievements, {len(builds)} versions, {n} conditions")

    # Super Mario Odyssey
    tid, game = "0100000000010000", "Super Mario Odyssey"
    d = load(tid)
    purple = writes_by_build(d, ["Max Purple Coins"])
    coins = writes_by_build(d, ["9999 Coins"])
    achievements = [(f"purple.{k}", f"{name} collector", f"Find all {n} purple coins in the {name} Kingdom"
                     if not name.endswith("'s") else f"Find all {n} purple coins in {name} Kingdom") for k, name, n in KINGDOMS]
    achievements += [(i, n, about) for i, n, about, *_ in SMO_EXTRA]
    builds = {}
    for build in sorted(set(purple) | set(coins)):
        cs = []
        for w in purple.get(build, []):
            if len(w) != len(KINGDOMS) or [x[2] for x in w] != [n for _, _, n in KINGDOMS]:
                print(f"!! {game} {build}: purple coins don't match the kingdoms: {[x[2] for x in w]}")
                continue
            cs += [(f"purple.{k}", cond(x[0], x[1], ">=", n)) for (k, _, n), x in zip(KINGDOMS, w)]
        for w in coins.get(build, []):
            m = re.fullmatch(r"heap\+([0-9A-F]+)", w[0][1])
            if not m:
                continue
            base = int(m.group(1), 16)
            cs += [(i, cond(width, f"heap+{base + off:X}", ">=", number)) for i, _, _, off, width, number in SMO_EXTRA]
        builds[build] = cs
    n = write_pack(tid, game, achievements, builds)
    print(f"{tid} {game}: {len(achievements)} achievements, {len(builds)} versions, {n} conditions")


main()
