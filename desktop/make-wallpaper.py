"""Draws the my-distro wallpapers (1920x1080 PNG), one per color theme, and
the Clock app's menu icon.

Run on any machine with Pillow:  python make-wallpaper.py
The results are checked in under overlay/usr/share/backgrounds/
(my-distro-<theme>.png) and overlay/usr/share/pixmaps/my-clock.png.
The theme colors match settings/settings.c.
"""
import math
import os

from PIL import Image, ImageDraw, ImageFilter, ImageFont

W, H = 1920, 1080
HERE = os.path.dirname(os.path.abspath(__file__))
BACKGROUNDS = os.path.join(HERE, "overlay", "usr", "share", "backgrounds")
PIXMAPS = os.path.join(HERE, "overlay", "usr", "share", "pixmaps")


def rgb(hex_color):
    return tuple(int(hex_color[i:i + 2], 16) for i in (1, 3, 5))


# name: (gradient dark, mid, light), accent, second glow, text
THEMES = {
    "teal":   (("#04181a", "#0b3032", "#145252"), "#4fd1c5", "#2c7a7b", "#d8f3f0"),
    "ocean":  (("#040c18", "#0b1f38", "#14355a"), "#5aa9ff", "#2c5a85", "#dce9f8"),
    "violet": (("#0c0818", "#1d1438", "#33245c"), "#b48cff", "#503d8a", "#ece6fb"),
    "ember":  (("#140806", "#2e130e", "#552218"), "#ff7a59", "#7d3a2e", "#fbe7e2"),
    "forest": (("#05120a", "#0f2a19", "#1b4a2c"), "#6ee7a0", "#316a46", "#e1f4e7"),
    "light":  (("#f6f9f9", "#e6efef", "#cfe2e0"), "#138a80", "#7fc4bd", "#12302f"),
}


def mix(a, b, t):
    return tuple(round(a[i] + (b[i] - a[i]) * t) for i in range(3))


def font(size):
    for name in ("segoeuil.ttf", "segoeui.ttf", "DejaVuSans-ExtraLight.ttf", "DejaVuSans.ttf"):
        try:
            return ImageFont.truetype(name, size)
        except OSError:
            pass
    return ImageFont.load_default()


def wallpaper(name, grad_colors, accent, glow2, text_color, out):
    dark, mid, light = (rgb(c) for c in grad_colors)
    acc, g2, txt = rgb(accent), rgb(glow2), rgb(text_color)
    is_light = name == "light"

    # 1. Diagonal gradient, top-left -> bottom-right (drawn small, then scaled:
    #    a gradient has no detail, and this is 100x faster than every pixel)
    sw, sh = W // 8, H // 8
    grad = Image.new("RGB", (sw, sh))
    px = grad.load()
    for y in range(sh):
        for x in range(sw):
            t = (x / sw) * 0.6 + (y / sh) * 0.4
            px[x, y] = mix(dark, mid, t / 0.6) if t < 0.6 else mix(mid, light, (t - 0.6) / 0.4)
    img = grad.resize((W, H), Image.BILINEAR).convert("RGBA")

    # 2. Soft glows of light
    glow = Image.new("RGBA", (W, H), (0, 0, 0, 0))
    g = ImageDraw.Draw(glow)
    g.ellipse((1050, -250, 1850, 550), fill=acc + (45 if is_light else 70,))
    g.ellipse((-300, 600, 500, 1400), fill=g2 + (70 if is_light else 90,))
    g.ellipse((700, 350, 1200, 850), fill=acc + (25,))
    img = Image.alpha_composite(img, glow.filter(ImageFilter.GaussianBlur(180)))

    # 3. Thin concentric rings around the right glow
    rings = Image.new("RGBA", (W, H), (0, 0, 0, 0))
    r = ImageDraw.Draw(rings)
    ring = mix(acc, (255, 255, 255), 0.3) if not is_light else acc
    cx, cy = 1450, 150
    for i, rad in enumerate(range(220, 1300, 90)):
        alpha = max(10, 60 - i * 5)
        r.ellipse((cx - rad, cy - rad, cx + rad, cy + rad), outline=ring + (alpha,), width=2)
    img = Image.alpha_composite(img, rings.filter(ImageFilter.GaussianBlur(0.8)))

    # 4. A sweep of small dots along a curve
    dots = Image.new("RGBA", (W, H), (0, 0, 0, 0))
    d = ImageDraw.Draw(dots)
    dot = acc if is_light else txt
    for k in range(140):
        t = k / 139
        x = 120 + t * 1700
        y = 820 - math.sin(t * math.pi) * 380 + math.sin(t * 9) * 18
        s = 1.5 + 2.5 * math.sin(t * math.pi)
        d.ellipse((x - s, y - s, x + s, y + s), fill=dot + (int(40 + 90 * math.sin(t * math.pi)),))
    img = Image.alpha_composite(img, dots)

    # 5. Wordmark, bottom right (above the taskbar)
    layer = Image.new("RGBA", (W, H), (0, 0, 0, 0))
    tdraw = ImageDraw.Draw(layer)
    f = font(64)
    label = "my-distro"
    tw = tdraw.textlength(label, font=f)
    tdraw.text((W - 260 - tw, H - 190), label, font=f, fill=txt + (150,))
    tdraw.rectangle((W - 260 - tw, H - 84, W - 260 - tw + 90, H - 81), fill=acc + (200,))
    img = Image.alpha_composite(img, layer)

    img.convert("RGB").save(out, optimize=True)
    print("wrote", out)


def clock_icon(out, size=48):
    """The Clock app's icon: the teal clock face at 10:10, like settings/dial.c."""
    k = 8                                  # draw 8x bigger, then shrink: smooth edges
    s = size * k
    img = Image.new("RGBA", (s, s), (0, 0, 0, 0))
    d = ImageDraw.Draw(img)
    c, r = s / 2, s / 2 - k
    d.ellipse((c - r, c - r, c + r, c + r), fill=rgb("#134e4f"), outline=rgb("#4fd1c5"), width=3 * k)
    for i in range(12):
        a = i * math.pi / 6
        r1, w = (0.62, 4 * k) if i % 3 == 0 else (0.74, 2 * k)
        d.line((c + math.sin(a) * r * r1, c - math.cos(a) * r * r1,
                c + math.sin(a) * r * 0.86, c - math.cos(a) * r * 0.86),
               fill=rgb("#d8f3f0") if i % 3 == 0 else rgb("#7fa9a6"), width=w)
    for frac, length, width in ((10 + 10 / 60) / 12, 0.5, 5 * k), (10 / 60, 0.76, 3 * k):
        a = frac * 2 * math.pi
        d.line((c, c, c + math.sin(a) * r * length, c - math.cos(a) * r * length),
               fill=rgb("#d8f3f0"), width=width)
    d.ellipse((c - 3 * k, c - 3 * k, c + 3 * k, c + 3 * k), fill=rgb("#4fd1c5"))
    img.resize((size, size), Image.LANCZOS).save(out)
    print("wrote", out)


if __name__ == "__main__":
    os.makedirs(BACKGROUNDS, exist_ok=True)
    os.makedirs(PIXMAPS, exist_ok=True)
    for name, (grad, accent, glow2, text) in THEMES.items():
        wallpaper(name, grad, accent, glow2, text,
                  os.path.join(BACKGROUNDS, "my-distro-%s.png" % name))
    clock_icon(os.path.join(PIXMAPS, "my-clock.png"))
