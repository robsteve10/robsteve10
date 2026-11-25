"""Renders DuoPlay's generated assets.

    python3 scripts/make-assets.py

  web/icons/icon-{180,192,512,1024}.png  app icon: two screen panels on a deep teal field
  web/splash/*.png                       launch images for both iPhone Duo screens. They show the
                                         first frame of the in-app launch animation (one folded
                                         panel), so the system splash hands over to it seamlessly.
  web/media/hdr-check.mp4                HDR10 (HEVC Main10, PQ, BT.2020) test clip: a 203-nit
                                         reference white tile next to bars from 300 to 1600 nits.
"""
import os
import subprocess

import numpy as np
from PIL import Image, ImageDraw, ImageFilter

ROOT = os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "web")
INK = (6, 17, 19)


def lerp(a, b, t):
    return tuple(int(a[i] + (b[i] - a[i]) * t) for i in range(3))


def panel_image(w, h, radius):
    """A glossy screen panel: vertical pale-teal gradient with a soft top highlight."""
    grad = np.zeros((h, w, 3), np.float32)
    top, bottom = np.array((236, 248, 246), np.float32), np.array((150, 208, 205), np.float32)
    t = np.linspace(0, 1, h)[:, None]
    grad[:] = (top * (1 - t) + bottom * t)[:, None, :][:, 0, :][:, None, :]
    img = Image.fromarray(grad.astype(np.uint8), "RGB")
    mask = Image.new("L", (w, h), 0)
    ImageDraw.Draw(mask).rounded_rectangle([0, 0, w - 1, h - 1], radius=radius, fill=255)
    return img, mask


