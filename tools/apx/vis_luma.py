#!/usr/bin/env python3
"""tools/apx/vis_luma.py -- luma and contrast of the BVIS1 screenshots by region (D84).

    python tools/apx/vis_luma.py <png> [<png> ...] [--json out.json]

Regions of a 1280x720 frame (fractions, so any size works):
  scene  the 3D view without the HUD strips: x 0-1, y 0.06-0.78
  far    the vanishing region a long view converges on: x 0.35-0.65, y 0.40-0.60
  near   the lower scene, closest geometry: x 0.10-0.90, y 0.55-0.78 (excludes the weapon column x 0.60-0.75)
Per region: mean luma (Rec. 709, 0-255), deciles p10/p50/p90, visible (luma > 8) and crushed (luma <= 2)
shares, rms contrast (std / mean), local contrast (mean |Laplacian| of luma, a 3x3 kernel).
"""
import json
import sys

import numpy as np
from PIL import Image

REGIONS = {
    "scene": (0.0, 1.0, 0.06, 0.78),
    "far": (0.35, 0.65, 0.40, 0.60),
    "near": (0.10, 0.90, 0.55, 0.78),
}


def luma(path):
    a = np.asarray(Image.open(path).convert("RGB"), dtype=np.float32)
    return 0.2126 * a[..., 0] + 0.7152 * a[..., 1] + 0.0722 * a[..., 2]


def stats(y, mask=None):
    v = y[mask] if mask is not None else y.ravel()
    lap = np.abs(4 * y[1:-1, 1:-1] - y[:-2, 1:-1] - y[2:, 1:-1] - y[1:-1, :-2] - y[1:-1, 2:])
    lapv = lap[mask[1:-1, 1:-1]] if mask is not None else lap.ravel()
    mean = float(v.mean())
    return {
        "mean": round(mean, 2),
        "p10": round(float(np.percentile(v, 10)), 1),
        "p50": round(float(np.percentile(v, 50)), 1),
        "p90": round(float(np.percentile(v, 90)), 1),
        "visible": round(float((v > 8).mean()), 4),
        "crushed": round(float((v <= 2).mean()), 4),
        "rms_contrast": round(float(v.std() / mean) if mean > 0 else 0.0, 4),
        "local_contrast": round(float(lapv.mean()), 3),
    }


def measure(path):
    y = luma(path)
    h, w = y.shape
    out = {"path": path, "size": [w, h]}
    for name, (x0, x1, y0, y1) in REGIONS.items():
        m = np.zeros_like(y, dtype=bool)
        m[int(y0 * h):int(y1 * h), int(x0 * w):int(x1 * w)] = True
        if name == "near":
            m[:, int(0.60 * w):int(0.75 * w)] = False
        out[name] = stats(y, m)
    return out


def main(argv):
    paths = [a for a in argv if not a.startswith("--")]
    js = argv[argv.index("--json") + 1] if "--json" in argv else None
    if js:
        paths = [p for p in paths if p != js]
    rows = [measure(p) for p in paths]
    for r in rows:
        print(r["path"])
        for k in REGIONS:
            s = r[k]
            print(f"  {k:<5} mean {s['mean']:6.1f}  p10 {s['p10']:5.1f} p50 {s['p50']:5.1f} p90 {s['p90']:5.1f}  "
                  f"visible {s['visible']:.3f} crushed {s['crushed']:.3f}  rms {s['rms_contrast']:.3f} local {s['local_contrast']:.2f}")
    if js:
        with open(js, "w") as f:
            json.dump(rows, f, indent=1)
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
