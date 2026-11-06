#!/usr/bin/env python3
"""Render the app icon ("Horizon") to every format the build needs.

No dependencies beyond the standard library: the icon is three flat shapes, so
it is drawn from signed distance fields with 1px anti-aliasing. icon.svg is the
same artwork for editing/reference. Run from the repo root:

    python3 assets/make_icons.py

Writes assets/icon.icns (macOS, needs `iconutil`), assets/icon.ico (Windows),
assets/icon.png (Linux / desktop entry) and content/icon.png (embedded into the
binary, set as the window icon at runtime on Windows/Linux).
"""
import math
import os
import shutil
import struct
import subprocess
import tempfile
import zlib

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)

INK = (0x14, 0x16, 0x1B)
LIGHT = (0xF4, 0xF4, 0xF1)
ACCENT = (0x4C, 0x7D, 0xFF)


def sd_round_box(px, py, cx, cy, hw, hh, r):
    qx = abs(px - cx) - hw + r
    qy = abs(py - cy) - hh + r
    outside = math.hypot(max(qx, 0.0), max(qy, 0.0))
    return outside + min(max(qx, qy), 0.0) - r


def sd_half_disc(px, py, cx, cy, r, upper):
    d_circle = math.hypot(px - cx, py - cy) - r
    d_plane = (py - cy) if upper else (cy - py)
    return max(d_circle, d_plane)


def render(size, inset):
    """RGBA bytes. Artwork is in 0..100 units; `inset` (units) leaves a
    transparent margin, as macOS icons expect (~10% each side)."""
    scale = size / 100.0  # px per unit
    k = (100.0 - 2 * inset) / 100.0  # tile size relative to the canvas
    cx, r = 50.0, 26 * k
    out = bytearray()
    for y in range(size):
        out.append(0)  # PNG filter: none
        for x in range(size):
            u = (x + 0.5) / scale  # pixel centre in artwork units
            v = (y + 0.5) / scale

            a_bg = clamp(0.5 - sd_round_box(u, v, 50, 50, 50 * k, 50 * k, 23 * k) * scale)
            if a_bg <= 0:
                out += b"\0\0\0\0"
                continue
            a_top = clamp(0.5 - sd_half_disc(u, v, cx, 50 - 3 * k, r, True) * scale)
            a_bot = clamp(0.5 - sd_half_disc(u, v, cx, 50 + 3 * k, r, False) * scale)
            col = mix(INK, LIGHT, a_top)
            col = mix(col, ACCENT, a_bot)
            out += bytes((*col, round(a_bg * 255)))
    return bytes(out)


def clamp(t):
    return 0.0 if t < 0 else 1.0 if t > 1 else t


def mix(a, b, t):
    return tuple(round(a[i] + (b[i] - a[i]) * t) for i in range(3))


def png(size, inset):
    raw = render(size, inset)

    def chunk(kind, data):
        c = kind + data
        return struct.pack(">I", len(data)) + c + struct.pack(">I", zlib.crc32(c) & 0xFFFFFFFF)

    ihdr = struct.pack(">IIBBBBB", size, size, 8, 6, 0, 0, 0)
    return b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", ihdr) + chunk(b"IDAT", zlib.compress(raw, 9)) + chunk(b"IEND", b"")


def write(path, data):
    with open(path, "wb") as f:
        f.write(data)
    print("wrote", os.path.relpath(path, ROOT))


def make_ico(path, sizes, inset):
    images = [png(s, inset) for s in sizes]
    header = struct.pack("<HHH", 0, 1, len(images))
    offset = 6 + 16 * len(images)
    entries, blobs = b"", b""
    for s, img in zip(sizes, images):
        dim = 0 if s >= 256 else s  # 0 means 256 in ICO
        entries += struct.pack("<BBBBHHII", dim, dim, 0, 0, 1, 32, len(img), offset)
        offset += len(img)
        blobs += img
    write(path, header + entries + blobs)


def make_icns(path, inset):
    if not shutil.which("iconutil"):
        print("skip icon.icns (iconutil not found; macOS only)")
        return
    with tempfile.TemporaryDirectory() as tmp:
        iconset = os.path.join(tmp, "icon.iconset")
        os.mkdir(iconset)
        cache = {}
        for base in (16, 32, 128, 256, 512):
            for mult in (1, 2):
                s = base * mult
                if s not in cache:
                    cache[s] = png(s, inset)
                name = f"icon_{base}x{base}{'@2x' if mult == 2 else ''}.png"
                with open(os.path.join(iconset, name), "wb") as f:
                    f.write(cache[s])
        subprocess.run(["iconutil", "-c", "icns", iconset, "-o", path], check=True)
        print("wrote", os.path.relpath(path, ROOT))


if __name__ == "__main__":
    MAC_INSET = 10  # macOS: tile is ~80% of the canvas, centred
    FLAT_INSET = 2  # Windows/Linux: near full-bleed tile
    make_icns(os.path.join(HERE, "icon.icns"), MAC_INSET)
    make_ico(os.path.join(HERE, "icon.ico"), [16, 24, 32, 48, 64, 128, 256], FLAT_INSET)
    write(os.path.join(HERE, "icon.png"), png(512, FLAT_INSET))
    write(os.path.join(ROOT, "content", "icon.png"), png(256, FLAT_INSET))