def icon(size):
    s = size * 2
    y, x = np.mgrid[0:s, 0:s].astype(np.float32) / s
    base = np.array(INK, np.float32)
    teal = np.array((22, 92, 101), np.float32)
    glow = np.exp(-(((x - 0.5) ** 2) / 0.10 + ((y - 0.55) ** 2) / 0.10))[..., None]
    diag = ((x + y) / 2)[..., None]
    rgb = base * (0.55 + 0.45 * diag) + teal * glow * 0.9
    img = Image.fromarray(np.clip(rgb, 0, 255).astype(np.uint8), "RGB")
    pw, ph = int(s * 0.21), int(s * 0.44)
    gap = int(s * 0.045)
    left = (s - (2 * pw + gap)) // 2
    top = (s - ph) // 2
    shadow = Image.new("L", (s, s), 0)
    sd = ImageDraw.Draw(shadow)
    for i in range(2):
        px = left + i * (pw + gap)
        sd.rounded_rectangle([px, top + s * 0.025, px + pw, top + ph + s * 0.025], radius=int(s * 0.05), fill=150)
    img.paste((0, 6, 8), mask=shadow.filter(ImageFilter.GaussianBlur(s * 0.025)))
    p, m = panel_image(pw, ph, int(s * 0.05))
    for i in range(2):
        img.paste(p, (left + i * (pw + gap), top), m)
    # the fold: a thin teal seam of light between the panels
    seam = Image.new("L", (s, s), 0)
    ImageDraw.Draw(seam).rounded_rectangle([s // 2 - 2, top + ph * 0.1, s // 2 + 2, top + ph * 0.9], radius=2, fill=220)
    img.paste((31, 184, 205), mask=seam.filter(ImageFilter.GaussianBlur(s * 0.006)))
    return img.resize((size, size), Image.LANCZOS)


def splash(w_pt, h_pt, scale=3):
    """First frame of the launch animation: one folded panel left of centre (see index.html)."""
    w, h = int(w_pt * scale), int(h_pt * scale)
    img = Image.new("RGB", (w, h), INK)
    pw, ph, r = 52 * scale, 104 * scale, 12 * scale
    cx, cy = w // 2, h // 2
    x0 = cx - 57 * scale
    y0 = cy - 52 * scale
    shadow = Image.new("L", (w, h), 0)
    ImageDraw.Draw(shadow).rounded_rectangle([x0, y0 + 10 * scale, x0 + pw, y0 + ph + 10 * scale], radius=r, fill=115)
    img.paste((0, 0, 0), mask=shadow.filter(ImageFilter.GaussianBlur(15 * scale)))
    p, m = panel_image(pw, ph, r)
    img.paste(p, (x0, y0), m)
    return img


# ---------------------------------------------------------------- HDR10 test clip
M1, M2 = 2610 / 16384, 2523 / 4096 * 128
C1, C2, C3 = 3424 / 4096, 2413 / 4096 * 32, 2392 / 4096 * 32


def pq(nits):
    y = np.clip(np.asarray(nits, np.float64) / 10000.0, 0, 1)
    ym = y ** M1
    return ((C1 + C2 * ym) / (1 + C3 * ym)) ** M2


def hdr_frames(w=1280, h=720, frames=120):
    """Yields yuv420p10le frames (limited range, BT.2020 matrix)."""
    yy, xx = np.mgrid[0:h, 0:w].astype(np.float32)
    bars = [300, 500, 800, 1200, 1600]
    for f in range(frames):
        t = f / frames
        r = np.full((h, w), 4.0, np.float32)  # dim background, nits
        g = r.copy()
        b = r.copy()
        # reference white (BT.2408 graphics white, 203 nits)
        tile = (xx > w * 0.05) & (xx < w * 0.27) & (yy > h * 0.2) & (yy < h * 0.8)
        for c in (r, g, b):
            c[tile] = 203
        # bars brighter than SDR white
        for i, nits in enumerate(bars):
            x0 = w * (0.34 + i * 0.125)
            m = (xx > x0) & (xx < x0 + w * 0.09) & (yy > h * 0.2) & (yy < h * 0.8)
            for c in (r, g, b):
                c[m] = nits
        # a warm "sun" sweeping across the bottom, peaking at 1000 nits
        sx, sy = w * (0.1 + 0.8 * (0.5 - 0.5 * np.cos(2 * np.pi * t))), h * 0.9
        d = np.hypot(xx - sx, yy - sy)
        core = np.exp(-(d / 18) ** 2) * 1000 + np.exp(-(d / 60) ** 2) * 120
        r += core
        g += core * 0.78
        b += core * 0.45
        rp, gp, bp = pq(r), pq(g), pq(b)
        yp = 0.2627 * rp + 0.6780 * gp + 0.0593 * bp
        cb = (bp - yp) / 1.8814
        cr = (rp - yp) / 1.4746
        Y = np.clip(np.round(64 + 876 * yp), 64, 940).astype(np.uint16)
        cb2 = cb.reshape(h // 2, 2, w // 2, 2).mean(axis=(1, 3))
        cr2 = cr.reshape(h // 2, 2, w // 2, 2).mean(axis=(1, 3))
        U = np.clip(np.round(512 + 896 * cb2), 64, 960).astype(np.uint16)
        V = np.clip(np.round(512 + 896 * cr2), 64, 960).astype(np.uint16)
        yield Y.tobytes() + U.tobytes() + V.tobytes()


def make_hdr_clip(path, w=1280, h=720, fps=30, frames=120):
    x265 = ("hdr10=1:hdr10-opt=1:repeat-headers=1:colorprim=bt2020:transfer=smpte2084:colormatrix=bt2020nc:range=limited:"
            "master-display=G(13250,34500)B(7500,3000)R(34000,16000)WP(15635,16450)L(16000000,1):max-cll=1600,400:"
            "keyint=60:log-level=error")
    cmd = ["ffmpeg", "-hide_banner", "-loglevel", "error", "-y",
           "-f", "rawvideo", "-pix_fmt", "yuv420p10le", "-s", f"{w}x{h}", "-r", str(fps), "-i", "-",
           "-c:v", "libx265", "-profile:v", "main10", "-preset", "medium", "-crf", "20", "-x265-params", x265,
           "-pix_fmt", "yuv420p10le", "-color_primaries", "bt2020", "-color_trc", "smpte2084", "-colorspace", "bt2020nc",
           "-color_range", "tv", "-tag:v", "hvc1", "-movflags", "+faststart", path]
    p = subprocess.Popen(cmd, stdin=subprocess.PIPE)
    for frame in hdr_frames(w, h, frames):
        p.stdin.write(frame)
    p.stdin.close()
    if p.wait() != 0:
        raise SystemExit("ffmpeg failed")


if __name__ == "__main__":
    os.makedirs(os.path.join(ROOT, "icons"), exist_ok=True)
    for size in (180, 192, 512, 1024):
        icon(size).save(os.path.join(ROOT, "icons", f"icon-{size}.png"), optimize=True)
    os.makedirs(os.path.join(ROOT, "splash"), exist_ok=True)
    for name, (w, h) in {"cover-portrait": (466, 678), "cover-landscape": (678, 466),
                         "inner-portrait": (669, 951), "inner-landscape": (951, 669)}.items():
        splash(w, h).save(os.path.join(ROOT, "splash", f"{name}.png"), optimize=True)
    os.makedirs(os.path.join(ROOT, "media"), exist_ok=True)
    make_hdr_clip(os.path.join(ROOT, "media", "hdr-check.mp4"))
    print("assets written under", os.path.abspath(ROOT))
