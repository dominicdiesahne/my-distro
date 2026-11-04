"""Draws the my-distro wallpaper (1920x1080 PNG) in the desktop's teal colours.

Run on any machine with Pillow:  python make-wallpaper.py <output.png>
The result is checked in as overlay/usr/share/backgrounds/my-distro.png.
"""
import math
import sys

from PIL import Image, ImageDraw, ImageFilter, ImageFont

W, H = 1920, 1080
out = sys.argv[1] if len(sys.argv) > 1 else "my-distro.png"


def mix(a, b, t):
    return tuple(round(a[i] + (b[i] - a[i]) * t) for i in range(3))


# 1. Diagonal gradient: deep teal top-left -> lighter teal bottom-right
dark, mid, light = (4, 24, 26), (11, 48, 50), (20, 82, 82)
grad = Image.new("RGB", (W, H))
px = grad.load()
for y in range(H):
    for x in range(W):
        t = (x / W) * 0.6 + (y / H) * 0.4
        px[x, y] = mix(dark, mid, t / 0.6) if t < 0.6 else mix(mid, light, (t - 0.6) / 0.4)
img = grad.convert("RGBA")

# 2. Soft glows of light
glow = Image.new("RGBA", (W, H), (0, 0, 0, 0))
g = ImageDraw.Draw(glow)
g.ellipse((1050, -250, 1850, 550), fill=(79, 209, 197, 70))
g.ellipse((-300, 600, 500, 1400), fill=(44, 122, 123, 90))
g.ellipse((700, 350, 1200, 850), fill=(79, 209, 197, 25))
img = Image.alpha_composite(img, glow.filter(ImageFilter.GaussianBlur(180)))

# 3. Thin concentric rings around the right glow
rings = Image.new("RGBA", (W, H), (0, 0, 0, 0))
r = ImageDraw.Draw(rings)
cx, cy = 1450, 150
for i, rad in enumerate(range(220, 1300, 90)):
    alpha = max(10, 60 - i * 5)
    r.ellipse((cx - rad, cy - rad, cx + rad, cy + rad),
              outline=(129, 230, 217, alpha), width=2)
img = Image.alpha_composite(img, rings.filter(ImageFilter.GaussianBlur(0.8)))

# 4. A sweep of small dots along a curve
dots = Image.new("RGBA", (W, H), (0, 0, 0, 0))
d = ImageDraw.Draw(dots)
for k in range(140):
    t = k / 139
    x = 120 + t * 1700
    y = 820 - math.sin(t * math.pi) * 380 + math.sin(t * 9) * 18
    s = 1.5 + 2.5 * math.sin(t * math.pi)
    d.ellipse((x - s, y - s, x + s, y + s), fill=(216, 243, 240, int(40 + 90 * math.sin(t * math.pi))))
img = Image.alpha_composite(img, dots)

# 5. Wordmark, bottom right (above the taskbar)
text = Image.new("RGBA", (W, H), (0, 0, 0, 0))
tdraw = ImageDraw.Draw(text)
font = None
for name in ("segoeuil.ttf", "segoeui.ttf", "DejaVuSans-ExtraLight.ttf", "DejaVuSans.ttf"):
    try:
        font = ImageFont.truetype(name, 64)
        break
    except OSError:
        pass
font = font or ImageFont.load_default()
label = "my-distro"
tw = tdraw.textlength(label, font=font)
tdraw.text((W - 260 - tw, H - 190), label, font=font, fill=(216, 243, 240, 150))
tdraw.rectangle((W - 260 - tw, H - 84, W - 260 - tw + 90, H - 81), fill=(79, 209, 197, 200))
img = Image.alpha_composite(img, text)

img.convert("RGB").save(out, optimize=True)
print("wrote", out)
