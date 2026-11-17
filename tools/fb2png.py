#!/usr/bin/env python3
"""Convert a display framebuffer dump from the firmware (serial command 'P') into a PNG.

The firmware prints

    FB-BEGIN page=<n>
    <8 lines with 256 hex digits each>
    FB-END

(SH1106/SSD1306 page layout: 8 rows of 128 bytes, each byte = 8 vertical pixels,
bit 0 at the top). This script reads a log file or stdin, takes every dump it finds
and writes one PNG per dump.

Usage:
    tools/fb2png.py serial.log -o screenshots/ [--scale 4] [--prefix screen]

Needs Pillow (pip install pillow).
"""
import argparse
import os
import sys

from PIL import Image, ImageDraw

W, H = 128, 64
ON = (90, 200, 255)      # light blue like the L1 OLED
OFF = (8, 10, 14)        # near black


def parse_dumps(lines):
    dumps, cur, page = [], None, None
    for line in lines:
        line = line.strip()
        if line.startswith("FB-BEGIN"):
            cur, page = [], None
            if "page=" in line:
                page = line.split("page=", 1)[1].strip()
        elif line == "FB-END" and cur is not None:
            if len(cur) == 8:
                dumps.append((page, cur))
            cur = None
        elif cur is not None and len(line) == 256:
            cur.append(bytes.fromhex(line))
    return dumps


def render(rows, scale, gap):
    img = Image.new("RGB", (W * scale, H * scale), OFF)
    d = ImageDraw.Draw(img)
    for r, data in enumerate(rows):
        for x in range(W):
            b = data[x]
            for bit in range(8):
                if b >> bit & 1:
                    y = r * 8 + bit
                    d.rectangle([x * scale, y * scale,
                                 (x + 1) * scale - 1 - gap, (y + 1) * scale - 1 - gap], fill=ON)
    # dark frame like the bezel
    border = 6 * scale
    framed = Image.new("RGB", (img.width + 2 * border, img.height + 2 * border), (0, 0, 0))
    framed.paste(img, (border, border))
    return framed


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("logfile", nargs="?", help="log with FB dumps (default: stdin)")
    ap.add_argument("-o", "--outdir", default=".")
    ap.add_argument("--scale", type=int, default=4)
    ap.add_argument("--prefix", default="screen")
    ap.add_argument("--last", type=int, default=0, help="only convert the last N dumps")
    ap.add_argument("--gap", type=int, default=0, help="dark gap between pixels (OLED grid look)")
    a = ap.parse_args()

    src = open(a.logfile, errors="replace") if a.logfile else sys.stdin
    dumps = parse_dumps(src)
    if a.last:
        dumps = dumps[-a.last:]
    if not dumps:
        sys.exit("no framebuffer dumps found")
    os.makedirs(a.outdir, exist_ok=True)
    gap = a.gap if a.scale > a.gap else 0
    for i, (page, rows) in enumerate(dumps, 1):
        name = f"{a.prefix}_{i:02d}" + (f"_page{page}" if page is not None else "") + ".png"
        render(rows, a.scale, gap).save(os.path.join(a.outdir, name))
        print(os.path.join(a.outdir, name))


if __name__ == "__main__":
    main()
