"""Draws the HomeNX sidebar icons (eshop/news/browser/friends) and icon.jpg.
Run from the repo root: python tools/make_icons.py"""
from PIL import Image, ImageDraw, ImageFont, ImageFilter

S = 4  # supersample
N = 512 * S
BOLD = "C:/Windows/Fonts/arialbd.ttf"


def canvas():
    return Image.new("RGBA", (N, N), (0, 0, 0, 0))


def save(im, path, size=512):
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


def sphaira():
    # Sphaira's own icon (tools/sphaira-icon.jpg, from ITotalJustice/sphaira) cropped to a circle
    im = canvas()
    logo = Image.open("tools/sphaira-icon.jpg").convert("RGBA").resize((p(420), p(420)), Image.LANCZOS)
    mask = Image.new("L", logo.size, 0)
    ImageDraw.Draw(mask).ellipse([0, 0, logo.size[0] - 1, logo.size[1] - 1], fill=255)
    im.paste(logo, (p(46), p(46)), mask)
    ImageDraw.Draw(im).ellipse([p(46), p(46), p(466), p(466)], outline=(120, 220, 200), width=p(10))
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
    im.resize((256, 256), Image.LANCZOS).save("projects/toggle/icon.jpg", quality=95)


if __name__ == "__main__":
    for name, fn in [("sphaira", sphaira), ("eshop", eshop), ("news", news), ("homebrew", homebrew), ("friends", friends)]:
        save(fn(), f"romfs/icons/{name}.png")
    app_icon()
    toggle_icon()
