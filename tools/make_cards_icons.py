"""CardsNX: the white glyphs its buttons draw (projects/cards/romfs/icons) and
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


def power():
    im = canvas(); d = ImageDraw.Draw(im)
    d.arc([q(20), q(22), q(76), q(78)], 300, 600 - 360 + 360 - 120 + 60, fill=W, width=q(7))
    d.arc([q(20), q(22), q(76), q(78)], -60, 240, fill=W, width=q(7))
    d.rounded_rectangle([q(44), q(10), q(52), q(50)], radius=q(4), fill=W)
    return im


# The Nintendo page's tiles: coloured squares with a white drawing, 256 px.
T = 256 * 4


def tile(top, bottom):
    im = Image.new("RGBA", (T, T), (0, 0, 0, 0))
    grad = Image.new("RGBA", (T, T))
    gd = ImageDraw.Draw(grad)
    for y in range(T):
        t = y / (T - 1)
        gd.line([(0, y), (T, y)], fill=tuple(int(a + (b - a) * t) for a, b in zip(top, bottom)) + (255,))
    mask = Image.new("L", (T, T), 0)
    ImageDraw.Draw(mask).rounded_rectangle([0, 0, T - 1, T - 1], radius=T // 5, fill=255)
    im.paste(grad, (0, 0), mask)
    return im


def u(v):
    return int(v * T / 256)


def sys_eshop():
    im = tile((255, 140, 60), (230, 80, 30)); d = ImageDraw.Draw(im)
    d.rounded_rectangle([u(60), u(96), u(196), u(206)], radius=u(18), fill=W)          # the bag
    d.arc([u(92), u(52), u(164), u(130)], 180, 360, fill=W, width=u(14))              # its handle
    d.ellipse([u(108), u(132), u(148), u(172)], fill=(240, 105, 40))
    return im


def sys_nso():
    im = tile((240, 70, 80), (190, 25, 45)); d = ImageDraw.Draw(im)
    d.rounded_rectangle([u(50), u(92), u(206), u(172)], radius=u(40), fill=W)          # a pad
    c = (210, 40, 60)
    d.rectangle([u(78), u(124), u(110), u(138)], fill=c)
    d.rectangle([u(87), u(115), u(101), u(147)], fill=c)
    d.ellipse([u(150), u(112), u(166), u(128)], fill=c)
    d.ellipse([u(170), u(130), u(186), u(146)], fill=c)
    return im


def sys_album():
    im = tile((80, 170, 255), (40, 110, 220)); d = ImageDraw.Draw(im)
    d.rounded_rectangle([u(58), u(70), u(198), u(186)], radius=u(16), outline=W, width=u(12))
    d.polygon([(u(72), u(172)), (u(118), u(118)), (u(150), u(150)), (u(166), u(134)), (u(186), u(172))], fill=W)
    d.ellipse([u(144), u(88), u(170), u(114)], fill=W)
    return im


def sys_mii():
    im = tile((120, 210, 90), (60, 160, 60)); d = ImageDraw.Draw(im)
    d.ellipse([u(84), u(56), u(172), u(150)], fill=W)                                 # a head
    d.chord([u(56), u(150), u(200), u(270)], 180, 360, fill=W)                        # shoulders
    c = (80, 170, 70)
    d.ellipse([u(106), u(92), u(118), u(108)], fill=c)
    d.ellipse([u(138), u(92), u(150), u(108)], fill=c)
    d.arc([u(108), u(104), u(148), u(132)], 20, 160, fill=c, width=u(6))
    return im


def sys_friends():
    im = tile((255, 170, 70), (240, 120, 40)); d = ImageDraw.Draw(im)
    d.ellipse([u(132), u(70), u(184), u(122)], fill=(255, 255, 255, 190))
    d.chord([u(112), u(130), u(208), u(226)], 180, 360, fill=(255, 255, 255, 190))
    d.ellipse([u(70), u(64), u(130), u(124)], fill=W)
    d.chord([u(46), u(134), u(156), u(244)], 180, 360, fill=W)
    return im


def sys_controllers():
    im = tile((110, 120, 140), (70, 78, 96)); d = ImageDraw.Draw(im)
    d.rounded_rectangle([u(66), u(56), u(122), u(200)], radius=u(26), fill=(70, 200, 255))   # left
    d.rounded_rectangle([u(134), u(56), u(190), u(200)], radius=u(26), fill=(255, 90, 90))   # right
    d.ellipse([u(84), u(86), u(104), u(106)], fill=W)
    d.ellipse([u(152), u(138), u(172), u(158)], fill=W)
    return im


def sys_browser():
    im = tile((70, 200, 220), (30, 140, 190)); d = ImageDraw.Draw(im)
    d.ellipse([u(56), u(56), u(200), u(200)], outline=W, width=u(12))
    d.ellipse([u(100), u(56), u(156), u(200)], outline=W, width=u(10))
    d.line([u(56), u(128), u(200), u(128)], fill=W, width=u(10))
    d.line([u(68), u(92), u(188), u(92)], fill=W, width=u(8))
    d.line([u(68), u(164), u(188), u(164)], fill=W, width=u(8))
    return im


def sys_settings():
    im = tile((150, 120, 255), (100, 70, 220)); d = ImageDraw.Draw(im)
    c, ro, ri = u(128), u(78), u(58)
    pts = []
    for i in range(8 * 4):
        a = (i / 32) * 2 * math.pi
        r = ro if (i % 4) in (1, 2) else ri
        pts.append((c + r * math.cos(a), c + r * math.sin(a)))
    d.polygon(pts, fill=W)
    d.ellipse([c - u(26), c - u(26), c + u(26), c + u(26)], fill=(125, 95, 240))
    return im


def save_tile(im, name):
    os.makedirs(os.path.join(ROOT, "romfs", "icons"), exist_ok=True)
    im.resize((256, 256), Image.LANCZOS).save(os.path.join(ROOT, "romfs", "icons", "sys_" + name + ".png"))


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
                     ("thermo", thermo), ("tag", tag), ("play", play), ("copy", copy), ("home", home),
                     ("power", power)]:
        save(fn(), name)
    for name, fn in [("eshop", sys_eshop), ("nso", sys_nso), ("album", sys_album), ("mii", sys_mii),
                     ("friends", sys_friends), ("controllers", sys_controllers), ("browser", sys_browser),
                     ("settings", sys_settings)]:
        save_tile(fn(), name)
    app_icon()
