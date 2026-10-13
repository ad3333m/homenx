"""Draws the HomeNX sidebar icons (eshop/news/browser/friends) and icon.jpg.
Run from the repo root: python tools/make_icons.py"""
from PIL import Image, ImageDraw, ImageFont, ImageFilter

S = 4  # supersample
N = 512 * S
BOLD = "C:/Windows/Fonts/arialbd.ttf"


def canvas():
    return Image.new("RGBA", (N, N), (0, 0, 0, 0))


def save(im, path, size=256):  # sidebar draws at 128; bigger PNGs only slow every HOME return
    im.resize((size, size), Image.LANCZOS).save(path)


def p(v):  # 512-space -> supersampled
    return int(v * S)


def eshop():
    im = canvas(); d = ImageDraw.Draw(im)
    d.rounded_rectangle([p(150), p(70), p(362), p(260)], radius=p(90), outline=(200, 80, 0), width=p(30))
    d.rounded_rectangle([p(70), p(170), p(442), p(470)], radius=p(60), fill=(250, 124, 0))
    d.rounded_rectangle([p(70), p(170), p(442), p(250)], radius=p(60), fill=(255, 150, 40))
    d.rectangle([p(70), p(215), p(442), p(250)], fill=(250, 124, 0))
    f = ImageFont.truetype(BOLD, p(230))
    d.text((p(256), p(330)), "e", font=f, fill="white", anchor="mm")
    return im


def news():
    im = canvas(); d = ImageDraw.Draw(im)
    d.rounded_rectangle([p(60), p(60), p(452), p(452)], radius=p(70), fill=(230, 0, 18))
    d.rounded_rectangle([p(115), p(115), p(397), p(215)], radius=p(18), fill="white")
    for i, w in enumerate([282, 282, 200, 282, 160]):
        y = 250 + i * 40
        d.rounded_rectangle([p(115), p(y), p(115 + w), p(y + 20)], radius=p(10), fill=(255, 210, 214))
    return im


def homebrew():
    # violet rounded tile with a white 2x2 "apps" grid and a sparkle
    im = canvas(); d = ImageDraw.Draw(im)
    d.rounded_rectangle([p(56), p(56), p(456), p(456)], radius=p(96), fill=(112, 72, 230))
    d.rounded_rectangle([p(56), p(56), p(456), p(256)], radius=p(96), fill=(132, 96, 245))
    d.rectangle([p(56), p(200), p(456), p(256)], fill=(132, 96, 245))
    for gx in (0, 1):
        for gy in (0, 1):
            x, y = 140 + gx * 128, 140 + gy * 128
            d.rounded_rectangle([p(x), p(y), p(x + 104), p(y + 104)], radius=p(26), fill="white")
    d.polygon([(p(392), p(92)), (p(404), p(122)), (p(434), p(134)), (p(404), p(146)),
               (p(392), p(176)), (p(380), p(146)), (p(350), p(134)), (p(380), p(122))], fill=(255, 226, 120))
    return im


def friends():
    im = canvas(); d = ImageDraw.Draw(im)
    def person(cx, top, col, scale):
        hr = 62 * scale
        d.ellipse([p(cx - hr), p(top), p(cx + hr), p(top + 2 * hr)], fill=col)
        bw, by = 120 * scale, top + 2 * hr + 18 * scale
        d.rounded_rectangle([p(cx - bw), p(by), p(cx + bw), p(by + 190 * scale)], radius=p(95 * scale), fill=col)
    person(330, 80, (0, 190, 170), 0.95)
    person(200, 130, (255, 180, 0), 1.05)
    return im


