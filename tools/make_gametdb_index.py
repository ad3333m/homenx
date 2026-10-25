"""HomeNX Cards: GameTDB's Switch box-art IDs by game name (gametdb.com, made for
homebrew launchers), from switchtdb.zip. CI runs this before building; the app
matches each game's name the same way (cards::nameKey in Library.cpp) and gets
its cover from art.gametdb.com.

    python3 tools/make_gametdb_index.py build/cards-romfs/gametdb.tsv

Each line: name key, tab, GameTDB id, tab, region (U, E, J or A for all).
"""
import io
import sys
import urllib.request
import xml.etree.ElementTree as ET
import zipfile

# U+00C0..U+017F to their base letter (" " for none). Library.cpp has the same.
FOLD = ("aaaaaa ceeeeiiii nooooo  uuuuy  aaaaaa ceeeeiiii nooooo  uuuuy yaaaaaaccccccccdd  eeeeeeeeeegggggggghh  "
        "iiiiiiiii iijjkk llllllll  nnnnnnn  oooooo  rrrrrrsssssssstttt  uuuuuuuuuuuuwwyyyzzzzzzs")
REGIONS = {"NTSC-U": "U", "PAL": "E", "NTSC-J": "J", "ALL": "A"}


def name_key(name):
    out = []
    for ch in name:
        cp = ord(ch)
        if cp < 128 and ch.isalnum():
            out.append(ch.lower())
        elif 0xC0 <= cp < 0x180:
            out.append(FOLD[cp - 0xC0])
        else:
            out.append(" ")
    return " ".join("".join(out).split())


def main():
    dest = sys.argv[1] if len(sys.argv) > 1 else "gametdb.tsv"
    req = urllib.request.Request("https://www.gametdb.com/switchtdb.zip", headers={"User-Agent": "HomeNX-Cards"})
    data = urllib.request.urlopen(req, timeout=120).read()
    xml = zipfile.ZipFile(io.BytesIO(data)).read("switchtdb.xml")
    best = {}  # key -> (rank, id, region): one cover per name, all-region and US first
    rank_of = {"A": 0, "U": 1, "E": 2, "J": 3}
    for _, game in ET.iterparse(io.BytesIO(xml)):
        if game.tag != "game":
            continue
        gid = (game.findtext("id") or "").strip()
        region = REGIONS.get((game.findtext("region") or "").strip(), "A")
        titles = [loc.findtext("title") for loc in game.findall("locale") if loc.get("lang") == "EN"]
        titles += [loc.findtext("title") for loc in game.findall("locale") if loc.get("lang") != "EN"][:1]
        for title in filter(None, titles):
            key = name_key(title)
            if key and gid:
                cand = (rank_of[region], gid, region)
                if key not in best or cand < best[key]:
                    best[key] = cand
        game.clear()
    with open(dest, "w", encoding="utf-8", newline="\n") as f:
        for key in sorted(best):
            _, gid, region = best[key]
            f.write(f"{key}\t{gid}\t{region}\n")
    print(f"{len(best)} names -> {dest}")


if __name__ == "__main__":
    main()
