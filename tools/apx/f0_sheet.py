#!/usr/bin/env python3
"""tools/apx/f0_sheet.py -- the D46 showcase sheet for slice F0 from a parity_f0.py matrix.

Reads <out>/parity.jsonl and <out>/summary.json, measures every floor-1 screenshot with the
Apockalypsis eye instrument (tools/eye/eye.py, --json) plus a per-pixel comparison of each
ours/vanilla pair, writes <out>/sheet/F0.json and renders it with Apockalypsis tools/showcase.py
into <out>/sheet (sheet.png + sheet.html). Screenshots show Turning Wheel's art, so the sheet lives
outside every repository.

Usage:
  python tools/apx/f0_sheet.py --commit <sha> [--out C:\\Apocrypha\\diagnostics\\barony-f0]
"""

import argparse
import datetime
import json
import subprocess
import sys
from pathlib import Path

import numpy as np
from PIL import Image

APOCKALYPSIS = Path(r"C:\Users\Apocky\source\repos\Apockalypsis")


def eye(png, out_json):
    r = subprocess.run([sys.executable, str(APOCKALYPSIS / "tools" / "eye" / "eye.py"), str(png), "--quiet",
                        "--json", str(out_json)], capture_output=True, text=True)
    if r.returncode != 0:
        raise RuntimeError(f"eye.py failed on {png}: {r.stderr[:400]}")
    d = json.loads(Path(out_json).read_text())
    return {"w": d["width"], "h": d["height"], "luma_mean": round(d["tonal"]["mean"], 2),
            "luma_std": round(d["tonal"]["std"], 2), "crushed": round(d["tonal"]["crushed_frac"], 4),
            "sat_mean": round(d["colour"]["sat_mean"], 4), "edge_energy": round(d["detail"]["edge_energy"], 3),
            "objects": len(d["objects"]), "verdict": d["verdict"]}


