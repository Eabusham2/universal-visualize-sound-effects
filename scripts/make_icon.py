#!/usr/bin/env python3
"""Generate assets/icon.ico for SoundOverlay (no third-party deps).

Draws the HUD compass - a grey ring on a dark rounded tile with a green
"footstep" marker up-left and a red "shot" marker up-right - at 16/32/48 px
and writes a multi-image 32bpp ICO.   Run:  python3 scripts/make_icon.py
"""
import math
import os
import struct

SIZES = (16, 32, 48)
DARK   = (0x14, 0x17, 0x1f)
RING   = (0x8a, 0x90, 0x9a)
GREEN  = (0x22, 0xe6, 0x82)
RED    = (0xff, 0x40, 0x50)


def pixel(x, y, s):
    """Return (B, G, R, A) for pixel (x, y) of an s*s icon."""
    cx = cy = (s - 1) / 2.0
    dx, dy = x - cx, y - cy
    dist = math.hypot(dx, dy)
    corner = s * 0.20

    inside = True
    if x < corner and y < corner:
        inside = math.hypot(corner - x, corner - y) <= corner
    elif x > s - 1 - corner and y < corner:
        inside = math.hypot(x - (s - 1 - corner), corner - y) <= corner
    elif x < corner and y > s - 1 - corner:
        inside = math.hypot(corner - x, y - (s - 1 - corner)) <= corner
    elif x > s - 1 - corner and y > s - 1 - corner:
        inside = math.hypot(x - (s - 1 - corner), y - (s - 1 - corner)) <= corner
    if not inside:
        return (0, 0, 0, 0)

    r_out = s * 0.40
    ring_w = max(1.0, s * 0.07)
    dot = max(1.2, s * 0.10)

    # two markers on the upper half of the ring (left = step, right = shot)
    mr = s * 0.24
    for ang, col in ((-135.0, GREEN), (-45.0, RED)):
        a = math.radians(ang)
        mx, my = cx + mr * math.cos(a), cy + mr * math.sin(a)
        if math.hypot(x - mx, y - my) <= dot:
            r, g, b = col
            return (b, g, r, 255)

    if abs(dist - r_out) <= ring_w / 2.0 or dist <= max(1.0, s * 0.05):
        r, g, b = RING
    else:
        r, g, b = DARK
    return (b, g, r, 255)


def image_bytes(s):
    hdr = struct.pack("<IiiHHIIiiII", 40, s, s * 2, 1, 32, 0, 0, 0, 0, 0, 0)
    xor = bytearray()
    for y in range(s - 1, -1, -1):           # bottom-up
        for x in range(s):
            xor += bytes(pixel(x, y, s))
    and_row = ((s + 31) // 32) * 4
    andmask = bytes(and_row * s)
    return hdr + bytes(xor) + andmask


def main():
    here = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
    out = os.path.join(here, "assets", "icon.ico")
    os.makedirs(os.path.dirname(out), exist_ok=True)

    images = [image_bytes(s) for s in SIZES]
    header = struct.pack("<HHH", 0, 1, len(SIZES))
    offset = 6 + 16 * len(SIZES)
    entries = b""
    for s, img in zip(SIZES, images):
        w = 0 if s >= 256 else s
        entries += struct.pack("<BBBBHHII", w, w, 0, 0, 1, 32, len(img), offset)
        offset += len(img)
    with open(out, "wb") as fh:
        fh.write(header + entries + b"".join(images))
    print("wrote", out, "(", os.path.getsize(out), "bytes )")


if __name__ == "__main__":
    main()
