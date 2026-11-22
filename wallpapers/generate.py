"""
Liquid Glass wallpaper generator.

Renders refractive "liquid glass" blobs over a soft, Perplexity-inspired
teal / off-black backdrop. Every pixel is computed here; no external images.

    python wallpapers/generate.py            # renders all variants at 3840x2160
    python wallpapers/generate.py --w 2560 --h 1440
"""
import argparse
import os

import numpy as np
from PIL import Image, ImageFilter

# Palette (community-sourced Perplexity brand values)
OFFBLACK = (9, 23, 23)          # #091717
INKY = (19, 52, 59)             # #13343B
PEACOCK = (46, 86, 94)          # #2E565E
TURQUOISE = (32, 128, 141)      # #20808D
PLEX = (31, 184, 205)           # #1FB8CD
SKY = (186, 222, 221)           # #BADEDD
PAPER = (251, 250, 244)         # #FBFAF4


def c(rgb):
    return np.array(rgb, dtype=np.float32) / 255.0


def blur(arr, radius):
    """Gaussian blur an HxWx3 float image via PIL (fast, no scipy needed)."""
    img = Image.fromarray(np.clip(arr * 255, 0, 255).astype(np.uint8))
    img = img.filter(ImageFilter.GaussianBlur(radius))
    return np.asarray(img).astype(np.float32) / 255.0


def blur1(arr, radius):
    img = Image.fromarray(np.clip(arr * 255, 0, 255).astype(np.uint8), "L")
    img = img.filter(ImageFilter.GaussianBlur(radius))
    return np.asarray(img).astype(np.float32) / 255.0


def smin(a, b, k):
    """Polynomial smooth minimum -> organic, liquid merges between shapes."""
    h = np.clip(0.5 + 0.5 * (b - a) / k, 0.0, 1.0)
    return b * (1 - h) + a * h - k * h * (1 - h)


def sd_circle(X, Y, cx, cy, r):
    return np.hypot(X - cx, Y - cy) - r


def sd_capsule(X, Y, ax, ay, bx, by, r):
    pax, pay = X - ax, Y - ay
    bax, bay = bx - ax, by - ay
    h = np.clip((pax * bax + pay * bay) / (bax * bax + bay * bay), 0, 1)
    return np.hypot(pax - bax * h, pay - bay * h) - r


def sd_rounded_rect(X, Y, cx, cy, hw, hh, r):
    qx = np.abs(X - cx) - hw + r
    qy = np.abs(Y - cy) - hh + r
    return (np.hypot(np.maximum(qx, 0), np.maximum(qy, 0))
            + np.minimum(np.maximum(qx, qy), 0) - r)


# Backgrounds

def background(W, H, mode, glows, rng):
    # Scene coordinates live in a 16:9 frame (x 0..1, y 0..0.5625) that is
    # fitted and centered on any screen shape, so nothing gets cropped.
    REF = 0.5625
    scale = min(W, H / REF)                     # pixels per scene unit
    xs = (np.arange(W, dtype=np.float32) - (W - scale) / 2) / scale
    ys = (np.arange(H, dtype=np.float32) - (H - scale * REF) / 2) / scale
    X, Y = np.meshgrid(xs, ys)
    if mode == "dark":
        top, bottom = c(INKY) * 0.35, c(OFFBLACK) * 0.8
    else:
        top, bottom = c(PAPER), c((236, 241, 236))
    t = ((Y - Y.min()) / (Y.max() - Y.min()))[..., None]
    img = top * (1 - t) + bottom * t

    for (gx, gy, gr, col, a) in glows:
        d = ((X - gx) ** 2 + (Y - gy * REF) ** 2) / (gr ** 2)
        w = (np.exp(-d) * a)[..., None]
        img = img * (1 - w) + c(col) * w

    # Fine diagonal light lines: they bend inside the glass, which is what
    # sells the refraction.
    u = (X * 0.8 + Y) * 46.0
    lines = np.exp(-((u - np.round(u)) ** 2) / 0.0009)
    fade = np.exp(-((X - 0.65) ** 2 + (Y - 0.30) ** 2) / 0.12)
    amt = 0.10 if mode == "dark" else 0.07
    line_col = c(SKY) if mode == "dark" else c(TURQUOISE)
    w = (lines * fade * amt)[..., None]
    img = img * (1 - w) + line_col * w
    return np.clip(img, 0, 1), X, Y


# Glass

