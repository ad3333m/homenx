"""HomeNX wallpapers for the Theme Shop: each one is a theme (themes/<id>/) with a
1280x720 background, a cover and colours to match, listed in themes.json (the
Theme Shop catalogue, at the repo root like SwitchU-Themes' index.json).
Everything is drawn here, so the pictures are HomeNX's own.

    python tools/make_wallpapers.py
"""
import colorsys
import json
import math
import os

import numpy as np
from PIL import Image, ImageDraw, ImageFilter
from scipy.spatial import Delaunay

W, H = 1280, 720
ROOT = os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "themes")


def hexc(s):
    s = s.lstrip("#")
    return np.array([int(s[i:i + 2], 16) / 255.0 for i in (0, 2, 4)])


def gradient(stops, axis=0, h=H, w=W):
    """Colour stops [(t, '#rrggbb'), ...] down (axis 0) or across (axis 1)."""
    n = h if axis == 0 else w
    t = np.linspace(0, 1, n)
    ts = [p for p, _ in stops]
    cols = np.array([hexc(c) for _, c in stops])
    line = np.stack([np.interp(t, ts, cols[:, k]) for k in range(3)], axis=-1)
    return np.repeat(line[:, None, :], w, 1) if axis == 0 else np.repeat(line[None, :, :], h, 0)


def fbm(h, w, scale, octaves=5, seed=0, gain=0.5):
    """Smooth fractal noise in 0..1."""
    rng = np.random.default_rng(seed)
    out = np.zeros((h, w))
    amp, total = 1.0, 0.0
    for o in range(octaves):
        cells = max(2, int(scale * (2 ** o)))
        gh, gw = max(2, int(cells * h / w) + 1), cells + 1
        grid = Image.fromarray(rng.random((gh, gw)).astype(np.float32), "F")
        out += amp * np.asarray(grid.resize((w, h), Image.BICUBIC), float)
        total += amp
        amp *= gain
    out /= total
    return (out - out.min()) / (out.max() - out.min() + 1e-9)


def fbm1(n, scale, octaves=5, seed=0):
    return fbm(1, n, scale, octaves, seed)[0]


def smooth(e0, e1, x):
    t = np.clip((x - e0) / (e1 - e0), 0, 1)
    return t * t * (3 - 2 * t)


def blend(base, color, alpha):
    a = alpha[..., None] if np.ndim(alpha) == 2 else alpha
    return base * (1 - a) + np.asarray(color) * a


def stars(img, count, seed, max_size=1.6, area=1.0):
    rng = np.random.default_rng(seed)
    layer = Image.new("L", (W, H), 0)
    d = ImageDraw.Draw(layer)
    for _ in range(count):
        x, y = rng.random() * W, (rng.random() ** 1.4) * H * area
        r = rng.random() ** 3 * max_size + 0.4
        d.ellipse([x - r, y - r, x + r, y + r], fill=int(120 + rng.random() * 135))
    glow = np.asarray(layer.filter(ImageFilter.GaussianBlur(1.2)), float) / 255
    sharp = np.asarray(layer, float) / 255
    return np.clip(img + (sharp * 0.9 + glow * 0.8)[..., None], 0, 1)


def grain(img, amount=0.012, seed=1):
    rng = np.random.default_rng(seed)
    return np.clip(img + rng.normal(0, amount, img.shape[:2])[..., None], 0, 1)


def ridge(n, base, amp, scale, seed, octaves=6):
    return base + (fbm1(n, scale, octaves, seed) - 0.5) * 2 * amp


def fill_below(img, line, color, soft=1.5):
    ys = np.arange(H)[:, None]
    mask = smooth(-soft, soft, ys - line[None, :])
    return blend(img, color, mask)


def blur(img, r):
    pil = Image.fromarray((np.clip(img, 0, 1) * 255).astype(np.uint8))
    return np.asarray(pil.filter(ImageFilter.GaussianBlur(r)), float) / 255


