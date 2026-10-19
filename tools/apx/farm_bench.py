#!/usr/bin/env python3
"""tools/apx/farm_bench.py -- throughput of floor jobs, before vs after the farm.

A floor job = test path to floor 1 for one seed, closed when the F0 line appears.
  before: how lanes ran until now -- one windowed launch at a time (submit, wait, next);
  after:  all jobs submitted at once, headless; the farm runs them under its capacity and memory budget.
Reports jobs per hour (MEASURED wall time), mean wall per job, the highest number running at once, and
the peak private bytes per game. Rows -> <out>/bench.jsonl, summary -> <out>/bench-summary.json.

  python tools/apx/farm_bench.py --exe <barony.exe> [--n 24] [--phase before|after|both]
"""

import argparse
import json
import statistics
import sys
import time
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
import farm  # noqa: E402

RE_LINE = r"successfully generated a dungeon with[^.]*\."


def floor_job(exe, seed, mode, timeout):
    args = ["-quickstart=barbarian", "-apxfloor=1", f"-apxmapseed={seed}"] + (["-apxheadless"] if mode == "headless" else [])
    return {"exe": str(exe), "args": args, "mode": mode, "timeout_s": timeout, "lane": f"bench-{mode}",
            "until": RE_LINE, "until_grace_s": 0.2}


def wait_all(ids, sample):
    peak = 0
    left = set(ids)
    while left:
        running = [p for p in farm.DIRS["running"].glob("*.json")]
        peak = max(peak, sum(1 for p in running if p.stem in ids))
        left = {j for j in left if not (farm.DIRS["done"] / f"{j}.json").exists()}
        if not farm.runner_alive():
            farm.ensure_runner()
        time.sleep(sample)
    return peak


def phase(exe, seeds, mode, serial, timeout):
    t0 = time.time()
    ids, peak = [], 0
    if serial:
        for s in seeds:
            jid = farm.submit(floor_job(exe, s, mode, timeout))
            farm.ensure_runner()
            peak = max(peak, wait_all([jid], 0.2))
            ids.append(jid)
    else:
        ids = [farm.submit(floor_job(exe, s, mode, timeout)) for s in seeds]
        farm.ensure_runner()
        peak = wait_all(ids, 0.2)
    wall = time.time() - t0
    done = [farm.read_json(farm.DIRS["done"] / f"{j}.json") for j in ids]
    ok = [d for d in done if d and d.get("ended_by") == "until"]
    mem = [d["peak_private_gb"] for d in done if d and d.get("peak_private_gb")]
    return {"mode": mode, "serial": serial, "jobs": len(ids), "ok": len(ok), "wall_s": round(wall, 1),
            "jobs_per_hour": round(len(ok) / (wall / 3600.0), 1) if wall > 0 else None,
            "job_wall_s_mean": round(statistics.mean(d["wall_s"] for d in ok), 2) if ok else None,
            "peak_running": peak, "peak_private_gb_median": round(statistics.median(mem), 3) if mem else None,
            "peak_private_gb_max": round(max(mem), 3) if mem else None,
            "lines": sorted({d.get("until_line") for d in ok})[:3], "ids": ids}


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--exe", required=True, type=Path)
    ap.add_argument("--n", type=int, default=24)
    ap.add_argument("--phase", choices=("before", "after", "both"), default="both")
    ap.add_argument("--timeout", type=float, default=240.0)
    ap.add_argument("--out", type=Path, default=Path(r"C:\Apocrypha\diagnostics\barony-farm"))
    a = ap.parse_args()
    seeds = [1000 + 7919 * i for i in range(a.n)]
    exe = a.exe.resolve()
    res = {}
    if a.phase in ("before", "both"):
        res["before"] = phase(exe, seeds, "windowed", True, a.timeout)
    if a.phase in ("after", "both"):
        res["after"] = phase(exe, seeds, "headless", False, a.timeout)
    summary = {k: {kk: vv for kk, vv in v.items() if kk != "ids"} for k, v in res.items()}
    if "before" in res and "after" in res and res["before"]["jobs_per_hour"]:
        summary["speedup"] = round(res["after"]["jobs_per_hour"] / res["before"]["jobs_per_hour"], 2)
        summary["same_lines"] = res["before"]["lines"] == res["after"]["lines"]
    summary["written"] = time.strftime("%Y-%m-%dT%H:%M:%S")
    summary["exe"] = str(exe)
    a.out.mkdir(parents=True, exist_ok=True)
    with open(a.out / "bench.jsonl", "a", encoding="ascii") as f:
        for k, v in res.items():
            f.write(json.dumps(dict(v, phase=k)) + "\n")
    (a.out / "bench-summary.json").write_text(json.dumps(summary, indent=2), encoding="ascii")
    print(json.dumps(summary, indent=2))
    return 0


if __name__ == "__main__":
    sys.exit(main())