def render_glass(bg, X, Y, sdf, mode, light=(-0.6, -0.8)):
    H, W = sdf.shape
    px = float(X[0, 1] - X[0, 0])   # one pixel in scene units
    k = 1.0 / (px * 3840)           # size factor relative to a 4K render
    d = sdf / px                    # signed distance in pixels
    inside = np.clip(-d + 0.5, 0, 1)  # anti-aliased mask

    gy, gx = np.gradient(sdf)
    n = np.hypot(gx, gy) + 1e-9
    nx, ny = gx / n, gy / n         # outward normal

    bevel = 150.0 * k          # thickness of the curved rim (px)
    depth = np.clip(-d / bevel, 0, 1)
    curve = (1 - depth) ** 2.2       # 1 at the edge, 0 in the flat middle

    # Frosted body: sample a blurred copy, displaced along the normal.
    frost = blur(bg, 18 * k)
    sharp = bg
    strength = 210.0 * k
    yy, xx = np.mgrid[0:H, 0:W].astype(np.float32)

    def sample(src, k):
        """Bilinear sample of a 2-D channel at refracted coordinates."""
        fx = np.clip(xx - nx * curve * strength * k, 0, W - 1.001)
        fy = np.clip(yy - ny * curve * strength * k, 0, H - 1.001)
        x0, y0 = fx.astype(np.int32), fy.astype(np.int32)
        tx, ty = fx - x0, fy - y0
        a = src[y0, x0] * (1 - tx) + src[y0, x0 + 1] * tx
        b = src[y0 + 1, x0] * (1 - tx) + src[y0 + 1, x0 + 1] * tx
        return a * (1 - ty) + b * ty

    body = 0.40 * frost + 0.60 * sharp
    # Chromatic aberration on the rim: R/G/B refract by slightly different amounts.
    refr = np.stack([sample(body[..., 0], 1.10),
                     sample(body[..., 1], 1.00),
                     sample(body[..., 2], 0.90)], axis=-1)

    # Glass tint + lift
    if mode == "dark":
        tint = c(PEACOCK)
        refr = refr * 1.08 + tint * 0.05 + 0.012
    else:
        tint = c(PAPER)
        refr = refr * 0.80 + tint * 0.22

    # Fresnel rim light (thin bright edge + softer inner glow)
    rim = np.exp(-np.maximum(-d, 0) / (2.2 * k))
    inner = np.exp(-np.maximum(-d, 0) / (22 * k))
    lx, ly = light
    ln = np.hypot(lx, ly)
    facing = (nx * lx + ny * ly) / ln            # +1 where edge faces the light
    hi = np.clip(facing, 0, 1) ** 1.5
    lo = np.clip(-facing, 0, 1) ** 1.5

    white = np.ones(3, dtype=np.float32)
    glow_col = c(PLEX) if mode == "dark" else c(TURQUOISE)
    refr = refr + (rim * (0.95 * hi + 0.30 * lo + 0.12))[..., None] * white
    refr = refr + (inner * 0.16 * hi)[..., None] * white
    refr = refr + (inner * 0.35 * lo)[..., None] * glow_col

    # Soft specular "pill" highlight near the lit edge
    spec_band = np.exp(-((-d - 18 * k) ** 2) / (2 * (9 * k) ** 2))
    refr = refr + (spec_band * hi ** 3 * 0.30)[..., None] * white

    # Drop shadow + ambient glow beneath the glass
    m = (sdf < 0).astype(np.float32)
    shift = int(28 * k)
    sh = np.zeros_like(m)
    sh[shift:, shift // 2:] = m[:-shift, : W - shift // 2]
    sh = blur1(sh, 48 * k)
    out = bg.copy()
    if mode == "dark":
        out = out * (1 - 0.45 * sh[..., None] * (1 - m[..., None]))
        halo = blur1(m, 90 * k)
        out = out + (halo * (1 - m) * 0.10)[..., None] * c(TURQUOISE)
        cs = int(60 * k)
        caus = np.zeros_like(m)
        caus[cs:, cs:] = m[:-cs, :-cs]
        caus = blur1(caus, 30 * k) * (1 - m)
        out = out + (caus * 0.28)[..., None] * c(PLEX)
    else:
        out = out * (1 - 0.12 * sh[..., None] * (1 - m[..., None]))

    a = inside[..., None]
    out = out * (1 - a) + np.clip(refr, 0, 1) * a
    return np.clip(out, 0, 1)


def grain(img, rng, amt):
    n = rng.normal(0, amt, img.shape[:2]).astype(np.float32)[..., None]
    return np.clip(img + n, 0, 1)


def save(img, path):
    Image.fromarray((img * 255 + 0.5).astype(np.uint8)).save(
        path, quality=95, subsampling=0, optimize=True)
    print("wrote", path)


# Compositions (coordinates normalized to width; y spans 0..H/W)

def scene_depth(W, H, rng):
    """Main desktop: big liquid form drifting off the right, two droplets."""
    glows = [(0.74, 0.27, 0.22, PLEX, 0.55),
             (0.70, 0.30, 0.34, TURQUOISE, 0.65),
             (0.95, 0.52, 0.22, PEACOCK, 0.55),
             (0.12, 0.55, 0.30, INKY, 0.55)]
    bg, X, Y = background(W, H, "dark", glows, rng)
    s = sd_circle(X, Y, 0.72, 0.27, 0.125)
    s = smin(s, sd_capsule(X, Y, 0.80, 0.385, 0.93, 0.385, 0.045), 0.06)
    s = np.minimum(s, sd_circle(X, Y, 0.555, 0.135, 0.028))
    s = np.minimum(s, sd_circle(X, Y, 0.88, 0.12, 0.018))
    s = np.minimum(s, sd_circle(X, Y, 0.60, 0.43, 0.012))
    img = render_glass(bg, X, Y, s, "dark")
    return grain(img, rng, 0.012)


def scene_panels(W, H, rng):
    """Floating rounded glass panels, like stacked UI cards."""
    glows = [(0.30, 0.25, 0.30, TURQUOISE, 0.6),
             (0.70, 0.40, 0.28, PLEX, 0.35),
             (0.55, 0.05, 0.30, PEACOCK, 0.5),
             (0.10, 0.50, 0.30, INKY, 0.6)]
    bg, X, Y = background(W, H, "dark", glows, rng)
    s = sd_rounded_rect(X, Y, 0.40, 0.24, 0.16, 0.10, 0.045)
    s = smin(s, sd_rounded_rect(X, Y, 0.60, 0.33, 0.14, 0.09, 0.045), 0.035)
    s = np.minimum(s, sd_circle(X, Y, 0.78, 0.16, 0.045))
    img = render_glass(bg, X, Y, s, "dark")
    return grain(img, rng, 0.012)


def scene_paper(W, H, rng):
    """Light variant on Paper White."""
    glows = [(0.72, 0.30, 0.30, SKY, 0.85),
             (0.86, 0.10, 0.22, PLEX, 0.30),
             (0.25, 0.50, 0.35, (226, 236, 230), 0.7)]
    bg, X, Y = background(W, H, "light", glows, rng)
    s = sd_circle(X, Y, 0.70, 0.29, 0.17)
    s = smin(s, sd_circle(X, Y, 0.84, 0.20, 0.08), 0.06)
    s = np.minimum(s, sd_circle(X, Y, 0.53, 0.42, 0.035))
    img = render_glass(bg, X, Y, s, "light", light=(-0.5, -0.9))
    return grain(img, rng, 0.010)


def scene_lock(W, H, rng):
    """Lock screen: glass sits in the lower half, leaving the clock area clear."""
    glows = [(0.52, 0.60, 0.30, TURQUOISE, 0.6),
             (0.56, 0.58, 0.14, PLEX, 0.45),
             (0.10, 0.05, 0.35, INKY, 0.6),
             (0.90, 0.55, 0.30, PEACOCK, 0.5)]
    bg, X, Y = background(W, H, "dark", glows, rng)
    s = sd_capsule(X, Y, 0.40, 0.36, 0.56, 0.36, 0.05)
    s = smin(s, sd_circle(X, Y, 0.585, 0.33, 0.075), 0.05)
    s = np.minimum(s, sd_circle(X, Y, 0.34, 0.27, 0.016))
    img = render_glass(bg, X, Y, s, "dark")
    return grain(img, rng, 0.012)


SCENES = {
    "liquid-glass-depth": scene_depth,
    "liquid-glass-panels": scene_panels,
    "liquid-glass-paper": scene_paper,
    "liquid-glass-lockscreen": scene_lock,
}


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--w", type=int, default=3840)
    ap.add_argument("--h", type=int, default=2160)
    ap.add_argument("--out", default=os.path.dirname(os.path.abspath(__file__)))
    ap.add_argument("--only", default=None)
    args = ap.parse_args()
    for name, fn in SCENES.items():
        if args.only and args.only != name:
            continue
        rng = np.random.default_rng(7)
        save(fn(args.w, args.h, rng), os.path.join(args.out, f"{name}.jpg"))


if __name__ == "__main__":
    main()