def glow_circle(cx, cy, r, falloff):
    ys, xs = np.mgrid[0:H, 0:W]
    d = np.hypot(xs - cx, ys - cy)
    return np.clip(np.exp(-np.maximum(d - r, 0) / falloff), 0, 1), d


# --- the wallpapers --------------------------------------------------------

def aurora():
    img = gradient([(0, "#02040c"), (0.55, "#061a33"), (1, "#0b3340")])
    img = stars(img, 900, 3, area=0.8)
    ys = np.arange(H)[:, None].astype(float)
    xs = np.arange(W)
    for k, (base, amp, col_lo, col_hi, height, seed) in enumerate([
            (430, 70, "#37f5a4", "#7c4dff", 260, 11),
            (360, 90, "#2ee6c8", "#b05cff", 200, 12),
            (300, 60, "#6cffb0", "#4d7dff", 160, 13)]):
        curve = base + amp * np.sin(xs / W * math.pi * (1.4 + k * 0.5) + k) + (fbm1(W, 4, 4, seed) - 0.5) * 80
        dy = curve[None, :] - ys
        body = np.where(dy > 0, np.exp(-dy / height), np.exp(-(dy / 6) ** 2))
        rays = 0.45 + 0.55 * fbm(1, W, 60, 3, seed + 50)[0][None, :] ** 1.5
        inten = body * rays * (0.55 - k * 0.08)
        tcol = np.clip(dy / (height * 1.6), 0, 1)[..., None]
        col = hexc(col_lo) * (1 - tcol) + hexc(col_hi) * tcol
        img = img + col * inten[..., None]
    land = ridge(W, 640, 40, 3, 21)
    img = fill_below(img, land, hexc("#01070a"))
    return np.clip(img, 0, 1)


def neon_grid():
    hz = int(H * 0.62)
    img = gradient([(0, "#0d0221"), (0.35, "#2b0a4a"), (0.6, "#a3245e"), (0.62, "#ff6b3d"), (1, "#0d0221")])
    img = stars(img, 300, 5, area=0.45)
    # the sun, with its stripes
    glow, d = glow_circle(W / 2, hz - 120, 150, 90)
    img = blend(img, hexc("#ff4f8b"), glow * 0.35)
    ys = np.arange(H)[:, None]
    sun = (d <= 150) & (ys < hz)
    t = np.clip((ys - (hz - 270)) / 300, 0, 1)
    suncol = hexc("#ffe66d") * (1 - t[..., None]) + hexc("#ff2e88") * t[..., None]
    stripe = ((ys - (hz - 120)) > 0) & (((ys - (hz - 120)) % 22) < (2 + (ys - (hz - 120)) / 14))
    m = (sun & ~stripe).astype(float)
    img = img * (1 - m[..., None]) + suncol * m[..., None]
    # the floor
    floor = (ys >= hz).astype(float)
    img = blend(img, hexc("#14012b"), floor)
    lines = Image.new("L", (W, H), 0)
    dr = ImageDraw.Draw(lines)
    for i in range(1, 30):
        y = hz + (i / 30) ** 2.2 * (H - hz) * 1.05
        dr.line([(0, y), (W, y)], fill=255, width=2)
    for i in range(-24, 25):
        dr.line([(W / 2 + i * 12, hz), (W / 2 + i * 150, H)], fill=255, width=2)
    lm = np.asarray(lines, float) / 255 * floor
    halo = blur(np.repeat(lm[..., None], 3, 2), 4)[..., 0]
    img = blend(img, hexc("#ff3df2"), np.clip(lm * 0.9 + halo * 1.2, 0, 1))
    img = blend(img, hexc("#ff6b3d"), np.exp(-np.abs(ys - hz) / 6).repeat(W, 1) * 0.8)
    return np.clip(img, 0, 1)


