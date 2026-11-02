# Achievement packs for every game in HamletDuFromage/switch-cheats-db with a plain code for a
# currency (money, gold, Gil...), a level or experience: tiers like "Have 100,000 Gil" or
# "Reach level 50". Hand-made packs (the Mario ones) are left alone. Each condition also has an
# upper bound (ten times the cheat's own value): a pointer read too soon reads nonsense.
# Usage: python tools/make_all_packs.py <a clone of github.com/HamletDuFromage/switch-cheats-db>
import json, os, re, sys
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from cheat_decode import decode, slotless

HERE = os.path.dirname(os.path.abspath(__file__))
DB = sys.argv[1] if len(sys.argv) > 1 else "switch-cheats-db"
OUT = os.path.join(HERE, "..", "projects", "achievements", "packs")
CURATED = {"0100000000010000", "010019401051C000", "010028600EBDA000", "010036B0034E4000", "010049900F546000",
           "010067300059A000", "01006FE013472000", "01009B90006DC000", "0100A3900C3E2000", "0100DCA0064A6000",
           "0100EA80032EA000", "0100ECD018EBE000"}

EXCLUDE = re.compile(
    r"\b(hp|health|mp|magic|ammo|bullets?|time|timer|clock|speed|jump|damage|attack|atk|def|defen[cs]e|stamina|energy|"
    r"fuel|durab\w*|fps|resolution|teleport|position|camera|zoom|cool ?down|gauge|meter|enemy|enemies|opponent|cpu|p2|"
    r"player ?2|multiplier|rate|drops?|x\d+|\d+x|one ?hit|ohko|god|invinc\w*|walk|run|fly|noclip|moon ?jump|shop|price|"
    r"costs?|free|discount|menu|debug|unlock\w*|lock|skip|select|stage|map|always|never|no|max ?stats?|stats?|"
    r"str|strength|vit|int|agi|luck|dex|mag|spd|sp|bp|ap|tp|lp|ep|limit|break|ex|bonus|reward|gain|boost|instant|"
    r"quick|fast|slow|freeze|frozen|lose|loss|steal|enemy's|bet|wager|casino|party ?members?|all ?items|items?)\b",
    re.I)
CURRENCY = re.compile(r"\b(money|gold|gil|munny|cash|rupees?|zenny|credits?|souls|dollars|funds|bells|coins?|gems|"
                      r"crystals|silver|guilders|gald|lucre|yen|medals|tokens|g-?coins|kinstones|berries|bolts|scrap|"
                      r"marks|pesos|euros)\b", re.I)
LEVEL = re.compile(r"\b(level|lv|lvl)\b", re.I)
EXP = re.compile(r"\b(exp|experience|xp)\b", re.I)
DISPLAY = {"gil": "Gil", "munny": "Munny", "gold": "gold", "coin": "coins", "coins": "coins", "rupee": "Rupees",
           "rupees": "Rupees", "zenny": "Zenny", "credit": "Credits", "credits": "Credits", "souls": "Souls",
           "dollars": "dollars", "bells": "Bells", "gems": "Gems", "crystals": "Crystals", "silver": "silver",
           "guilders": "Guilders", "gald": "Gald", "lucre": "Lucre", "yen": "yen", "medals": "Medals", "tokens": "Tokens",
           "kinstones": "Kinstones", "berries": "Berries", "bolts": "Bolts", "scrap": "Scrap", "marks": "Marks",
           "pesos": "pesos", "euros": "euros"}
MONEYISH = {"money", "cash", "funds"}


def nice(n):
    return max(5, int(round(n / 5.0)) * 5)


def tiers(kind, v):
    # (A cheat's value is often far past the game's own cap, so tiers stay well below it.)
    if kind == "level":
        if v >= 99:
            return [25, 50, 99]
        return sorted({nice(v * 0.25), nice(v * 0.5), int(v)}) if v >= 20 else []
    if kind != "currency" or v < 100:
        return []  # (experience scales differ too much from game to game)
    t = [x for x in (100, 1_000, 10_000, 100_000, 1_000_000, 10_000_000) if x <= v / 10][-3:]
    if v <= 9_999 and int(v) not in t:
        t.append(int(v))  # (a small cap like 999 or 9,999 is most likely the game's own: "Maxed out")
    return t


def fmt(n):
    return f"{n:,}"


