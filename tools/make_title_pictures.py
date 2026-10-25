"""CardsNX's title pictures: Microsoft's Fluent 3D emoji (MIT) that
projects/cards/src/Library.cpp's titlePicture() names, 96 px with no background,
into projects/cards/romfs/titles/. Run it after adding a picture to that table."""
import io
import os
import re
import urllib.parse
import urllib.request

from PIL import Image

ROOT = os.path.join(os.path.dirname(__file__), "..")
OUT = os.path.join(ROOT, "projects", "cards", "romfs", "titles")
BASE = "https://raw.githubusercontent.com/microsoft/fluentui-emoji/main/assets/"

src = open(os.path.join(ROOT, "projects", "cards", "src", "Library.cpp"), encoding="utf-8").read()
table = src[src.index("// TITLE PICTURES"):]
table = table[:table.index("};")]
names = sorted(set(re.findall(r'\{"[^"]+", "([a-z0-9_-]+)"\}', table)))

os.makedirs(OUT, exist_ok=True)
for name in names:
    folder = name.replace("_", " ").capitalize()  # racing_car -> "Racing car"
    url = BASE + urllib.parse.quote(f"{folder}/3D/{name}_3d.png")
    data = urllib.request.urlopen(url, timeout=30).read()
    im = Image.open(io.BytesIO(data)).convert("RGBA").resize((96, 96), Image.LANCZOS)
    im.save(os.path.join(OUT, name + ".png"), optimize=True)
print(len(names), "pictures")