def sunset_dunes():
    img = gradient([(0, "#3b2667"), (0.45, "#bc4e9c"), (0.7, "#ff8a5b"), (1, "#ffcf87")])
    glow, d = glow_circle(W * 0.68, 430, 70, 160)
    img = blend(img, hexc("#fff1b8"), glow * 0.85)
    img = blend(img, hexc("#fffbe8"), (d < 70).astype(float))
    cols = ["#f3a46b", "#d9764e", "#b5523b", "#7d3329", "#4a1f22"]
    for i, c in enumerate(cols):
        base = 470 + i * 55
        xs = np.arange(W)
        line = base - 60 * np.sin(xs / W * math.pi * (1 + i * 0.6) + i * 1.7) ** 2 + (fbm1(W, 2, 3, 40 + i) - 0.5) * 50
        img = fill_below(img, line, hexc(c), 2)
        # light on the ridge
        ys = np.arange(H)[:, None]
        rim = np.exp(-np.maximum(ys - line[None, :], 0) / 14) * (ys >= line[None, :])
        img = blend(img, hexc("#ffd29a"), rim * 0.25)
    return grain(np.clip(img, 0, 1), 0.01)


def ocean_depths():
    img = gradient([(0, "#1fb5e8"), (0.25, "#0a6fb0"), (0.6, "#05305e"), (1, "#010a1c")])
    ys, xs = np.mgrid[0:H, 0:W].astype(float)
    ang = np.arctan2(xs - W * 0.45, ys + 300)
    rays = fbm(1, 400, 30, 3, 7)[0]
    idx = np.clip(((ang + 1.2) / 2.4 * 399).astype(int), 0, 399)
    r = rays[idx] ** 3 * np.exp(-ys / 420)
    img = img + hexc("#bff6ff") * (r * 0.55)[..., None]
    caust = fbm(H, W, 18, 3, 9)
    c = smooth(0.62, 0.7, caust) * np.exp(-ys / 120) * 0.3
    img = img + hexc("#e6fdff") * c[..., None]
    rng = np.random.default_rng(4)
    layer = Image.new("L", (W, H), 0)
    dr = ImageDraw.Draw(layer)
    for _ in range(70):
        x, y = rng.random() * W, 250 + rng.random() * 470
        rad = 2 + rng.random() ** 2 * 12
        dr.ellipse([x - rad, y - rad, x + rad, y + rad], outline=int(90 + rng.random() * 120), width=1)
    b = np.asarray(layer.filter(ImageFilter.GaussianBlur(0.6)), float) / 255
    img = img + hexc("#d8fbff") * (b * 0.7)[..., None]
    return np.clip(img, 0, 1)


def midnight_mountains():
    img = gradient([(0, "#070b1f"), (0.5, "#1b2257"), (0.78, "#4a3f8f"), (1, "#7c5fb0")])
    img = stars(img, 700, 8, area=0.7)
    glow, d = glow_circle(W * 0.24, 170, 46, 110)
    img = blend(img, hexc("#c9c3ff"), glow * 0.5)
    img = blend(img, hexc("#f4f1ff"), smooth(47, 45, d))
    layers = [("#3c3a7a", 470, 90, 4), ("#2a2963", 520, 80, 5), ("#1b1b47", 580, 70, 6), ("#0e0f2b", 640, 50, 7)]
    for i, (c, base, amp, seed) in enumerate(layers):
        line = ridge(W, base, amp, 3 + i, seed, 7)
        img = fill_below(img, line, hexc(c), 1.2)
        ys = np.arange(H)[:, None]
        mist = np.exp(-np.abs(ys - (line[None, :] + 40)) / 30) * 0.18
        img = blend(img, hexc("#8c84d6"), mist)
    return np.clip(img, 0, 1)


