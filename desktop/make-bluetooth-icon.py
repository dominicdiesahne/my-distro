"""Draws the Bluetooth icon of the Start menu and the Bluetooth window
(overlay/usr/share/pixmaps/my-bluetooth.png, 48x48): the Bluetooth rune in
white on a blue circle. The Adwaita icon theme only has a dark "symbolic"
one, which can't be seen on dark menus.

Needs only Python's standard library:  python make-bluetooth-icon.py
"""
import math
import os
import struct
import zlib

S = 48                       # size in pixels
OVERSAMPLE = 4               # smooth edges: 4x4 samples per pixel
HERE = os.path.dirname(os.path.abspath(__file__))
OUT = os.path.join(HERE, "overlay", "usr", "share", "pixmaps", "my-bluetooth.png")

BLUE = (0x1e, 0x6f, 0xd9)
WHITE = (0xff, 0xff, 0xff)

# The rune as line segments (in a 48x48 box): the vertical line and the
# two arrows, like the taskbar draws it (panel/my-panel.c, draw_bluetooth)
cx, cy, h, w = 24, 24, 13, 7.5
SEGMENTS = [
    ((cx - w, cy - h / 2), (cx + w, cy + h / 2)),
    ((cx + w, cy + h / 2), (cx, cy + h)),
    ((cx, cy + h), (cx, cy - h)),
    ((cx, cy - h), (cx + w, cy - h / 2)),
    ((cx + w, cy - h / 2), (cx - w, cy + h / 2)),
]
LINE = 2.0                   # half the line width


def dist(px, py, a, b):
    """Distance of point p from the segment a-b"""
    (ax, ay), (bx, by) = a, b
    dx, dy = bx - ax, by - ay
    t = max(0.0, min(1.0, ((px - ax) * dx + (py - ay) * dy) / (dx * dx + dy * dy)))
    return math.hypot(px - ax - t * dx, py - ay - t * dy)


def pixel(x, y):
    circle = rune = 0
    for i in range(OVERSAMPLE):
        for j in range(OVERSAMPLE):
            px = x + (i + 0.5) / OVERSAMPLE
            py = y + (j + 0.5) / OVERSAMPLE
            if math.hypot(px - 24, py - 24) <= 23:
                circle += 1
                if min(dist(px, py, a, b) for a, b in SEGMENTS) <= LINE:
                    rune += 1
    n = OVERSAMPLE * OVERSAMPLE
    if not circle:
        return (0, 0, 0, 0)
    t = rune / circle
    rgb = tuple(round(BLUE[k] + (WHITE[k] - BLUE[k]) * t) for k in range(3))
    return rgb + (round(255 * circle / n),)


def png(width, height, rows):
    raw = b"".join(b"\0" + bytes(c for p in row for c in p) for row in rows)

    def chunk(kind, data):
        c = kind + data
        return struct.pack(">I", len(data)) + c + struct.pack(">I", zlib.crc32(c))
    return (b"\x89PNG\r\n\x1a\n" +
            chunk(b"IHDR", struct.pack(">IIBBBBB", width, height, 8, 6, 0, 0, 0)) +
            chunk(b"IDAT", zlib.compress(raw, 9)) + chunk(b"IEND", b""))


rows = [[pixel(x, y) for x in range(S)] for y in range(S)]
os.makedirs(os.path.dirname(OUT), exist_ok=True)
with open(OUT, "wb") as f:
    f.write(png(S, S, rows))
print("wrote", OUT)
