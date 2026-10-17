"""Turns romfs/icons/discord_mark.png (Discord's mark, white) into the Discord
overlay's DM button: a small 4-bit alpha mask, one hex digit a pixel, in
projects/overlay/source/discord_mark.inc. Run from the repo root."""
from PIL import Image

W = 28
src = Image.open("romfs/icons/discord_mark.png").convert("RGBA")
alpha = src.getchannel("A").crop(src.getchannel("A").getbbox())
H = round(W * alpha.height / alpha.width)
small = alpha.resize((W, H), Image.LANCZOS)
digits = "".join("%X" % min(15, round(small.getpixel((x, y)) / 17)) for y in range(H) for x in range(W))
rows = [digits[i:i + W] for i in range(0, len(digits), W)]
with open("projects/overlay/source/discord_mark.inc", "w", newline="\n") as f:
    f.write("// Discord's mark for the DM button, 4-bit alpha a pixel (tools/make_overlay_mark.py).\n")
    f.write("constexpr int kMarkW = %d, kMarkH = %d;\n" % (W, H))
    f.write("constexpr const char kMarkAlpha[] =\n")
    for r in rows:
        f.write('    "%s"\n' % r)
    f.write("    ;\n")
print(W, H)
