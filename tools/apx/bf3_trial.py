#!/usr/bin/env python3
"""tools/apx/bf3_trial.py -- run the BF3 natural-limits trial under the machine's Barony lock and summarise it.

Each run: wait until no *.LOCK is in the lock dir and no Barony process runs (checked every 60 s), take
<lock-dir>/barony-bf3.LOCK, launch barony.exe from an EMPTY run folder with BARONY_DATADIR set at run time
(never at configure time), wait for it to exit (kill on timeout), delete the lock. The game itself runs
the trial (-apx-bf3trial, see src/apx/bf3trial.cpp) and rejects human input (dropped and counted).

Output: every row of every run plus one baseline row to C:/Apocrypha/diagnostics/bf3-<tag>.jsonl, and a
printed table. Exit 1 if a run failed, a row is flagged, or the control did not fail (limits off must
take more damage than limits on).

  python tools/apx/bf3_trial.py --exe build/vs2022/Release/barony.exe --runs 3 --tag limits3x
"""
import argparse
import json
import math
import os
import subprocess
import sys
import time
from pathlib import Path

STEAM = Path(r"C:\Program Files (x86)\Steam\steamapps\common\Barony")
LOCK_DIR = Path(r"C:\Apocrypha\locks\barony")
RUNS = Path(r"C:\Apocrypha\runs")
DIAG = Path(r"C:\Apocrypha\diagnostics")
LOCK_NAME = "barony-bf3.LOCK"


def barony_running():
    out = subprocess.run(["tasklist", "/FO", "CSV", "/NH"], capture_output=True, text=True).stdout.lower()
    return [n for n in ("barony.exe", "barony_vanilla.exe", "editor.exe") if f'"{n}"' in out]


# Lanes on this machine use more than one lock folder: the shared C:/Apocrypha/locks/barony, its parent
# (barony-f2-gate.LOCK lives there) and the side-session notes folder of the S-track era. Honour them all.
EXTRA_LOCK_DIRS = [Path(r"C:\Apocrypha\locks"),
                   Path(r"C:\Users\Apocky\AppData\Roaming\Claude\side-session-notes\local_72659dda-e45a-46f2-b952-7b8cbf18fac8")]


def other_locks(lock_dir):
    found = []
    for d in [lock_dir] + EXTRA_LOCK_DIRS:
        if d.exists():
            found += [f"{d.name}/{p.name}" for p in d.glob("*.LOCK") if not (d == lock_dir and p.name == LOCK_NAME)]
    return found


def take_lock(lock_dir, limit_s):
    t0 = time.time()
    while True:
        busy = barony_running() + other_locks(lock_dir)
        if not busy:
            lock_dir.mkdir(parents=True, exist_ok=True)
            lock = lock_dir / LOCK_NAME
            lock.write_text(f"{time.strftime('%Y-%m-%d %H:%M:%S')} pid {os.getpid()} barony-bf3: "
                            f"barony-apx tools/apx/bf3_trial.py\n", encoding="ascii")
            time.sleep(1.0)
            busy = barony_running() + other_locks(lock_dir)
            if not busy:
                return lock
            lock.unlink(missing_ok=True)
        if time.time() - t0 > limit_s:
            sys.exit(f"Barony busy ({', '.join(busy)}) for {limit_s:.0f} s; giving up")
        print(f"[bf3] waiting: {', '.join(busy)}", flush=True)
        time.sleep(60)


def read_rows(p):
    rows = []
    if p.exists():
        for line in p.read_text(encoding="ascii", errors="replace").splitlines():
            line = line.strip()
            if line:
                rows.append(json.loads(line))
    return rows


def run_once(args, n, env_extra):
    run_dir = RUNS / f"bf3-{args.tag}-{n}"
    k = 0
    while run_dir.exists() and any(run_dir.iterdir()):
        k += 1
        run_dir = RUNS / f"bf3-{args.tag}-{n}-{k}"
    run_dir.mkdir(parents=True, exist_ok=True)
    out = run_dir / "apx"
    env = dict(os.environ)
    env["BARONY_DATADIR"] = str(STEAM)
    env["APX_BUILD_HASH"] = args.build_hash
    env.update(env_extra)
    cmd = [str(Path(args.exe).resolve()), "-quickstart=barbarian", f"-apx-out={out}", f"-apx-seed={args.seed}", "-apx-bf3trial"]
    lock = take_lock(Path(args.lock_dir), args.wait_limit)
    t0 = time.time()
    finish = "exit"
    try:
        print(f"[bf3] run {n}: {run_dir} {' '.join(f'{k}={v}' for k, v in env_extra.items())}", flush=True)
        proc = subprocess.Popen(cmd, cwd=run_dir, env=env)
        try:
            code = proc.wait(timeout=args.timeout)
        except subprocess.TimeoutExpired:
            proc.kill()
            proc.wait(30)
            code, finish = -1, "timeout"
    finally:
        lock.unlink(missing_ok=True)
    wall = time.time() - t0
    arms = read_rows(out / "bf3-arms.jsonl")
    morale = read_rows(out / "bf3-morale.jsonl")
    runrow = read_rows(out / "bf3-run.jsonl")
    print(f"[bf3] run {n}: code {code} ({finish}), {wall:.0f} s, {len(arms)} arm rows, {len(morale)} morale rows", flush=True)
    return {"run_dir": str(run_dir), "code": code, "finish": finish, "wall_s": round(wall, 1),
            "arms": arms, "morale": morale, "run": runrow}


