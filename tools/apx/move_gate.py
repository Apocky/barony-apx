"""move_gate.py -- M1 gate for apx::move (spec 04b section 8 row M1). Real input through the QA driver (-apxqa=<dir>):
SDL key events pushed into the game's own queue; the window never takes focus or the user's mouse; one D70 slot per launch.

For each entry (normal start, -apxfloor=1) and each mode (new, control APX_MOVE_OLD=1) one launch plays a fixed route:
  walk forward, sprint, sneak, backpedal, strafe, diagonal, sprint to zero stamina, two impulses, wall slide (+ strafe into it).
The game writes one JSONL row per 50 Hz tick (APX_MOVE_LOG=<run dir>); this script computes, per entry and mode:
  speed ratio (measured / table) per state, sprint drain per second, stop at zero, impulse distance vs table, stuck ticks,
  slide ticks along a wall, frame cost p50/p99 (step + collide, microseconds), and checks them against the gate.
The CONTROL (APX_MOVE_OLD=1) must fail at least one check. Output: C:\\Apocrypha\\diagnostics\\move\\<stamp>-<entry>-<mode>\\ (JSONL, shots,
log) and gate-<stamp>.jsonl plus baseline.json (the new-mode numbers) in C:\\Apocrypha\\diagnostics\\move\\.

    python tools/apx/move_gate.py --exe build/vs2022/Release/barony.exe [--entry normal|floor1] [--no-control]
"""
import argparse
import datetime
import json
import os
import shutil
import statistics
import subprocess
import sys
import time
from pathlib import Path

HERE = Path(__file__).resolve().parent
sys.path.insert(0, str(HERE))
import apx_locks  # noqa: E402
import parity_f0 as pf  # noqa: E402

ROOT = HERE.parents[1]
DATADIR = r"C:\Program Files (x86)\Steam\steamapps\common\Barony"
OUT = Path(r"C:\Apocrypha\diagnostics\move")
TOL = 0.02  # speeds within 2 percent of the table

ROUTE = """\
# apx::move gate route (real SDL input). Setup commands only face the player and set stamina; every move is a key.
wait 6000
console /enablecheats
wait 300
console /killmonsters
wait 500
# --- walk forward (plain), from dry floor
console /apxmove_dry
wait 500
console /apxmove_face open
wait 300
kdown W
wait 1300
kup W
wait 900
# --- sprint (W + Left Shift), picture mid-run
console /apxmove_stamina 100
console /apxmove_face open
wait 300
kdown W
kdown Left Shift
wait 700
shot sprint
wait 700
kup Left Shift
kup W
wait 900
# --- sneak (Space) forward
console /apxmove_face open
wait 300
kdown Space
kdown W
wait 1300
kup W
kup Space
wait 900
# --- backpedal
console /apxmove_face open
wait 300
kdown S
wait 1000
kup S
wait 900
# --- strafe right
console /apxmove_face open_right
wait 300
kdown D
wait 1000
kup D
wait 900
# --- diagonal forward + right
console /apxmove_face open
wait 300
kdown W
kdown D
wait 1000
kup W
kup D
wait 900
# --- sprint to zero stamina (8 stamina = 2 s), key held for 3.5 s: it must stop at 0 and stay off
console /apxmove_face open
wait 300
console /apxmove_stamina 8
kdown W
kdown Left Shift
wait 3500
kup Left Shift
kup W
wait 900
# --- impulses (open floor), two sizes
console /apxmove_impulse 3 20
wait 1500
console /apxmove_impulse 2 40
wait 2000
console /apxmove_impulse 5 10
wait 1500
# --- wall slide: face a wall 40 degrees off the normal, walk into it and along it, picture mid-slide
console /apxmove_face wall
wait 300
kdown W
wait 1200
shot wallslide
wait 1400
kup W
wait 900
# --- wall slide while strafing into it
console /apxmove_face wall
wait 300
kdown W
kdown D
wait 2000
kup W
kup D
wait 800
# --- a second wall approach, other side
console /apxmove_face wall
wait 300
kdown W
kdown A
wait 2000
kup W
kup A
wait 800
quit
"""


def load_rows(path):
    rows, events = [], []
    for line in Path(path).read_text(errors="replace").splitlines():
        line = line.strip()
        if not line:
            continue
        try:
            r = json.loads(line)
        except ValueError:
            continue
        (events if "ev" in r else rows).append(r)
    return rows, events


def runs(rows, key):
    """Maximal contiguous runs of rows (by tick) with the same key(row) value."""
    out, cur = [], []
    for r in rows:
        if cur and (r["t"] != cur[-1]["t"] + 1 or key(r) != key(cur[-1])):
            out.append(cur)
            cur = []
        cur.append(r)
    if cur:
        out.append(cur)
    return out