def achievement(kind, word, tier, index, count, cap):
    w = DISPLAY.get(word, word)
    if kind == "level":
        names = ["Leveling up", "Seasoned", "Max level" if tier == cap else "Veteran"]
        return f"level.{tier}", names[min(index, 2)] if count == 3 else names[min(index + 3 - count, 2)], f"Reach level {tier}"
    if kind == "exp":
        names = ["Learning the ropes", "Experienced", "Master"]
        return f"exp.{tier}", names[min(index + 3 - count, 2)], f"Earn {fmt(tier)} EXP"
    names = ["Saving up", "Well off", "Rolling in it", "Maxed out"]
    name = names[3] if (tier == cap and cap <= 9_999) else names[min(index + 3 - min(count, 3), 2)]
    what = f"Save up {fmt(tier)}" if word in MONEYISH else f"Have {fmt(tier)} {w}"
    return f"{word}.{tier}", name, what


def classify(name):
    n = slotless(name)
    if EXCLUDE.search(n.replace("max money", "money")):
        # (still allow plain "Max Gil" style names: only the words above rule a cheat out)
        pass
    if EXCLUDE.search(n):
        return None
    found = [x.lower() for x in CURRENCY.findall(n)]
    if found:
        word = next((x for x in found if x not in MONEYISH), found[0]) # "Money/GIL": Gil
        word = {"coin": "coins", "rupee": "rupees", "credit": "credits", "g-coins": "coins", "gcoins": "coins"}.get(word, word)
        return ("currency", word)
    if LEVEL.search(n):
        return ("level", "level")
    if EXP.search(n):
        return ("exp", "exp")
    return None


def main():
    sys.stdout.reconfigure(encoding="utf-8")
    versions = json.load(open(os.path.join(DB, "versions.json"), encoding="utf-8"))
    made = 0
    for f in sorted(os.listdir(os.path.join(DB, "cheats"))):
        tid = f[:-5].upper()
        if tid in CURATED or not f.endswith(".json"):
            continue
        d = json.load(open(os.path.join(DB, "cheats", f), encoding="utf-8"))
        # (kind, word) -> build -> [(width, address, value)]
        found = {}
        for build, cheats in d.items():
            if build == "attribution" or not isinstance(cheats, dict):
                continue
            for name, code in cheats.items():
                c = classify(name)
                if not c:
                    continue
                w, _ = decode(code)
                if not w or len(w) > 8:
                    continue
                found.setdefault(c, {}).setdefault(build.upper(), []).extend(w)
        achievements, builds = [], {}
        for (kind, word), byBuild in sorted(found.items()):
            v = max(x[2] for ws in byBuild.values() for x in ws)
            if kind != "level" and v > 2_000_000_000:
                continue  # (nonsense, or an instruction: not a counter)
            ts = tiers(kind, v)
            for i, t in enumerate(ts):
                aid, name, what = achievement(kind, word, t, i, len(ts), int(v))
                if any(a[0] == aid for a in achievements):
                    continue
                achievements.append((aid, name, what))
                for build, ws in byBuild.items():
                    for width, address, value in ws:
                        if width not in (1, 2, 4, 8) or (width < 4 and t > (1 << (8 * width)) - 1):
                            continue
                        top = min(max(value, t) * 10, (1 << (8 * width)) - 1)
                        line = f"u{8 * width} {address} >= {t} <= {top}"
                        if (aid, line) not in builds.setdefault(build, []):
                            builds[build].append((aid, line))
        if not achievements or not any(builds.values()):
            continue
        title = re.sub(r"[\u2122\u00ae\u00a9]", "", versions.get(tid, versions.get(tid.lower(), {})).get("title", "")).strip()
        lines = [f"# {title or tid}: achievements made from HamletDuFromage/switch-cheats-db's codes for its",
                 "# currency, level or experience (tools/make_all_packs.py). Not checked on a console.",
                 f"game {tid} {title}".rstrip(), ""]
        used = {a for b in builds.values() for a, _ in b}
        lines += [f"a {a} | {n} | {w}" for a, n, w in achievements if a in used]
        for build in sorted(builds):
            if builds[build]:
                lines += ["", f"build {build}"] + [f"c {a} | {c}" for a, c in builds[build]]
        open(os.path.join(OUT, f"{tid}.txt"), "w", encoding="utf-8", newline="\n").write("\n".join(lines) + "\n")
        made += 1
    print("packs made:", made)


main()