def sakura():
    img = gradient([(0, "#fff0f5"), (0.5, "#ffd1e3"), (1, "#f7a8c4")], axis=0)
    img = blend(img, hexc("#ffe9c9"), gradient([(0, "#000000"), (1, "#ffffff")], axis=1)[..., 0] * 0.25)
    rng = np.random.default_rng(12)
    for pass_r, n, alpha in [(14, 26, 0.35), (5, 40, 0.45), (1.2, 60, 0.8)]:
        layer = Image.new("RGBA", (W, H), (0, 0, 0, 0))
        dr = ImageDraw.Draw(layer)
        for _ in range(n):
            x, y = rng.random() * W, rng.random() * H
            size = (6 + rng.random() * 16) * (1 + pass_r / 6)
            col = [(255, 255, 255), (255, 183, 207), (255, 140, 180), (255, 214, 231)][rng.integers(4)]
            if pass_r > 2:
                dr.ellipse([x - size, y - size, x + size, y + size], fill=col + (int(255 * alpha),))
            else:  # a petal
                petal = Image.new("RGBA", (64, 64), (0, 0, 0, 0))
                pd = ImageDraw.Draw(petal)
                pd.ellipse([18, 6, 46, 58], fill=col + (230,))
                pd.polygon([(32, 4), (26, 14), (38, 14)], fill=(0, 0, 0, 0))
                petal = petal.rotate(rng.random() * 360, resample=Image.BICUBIC).resize(
                    (int(size * 1.4), int(size * 1.4)), Image.LANCZOS)
                layer.alpha_composite(petal, (int(x), int(y)))
        a = np.asarray(layer, float) / 255
        pre = blur(np.concatenate([a[..., :3] * a[..., 3:4]], -1), pass_r)  # premultiplied: no dark rims
        alpha = blur(np.repeat(a[..., 3:4], 3, 2), pass_r)[..., :1]
        img = img * (1 - alpha) + pre
    return grain(img, 0.008)


def galaxy():
    img = gradient([(0, "#03010a"), (1, "#0a0420")])
    n1, n2, mask = fbm(H, W, 3, 6, 31), fbm(H, W, 4, 6, 32), fbm(H, W, 2, 5, 33)
    ys, xs = np.mgrid[0:H, 0:W].astype(float)
    band = np.exp(-((ys - (H * 0.5 + (xs - W / 2) * 0.35)) / 260) ** 2)
    neb = smooth(0.2, 0.75, mask) * band
    img = img + hexc("#d0249a") * (neb * n1 ** 1.5 * 1.6)[..., None]
    img = img + hexc("#3a7bff") * (neb * n2 ** 1.5 * 1.5)[..., None]
    img = img + hexc("#ffd6f5") * (neb * smooth(0.7, 1, n1 * n2 * 1.6) * 0.6)[..., None]
    img = stars(img, 1400, 34, 1.4)
    rng = np.random.default_rng(35)
    for _ in range(9):
        g, _ = glow_circle(rng.random() * W, rng.random() * H, 1.5, 7 + rng.random() * 8)
        img = img + hexc("#dfe8ff") * (g * 0.7)[..., None]
    return np.clip(img, 0, 1)


def forest_mist():
    img = gradient([(0, "#e8f1ec"), (0.5, "#c7ddd1"), (1, "#8fb5a2")])
    glow, _ = glow_circle(W * 0.7, 120, 40, 260)
    img = blend(img, hexc("#fffbe6"), glow * 0.6)
    rng = np.random.default_rng(41)
    cols = ["#a9c7b8", "#86ab98", "#5f8a76", "#3d6655", "#22443a"]
    for i, c in enumerate(cols):
        layer = Image.new("L", (W, H), 0)
        dr = ImageDraw.Draw(layer)
        ground = 430 + i * 62
        x = -40.0
        while x < W + 40:
            th = (90 + rng.random() * 90) * (0.7 + i * 0.22)
            tw = th * (0.32 + rng.random() * 0.1)
            base = ground + rng.random() * 18
            for tier in range(4):
                top = base - th * (1 - tier * 0.2)
                half = tw * (1 - tier * 0.22) / 2
                yb = base - th * tier * 0.18
                dr.polygon([(x, top), (x - half, yb), (x + half, yb)], fill=255)
            dr.rectangle([x - 3 * (1 + i * 0.3), base - 6, x + 3 * (1 + i * 0.3), H], fill=255)
            x += tw * (0.45 + rng.random() * 0.5)
        dr.rectangle([0, ground + 10, W, H], fill=255)
        m = np.asarray(layer.filter(ImageFilter.GaussianBlur(max(0.6, 2.4 - i * 0.5))), float) / 255
        img = blend(img, hexc(c), m)
        ys = np.arange(H)[:, None]
        fog = np.exp(-np.abs(ys - ground) / 45) * 0.35
        img = blend(img, hexc("#eef6f1"), np.repeat(fog, W, 1))
    return grain(img, 0.008)