def speed_groups(rows):
    """Steady ratio disp/exp per group: walk, sprint, sneak, backpedal, strafe, diagonal. Rows with the same state and the same wish."""
    def k(r):
        return (r["st"], round(r["fw"], 2), round(r["sd"], 2), r["sprint_key"])
    groups = {}
    for run in runs(rows, k):
        r0 = run[0]
        if r0["wl"] <= 0 or r0["st"] in ("knocked", "swim", "levitate", "crawl"):
            continue
        steady = [r for r in run[12:] if r["blk"] == 0 and r["exp"] > 0]  # 12 ticks (0.24 s) past every curve, no wall contact
        if len(steady) < 15:
            continue
        if r0["fw"] < 0 and r0["sd"] == 0:
            name = "backpedal"
        elif r0["fw"] == 0 and r0["sd"] != 0:
            name = "strafe"
        elif r0["fw"] > 0 and r0["sd"] != 0:
            name = "diagonal_" + r0["st"]
        else:
            name = r0["st"]
        ratios = [r["disp"] / r["exp"] for r in steady]
        g = groups.setdefault(name, [])
        g.append({"ratio": statistics.mean(ratios), "n": len(steady), "exp_u_s": steady[0]["exp"] * 50, "got_u_s": statistics.mean(r["disp"] for r in steady) * 50})
    return groups


