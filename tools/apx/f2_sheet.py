#!/usr/bin/env python3
"""tools/apx/f2_sheet.py -- the D46 showcase sheet for slice F2 (apx-gen floors in barony-apx).

Reads <out>/f2.jsonl (gate_f2.py rows), measures the F6 pictures of the W1 runs with the Apockalypsis
eye instrument, draws each W1 floor top-down from the run's apxgen_floor.lmp (the tiles apx-gen
wrote, before door narrowing), computes the W2/W5/W1 gates, writes <out>/sheet/F2.json + gates.json
and renders the sheet with Apockalypsis tools/showcase.py. Game art: the sheet stays outside every repo.

Usage:
  python tools/apx/f2_sheet.py --commit <sha> [--since <iso ts>] [--out C:\\Apocrypha\\diagnostics\\barony-f2]
"""

import argparse
import datetime
import json
import struct
import subprocess
import sys
from collections import defaultdict
from pathlib import Path

from PIL import Image

APOCKALYPSIS = Path(r"C:\Users\Apocky\source\repos\Apockalypsis")


def eye(png, out_json):
    r = subprocess.run([sys.executable, str(APOCKALYPSIS / "tools" / "eye" / "eye.py"), str(png), "--quiet",
                        "--json", str(out_json)], capture_output=True, text=True)
    if r.returncode != 0:
        return {"error": r.stderr[:300]}
    d = json.loads(Path(out_json).read_text())
    return {"w": d["width"], "h": d["height"], "luma_mean": round(d["tonal"]["mean"], 2),
            "luma_std": round(d["tonal"]["std"], 2), "edge_energy": round(d["detail"]["edge_energy"], 3),
            "objects": len(d["objects"]), "verdict": d["verdict"]}


def lmp_topdown(lmp, png, scale=4):
    b = Path(lmp).read_bytes()
    off = 14 + 32 + 32
    w, h, _sky = struct.unpack_from("<III", b, off)
    off += 12 + 16 * 4
    tiles = struct.unpack_from(f"<{w * h * 3}i", b, off)
    img = Image.new("RGB", (w, h))
    px = img.load()
    for x in range(w):
        for y in range(h):
            t = tiles[y * 3 + x * 3 * h: y * 3 + x * 3 * h + 3]
            px[x, y] = (40, 40, 48) if t[1] else ((0, 0, 0) if t[0] == 0 else (200, 190, 160))
    img.resize((w * scale, h * scale), Image.NEAREST).save(png)
    return w, h


