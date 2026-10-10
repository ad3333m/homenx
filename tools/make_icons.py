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


def browser():
    im = canvas(); d = ImageDraw.Draw(im)
    c, r = 256, 200
    d.ellipse([p(c - r), p(c - r), p(c + r), p(c + r)], fill=(10, 132, 255))
    line = (220, 238, 255); w = p(16)
    d.ellipse([p(c - r), p(c - r), p(c + r), p(c + r)], outline=line, width=w)
    d.ellipse([p(c - 85), p(c - r), p(c + 85), p(c + r)], outline=line, width=w)
    d.line([p(c), p(c - r), p(c), p(c + r)], fill=line, width=w)
    d.line([p(c - r), p(c), p(c + r), p(c)], fill=line, width=w)
    for dy in (-110, 110):
        half = int((r * r - dy * dy) ** 0.5)
        d.line([p(c - half), p(c + dy), p(c + half), p(c + dy)], fill=line, width=w)
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
    # glossy sphere: dark-to-violet radial body with a soft highlight
    im = canvas(); r = 200; c = 256
    body = Image.new("RGBA", (N, N), (0, 0, 0, 0)); d = ImageDraw.Draw(body)
    for i in range(r, 0, -2):
        t = i / r
        d.ellipse([p(c - 30 * (1 - t) - i), p(c - 40 * (1 - t) - i), p(c - 30 * (1 - t) + i), p(c - 40 * (1 - t) + i)],
                  fill=(int(40 + 140 * (1 - t)), int(30 + 90 * (1 - t)), int(120 + 120 * (1 - t)), 255))
    mask = Image.new("L", (N, N), 0)
    ImageDraw.Draw(mask).ellipse([p(c - r), p(c - r), p(c + r), p(c + r)], fill=255)
    im.paste(body, (0, 0), mask)
    hl = Image.new("RGBA", (N, N), (0, 0, 0, 0))
    ImageDraw.Draw(hl).ellipse([p(150), p(105), p(290), p(195)], fill=(255, 255, 255, 150))
    im.alpha_composite(hl.filter(ImageFilter.GaussianBlur(p(14))))
    ring = ImageDraw.Draw(im)
    ring.ellipse([p(c - r), p(c - r), p(c + r), p(c + r)], outline=(200, 180, 255), width=p(10))
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


if __name__ == "__main__":
    for name, fn in [("sphaira", sphaira), ("eshop", eshop), ("news", news), ("browser", browser), ("friends", friends)]:
        save(fn(), f"romfs/icons/{name}.png")
    app_icon()