def ember():
    n = fbm(H, W, 3, 6, 51)
    warp = fbm(H, W, 2, 4, 52)
    v = (n * 0.7 + warp * 0.3)
    bands = np.abs(np.sin(v * 22))
    lines = smooth(0.92, 1.0, bands)
    heat = smooth(0.35, 0.9, fbm(H, W, 2, 4, 53))
    img = gradient([(0, "#12030a"), (1, "#1f0608")])
    img = img + hexc("#ff3d1f") * (heat * 0.35)[..., None]
    img = img + hexc("#ff9d2e") * (lines * heat * 0.9)[..., None]
    img = img + hexc("#ffe7a3") * (smooth(0.985, 1, bands) * heat * 0.6)[..., None]
    img = (img + blur(img, 6) * 0.6)
    return np.clip(img, 0, 1)


def pastel_waves():
    img = gradient([(0, "#fdf6ff"), (1, "#eef3ff")])
    cols = ["#d7c4ff", "#c4e3ff", "#c6f5e1", "#ffe0c7", "#ffc9de"]
    xs = np.arange(W)
    for i, c in enumerate(cols):
        base = 300 + i * 85
        line = base + 45 * np.sin(xs / W * math.pi * 2 * (0.7 + i * 0.15) + i * 1.3) \
            + 20 * np.sin(xs / W * math.pi * 6 + i)
        ys = np.arange(H)[:, None]
        shadow = smooth(0, 40, ys - line[None, :] + 12) * (ys < line[None, :] + 40)
        img = blend(img, hexc("#8f86b8"), np.exp(-np.abs(ys - line[None, :] + 6) / 10) * 0.08 * (ys < line[None, :]))
        img = fill_below(img, line, hexc(c), 1.2)
    return grain(img, 0.006)


def low_poly():
    rng = np.random.default_rng(61)
    pts = np.vstack([rng.random((260, 2)) * [W + 200, H + 200] - 100,
                     [[-100, -100], [W + 100, -100], [-100, H + 100], [W + 100, H + 100]]])
    tri = Delaunay(pts)
    base = gradient([(0, "#1c2333"), (1, "#0b0f18")], axis=0)
    accent = gradient([(0, "#2d6cdf"), (1, "#7b3fe4")], axis=1)
    pil = Image.new("RGB", (W, H))
    dr = ImageDraw.Draw(pil)
    light = np.array([-0.4, -0.6, 0.7])
    light /= np.linalg.norm(light)
    for s in tri.simplices:
        p = pts[s]
        cx, cy = np.clip(p.mean(0), [0, 0], [W - 1, H - 1]).astype(int)
        z = rng.random(3) * 60
        a = np.array([*p[1] - p[0], z[1] - z[0]])
        b = np.array([*p[2] - p[0], z[2] - z[0]])
        nrm = np.cross(a, b)
        nrm /= np.linalg.norm(nrm) + 1e-9
        shade = 0.55 + 0.45 * abs(nrm @ light)
        diag = (cx / W + (1 - cy / H)) / 2
        col = base[cy, cx] * shade + accent[cy, cx] * smooth(0.55, 1.0, diag) * 0.55 * shade
        dr.polygon([tuple(q) for q in p], fill=tuple(int(v * 255) for v in np.clip(col, 0, 1)))
    img = np.asarray(pil, float) / 255
    return grain(img, 0.01)


