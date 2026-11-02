# Writes docs/achievement-games.md: every game with an achievement pack, and how many it has.
# python tools/list_achievement_games.py
import os

ROOT = os.path.join(os.path.dirname(os.path.abspath(__file__)), "..")
PACKS = os.path.join(ROOT, "projects", "achievements", "packs")
rows = []
for f in os.listdir(PACKS):
    name, count = f[:16], 0
    for line in open(os.path.join(PACKS, f), encoding="utf-8"):
        if line.startswith("game "):
            parts = line.strip().split(" ", 2)
            name = parts[2] if len(parts) > 2 else parts[1]
        elif line.startswith("a "):
            count += 1
    rows.append((name.strip(), f[:16], count))
rows.sort(key=lambda r: r[0].lower())
md = ["# Games with achievements", "",
      f"{len(rows)} games have their own achievements: the packs in `projects/achievements/packs`, made by",
      "`tools/make_mario_packs.py` and `tools/make_all_packs.py` from switch-cheats-db. Every game also has the",
      "play-time milestones. None of these have been checked on a console yet.", "",
      "| Game | Title ID | Achievements |", "|---|---|---|"]
md += [f"| {n.replace('|', '/')} | {t} | {k} |" for n, t, k in rows]
open(os.path.join(ROOT, "docs", "achievement-games.md"), "w", encoding="utf-8", newline="\n").write("\n".join(md) + "\n")
print(len(rows), "games")
