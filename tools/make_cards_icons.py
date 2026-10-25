"""HomeNX Cards: the white glyphs its buttons draw (projects/cards/romfs/icons) and
its app icon (projects/cards/icon.jpg). Drawn 4x and scaled down, so the edges
are smooth.

    python tools/make_cards_icons.py
"""
import math
import os

from PIL import Image, ImageDraw

ROOT = os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "projects", "cards")
S = 96 * 4  # drawn at 4x
W = "white"


def canvas():
    return Image.new("RGBA", (S, S), (0, 0, 0, 0))


def q(v):
    return int(v * S / 96)


def save(im, name):
    os.makedirs(os.path.join(ROOT, "romfs", "icons"), exist_ok=True)
    im.resize((96, 96), Image.LANCZOS).save(os.path.join(ROOT, "romfs", "icons", name + ".png"))


def dots():
    im = canvas(); d = ImageDraw.Draw(im)
    for x in (30, 48, 66):
        d.ellipse([q(x - 5), q(43), q(x + 5), q(53)], fill=W)
    return im


def sort():
    im = canvas(); d = ImageDraw.Draw(im)
    w = q(5)
    d.line([q(36), q(24), q(36), q(72)], fill=W, width=w)          # up arrow
    d.line([q(24), q(36), q(36), q(24), q(48), q(36)], fill=W, width=w, joint="curve")
    d.line([q(60), q(24), q(60), q(72)], fill=W, width=w)          # down arrow
    d.line([q(48), q(60), q(60), q(72), q(72), q(60)], fill=W, width=w, joint="curve")
    for x, y in ((36, 24), (60, 72)):
        d.ellipse([q(x) - w // 2, q(y) - w // 2, q(x) + w // 2, q(y) + w // 2], fill=W)
    return im


def friends():
    im = canvas(); d = ImageDraw.Draw(im)
    d.ellipse([q(52), q(26), q(70), q(44)], fill=(255, 255, 255, 170))      # the one behind
    d.chord([q(44), q(48), q(80), q(84)], 180, 360, fill=(255, 255, 255, 170))
    d.ellipse([q(24), q(24), q(46), q(46)], fill=W)
    d.chord([q(14), q(50), q(56), q(92)], 180, 360, fill=W)
    return im


def bell():
    im = canvas(); d = ImageDraw.Draw(im)
    d.ellipse([q(44), q(14), q(52), q(22)], fill=W)
    d.pieslice([q(26), q(20), q(70), q(64)], 180, 360, fill=W)
    d.rectangle([q(26), q(41), q(70), q(64)], fill=W)
    d.polygon([(q(26), q(60)), (q(70), q(60)), (q(80), q(72)), (q(16), q(72))], fill=W)
    d.rounded_rectangle([q(14), q(68), q(82), q(76)], radius=q(4), fill=W)
    d.chord([q(39), q(70), q(57), q(88)], 0, 180, fill=W)
    return im


def gear():
    im = canvas(); d = ImageDraw.Draw(im)
    c, r_out, r_in = q(48), q(34), q(26)
    pts = []
    teeth = 8
    for i in range(teeth * 4):
        a = (i / (teeth * 4)) * 2 * math.pi
        r = r_out if (i % 4) in (1, 2) else r_in
        pts.append((c + r * math.cos(a), c + r * math.sin(a)))
    d.polygon(pts, fill=W)
    d.ellipse([c - q(13), c - q(13), c + q(13), c + q(13)], fill=(0, 0, 0, 0))
    return im


def gear_outline():
    # the outlined gear in the reference
    im = gear()
    hole = canvas(); ImageDraw.Draw(hole).ellipse([q(48 - 20), q(48 - 20), q(48 + 20), q(48 + 20)], fill=W)
    inner = canvas(); ImageDraw.Draw(inner).ellipse([q(48 - 13), q(48 - 13), q(48 + 13), q(48 + 13)], fill=W)
    # gear minus a ring: draw the ring area clear, then the inner ring back
    out = Image.new("RGBA", im.size, (0, 0, 0, 0))
    out.paste(im, (0, 0), im)
    d = ImageDraw.Draw(out)
    d.ellipse([q(48 - 22), q(48 - 22), q(48 + 22), q(48 + 22)], fill=(0, 0, 0, 0))
    d.ellipse([q(48 - 13), q(48 - 13), q(48 + 13), q(48 + 13)], outline=W, width=q(5))
    return out


def thermo():
    im = canvas(); d = ImageDraw.Draw(im)
    d.rounded_rectangle([q(40), q(12), q(56), q(64)], radius=q(8), outline=W, width=q(4))
    d.ellipse([q(32), q(54), q(64), q(86)], fill=W)
    d.rectangle([q(45), q(36), q(51), q(62)], fill=W)
    for y in (24, 34, 44):
        d.line([q(58), q(y), q(66), q(y)], fill=W, width=q(3))
    return im


def tag():
    im = canvas(); d = ImageDraw.Draw(im)
    d.polygon([(q(18), q(48)), (q(48), q(18)), (q(78), q(18)), (q(78), q(48)), (q(48), q(78))], fill=W)
    d.ellipse([q(60), q(28), q(70), q(38)], fill=(0, 0, 0, 0))
    return im


def play():
    im = canvas(); d = ImageDraw.Draw(im)
    d.polygon([(q(26), q(16)), (q(80), q(48)), (q(26), q(80))], fill=W)
    return im


def copy():
    im = canvas(); d = ImageDraw.Draw(im)
    d.rounded_rectangle([q(34), q(18), q(78), q(62)], radius=q(9), outline=W, width=q(6))
    d.rounded_rectangle([q(18), q(34), q(62), q(78)], radius=q(9), fill=(0, 0, 0, 0))
    d.rounded_rectangle([q(18), q(34), q(62), q(78)], radius=q(9), outline=W, width=q(6))
    return im


def home():
    im = canvas(); d = ImageDraw.Draw(im)
    w = q(6)
    d.line([(q(14), q(46)), (q(48), q(16)), (q(82), q(46))], fill=W, width=w, joint="curve")
    d.rounded_rectangle([q(24), q(40), q(72), q(82)], radius=q(6), outline=W, width=w)
    d.rectangle([q(28), q(42), q(68), q(48)], fill=(0, 0, 0, 0))
    d.rounded_rectangle([q(40), q(56), q(56), q(82)], radius=q(3), outline=W, width=q(5))
    d.ellipse([q(14) - w // 2, q(46) - w // 2, q(14) + w // 2, q(46) + w // 2], fill=W)
    d.ellipse([q(82) - w // 2, q(46) - w // 2, q(82) + w // 2, q(46) + w // 2], fill=W)
    return im


def app_icon():
    # three box-art cards fanned out on a dusky blue tile
    n = 1024
    im = Image.new("RGB", (n, n), (34, 40, 74))
    top = Image.new("RGB", (n, n // 2), (64, 76, 140))
    mask = Image.linear_gradient("L").resize((n, n // 2)).transpose(Image.FLIP_TOP_BOTTOM)
    im.paste(top, (0, 0), mask)
    for angle, dx, col in ((14, 150, (255, 140, 110)), (-14, -150, (110, 200, 255)), (0, 0, (255, 255, 255))):
        card = Image.new("RGBA", (360, 540), (0, 0, 0, 0))
        cd = ImageDraw.Draw(card)
        cd.rounded_rectangle([0, 0, 359, 539], radius=48, fill=col + (255,))
        if col == (255, 255, 255):
            cd.rounded_rectangle([0, 0, 359, 539], radius=48, outline=(255, 255, 255, 255), width=14)
            cd.rounded_rectangle([28, 28, 331, 511], radius=30, fill=(90, 110, 220, 255))
            cd.polygon([(140, 200), (240, 270), (140, 340)], fill=(255, 255, 255, 255))
        card = card.rotate(angle, resample=Image.BICUBIC, expand=True)
        im.paste(card, ((n - card.width) // 2 + dx, (n - card.height) // 2 + 30), card)
    im.resize((256, 256), Image.LANCZOS).save(os.path.join(ROOT, "icon.jpg"), quality=95)


if __name__ == "__main__":
    for name, fn in [("dots", dots), ("sort", sort), ("friends", friends), ("bell", bell), ("gear", gear_outline),
                     ("thermo", thermo), ("tag", tag), ("play", play), ("copy", copy), ("home", home)]:
        save(fn(), name)
    app_icon()