# Discord's logo (the "Clyde" mark) as Simple Icons publishes it (CC0 path data,
# 24x24 viewBox). Discord's trademark; used to label the Discord panel.
DISCORD_PATH = ("M20.317 4.3698a19.7913 19.7913 0 00-4.8851-1.5152.0741.0741 0 00-.0785.0371c-.211.3753-.4447.8648-.6083 1.2495-1.8447-.2762-3.68-.2762-5.4868 0-.1636-.3933-.4058-.8742-.6177-1.2495a.077.077 0 00-.0785-.037 19.7363 19.7363 0 00-4.8852 1.515.0699.0699 0 00-.0321.0277C.5334 9.0458-.319 13.5799.0992 18.0578a.0824.0824 0 00.0312.0561c2.0528 1.5076 4.0413 2.4228 5.9929 3.0294a.0777.0777 0 00.0842-.0276c.4616-.6304.8731-1.2952 1.226-1.9942a.076.076 0 00-.0416-.1057c-.6528-.2476-1.2743-.5495-1.8722-.8923a.077.077 0 01-.0076-.1277c.1258-.0943.2517-.1923.3718-.2914a.0743.0743 0 01.0776-.0105c3.9278 1.7933 8.18 1.7933 12.0614 0a.0739.0739 0 01.0785.0095c.1202.099.246.1981.3728.2924a.077.077 0 01-.0066.1276 12.2986 12.2986 0 01-1.873.8914.0766.0766 0 00-.0407.1067c.3604.698.7719 1.3628 1.225 1.9932a.076.076 0 00.0842.0286c1.961-.6067 3.9495-1.5219 6.0023-3.0294a.077.077 0 00.0313-.0552c.5004-5.177-.8382-9.6739-3.5485-13.6604a.061.061 0 00-.0312-.0286zM8.02 15.3312c-1.1825 0-2.1569-1.0857-2.1569-2.419 0-1.3332.9555-2.4189 2.157-2.4189 1.2108 0 2.1757 1.0952 2.1568 2.419 0 1.3332-.9555 2.4189-2.1569 2.4189zm7.9748 0c-1.1825 0-2.1569-1.0857-2.1569-2.419 0-1.3332.9554-2.4189 2.1569-2.4189 1.2108 0 2.1757 1.0952 2.1568 2.419 0 1.3332-.946 2.4189-2.1568 2.4189Z")


def svg_subpaths(d):
    """Flattens an SVG path (M L H V C S Q A Z; arcs as straight lines, they're
    only tiny corner roundings here) into lists of points."""
    import re
    num_re = re.compile(r"\s*,?\s*(-?(?:\d+\.?\d*|\.\d+)(?:[eE][-+]?\d+)?)")
    pos = 0
    out, cur = [], []
    x = y = sx = sy = 0.0
    lc = None  # last cubic control point
    cmd = None

    def num():
        nonlocal pos
        m = num_re.match(d, pos)
        pos = m.end()
        return float(m.group(1))

    def flag():  # arc flags are single digits, often packed together ("00")
        nonlocal pos
        while d[pos] in " ,":
            pos += 1
        pos += 1
        return d[pos - 1] == "1"

    def cubic(p0, p1, p2, p3):
        for k in range(1, 17):
            t = k / 16
            a = (1 - t) ** 3; b = 3 * (1 - t) ** 2 * t; c = 3 * (1 - t) * t * t; e = t ** 3
            cur.append((a * p0[0] + b * p1[0] + c * p2[0] + e * p3[0], a * p0[1] + b * p1[1] + c * p2[1] + e * p3[1]))

    while True:
        while pos < len(d) and d[pos] in " ,":
            pos += 1
        if pos >= len(d):
            break
        if d[pos].isalpha():
            cmd = d[pos]; pos += 1
            if cmd in "Zz":
                if cur: out.append(cur)
                cur = []; x, y = sx, sy; lc = None
                continue
        rel = cmd.islower(); C = cmd.upper()
        ox, oy = (x, y) if rel else (0.0, 0.0)
        if C == "M":
            if cur: out.append(cur)
            x, y = ox + num(), oy + num(); sx, sy = x, y; cur = [(x, y)]
            cmd = "l" if rel else "L"; lc = None
        elif C == "L":
            x, y = ox + num(), oy + num(); cur.append((x, y)); lc = None
        elif C == "H":
            x = ox + num(); cur.append((x, y)); lc = None
        elif C == "V":
            y = oy + num(); cur.append((x, y)); lc = None
        elif C == "C":
            p1 = (ox + num(), oy + num()); p2 = (ox + num(), oy + num()); p3 = (ox + num(), oy + num())
            cubic((x, y), p1, p2, p3); lc = p2; x, y = p3
        elif C == "S":
            p1 = (2 * x - lc[0], 2 * y - lc[1]) if lc else (x, y)
            p2 = (ox + num(), oy + num()); p3 = (ox + num(), oy + num())
            cubic((x, y), p1, p2, p3); lc = p2; x, y = p3
        elif C == "Q":
            q = (ox + num(), oy + num()); p3 = (ox + num(), oy + num())
            cubic((x, y), (x + 2 / 3 * (q[0] - x), y + 2 / 3 * (q[1] - y)),
                  (p3[0] + 2 / 3 * (q[0] - p3[0]), p3[1] + 2 / 3 * (q[1] - p3[1])), p3)
            lc = None; x, y = p3
        elif C == "A":
            num(); num(); num(); flag(); flag()  # radii, rotation, flags
            x, y = ox + num(), oy + num(); cur.append((x, y)); lc = None
    if cur: out.append(cur)
    return out