def candy_mesh():
    img = np.ones((H, W, 3)) * hexc("#fff6fb")
    blobs = [(0.15, 0.2, "#ff9ecd", 420), (0.85, 0.15, "#9ed8ff", 460), (0.55, 0.85, "#ffe08a", 440),
             (0.05, 0.9, "#b9a6ff", 380), (0.95, 0.85, "#8affd2", 380), (0.5, 0.4, "#ffc4a8", 300)]
    ys, xs = np.mgrid[0:H, 0:W].astype(float)
    acc = np.zeros((H, W, 3))
    wsum = np.zeros((H, W))
    for bx, by, c, r in blobs:
        w = np.exp(-((xs - bx * W) ** 2 + (ys - by * H) ** 2) / (2 * r * r))
        acc += hexc(c) * w[..., None]
        wsum += w
    mix = acc / (wsum[..., None] + 1e-9)
    img = blend(img, mix, np.clip(wsum, 0, 1) * 0.95)
    return grain(img, 0.01)


def snow_peaks():
    img = gradient([(0, "#7fbfff"), (0.55, "#cfe6ff"), (1, "#eef6ff")])
    glow, _ = glow_circle(W * 0.8, 110, 30, 200)
    img = blend(img, hexc("#ffffff"), glow * 0.7)
    rng = np.random.default_rng(70)
    S = 2  # drawn at twice the size, then scaled down (smooth edges)
    for i, (snow, shade, top, bottom, width) in enumerate([("#dce8f6", "#b3c6df", 250, 470, 230),
                                                           ("#eef4fb", "#a6bcda", 330, 560, 190),
                                                           ("#ffffff", "#bccfe6", 470, 650, 150)]):
        layer = Image.new("RGBA", (W * S, H * S), (0, 0, 0, 0))
        dr = ImageDraw.Draw(layer)
        pts, peaks = [], []
        x = -200.0
        while x < W + 200:
            valley = (x, bottom - rng.random() * 40)
            px = x + width * (0.5 + rng.random() * 0.5)
            peak = (px, top + rng.random() * (bottom - top) * 0.45)
            pts += [valley, peak]
            peaks.append(len(pts) - 1)
            x = px + width * (0.5 + rng.random() * 0.5)
        pts.append((x, bottom))
        sc = lambda p: (p[0] * S, p[1] * S)
        dr.polygon([sc(p) for p in pts] + [sc((W + 200, H)), sc((-200, H))], fill=snow)
        for k in peaks:  # the face away from the sun, down to the next valley
            (pkx, pky), (vx, vy) = pts[k], pts[k + 1]
            foot = (pkx + (vx - pkx) * 0.3, vy + (vy - pky) * 0.15)
            dr.polygon([sc((pkx, pky)), sc((vx, vy)), sc(foot)], fill=shade)
        layer = layer.resize((W, H), Image.LANCZOS)
        a = np.asarray(layer, float) / 255
        img = img * (1 - a[..., 3:4]) + a[..., :3] * a[..., 3:4]
        ys = np.arange(H)[:, None]
        img = blend(img, hexc("#f4f9ff"), np.repeat(smooth(bottom - 120, bottom + 30, ys) * 0.55, W, 1))
    return grain(img, 0.006)


