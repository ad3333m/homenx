"""Checks projects/cards/romfs/lang.tsv: every row has each language, a
translation keeps the English's {placeholders}, none reads like another row's
English (it would be translated twice), and every English is still in the code.
python tools/check_cards_lang.py"""
import pathlib, re, sys

root = pathlib.Path(__file__).resolve().parent.parent
src = "".join(p.read_text(encoding="utf-8") for p in (root / "projects/cards/src").glob("*.cpp"))
lines = [l for l in (root / "projects/cards/romfs/lang.tsv").read_text(encoding="utf-8").splitlines() if l and l[0] != "#"]
langs, rows = lines[0].split("\t"), [l.split("\t") for l in lines[1:]]
keys, ph, problems = {r[0] for r in rows}, re.compile(r"\{(\w*)\}"), []
for r in rows:
    if len(r) != len(langs): problems.append(f"{len(r)} columns: {r[0]}")
    if '"' + r[0].replace('"', '\\"') + '"' not in src: problems.append(f"not in the code any more: {r[0]}")
    for lang, t in zip(langs[1:], r[1:]):
        if t and sorted(ph.findall(t)) != sorted(ph.findall(r[0])): problems.append(f"{lang} placeholders: {r[0]}")
        if t and t != r[0] and t in keys: problems.append(f"{lang} reads like another row: {r[0]} -> {t}")
print("\n".join(problems) or f"ok: {len(rows)} rows, {len(langs) - 1} languages")
sys.exit(1 if problems else 0)