def discord_mark(size_px, colour=(255, 255, 255, 255)):
    """The logo filled (even-odd: the eyes are holes) on a transparent square."""
    from PIL import ImageChops
    mask = Image.new("L", (size_px, size_px), 0)
    k = size_px / 24.0
    for sub in svg_subpaths(DISCORD_PATH):
        m = Image.new("L", (size_px, size_px), 0)
        ImageDraw.Draw(m).polygon([(px * k, py * k) for px, py in sub], fill=255)
        mask = ImageChops.logical_xor(mask.convert("1"), m.convert("1")).convert("L")
    im = Image.new("RGBA", (size_px, size_px), colour)
    im.putalpha(mask)
    return im


def discord():
    # Discord's app icon: the white logo on a blurple squircle.
    im = canvas(); d = ImageDraw.Draw(im)
    d.rounded_rectangle([p(40), p(40), p(472), p(472)], radius=p(120), fill=(88, 101, 242))
    mark = discord_mark(p(300))
    im.alpha_composite(mark, (p(106), p(110)))
    return im


def discord_white():
    # Just the white logo, for drawing on the panel's own backgrounds.
    im = canvas()
    im.alpha_composite(discord_mark(p(440)), (p(36), p(36)))
    return im


COMFORTAA = "tools/fonts/Comfortaa.ttf"  # OFL (Google Fonts), close to Sphaira's wordmark


def sphaira():
    # Sphaira's icon redrawn sharp (its own icon.jpg is 256 px and blurry): navy disc,
    # a green-to-blue planet rising at the bottom, "sphaira" in a rounded face.
    im = canvas()
    c0, c1 = p(46), p(466)
    size = c1 - c0
    disc = Image.new("RGBA", (size, size), (20, 49, 67, 255))
    d = ImageDraw.Draw(disc)
    # The planet: a huge circle whose top shows, coloured left green -> right blue.
    grad = Image.new("RGBA", (size, size))
    gd = ImageDraw.Draw(grad)
    for x in range(size):
        t = x / (size - 1)
        gd.line([x, 0, x, size], fill=(int(62 - 2 * t), int(229 - 60 * t), int(113 + 120 * t), 255))
    planet = Image.new("L", (size, size), 0)
    r = size * 1.35
    cx, top = size * 0.5, size * 0.67
    ImageDraw.Draw(planet).ellipse([cx - r, top, cx + r, top + 2 * r], fill=255)
    # A soft glow over the horizon, then the planet itself.
    glow = planet.filter(ImageFilter.GaussianBlur(size * 0.06)).point(lambda v: int(v * 0.7))
    disc.paste(grad, (0, 0), glow)
    disc.paste(grad, (0, 0), planet)
    f = ImageFont.truetype(COMFORTAA, int(size * 0.215))
    f.set_variation_by_axes([700])
    d = ImageDraw.Draw(disc)
    d.text((size * 0.5, size * 0.46), "sphaira", font=f, fill="white", anchor="mm",
           stroke_width=int(size * 0.007), stroke_fill="white")
    mask = Image.new("L", (size, size), 0)
    ImageDraw.Draw(mask).ellipse([0, 0, size - 1, size - 1], fill=255)
    im.paste(disc, (c0, c0), mask)
    ImageDraw.Draw(im).ellipse([c0, c0, c1, c1], outline=(120, 220, 200), width=p(10))
    return im


