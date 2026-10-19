"""apx_baseline.py -- BF1-6 vanilla baselines: bots walk floors 1-10 through the real dungeon.

  python tools\\apx\\apx_baseline.py --exe <barony.exe> --floors 1-10 --parties 1 --runs 40

Per floor (and party size) it runs the walk scenario (bots follow the game's own pathing to the down ladder and
fight what engages them), then aggregates encounters.jsonl and runs.jsonl into baseline.json:
time_per_floor_s p50/p90, encounters per floor, dmg_in_per_player_per_min p25/p50/p75/p90, deaths per floor,
hits_to_kill by monster type, first_contact_dist_tiles, floor_hash list. Cells below --min-encounters are
marked `insufficient` (never silently passed). Party sizes the sim cannot build yet are written as `unsupported`.

One Barony lock hold per cell with a 60 s gap after it (other lanes wait for the same machine-wide slot).
Output: C:\\Apocrypha\\diagnostics\\bf1-<run>\\ (baseline.json, per-cell raw rows) and, with --publish,
Apockalypsis data/barony-fork/baseline-vanilla.json.
"""
import argparse
import datetime
import json
import statistics
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).parent))
import apx_check as A  # noqa: E402

CLASSES = ["barbarian", "healer", "wizard", "rogue"]


def pct(xs, q):
    if not xs:
        return None
    xs = sorted(xs)
    k = (len(xs) - 1) * q
    lo, hi = int(k), min(int(k) + 1, len(xs) - 1)
    return round(xs[lo] + (xs[hi] - xs[lo]) * (k - lo), 3)


def read_jsonl(p):
    p = Path(p)
    return [json.loads(l) for l in p.read_text(encoding="ascii").splitlines() if l.strip()] if p.exists() else []


def half_width(xs):
    """95% half-width of the mean (normal approximation) and the mean."""
    n = len(xs)
    if n < 2:
        return None, (xs[0] if xs else None)
    m = statistics.mean(xs)
    return 1.96 * statistics.stdev(xs) / (n ** 0.5), m


STOP = {"tol_rel": 0.10, "min_enc": 150, "chunk": 200, "cap_runs": 600, "no_hostile_after": 200}


def cell(exe, floor, party, runs, skill, cls, out_root, seed, wall_s, tag_suffix=""):
    """D76 stop rule: walk the floor in chunks of 200 and stop when the critical number (damage taken per player per
    minute) has a 95% half-width under 10% of its mean AND n >= 150 encounters (the card minimum), or at the cap
    (--runs). A floor with fewer than 5 hostile encounters after 200 walks is `no_hostiles` and stops there."""
    enc, runsr, floors = [], [], []
    wall = 0.0
    last = None
    chunk_i = 0
    stop_reason = "cap"
    while len(runsr) < runs:
        n = min(STOP["chunk"], runs - len(runsr))
        sc = out_root / "scenarios" / f"walk-f{floor}-p{party}{tag_suffix}-c{chunk_i}.json"
        sc.parent.mkdir(parents=True, exist_ok=True)
        sc.write_text(json.dumps({"name": f"walk-f{floor}-p{party}{tag_suffix}", "mode": "walk", "party": party, "floor": floor,
                                  "seed": seed + chunk_i * 7919, "runs": n, "bot": {"skill": skill}}), encoding="ascii")
        lk = A.acquire()
        try:
            r = A.run_sim(exe, str(sc), f"base-f{floor}-p{party}{tag_suffix}" + (f"-c{chunk_i}" if chunk_i else ""), wall_s=wall_s, cls=cls)
        finally:
            A.release(lk)
        wall += r["wall_s"]
        last = r
        enc += r["rows"]
        runsr += read_jsonl(r["out_dir"] / "runs.jsonl")
        floors += read_jsonl(r["out_dir"] / "floors.jsonl")
        chunk_i += 1
        rates = [v for e in enc if e["creatures_n"] > 0 for v in e["dmg_in_per_player_per_min"]]
        hw, mean = half_width(rates)
        hostile = len([e for e in enc if e["creatures_n"] > 0])
        if r["exit"] != 0:
            stop_reason = "run_failed"
            break
        if len(runsr) >= STOP["no_hostile_after"] and hostile < 5:
            stop_reason = "no_hostiles"
            break
        if len(enc) >= STOP["min_enc"] and hw is not None and mean and hw / mean < STOP["tol_rel"]:
            stop_reason = "half_width"
            break
    last = dict(last)
    last["wall_s"] = wall
    last["stop_reason"] = stop_reason
    last["rows"] = enc
    return last, enc, runsr, floors