def analyse(rows, events):
    res = {"rows": len(rows)}
    groups = speed_groups(rows)
    res["speed"] = {n: {"ratio": round(statistics.mean(x["ratio"] for x in v), 4), "n": sum(x["n"] for x in v),
                        "expected_u_s": round(v[0]["exp_u_s"], 2), "measured_u_s": round(statistics.mean(x["got_u_s"] for x in v), 2)} for n, v in groups.items()}
    # sprint drain
    sp = []
    starts = [e for e in events if e["ev"] == "sprint_start"]
    stops = [e for e in events if e["ev"] == "sprint_stop"]
    for a, b in zip(starts, stops):
        dur = (b["t"] - a["t"]) / 50.0
        if dur >= 0.8:
            # the start row is written after that tick's drain, so n ticks on = n - 1 drains between start and stop
            ndrain = max(1, b["t"] - a["t"] - 1) / 50.0
            sp.append({"seconds": round(dur, 2), "stam_start": a["stam"], "drained": round(a["stam"] - b["stam"], 3), "rate": round((a["stam"] - b["stam"]) / ndrain, 3),
                       "exhausted": b["exhausted"], "stam_end": b["stam"]})
    res["sprint"] = sp
    # after exhaustion with the key held: never sprint again
    bad = 0
    for b in stops:
        if b["exhausted"]:
            after = [r for r in rows if r["t"] > b["t"] and r["sprint_key"] == 1]
            for r in after:
                if r["st"] == "sprint":
                    bad += 1
                if r["sprint_key"] == 0:
                    break
    res["sprint_after_exhaustion_ticks"] = bad
    res["impulses"] = [{"speed": e["speed"], "ticks": e["ticks"], "tabled": e["tabled"], "got": e["got"], "open_run": e["open_run"],
                        "err_pct": round(100 * (e["got"] - e["tabled"]) / e["tabled"], 2) if e["tabled"] else 0} for e in events if e["ev"] == "impulse_test"]
    res["stuck_ticks"] = sum(r["stk"] for r in rows)
    res["blocked_ticks"] = sum(r["blk"] for r in rows)
    # slide: ticks in contact (a wall in the way) that still advanced at >= 30 percent of the wanted speed
    res["slide_ticks"] = sum(1 for r in rows if r["blk"] == 1 and r["exp"] > 0 and r["disp"] >= 0.3 * r["exp"])
    costs = sorted(r["cost_us"] for r in rows if r["cost_us"] > 0)
    if costs:
        res["cost_us"] = {"n": len(costs), "p50": round(costs[len(costs) // 2], 2), "p99": round(costs[min(len(costs) - 1, int(len(costs) * 0.99))], 2), "max": round(costs[-1], 2)}
    return res


def verdict(res, mode):
    checks = []

    def chk(name, ok, detail):
        checks.append({"check": name, "pass": bool(ok), "detail": detail})
    for name in ("walk", "sprint", "sneak", "backpedal", "strafe", "diagonal_walk"):
        s = res["speed"].get(name)
        chk(f"speed_{name}_within_2pct", s is not None and abs(s["ratio"] - 1.0) <= TOL, s if s else "no steady window")
    full = [s for s in res["sprint"] if not s["exhausted"]]
    zero = [s for s in res["sprint"] if s["exhausted"]]
    chk("sprint_drain_4_per_s", bool(res["sprint"]) and all(abs(s["rate"] - 4.0) <= 0.08 for s in res["sprint"]), res["sprint"])
    chk("sprint_stops_at_zero", bool(zero) and all(s["stam_end"] <= 0.05 and abs(s["seconds"] - s["stam_start"] / 4.0) <= 0.15 for s in zero), zero if zero else "no exhausted sprint")
    chk("sprint_stays_off_after_zero", bool(zero) and res["sprint_after_exhaustion_ticks"] == 0, res["sprint_after_exhaustion_ticks"])
    usable = [i for i in res["impulses"] if i["open_run"] >= i["tabled"] + 16]
    chk("impulse_distance_within_2pct", len(usable) >= 2 and all(abs(i["err_pct"]) <= 2.0 for i in usable), res["impulses"])
    chk("stuck_ticks_0", res["stuck_ticks"] == 0, res["stuck_ticks"])
    chk("wall_slide_ticks_>=40", res["slide_ticks"] >= 40, res["slide_ticks"])
    c = res.get("cost_us")
    if mode == "new":
        chk("frame_cost_p99_<_100us", c is not None and c["p99"] < 100.0, c)
    return checks


def play(exe, entry, mode, stamp, args):
    name = f"{stamp}-{entry}-{mode}"
    run = OUT / name
    run.mkdir(parents=True)
    qa = run / "qa"
    qa.mkdir()
    (qa / "cmd.txt").write_text(ROUTE, encoding="ascii")
    env = dict(os.environ, BARONY_DATADIR=DATADIR, APX_MOVE_LOG=str(run))
    if mode == "old":
        env["APX_MOVE_OLD"] = "1"
    cmd = [str(Path(exe).resolve()), "-windowed", "-quickstart=barbarian", f"-apxqa={qa}"]
    if entry == "floor1":
        cmd += ["-apxfloor=1", "-apxmapseed=101"]
    slot = apx_locks.take_slot(f"move_gate {name}", 4 * 3600, log=lambda t: print(t, flush=True))
    t0 = time.monotonic()
    try:
        p = subprocess.Popen(cmd, cwd=run, env=env)
        try:
            p.wait(args.timeout)
            ended = "exited"
        except subprocess.TimeoutExpired:
            ended = pf.stop(p)
    finally:
        slot.unlink(missing_ok=True)
    secs = time.monotonic() - t0
    jl = run / "move.jsonl"
    if not jl.exists():
        return {"entry": entry, "mode": mode, "run": str(run), "ended": ended, "error": "no move.jsonl"}
    rows, events = load_rows(jl)
    res = analyse(rows, events) if rows else {"rows": 0, "speed": {}, "sprint": [], "impulses": [], "stuck_ticks": 0, "slide_ticks": 0, "sprint_after_exhaustion_ticks": 0}
    log = (run / "log.txt").read_text(errors="replace") if (run / "log.txt").exists() else ""
    res["load_errors_apx_move"] = sum(1 for l in log.splitlines() if "[apx-move]" in l and "ERROR" in l)
    res["map_levelset"] = next((l.strip() for l in log.splitlines() if "Generating" in l), "")[:120]
    res["shots"] = [str(s) for s in sorted(qa.glob("*.png"))]
    checks = verdict(res, mode)
    return {"entry": entry, "mode": mode, "run": str(run), "ended": ended, "secs": round(secs, 1), "result": res, "checks": checks,
            "pass": all(c["pass"] for c in checks), "failed": [c["check"] for c in checks if not c["pass"]]}


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--exe", required=True)
    ap.add_argument("--entry", choices=["normal", "floor1", "both"], default="both")
    ap.add_argument("--no-control", action="store_true")
    ap.add_argument("--timeout", type=float, default=240)
    args = ap.parse_args()
    OUT.mkdir(parents=True, exist_ok=True)
    stamp = datetime.datetime.now().strftime("%Y%m%d-%H%M%S")
    entries = ["normal", "floor1"] if args.entry == "both" else [args.entry]
    modes = ["new"] + ([] if args.no_control else ["old"])
    rows = []
    for e in entries:
        for m in modes:
            print(f"[move_gate] {e} {m} ...", flush=True)
            row = play(args.exe, e, m, stamp, args)
            rows.append(row)
            print(f"[move_gate]   {e} {m}: ended={row.get('ended')} pass={row.get('pass')} failed={row.get('failed')}", flush=True)
            with open(OUT / f"gate-{stamp}.jsonl", "a", encoding="ascii") as f:
                f.write(json.dumps(row) + "\n")
    newp = [r for r in rows if r["mode"] == "new"]
    oldp = [r for r in rows if r["mode"] == "old"]
    ok_new = bool(newp) and all(r.get("pass") for r in newp)
    ok_ctl = all((not r.get("pass")) and not r.get("error") for r in oldp)
    summary = {"stamp": stamp, "exe": str(args.exe), "new_pass": ok_new, "control_fails_as_required": ok_ctl if oldp else None,
               "entries": {r["entry"] + "/" + r["mode"]: {"pass": r.get("pass"), "failed": r.get("failed")} for r in rows}}
    (OUT / f"summary-{stamp}.json").write_text(json.dumps(summary, indent=1), encoding="ascii")
    if ok_new:
        (OUT / "baseline.json").write_text(json.dumps({"stamp": stamp, "new": {r["entry"]: r["result"] for r in newp}}, indent=1), encoding="ascii")
    print("[move_gate] " + json.dumps(summary))
    print("MOVE-GATE: " + ("PASS" if ok_new and (ok_ctl or not oldp) else "FAIL"))
    sys.exit(0 if ok_new and (ok_ctl or not oldp) else 1)


if __name__ == "__main__":
    main()