def pct(v, p):
    v = sorted(v)
    return v[min(len(v) - 1, int(p * (len(v) - 1) + 0.5))] if v else None


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--out", type=Path, default=Path(r"C:\Apocrypha\diagnostics\barony-f2"))
    ap.add_argument("--commit", required=True)
    ap.add_argument("--since", default="", help="only rows with ts >= this ISO time")
    ap.add_argument("--showcase", type=Path, default=APOCKALYPSIS / "tools" / "showcase.py")
    args = ap.parse_args()
    rows = [json.loads(l) for l in (args.out / "f2.jsonl").read_text().splitlines() if l.strip()]
    rows = [r for r in rows if r.get("ts", "") >= args.since]
    sheet = args.out / "sheet"
    sheet.mkdir(parents=True, exist_ok=True)

    floors = [r for r in rows if r["kind"] == "floor" and r["finish"] == "ok" and "w2" in r]
    normal = [r for r in floors if r["w2"]["control"] == 0]
    control = [r for r in floors if r["w2"]["control"] == 1]
    fails = [r for r in rows if r["kind"] == "floor" and (r["finish"] != "ok" or "apx_fail" in r)]

    # W2: in-engine on distinct seeds (first verdict per seed)
    first = {}
    for r in normal:
        first.setdefault(r["seed"], r)
    w2_pass = [s for s, r in first.items() if r["w2"]["verdict"] == "PASS"]
    w2_fail = {s: r["w2"] for s, r in first.items() if r["w2"]["verdict"] != "PASS"}
    ctrl_fail = [r for r in control if r["w2"]["verdict"] == "FAIL"]

    # W5: every seed seen in two or more launches has one hash
    by_seed = defaultdict(list)
    for r in normal:
        by_seed[r["seed"]].append((r["run_dir"], r["hash"], r.get("level")))
    repeated = {s: v for s, v in by_seed.items() if len({d for d, _, _ in v}) >= 2}
    w5_bad = {s: v for s, v in repeated.items() if len({h for _, h, _ in v}) != 1}
    hashes = {s: v[0][1] for s, v in by_seed.items()}
    w5_control = len(set(hashes.values())) == len(hashes)

    gen_ms = [r["ms_gen"] for r in normal if "ms_gen" in r]
    load_ms = [r["ms_load"] + r["ms_entities"] for r in normal if "ms_load" in r]
    level_s = [r["level_wall_s"] for r in normal if "level_wall_s" in r]

    # W1
    w1 = [r for r in rows if r["kind"] == "w1" and r["finish"] == "ok"]
    w1_by = {}
    for r in w1:
        size = r["env"].get("APXGEN_SIZE", "vanilla")
        w1_by[size] = r
    measured = {}
    topdown = {}
    for size, r in w1_by.items():
        for k in ("shot_arrive", "shot_hall"):
            if r.get(k):
                measured[f"{size}:{k}"] = eye(r[k], sheet / f"eye-{size}-{k}.json")
        lmp = Path(r["run_dir"]) / "apxgen_floor.lmp"
        if lmp.exists():
            topdown[size] = str(sheet / f"top-{size}.png")
            lmp_topdown(lmp, topdown[size], scale=4 if int(size) <= 96 else 3)

    def pf(ok):
        return "PASS" if ok else "FAIL"

    gates = [
        [f"W2 in-engine, {len(first)} seeds", "connected, locks after keys, locks seal, exit reachable",
         f"{len(w2_pass)}/{len(first)} pass", pf(len(first) >= 50 and not w2_fail)],
        ["W2 control (keys moved behind their locks)", "every floor fails", f"{len(ctrl_fail)}/{len(control)} fail",
         pf(control and len(ctrl_fail) == len(control))],
        [f"W5 same seed, two launches ({len(repeated)} seeds)", "one hash per seed", f"{len(w5_bad)} differ",
         pf(repeated and not w5_bad)],
        ["W5 control: different seeds", "different hashes", f"{len(set(hashes.values()))} hashes / {len(hashes)} seeds",
         pf(w5_control)],
        ["generator failures / fallbacks", "0", str(len(fails)), pf(not fails)],
    ]
    for size in ("128", "96", "48", "vanilla"):
        r = w1_by.get(size)
        if not r:
            continue
        f = r.get("fps", {})
        gates.append([f"W1 {size}: frame time in {'largest room' if size != 'vanilla' else 'arrival room'}",
                      "measured (p50/p99 fps)", f"{f.get('fps_p50')}/{f.get('fps_p99')} fps over {f.get('frames')} frames",
                      "MEASURED" if f else "MISSING"])
    (sheet / "gates.json").write_text(json.dumps({"gates": gates, "w2_fail": w2_fail, "w5_bad": w5_bad,
                                                  "fails": [r.get("error") or r.get("apx_fail") for r in fails],
                                                  "eye": measured}, indent=2))

    def w1line(size):
        r = w1_by.get(size)
        if not r:
            return f"{size}: not run"
        f = r.get("fps", {})
        sav = sum(r.get("saves", {}).values())
        return (f"{size}: map {r.get('W', '-')}x{r.get('H', '-')}, rooms {r.get('generated', {}).get('rooms')}, "
                f"monsters {r.get('generated', {}).get('monsters')}, items {r.get('generated', {}).get('items')}, "
                f"gen {r.get('ms_gen', '-')} ms + load {r.get('ms_load', '-')} ms + entities {r.get('ms_entities', '-')} ms, "
                f"level change {r.get('level_wall_s')} s wall, frame ms p50 {f.get('ms_p50')} p99 {f.get('ms_p99')}, "
                f"saves {sav} bytes {r.get('saves')}" + (f", vanilla walk tiles {r['vanilla']['walk']}" if "vanilla" in r else "")
                + (f", hall {r.get('hall')}" if r.get("hall") else ""))

    hero = w1_by.get("128") or next(iter(w1_by.values()), None)
    side, strip = [], []
    if hero and hero.get("shot_hall"):
        side.append({"img": hero["shot_hall"], "caption": f"128x128 largest room (/apxhall), F6 framebuffer; eye {measured.get('128:shot_hall')}"})
    if "128" in topdown:
        side.append({"img": topdown["128"], "caption": "128x128 tiles as apx-gen wrote them (top-down, wall dark, floor light)"})
    for size in ("96", "48", "vanilla"):
        r = w1_by.get(size)
        if r and r.get("shot_arrive"):
            strip.append({"img": r["shot_arrive"], "caption": f"{size}: arrival, F6; " + w1line(size)[:160]})
    for size in ("48",):
        if size in topdown:
            strip.append({"img": topdown[size], "caption": f"{size}x{size} tiles top-down"})

    spec = {
        "card": "F2", "title": "apx-gen floors inside barony-apx",
        "subtitle": "generateDungeon behind -apxgen: apx-gen tiles, doors, locks, keys, torches and Barony entity rows; vanilla is the control",
        "commit": args.commit, "date": datetime.date.today().isoformat(), "out_dir": str(sheet),
        "hero": {"img": (hero or {}).get("shot_arrive", ""), "caption": "128x128 floor, arrival view (F6 = glReadPixels of the back buffer); " + w1line("128")[:200]},
        "side": side, "strip": strip, "gates": gates,
        "shows": [
            "Floors from the apx-gen core played in Barony: the level set's own floor/wall/ceiling textures, Barony doors on every Lock-room opening, skeleton keys, torches, monsters from the band's monstercurve.",
            "The W2 check runs inside the game on Barony's own tiles and door entities after loadMap, not on apx-gen's grid.",
        ],
        "measured": [
            f"W2: {len(w2_pass)}/{len(first)} seeds pass; fails {w2_fail}",
            f"W2 control: {len(ctrl_fail)}/{len(control)} fail as required",
            f"W5: {len(repeated)} seeds launched twice, {len(w5_bad)} hash mismatches",
            f"apx gen ms p50 {pct(gen_ms, 0.5)} p99 {pct(gen_ms, 0.99)}; load+entities ms p50 {pct(load_ms, 0.5)} p99 {pct(load_ms, 0.99)}; "
            f"level change wall s p50 {pct(level_s, 0.5)} (n={len(gen_ms)}, 128x128)",
        ] + [w1line(s) for s in ("128", "96", "48", "vanilla")],
        "next": ["Clients: live host + client hash check and per-client bytes/s (needs two processes).",
                 "Map DRESS fixtures beyond table/bench to Barony sprites; boss per band; population calibration against vanilla walk tiles."],
    }
    (sheet / "F2.json").write_text(json.dumps(spec, indent=2))
    r = subprocess.run([sys.executable, str(args.showcase), str(sheet / "F2.json")], cwd=str(APOCKALYPSIS),
                       capture_output=True, text=True)
    print(r.stdout[-600:], r.stderr[-600:])
    for g in gates:
        print(" | ".join(map(str, g)))
    return r.returncode


if __name__ == "__main__":
    sys.exit(main())