def mean(v):
    return sum(v) / len(v) if v else float("nan")


def sd(v):
    if len(v) < 2:
        return 0.0
    m = mean(v)
    return math.sqrt(sum((x - m) ** 2 for x in v) / (len(v) - 1))


def summarise(results, args):
    arms = [a for r in results for a in r["arms"]]
    by = {}
    for a in arms:
        by.setdefault(a["arm_kind"], []).append(a)
    pairs = []
    for r in results:
        rows = r["arms"]
        for i in range(0, len(rows) - 1, 2):
            a, b = rows[i], rows[i + 1]
            kinds = {a["arm_kind"]: a, b["arm_kind"]: b}
            if args.control in kinds and "on" in kinds:
                pairs.append((kinds[args.control]["dmg"], kinds["on"]["dmg"]))
    flagged = [a for a in arms if a.get("flags")]
    base = {"tag": args.tag, "kind": "baseline", "control": args.control, "runs": len(results),
            "codes": [r["code"] for r in results], "wall_s": [r["wall_s"] for r in results]}
    fields = ["dmg", "dmg_pm", "hits", "burst_share", "swing_attempts", "standoff_mean", "attackers_mean",
              "attackers_max_per_target", "in_reach_mean", "near6_mean", "reach_slots", "reach_overflow",
              "ally_blocked", "blocked_thin", "blocked_wide", "reposition", "gap_wait", "ring_holds", "ring_slot_fail",
              "stuck", "steer_ticks", "waiters_pair_dist_mean", "flank_started", "flank_arrived", "flank_failed",
              "swing_phase_spread_ticks", "attempt_lockstep_share", "decide_us_p50", "decide_us_p99", "move_us_p99",
              "tick_us_p99", "target_lost_ticks", "promoted", "keep_engaged", "input_keys", "input_buttons", "input_motion",
              "creature_dmg", "dmg_foreign"]
    for kind, rows in sorted(by.items()):
        base[kind] = {"n": len(rows)}
        for f in fields:
            v = [x.get(f, 0) for x in rows]
            base[kind][f] = {"mean": round(mean(v), 3), "sd": round(sd(v), 3), "max": max(v) if v else 0}
    if pairs:
        ratios = [c / o if o else float("inf") for c, o in pairs]
        base["pairs"] = len(pairs)
        base["pairs_on_lower"] = sum(1 for c, o in pairs if o < c)
        base["pair_ratio_min"] = round(min(ratios), 3)
        base["pair_ratio_max"] = round(max(ratios), 3)
        mc, mo = mean([c for c, _ in pairs]), mean([o for _, o in pairs])
        base["dmg_control_mean"] = round(mc, 1)
        base["dmg_on_mean"] = round(mo, 1)
        base["ratio_control_over_on"] = round(mc / mo, 3) if mo else None
    base["flagged_rows"] = len(flagged)
    base["morale"] = [m for r in results for m in r["morale"]]
    return base


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--exe", default="build/vs2022/Release/barony.exe")
    ap.add_argument("--runs", type=int, default=1)
    ap.add_argument("--tag", default="limits3x")
    ap.add_argument("--seed", type=int, default=1514995713)
    ap.add_argument("--arms", default=None, help="APX_BF3_ARMS for the game (default: off,on,on,off,off,on,on,off)")
    ap.add_argument("--control", default="off", help="arm kind that must take more damage than 'on'")
    ap.add_argument("--env", action="append", default=[], help="extra KEY=VALUE for the game")
    ap.add_argument("--lock-dir", default=str(LOCK_DIR))
    ap.add_argument("--timeout", type=float, default=1500)
    ap.add_argument("--wait-limit", type=float, default=4 * 3600)
    ap.add_argument("--build-hash", default="unknown")
    args = ap.parse_args()
    env_extra = {}
    if args.arms:
        env_extra["APX_BF3_ARMS"] = args.arms
    for kv in args.env:
        k, _, v = kv.partition("=")
        env_extra[k] = v
    results = [run_once(args, n, env_extra) for n in range(1, args.runs + 1)]
    base = summarise(results, args)
    DIAG.mkdir(parents=True, exist_ok=True)
    out = DIAG / f"bf3-{args.tag}.jsonl"
    with out.open("w", encoding="ascii") as f:
        for r in results:
            for row in r["arms"] + r["morale"] + r["run"]:
                row = dict(row)
                row["run_dir"] = r["run_dir"]
                f.write(json.dumps(row, ensure_ascii=True) + "\n")
        f.write(json.dumps(base, ensure_ascii=True) + "\n")
    print(json.dumps({k: v for k, v in base.items() if k != "morale"}, indent=1)[:6000])
    for m in base["morale"]:
        print("morale", m.get("arm_kind"), "retreating_at_3s", m.get("retreating_at_3s"), "of", m.get("members_at_kill"),
              "dist", m.get("dist_at_kill"), "->", m.get("dist_at_3s"), "regroups", m.get("regroups"), "first_regroup_s", m.get("first_regroup_s"))
    print(f"[bf3] rows + baseline: {out}")
    bad = [r for r in results if r["code"] != 0 or r["finish"] != "exit"]
    ok_control = base.get("pairs", 0) > 0 and base.get("pairs_on_lower", 0) == base.get("pairs", 0)
    if bad or base["flagged_rows"] or (base.get("pairs", 0) and not ok_control):
        print(f"[bf3] NOT CLEAN: failed runs {len(bad)}, flagged rows {base['flagged_rows']}, control ok {ok_control}")
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