def nextendo():
    # Nextendo (prelude.nro): rainbow ring, dark disc, red X, blue signal arcs.
    import colorsys
    im = canvas()
    c, R = p(256), p(232)
    ring = Image.new("RGBA", (N, N), (0, 0, 0, 0))
    rd = ImageDraw.Draw(ring)
    for a in range(360):  # hue runs clockwise from red at the top
        r, g, b = colorsys.hsv_to_rgb(a / 360.0, 0.95, 1.0)
        rd.pieslice([c - R, c - R, c + R, c + R], a - 90, a - 89 + 1.5, fill=(int(r * 255), int(g * 255), int(b * 255), 255))
    mask = Image.new("L", (N, N), 0)
    md = ImageDraw.Draw(mask)
    md.ellipse([c - R, c - R, c + R, c + R], fill=255)
    inner = p(200)
    md.ellipse([c - inner, c - inner, c + inner, c + inner], fill=0)
    im.paste(ring, (0, 0), mask)
    d = ImageDraw.Draw(im)
    d.ellipse([c - inner, c - inner, c + inner, c + inner], fill=(225, 20, 40))      # red rim
    disc = p(186)
    d.ellipse([c - disc, c - disc, c + disc, c + disc], fill=(43, 34, 38))
    # The X, with round ends.
    w, h = p(44), p(82)
    red = (229, 24, 44)
    xc, yc = c - p(10), c + p(16)
    for (x0, y0, x1, y1) in [(-1, -1, 1, 1), (1, -1, -1, 1)]:
        a0 = (xc + x0 * h, yc + y0 * h)
        a1 = (xc + x1 * h, yc + y1 * h)
        d.line([a0, a1], fill=red, width=w)
        for (x, y) in (a0, a1):
            d.ellipse([x - w / 2, y - w / 2, x + w / 2, y + w / 2], fill=red)
    # Two signal arcs at the top right.
    blue = (30, 144, 232)
    ox, oy = xc + h, yc - h  # from the tip of the X's upper-right arm
    for rr in (p(54), p(92)):
        d.arc([ox - rr, oy - rr, ox + rr, oy + rr], -88, -2, fill=blue, width=p(15))
    return im


def app_icon():
    W = 256 * S
    im = Image.new("RGB", (W, W))
    d = ImageDraw.Draw(im)
    for y in range(W):  # black -> deep blue (OLED friendly)
        t = y / W
        d.line([0, y, W, y], fill=(int(8 + 10 * t), int(10 + 40 * t), int(20 + 110 * t)))
    glow = Image.new("RGBA", (W, W), (0, 0, 0, 0))
    ImageDraw.Draw(glow).ellipse([W * .18, W * .12, W * .82, W * .76], fill=(60, 160, 255, 110))
    im.paste(glow.filter(ImageFilter.GaussianBlur(W * .08)), (0, 0), glow.filter(ImageFilter.GaussianBlur(W * .08)))
    d = ImageDraw.Draw(im)
    # house glyph
    x0, x1, yt, yb = W * .28, W * .72, W * .14, W * .62
    d.polygon([(W * .5, yt), (x1 + W * .06, W * .36), (x0 - W * .06, W * .36)], fill="white")
    d.rounded_rectangle([x0, W * .33, x1, yb], radius=int(W * .04), fill="white")
    d.rounded_rectangle([W * .44, W * .44, W * .56, yb], radius=int(W * .03), fill=(30, 80, 160))
    f = ImageFont.truetype(BOLD, int(W * .2))
    d.text((W * .5, W * .80), "HomeNX", font=f, fill="white", anchor="mm")
    im.resize((256, 256), Image.LANCZOS).save("icon.jpg", quality=95)


def toggle_icon():
    # HomeNX's icon with a power symbol, for the HomeNX Toggle .nro
    W = 256 * S
    im = Image.open("icon.jpg").convert("RGB").resize((W, W), Image.LANCZOS)
    d = ImageDraw.Draw(im)
    cx, cy, r = W * 0.8, W * 0.2, W * 0.16
    d.ellipse([cx - r, cy - r, cx + r, cy + r], fill=(230, 40, 60))
    w = int(W * 0.035)
    rr = r * 0.55
    d.arc([cx - rr, cy - rr, cx + rr, cy + rr], start=-60, end=240, fill="white", width=w)
    d.line([cx, cy - rr * 1.15, cx, cy - rr * 0.1], fill="white", width=w)
    im.resize((256, 256), Image.LANCZOS).save("projects/manager/icon.jpg", quality=95)


if __name__ == "__main__":
    save(sphaira(), "romfs/icons/sphaira.png", 512)  # sharp at the launch card's size too
    save(nextendo(), "romfs/icons/nextendo.png")
    for name, fn in [("eshop", eshop), ("news", news), ("homebrew", homebrew), ("friends", friends), ("discord", discord), ("discord_mark", discord_white)]:
        save(fn(), f"romfs/icons/{name}.png")
    app_icon()
    toggle_icon()
