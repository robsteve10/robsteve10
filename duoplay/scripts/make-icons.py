"""Renders DuoPlay's app icons: two rounded panels (the two screens) on a teal field."""
import os
from PIL import Image, ImageDraw, ImageFilter

OUT = os.path.join(os.path.dirname(__file__), "..", "web", "icons")


def icon(size):
    s = size * 4  # supersample
    img = Image.new("RGB", (s, s), (9, 23, 23))
    # diagonal teal gradient
    grad = Image.new("RGB", (s, s))
    px = grad.load()
    a, b = (31, 184, 205), (19, 52, 59)
    for y in range(s):
        for x in range(0, s, 4):
            t = (x + y) / (2 * s)
            c = tuple(int(a[i] * (1 - t) + b[i] * t) for i in range(3))
            for k in range(4):
                if x + k < s:
                    px[x + k, y] = c
    img.paste(grad)
    d = ImageDraw.Draw(img)
    # the two screens
    w, h = int(s * 0.22), int(s * 0.46)
    gap = int(s * 0.06)
    top = (s - h) // 2
    left = (s - (2 * w + gap)) // 2
    shadow = Image.new("L", (s, s), 0)
    ds = ImageDraw.Draw(shadow)
    for i in range(2):
        x = left + i * (w + gap)
        ds.rounded_rectangle([x, top + s * 0.02, x + w, top + h + s * 0.02], radius=int(s * 0.05), fill=120)
    shadow = shadow.filter(ImageFilter.GaussianBlur(s * 0.02))
    img.paste((4, 14, 14), mask=shadow)
    for i, fill in enumerate([(251, 250, 244), (186, 222, 221)]):
        x = left + i * (w + gap)
        d.rounded_rectangle([x, top, x + w, top + h], radius=int(s * 0.05), fill=fill)
    # a play mark spanning both panels
    cx, cy, r = s // 2, s // 2, int(s * 0.12)
    d.polygon([(cx - r * 0.7, cy - r), (cx - r * 0.7, cy + r), (cx + r, cy)], fill=(9, 23, 23))
    return img.resize((size, size), Image.LANCZOS)


if __name__ == "__main__":
    os.makedirs(OUT, exist_ok=True)
    for size in (180, 192, 512):
        icon(size).save(os.path.join(OUT, f"icon-{size}.png"), optimize=True)
    print("icons written to", os.path.abspath(OUT))
