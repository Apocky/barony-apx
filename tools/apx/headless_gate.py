#!/usr/bin/env python3
"""tools/apx/headless_gate.py -- gate: -apxheadless gives the same floor as a windowed run.

For each seed the farm runs floor 1 on the test path twice, windowed and headless, with F1's floor rows
(-apx-out=apx-out -> apx-out/floors.jsonl, field floor_hash). PASS when every pair has the same floor
hash and the same F0 line ("successfully generated a dungeon with ..."), and both controls differ:
  - one changed seed (first seed + 1, headless) gives another hash;
  - APX_HASH_POKE=1 (F1's knob: one wall tile of the hashed copy changed) gives another hash.
Also measured per mode while the jobs run: whether the game window was ever visible, and whether an
audio device was opened ("Audio device found" lines in log.txt).
Needs an exe that has both -apxheadless and F1's floor hash (apx/farm merged with apx/f1-instruments).

  python tools/apx/headless_gate.py --exe <barony.exe> [--seeds 10]
"""

import argparse
import ctypes
import ctypes.wintypes as wt
import json
import re
import sys
import time
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
import farm  # noqa: E402

RE_LINE = r"successfully generated a dungeon with[^.]*\."
user32 = ctypes.WinDLL("user32")


def visible_windows(pid):
    return any(user32.IsWindowVisible(h) for h in farm.windows_of(pid))


def floor1_hash(cwd):
    p = Path(cwd) / "apx-out" / "floors.jsonl"
    try:
        rows = [json.loads(l) for l in p.read_text(encoding="utf-8").splitlines() if l.strip()]
    except (OSError, ValueError):
        return None
    hits = [r for r in rows if r.get("floor") == 1 and not r.get("secret")]
    return hits[-1].get("floor_hash") if hits else None


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--exe", required=True, type=Path)
    ap.add_argument("--seeds", type=int, default=10)
    ap.add_argument("--out", type=Path, default=Path(r"C:\Apocrypha\diagnostics\barony-farm"))
    ap.add_argument("--timeout", type=float, default=240.0)
    a = ap.parse_args()
    seeds = [1234567, 7654321, 1000003, 2000029, 3000017, 4000037, 5000011, 6000043, 7000003, 8000009][: a.seeds]
    base = ["-quickstart=barbarian", "-apxfloor=1", "-apx-out=apx-out"]

    def job(seed, mode, env=None, tag=""):
        args = base + [f"-apxmapseed={seed}"] + (["-apxheadless"] if mode == "headless" else [])
        j = {"exe": str(a.exe.resolve()), "args": args, "mode": mode, "timeout_s": a.timeout, "lane": f"gate{tag}",
             "until": RE_LINE, "until_grace_s": 2.0}
        if env:
            j["env"] = env
        return j

    plan = []
    for s in seeds:
        plan += [("pair", s, "windowed", None), ("pair", s, "headless", None)]
    plan += [("seed_control", seeds[0] + 1, "headless", None), ("poke_control", seeds[0], "headless", {"APX_HASH_POKE": "1"})]
    t0 = time.time()
    ids = [(kind, s, mode, farm.submit(job(s, mode, env, "-" + kind))) for kind, s, mode, env in plan]
    farm.ensure_runner()
    seen_visible = {}
    waiting = {jid for *_, jid in ids}
    while waiting:
        for p in farm.DIRS["running"].glob("*.json"):
            r = farm.read_json(p)
            if r and r.get("id") in waiting and r.get("pid"):
                seen_visible[r["id"]] = seen_visible.get(r["id"], False) or visible_windows(r["pid"])
        waiting = {j for j in waiting if not (farm.DIRS["done"] / f"{j}.json").exists()}
        if not farm.runner_alive():
            farm.ensure_runner()
        time.sleep(0.25)
    rows = []
    for kind, s, mode, jid in ids:
        d = farm.read_json(farm.DIRS["done"] / f"{jid}.json")
        log = Path(d["cwd"]) / "log.txt" if d.get("cwd") else None
        text = log.read_text(encoding="utf-8", errors="replace") if log and log.exists() else ""
        rows.append({"kind": kind, "seed": s, "mode": mode, "id": jid, "ended_by": d.get("ended_by"),
                     "wall_s": d.get("wall_s"), "line": d.get("until_line"), "floor_hash": floor1_hash(d["cwd"]) if d.get("cwd") else None,
                     "window_visible": seen_visible.get(jid, False), "audio_devices": text.count("Audio device found"),
                     "peak_private_gb": d.get("peak_private_gb"), "exe": str(a.exe)})
    pairs = {}
    for r in rows:
        if r["kind"] == "pair":
            pairs.setdefault(r["seed"], {})[r["mode"]] = r
    pair_ok = {s: (m.get("windowed", {}).get("floor_hash") is not None
                   and m["windowed"]["floor_hash"] == m.get("headless", {}).get("floor_hash")
                   and m["windowed"]["line"] == m["headless"]["line"]) for s, m in pairs.items()}
    h0 = pairs[seeds[0]]["headless"]["floor_hash"]
    seed_ctrl = next(r for r in rows if r["kind"] == "seed_control")
    poke_ctrl = next(r for r in rows if r["kind"] == "poke_control")
    hl = [r for r in rows if r["mode"] == "headless"]
    wn = [r for r in rows if r["mode"] == "windowed"]
    summary = {
        "written": time.strftime("%Y-%m-%dT%H:%M:%S"), "exe": str(a.exe), "wall_s": round(time.time() - t0, 1),
        "pairs_equal": f"{sum(pair_ok.values())}/{len(pair_ok)}",
        "seed_control_differs": seed_ctrl["floor_hash"] is not None and seed_ctrl["floor_hash"] != h0,
        "poke_control_differs": poke_ctrl["floor_hash"] is not None and poke_ctrl["floor_hash"] != h0,
        "headless_window_ever_visible": any(r["window_visible"] for r in hl),
        "windowed_window_visible": all(r["window_visible"] for r in wn),
        "headless_audio_devices": sum(r["audio_devices"] for r in hl),
        "windowed_audio_devices_min": min((r["audio_devices"] for r in wn), default=None),
        "headless_wall_s_mean": round(sum(r["wall_s"] or 0 for r in hl) / max(1, len(hl)), 2),
        "windowed_wall_s_mean": round(sum(r["wall_s"] or 0 for r in wn) / max(1, len(wn)), 2),
        "peak_private_gb_max": max((r["peak_private_gb"] or 0 for r in rows), default=None),
    }
    summary["gate"] = "PASS" if (all(pair_ok.values()) and summary["seed_control_differs"]
                                 and summary["poke_control_differs"] and not summary["headless_window_ever_visible"]
                                 and summary["headless_audio_devices"] == 0) else "FAIL"
    a.out.mkdir(parents=True, exist_ok=True)
    with open(a.out / "gate.jsonl", "a", encoding="ascii") as f:
        for r in rows:
            f.write(json.dumps(r) + "\n")
    (a.out / "gate-summary.json").write_text(json.dumps(summary, indent=2), encoding="ascii")
    print(json.dumps(summary, indent=2))
    return 0 if summary["gate"] == "PASS" else 1


if __name__ == "__main__":
    sys.exit(main())