# id, name, function, mode, cursor, accent
WALLPAPERS = [
    ("homenx-aurora", "Aurora", aurora, "dark", "#37f5a4", "#7c4dff"),
    ("homenx-neon-grid", "Neon Grid", neon_grid, "dark", "#ff3df2", "#ffb347"),
    ("homenx-sunset-dunes", "Sunset Dunes", sunset_dunes, "dark", "#ffb36b", "#ff6f91"),
    ("homenx-ocean-depths", "Ocean Depths", ocean_depths, "dark", "#5ce1ff", "#1fb5e8"),
    ("homenx-midnight-mountains", "Midnight Mountains", midnight_mountains, "dark", "#a99cff", "#6c63ff"),
    ("homenx-sakura", "Sakura", sakura, "light", "#ff5c93", "#ff8fb5"),
    ("homenx-galaxy", "Galaxy", galaxy, "dark", "#ff4fc3", "#4d8bff"),
    ("homenx-forest-mist", "Forest Mist", forest_mist, "light", "#2f8f6a", "#3d6655"),
    ("homenx-ember", "Ember", ember, "dark", "#ff7a2e", "#ffb347"),
    ("homenx-pastel-waves", "Pastel Waves", pastel_waves, "light", "#8a6cff", "#ff8fb5"),
    ("homenx-low-poly", "Low Poly Night", low_poly, "dark", "#4d8bff", "#7b3fe4"),
    ("homenx-candy", "Candy", candy_mesh, "light", "#ff5ca8", "#5cb8ff"),
    ("homenx-snow-peaks", "Snow Peaks", snow_peaks, "light", "#2f7fe0", "#5aa8ff"),
]


def hsl(c):
    r, g, b = c
    h, l, s = colorsys.rgb_to_hls(r, g, b)
    return [round(h, 4), round(s, 4), round(l, 4)]


def main():
    os.makedirs(ROOT, exist_ok=True)
    index = {"schemaVersion": 1, "themes": []}
    for tid, name, fn, mode, cursor, accent in WALLPAPERS:
        print("drawing", name)
        img = (np.clip(fn(), 0, 1) * 255).astype(np.uint8)
        pil = Image.fromarray(img)
        base = os.path.join(ROOT, tid)
        os.makedirs(os.path.join(base, "media", "backgrounds"), exist_ok=True)
        os.makedirs(os.path.join(base, "media", "screenshots"), exist_ok=True)
        pil.save(os.path.join(base, "media", "backgrounds", "wallpaper.jpg"), quality=92, optimize=True)
        pil.resize((640, 360), Image.LANCZOS).save(os.path.join(base, "media", "screenshots", "00.jpg"),
                                                  quality=88, optimize=True)
        avg = img.reshape(-1, 3).mean(0) / 255
        bg = hsl(avg)
        bg[2] = 0.08 if mode == "dark" else 0.92
        bg_acc = hsl(avg)
        bg_acc[2] = 0.16 if mode == "dark" else 0.84
        manifest = {
            "id": tid,
            "name": name,
            "author": "HomeNX",
            "version": "1.0.0",
            "preview": {"screenshots": ["media/screenshots/00.jpg"]},
            "theme": {
                "mode": mode,
                "colors": {
                    "cursor": hsl(hexc(cursor)),
                    "accent": hsl(hexc(accent)),
                    "background": bg,
                    "backgroundAccent": bg_acc,
                    "shapes": hsl(hexc(accent)),
                },
                "background": {
                    "layout": "floating",
                    "count": 0,
                    "image": {"path": "media/backgrounds/wallpaper.jpg", "opacity": 1.0, "fit": "cover"},
                },
            },
        }
        with open(os.path.join(base, "theme.json"), "w", encoding="utf-8") as f:
            json.dump(manifest, f, indent=2)
        index["themes"].append({
            "id": tid,
            "name": name,
            "author": "HomeNX",
            "version": "1.0.0",
            "path": "themes/" + tid,
            "manifest": "themes/" + tid + "/theme.json",
            "cover": "themes/" + tid + "/media/screenshots/00.jpg",
        })
    with open(os.path.join(ROOT, "..", "themes.json"), "w", encoding="utf-8") as f:
        json.dump(index, f, indent=2)


if __name__ == "__main__":
    main()