def pixel_compare(a_png, b_png, overlay_png, thresh=24):
    a = np.asarray(Image.open(a_png).convert("RGB"), dtype=np.int16)
    b = np.asarray(Image.open(b_png).convert("RGB"), dtype=np.int16)
    if a.shape != b.shape:
        return {"same_size": False, "a": list(a.shape), "b": list(b.shape)}
    d = np.abs(a - b).max(axis=2)
    mse = float(((a - b).astype(np.float64) ** 2).mean())
    psnr = float("inf") if mse == 0 else 10 * np.log10(255 ** 2 / mse)
    over = (b * 0.45).astype(np.uint8)
    over[d > thresh] = (255, 70, 70)
    Image.fromarray(over).save(overlay_png)
    return {"same_size": True, "identical_frac": round(float((d == 0).mean()), 4),
            "over_thresh_frac": round(float((d > thresh).mean()), 4), "thresh": thresh,
            "mean_abs": round(float(np.abs(a - b).mean()), 3), "psnr_db": round(psnr, 2)}


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--out", type=Path, default=Path(r"C:\Apocrypha\diagnostics\barony-f0"))
    ap.add_argument("--commit", required=True)
    ap.add_argument("--showcase", type=Path, default=APOCKALYPSIS / "tools" / "showcase.py")
    args = ap.parse_args()

    summary = json.loads((args.out / "summary.json").read_text())
    rows = [json.loads(l) for l in (args.out / "parity.jsonl").read_text().splitlines() if l.strip()]
    rows = rows[-summary["runs"]:]                      # the matrix the summary describes
    sheet = args.out / "sheet"
    sheet.mkdir(parents=True, exist_ok=True)
    seeds = summary["seeds"]

    def pick(build, mode, seed, rep=1):
        for r in rows:
            if (r["build"], r["mode"], r["seed"], r["repeat"]) == (build, mode, seed, rep) and r.get("screenshot"):
                return r
        return None

    measured, pairs = {}, {}
    for r in rows:
        if r.get("screenshot"):
            key = f"{r['build']}:{r['mode']}:s{r['seed']}:r{r['repeat']}"
            measured[key] = eye(r["screenshot"], sheet / f"eye-{key.replace(':', '_')}.json")
    for s in seeds:
        a, b = pick("ours", "datadir", s), pick("vanilla", "mirror", s)
        if a and b:
            pairs[str(s)] = pixel_compare(a["screenshot"], b["screenshot"], sheet / f"diff-s{s}.png")
    (sheet / "measurements.json").write_text(json.dumps({"eye": measured, "pairs": pairs}, indent=2))

    a0, b0 = pick("ours", "datadir", seeds[0]), pick("vanilla", "mirror", seeds[0])
    line = {str(s): (summary["lines_per_seed"].get(str(s)) or ["(none)"]) for s in seeds}

    def short(l):
        return l.replace("successfully generated a dungeon with ", "").rstrip(".")

    def cap(r, what):
        m = measured.get(f"{r['build']}:{r['mode']}:s{r['seed']}:r{r['repeat']}", {})
        return (f"{what}, seed {r['seed']}: {short(r['generated_line'])}. eye.py: luma {m.get('luma_mean')}"
                f" +/- {m.get('luma_std')}, edges {m.get('edge_energy')}, {m.get('objects')} objects")

    def pf(ok):
        return "PASS" if ok else "FAIL"

    gates = []
    for s in seeds:
        ok = summary["parity_per_seed"].get(str(s), False)
        gates.append([f"same seed {s}: ours = vanilla", "one identical generation line",
                      " | ".join(short(l) for l in line[str(s)]), pf(ok)])
    gates.append(["control: other seed differs", "different lines per seed",
                  "yes" if summary["control_different_seeds_differ"] else "no",
                  pf(summary["control_different_seeds_differ"])])
    gates.append(["repeat runs agree", "same line every repeat", str(summary["repeats_agree"]),
                  pf(summary["repeats_agree"])])
    gates.append(["base data loads", "no load error vanilla lacks",
                  f"{len(summary['load_errors_only_in_ours'])} extra", pf(summary["clean_data_loads"])])
    gates.append(["Steam install untouched", "0 data files changed",
                  f"{len(summary['install_data_files_changed'])} of {summary['install_data_files_checked']}",
                  pf(not summary["install_data_files_changed"])])
    p0 = pairs.get(str(seeds[0]), {})
    # the spec asks for the pair and a measurement, not for pixel identity (player looks, torches and
    # monsters are randomised outside the map seed), so the gate is "both pictures exist and were read"
    pair_ok = bool(p0.get("same_size")) and all(
        f"{b}:{m}:s{seeds[0]}:r1" in measured for b, m in (("ours", "datadir"), ("vanilla", "mirror")))
    gates.append([f"floor 1 picture pair, seed {seeds[0]}", "both taken (F6) and read by eye.py",
                  f"{p0.get('identical_frac', 0) * 100:.1f}% px identical, PSNR {p0.get('psnr_db')} dB", pf(pair_ok)])

    spec = {
        "card": "F0", "title": "barony-apx builds and matches Steam Barony v5.0.2",
        "subtitle": "upstream master 962a5ce3 built with CMake + VS 2022 against the Steam assets; same seed, same dungeon",
        "commit": args.commit, "date": datetime.date.today().isoformat(), "out_dir": str(sheet),
        "hero": {"img": a0["screenshot"], "caption": cap(a0, "barony-apx (ours, BARONY_DATADIR = Steam install)")},
        "side": [
            {"img": b0["screenshot"], "caption": cap(b0, "barony_vanilla.exe (Steam v5.0.2)")},
            {"img": str(sheet / f"diff-s{seeds[0]}.png"),
             "caption": f"pixels differing by > {p0.get('thresh')} levels (red) over vanilla: "
                        f"{p0.get('over_thresh_frac', 0) * 100:.2f}%; mean |d| {p0.get('mean_abs')}"},
        ],
        "strip": [{"img": r["screenshot"], "caption": cap(r, f"{r['build']}:{r['mode']}")}
                  for r in [pick("ours", "datadir", seeds[-1]), pick("vanilla", "mirror", seeds[-1]),
                            pick("ours", "mirror", seeds[0])] if r],
        "gates": gates,
        "shows": [
            "Floor 1 of the same seed generated by our source build and by the shipped barony_vanilla.exe.",
            "The game reports identical room, monster, gold, item and decoration counts for each seed.",
        ],
        "measured": [f"seed {s}: " + " | ".join(line[str(s)]) for s in seeds]
                    + [f"runs {summary['runs']}, ok {summary['ok']}, gate {summary['gate']}"],
        "next": ["F1: instruments (floor and encounter JSONL, floor hash, headless sim, screenshot hook)."],
    }
    (sheet / "F0.json").write_text(json.dumps(spec, indent=2))
    r = subprocess.run([sys.executable, str(args.showcase), str(sheet / "F0.json")], cwd=str(APOCKALYPSIS),
                       capture_output=True, text=True)
    print(r.stdout[-800:], r.stderr[-800:])
    return r.returncode


if __name__ == "__main__":
    sys.exit(main())
