#!/usr/bin/env python3
"""tools/apx/f2_topdown.py -- draw an apx-gen floor top-down from a run folder: apxgen_floor.lmp (tiles
as written, before door narrowing) + apxgen_floor_entities.txt (sprite, world x, world y).

Colours: wall dark, floor light, pit black, water blue, lava red; torches yellow, doors brown,
locked doors red, monsters magenta, items green, player starts white, ladder cyan.
  python tools/apx/f2_topdown.py <run_dir> <out.png> [--crop x0,y0,x1,y1] [--scale N]
"""
import argparse
import struct
from pathlib import Path

from PIL import Image, ImageDraw

SPRITE = {1: (255, 255, 255), 2: (150, 90, 40), 3: (150, 90, 40), 4: (255, 220, 0), 5: (255, 220, 0),
          6: (255, 220, 0), 7: (255, 220, 0), 8: (0, 200, 0), 9: (230, 200, 0), 10: (220, 0, 220),
          11: (0, 220, 220), 21: (200, 120, 0), 35: (0, 120, 255), 77: (255, 0, 0)}


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("run_dir", type=Path)
    ap.add_argument("out", type=Path)
    ap.add_argument("--crop", default="")
    ap.add_argument("--scale", type=int, default=6)
    ap.add_argument("--water", type=int, default=-1)
    ap.add_argument("--lava", type=int, default=-1)
    a = ap.parse_args()
    b = (a.run_dir / "apxgen_floor.lmp").read_bytes()
    off = 14 + 64
    w, h, _ = struct.unpack_from("<III", b, off)
    off += 12 + 64
    t = struct.unpack_from(f"<{w * h * 3}i", b, off)
    s = a.scale
    img = Image.new("RGB", (w * s, h * s))
    d = ImageDraw.Draw(img)
    for x in range(w):
        for y in range(h):
            fl, wl = t[y * 3 + x * 3 * h], t[y * 3 + x * 3 * h + 1]
            c = (45, 45, 55) if wl else (0, 0, 0) if fl == 0 else (40, 80, 200) if fl == a.water else \
                (200, 40, 20) if fl == a.lava else (190, 180, 150)
            d.rectangle([x * s, y * s, x * s + s - 1, y * s + s - 1], fill=c)
    ents = a.run_dir / "apxgen_floor_entities.txt"
    if ents.exists():
        for line in ents.read_text().splitlines():
            sp, ex, ey = map(int, line.split())
            if sp not in SPRITE:
                continue
            px, py = ex / 16 * s, ey / 16 * s
            r = max(1, s // 3)
            d.ellipse([px - r, py - r, px + r, py + r], fill=SPRITE[sp])
    if a.crop:
        x0, y0, x1, y1 = map(int, a.crop.split(","))
        img = img.crop((x0 * s, y0 * s, x1 * s, y1 * s))
    img.save(a.out)
    print(a.out, img.size)


if __name__ == "__main__":
    main()