def summarise(floor, party, r, enc, runsr, floors, min_enc):
    ok_runs = [x for x in runsr]
    times = [x["time_on_floor_s"] for x in ok_runs if x["outcome"] == "descended"]
    outcomes = {}
    for x in ok_runs:
        outcomes[x["outcome"]] = outcomes.get(x["outcome"], 0) + 1
    rates = [v for e in enc if e["creatures_n"] > 0 for v in e["dmg_in_per_player_per_min"]]
    htk = {}
    for e in enc:
        for k, v in e["hits_to_kill_by_type"].items():
            htk.setdefault(k, []).extend(v)
    contact = [e["first_contact_dist_tiles"] for e in enc if e["first_contact_dist_tiles"] >= 0]
    deaths = sum(e["deaths"] for e in enc)
    n_runs = max(1, len(ok_runs))
    status = "ok" if len(enc) >= min_enc and r["exit"] == 0 else ("no_hostiles" if r.get("stop_reason") == "no_hostiles" else "insufficient")
    return {
        "floor": floor, "party": party, "status": status, "exit": r["exit"], "wall_s": round(r["wall_s"], 1),
        "runs": len(ok_runs), "run_outcomes": outcomes, "encounters": len(enc),
        "time_per_floor_s": {"p50": pct(times, 0.5), "p90": pct(times, 0.9), "n": len(times)},
        "encounters_per_floor": round(len(enc) / n_runs, 2),
        "dmg_in_per_player_per_min": {"p25": pct(rates, 0.25), "p50": pct(rates, 0.5), "p75": pct(rates, 0.75), "p90": pct(rates, 0.9), "n": len(rates)},
        "deaths": deaths, "deaths_per_floor": round(deaths / n_runs, 3),
        "hits_to_kill": {k: {"mean": round(statistics.mean(v), 2), "n": len(v)} for k, v in sorted(htk.items())},
        "first_contact_dist_tiles": {"p50": pct(contact, 0.5), "p90": pct(contact, 0.9), "n": len(contact)},
        "floor_hashes": [x["floor_hash"] for x in floors if x["rooms"] > 0],
        "bot_skill": runsr[0].get("bot_skill") if runsr else None,
        "stop_reason": r.get("stop_reason", "ingested"),
        "dmg_in_halfwidth_rel": (lambda hm: round(hm[0] / hm[1], 3) if hm[0] and hm[1] else None)(half_width(rates)),
        "samples_runs": len(ok_runs),
        "stuck_runs": outcomes.get("stuck", 0),
    }


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--exe", required=True)
    ap.add_argument("--floors", default="1-10")
    ap.add_argument("--parties", default="1")
    ap.add_argument("--runs", type=int, default=40, help="floor walks per cell")
    ap.add_argument("--min-encounters", type=int, default=150)
    ap.add_argument("--skill", type=float, default=0.7)
    ap.add_argument("--seed", type=int, default=2026)
    ap.add_argument("--wall-s", type=int, default=2400)
    ap.add_argument("--publish", type=Path, default=None, help="copy baseline.json here (Apockalypsis data/barony-fork/baseline-vanilla.json)")
    ap.add_argument("--workers", type=int, default=2, help="cells in parallel (each holds one D70 slot); the coordinator keeps one slot free")
    ap.add_argument("--ingest-since", default="", help="yyyymmdd-HHMMSS: reuse finished cell run dirs from an earlier driver instead of re-running them")
    ap.add_argument("--control-cell", default="", help="floor:party to re-run with skill 0.0 (degenerate-bot control)")
    ap.add_argument("--repeat-cell", default="", help="floor:party to run twice (repeatability)")
    a = ap.parse_args()
    lo, hi = (int(x) for x in a.floors.split("-")) if "-" in a.floors else (int(a.floors), int(a.floors))
    parties = [int(x) for x in a.parties.split(",")]
    exe = Path(a.exe).resolve()
    run_id = f"{datetime.datetime.now():%Y%m%d-%H%M%S}-{A.git_head()}"
    out_root = Path(r"C:\Apocrypha\diagnostics") / f"bf1-{run_id}"
    out_root.mkdir(parents=True)
    result = {"run_id": run_id, "exe": str(exe), "build": A.git_head(), "skill": a.skill, "runs_per_cell": a.runs,
              "min_encounters": a.min_encounters, "cells": [], "notes": [
                  "bots, not humans: bands are for these bots (skill recorded); calibrate against one human run before trusting absolutes",
                  "sim mode runs with buddha mode: a lethal hit is counted as a death and the run ends, the player never actually dies",
                  "steam barony_vanilla.exe cannot host the instruments: only the floor generator line is comparable (F0 parity)"]}
    from concurrent.futures import ThreadPoolExecutor
    jobs = [(fl, pt) for fl in range(lo, hi + 1) for pt in parties]

    def ingest(fl, pt):
        """A cell an earlier driver already finished: all its walks are in runs.jsonl."""
        if not a.ingest_since:
            return None
        base = Path(r"C:\Apocrypha\diagnostics\barony-f1\runs")
        for d in sorted(base.glob(f"*-base-f{fl}-p{pt}"), reverse=True):
            if d.name.split("-base-")[0].replace("-", "") < a.ingest_since.replace("-", ""):
                continue
            ro = read_jsonl(d / "apx-out" / "runs.jsonl")
            if len(ro) >= a.runs:
                enc = read_jsonl(d / "apx-out" / "encounters.jsonl")
                fls = read_jsonl(d / "apx-out" / "floors.jsonl")
                wall = (d / "apx-out" / "runs.jsonl").stat().st_mtime - d.stat().st_ctime
                return {"exit": 0, "wall_s": wall, "run_dir": d}, enc, ro, fls
        return None

    def one(fp):
        fl, pt = fp
        cls = CLASSES[(fl - 1) % len(CLASSES)]
        got = ingest(fl, pt)
        r, enc, runsr, floors = got if got else cell(exe, fl, pt, a.runs, a.skill, cls, out_root, a.seed + fl, a.wall_s)
        c = summarise(fl, pt, r, enc, runsr, floors, a.min_encounters)
        c["class"] = cls
        c["raw"] = str(r["run_dir"])
        print(json.dumps({k: c[k] for k in ("floor", "party", "status", "runs", "encounters", "deaths_per_floor", "wall_s")}), flush=True)
        return c

    with ThreadPoolExecutor(max_workers=a.workers) as ex:      # each cell takes its own D70 slot
        result["cells"] = sorted(ex.map(one, jobs), key=lambda c: (c["floor"], c["party"]))
    (out_root / "baseline.json").write_text(json.dumps(result, indent=1), encoding="ascii")
    for key, kw in (("repeat_cell", a.repeat_cell), ("control_cell", a.control_cell)):
        if not kw:
            continue
        fl, pt = (int(x) for x in kw.split(":"))
        skill = a.skill if key == "repeat_cell" else 0.0
        cls = CLASSES[(fl - 1) % len(CLASSES)]
        r, enc, runsr, floors = cell(exe, fl, pt, a.runs, skill, cls, out_root, a.seed + fl, a.wall_s, tag_suffix="-" + key)
        c = summarise(fl, pt, r, enc, runsr, floors, a.min_encounters)
        base = next((x for x in result["cells"] if x["floor"] == fl and x["party"] == pt and x["status"] != "unsupported"), None)
        if key == "repeat_cell" and base and base["dmg_in_per_player_per_min"]["p50"] and c["dmg_in_per_player_per_min"]["p50"]:
            d = abs(c["dmg_in_per_player_per_min"]["p50"] - base["dmg_in_per_player_per_min"]["p50"]) / base["dmg_in_per_player_per_min"]["p50"]
            c["repeat_p50_rel_diff"] = round(d, 3)
            c["repeat_ok"] = d <= 0.10
        if key == "control_cell" and base:
            c["deaths_ratio_vs_base"] = round(c["deaths_per_floor"] / max(base["deaths_per_floor"], 1e-9), 2) if base["deaths_per_floor"] else None
            c["bot_degenerate"] = c["deaths_per_floor"] >= 5 * base["deaths_per_floor"] if base["deaths_per_floor"] else c["deaths_per_floor"] > 0
        result[key] = c
    solo = [c for c in result["cells"] if c["status"] != "unsupported"]
    d = [c["deaths_per_floor"] for c in solo if c["status"] == "ok"]
    result["deaths_trend_non_decreasing"] = all(d[i] <= d[i + 1] + 1e-9 for i in range(len(d) - 1)) if len(d) > 1 else None
    result["gate"] = {"cells_total": len(result["cells"]), "cells_ok": len([c for c in solo if c["status"] == "ok"]),
                      "cells_unsupported": len([c for c in result["cells"] if c["status"] == "unsupported"])}
    (out_root / "baseline.json").write_text(json.dumps(result, indent=1), encoding="ascii")
    if a.publish:
        a.publish.parent.mkdir(parents=True, exist_ok=True)
        a.publish.write_text(json.dumps(result, indent=1), encoding="ascii")
    print(json.dumps(result["gate"]))
    nh = len([c for c in solo if c["status"] == "no_hostiles"])
    result["gate"]["cells_no_hostiles"] = nh
    sys.exit(0 if result["gate"]["cells_ok"] + nh == len(solo) and solo else 1)


if __name__ == "__main__":
    main()
