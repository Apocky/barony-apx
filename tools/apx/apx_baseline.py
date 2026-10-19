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


def cell(exe, floor, party, runs, skill, cls, out_root, seed, wall_s, tag_suffix=""):
    sc = out_root / "scenarios" / f"walk-f{floor}-p{party}{tag_suffix}.json"
    sc.parent.mkdir(parents=True, exist_ok=True)
    sc.write_text(json.dumps({"name": f"walk-f{floor}-p{party}{tag_suffix}", "mode": "walk", "party": party, "floor": floor,
                              "seed": seed, "runs": runs, "bot": {"skill": skill}}), encoding="ascii")
    lk = A.acquire()
    try:
        r = A.run_sim(exe, str(sc), f"base-f{floor}-p{party}{tag_suffix}", wall_s=wall_s, cls=cls)
    finally:
        A.release(lk)
    enc = r["rows"]
    runsr = read_jsonl(r["out_dir"] / "runs.jsonl")
    floors = read_jsonl(r["out_dir"] / "floors.jsonl")
    return r, enc, runsr, floors


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
    status = "ok" if len(enc) >= min_enc and r["exit"] == 0 else "insufficient"
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

    def one(fp):
        fl, pt = fp
        cls = CLASSES[(fl - 1) % len(CLASSES)]
        r, enc, runsr, floors = cell(exe, fl, pt, a.runs, a.skill, cls, out_root, a.seed + fl, a.wall_s)
        c = summarise(fl, pt, r, enc, runsr, floors, a.min_encounters)
        c["class"] = cls
        c["raw"] = str(r["run_dir"])
        print(json.dumps({k: c[k] for k in ("floor", "party", "status", "runs", "encounters", "deaths_per_floor", "wall_s")}), flush=True)
        return c

    with ThreadPoolExecutor(max_workers=3) as ex:           # each cell takes its own D70 slot
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
    sys.exit(0 if result["gate"]["cells_ok"] == len(solo) and solo else 1)


if __name__ == "__main__":
    main()
