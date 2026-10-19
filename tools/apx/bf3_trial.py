#!/usr/bin/env python3
"""tools/apx/bf3_trial.py -- run the BF3 natural-limits trial under the machine's Barony lock and summarise it.

Each run takes the machine's Barony run protocol (D70, apx_locks.py): --mode slot (default) holds a free
C:/Apocrypha/locks/barony-slots/slotN.LOCK and runs windowed; --mode exclusive writes a .WANT, then takes
C:/Apocrypha/locks/barony/barony-bf3.LOCK (use it when decide_us / tick_us are reported). It launches
barony.exe from an EMPTY run folder with BARONY_DATADIR set at run time (never at configure time), waits for
it to exit (kill on timeout) and releases the lock. The game itself runs
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

import apx_locks   # D70 run protocol (slots, exclusive lock, .WANT fairness)

STEAM = Path(r"C:\Program Files (x86)\Steam\steamapps\common\Barony")
RUNS = Path(r"C:\Apocrypha\runs")
DIAG = Path(r"C:\Apocrypha\diagnostics")
LOCK_NAME = "barony-bf3.LOCK"


def take_lock(args):
    """D70 (apx_locks.py): slot mode for damage comparisons (tick-based correctness), exclusive mode when the
    run's timing numbers (decide_us, tick_us) are to be reported. Returns the path to release."""
    log = lambda t: print(t, flush=True)
    if args.mode == "exclusive":
        return apx_locks.take_exclusive(LOCK_NAME, args.wait_limit, args.poll, log)
    return apx_locks.take_slot("barony-bf3 trial", args.wait_limit, args.poll, log)


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
    if args.mode == "slot":
        cmd.append("-windowed")   # D70: slot runs are windowed
    lock = take_lock(args)
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
    helprows = read_rows(out / "bf3-help.jsonl")
    flowtest = read_rows(out / "bf3-flowtest.jsonl")
    print(f"[bf3] run {n}: code {code} ({finish}), {wall:.0f} s, {len(arms)} arm rows, {len(morale)} morale rows, "
          f"{len(helprows)} help rows, {len(flowtest)} flowtest rows", flush=True)
    return {"run_dir": str(run_dir), "code": code, "finish": finish, "wall_s": round(wall, 1), "lock": lock.name,
            "mode": args.mode, "arms": arms, "morale": morale, "run": runrow, "help": helprows, "flowtest": flowtest}


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
    # pairs: consecutive arms (0,1), (2,3), ... of the same creature count, one the control, one "on";
    # keyed by creature count so 3x (12) and 5x (20) pairs are reported apart
    pairs_by_n = {}
    for r in results:
        rows = r["arms"]
        for i in range(0, len(rows) - 1, 2):
            a, b = rows[i], rows[i + 1]
            if a.get("creatures") != b.get("creatures") or a.get("kind") != b.get("kind"):
                continue
            kinds = {a.get("arm_base", a["arm_kind"]): a, b.get("arm_base", b["arm_kind"]): b}
            if args.control in kinds and "on" in kinds:
                key = a["creatures"] if a.get("kind", "skeleton") == "skeleton" else f'{a["creatures"]}:{a.get("kind")}'
                pairs_by_n.setdefault(key, []).append((kinds[args.control]["dmg"], kinds["on"]["dmg"]))
    main_n = max(pairs_by_n, key=lambda n: len(pairs_by_n[n])) if pairs_by_n else None
    pairs = pairs_by_n.get(main_n, [])
    flagged = [a for a in arms if any(f.startswith("CONTAMINATED") or f == "SPAWN_SHORT" for f in a.get("flags", "").split(";"))]
    base = {"tag": args.tag, "kind": "baseline", "control": args.control, "runs": len(results), "mode": args.mode,
            "timing_valid": args.mode == "exclusive",
            "codes": [r["code"] for r in results], "wall_s": [r["wall_s"] for r in results]}
    fields = ["dmg", "dmg_pm", "hits", "burst_share", "swing_attempts", "standoff_mean", "attackers_mean",
              "attackers_max_per_target", "in_reach_mean", "near6_mean", "reach_slots", "reach_overflow",
              "ally_blocked", "blocked_thin", "blocked_wide", "reposition", "gap_wait", "ring_holds", "ring_slot_fail",
              "stuck", "steer_ticks", "waiters_pair_dist_mean", "flank_started", "flank_arrived", "flank_failed",
              "swing_phase_spread_ticks", "attempt_lockstep_share", "decide_us_p50", "decide_us_p99", "move_us_p99",
              "tick_us_p99", "target_lost_ticks", "promoted", "keep_engaged", "input_keys", "input_buttons", "input_motion",
              "creature_dmg", "dmg_foreign", "tactics_calls", "population_x", "stuck_episodes", "reach_slots_mean",
              "jitter_mean", "swing_resets_jittered", "feints", "player_moved", "reach_assign", "ring_assign",
              "st_wait", "st_attack", "st_path", "st_hunt", "targeting_player_mean", "dist_mean", "overlap_ticks",
              "pack_reacquire", "friendly_fire_dmg", "player_attack_ticks", "creatures_lost_ticks",
              "contact_attackers", "contact_mean", "blocked_share", "waiters_nn_mean", "waiters_overlap_share", "route_around",
              "flow_builds", "flow_paths", "flow_fallback_astar", "astar_paths", "astar_null", "flow_path_us_mean",
              "astar_path_us_mean", "flow_build_us_p99"]
    for kind, rows in sorted(by.items()):
        base[kind] = {"n": len(rows)}
        for f in fields:
            v = [x.get(f, 0) for x in rows]
            base[kind][f] = {"mean": round(mean(v), 3), "sd": round(sd(v), 3), "max": max(v) if v else 0}
    base["pairs_by_creatures"] = {
        str(n): {"pairs": len(p), "on_lower": sum(1 for c, o in p if o < c),
                 "dmg_control_mean": round(mean([c for c, _ in p]), 1), "dmg_on_mean": round(mean([o for _, o in p]), 1),
                 "ratio": round(mean([c for c, _ in p]) / mean([o for _, o in p]), 3) if mean([o for _, o in p]) else None}
        for n, p in pairs_by_n.items()}
    base["main_creatures"] = main_n
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
    # D72: per monster kind (fodder beside the sword skeletons), per arm base; the skeleton gates read only plain arm kinds
    by_monster = {}
    for a in arms:
        by_monster.setdefault(a.get("kind", "?"), {}).setdefault(a.get("arm_base", a["arm_kind"]), []).append(a)
    base["by_monster"] = {k: {b: {"n": len(v), "dmg_mean": round(mean([x["dmg"] for x in v]), 1),
                                  "attackers_mean": round(mean([x["attackers_mean"] for x in v]), 2),
                                  "attackers_max": max(x["attackers_max_per_target"] for x in v), "reach_slots": v[0].get("reach_slots"),
                                  "friendly_fire_mean": round(mean([x.get("friendly_fire_dmg", 0) for x in v]), 1),
                                  "hits_mean": round(mean([x.get("hits", 0) for x in v]), 1),
                                  "reach_overflow": sum(x.get("reach_overflow", 0) for x in v), "group_kind": v[0].get("group_kind"),
                                  "stuck": sum(x.get("stuck", 0) for x in v), "stuck_episodes": sum(x.get("stuck_episodes", 0) for x in v),
                                  "steer_ticks": sum(x.get("steer_ticks", 0) for x in v), "no_path_holds": sum(x.get("no_path_holds", 0) for x in v)}
                              for b, v in sorted(kinds.items())} for k, kinds in sorted(by_monster.items())}
    base["flagged_rows"] = len(flagged)
    base["morale"] = [m for r in results for m in r["morale"]]
    base["help"] = [h for r in results for h in r.get("help", [])]
    base["flowtest"] = [f for r in results for f in r.get("flowtest", [])]
    base["gates"] = gates(by, base, args)
    return base


def gates(by, base, args):
    """D71 (Apockalypsis DECISIONS-2.md) and card BF3 gates. A gate with no rows is 'no data', never a pass."""
    g = {}
    on = [a for a in by.get("on", []) if a.get("creatures") == 12]
    off = [a for a in by.get("off", []) if a.get("creatures") == 12]
    if on:
        dm = mean([a["dmg"] for a in on])
        att = mean([a["attackers_mean"] for a in on])
        blk = mean([a.get("blocked_share", 0) for a in on])
        g["damage_on_within_20pct_of_ref"] = {"pass": abs(dm - args.ref_dmg) <= 0.2 * args.ref_dmg and att >= 6.0 and blk <= 0.1,
                                              "dmg_mean": round(dm, 1), "ref": args.ref_dmg, "attackers_mean": round(att, 2),
                                              "blocked_share": round(blk, 3), "n": len(on)}
        ov = sum(a["reach_overflow"] for a in on)
        mx = max(a["attackers_max_per_target"] - a["reach_slots"] for a in on)
        g["reach_overflow_zero"] = {"pass": ov == 0 and mx <= 0, "overflow_ticks": ov, "max_over_slots": mx, "n": len(on)}
    else:
        g["damage_on_within_20pct_of_ref"] = {"pass": None, "why": "no 12-creature on arms"}
    if off and on:
        blk = mean([a.get("blocked_share", 0) for a in off])
        con_off = mean([a.get("contact_mean", 0) for a in off])   # creatures in contact at once (clear swing line)
        con_on = mean([a.get("contact_mean", 0) for a in on])
        g["control_native_jam"] = {"pass": blk >= 0.5 and con_off <= con_on / 2, "off_blocked_share": round(blk, 3),
                                   "off_contact_mean": round(con_off, 2), "on_contact_mean": round(con_on, 2),
                                   "off_contact_attackers_20s": round(mean([a.get("contact_attackers", 0) for a in off]), 2),
                                   "off_dmg_mean": round(mean([a["dmg"] for a in off]), 1), "n": len(off)}
    noring = [a for a in by.get("noring", []) if a.get("creatures") == 12]
    if noring and on:
        # Bunching is measured directly: the share of waiting creatures whose nearest neighbour is closer than two body
        # radii (bodies overlapping). Card BF3-4 named the mean pairwise distance of waiters; it does not separate here
        # (s5-route: 50.7 with the ring, 50.5 without) because waiters without the ring queue behind the slot on their
        # own bearing, spread round the target like ring slots, but stacked on the same point; both are reported.
        # Card BF3-4 also bounds stuck: under 2 percent of steered ticks with the ring.
        ov_on = mean([a.get("waiters_overlap_share", 0) for a in on])
        ov_off = mean([a.get("waiters_overlap_share", 0) for a in noring])
        st_on = sum(a.get("stuck", 0) for a in on) / max(1, sum(a.get("steer_ticks", 0) for a in on))
        g["ring_control_bunching"] = {"pass": ov_off >= 0.1 and ov_on <= ov_off / 3 and st_on < 0.02,
                                      "overlap_ring": round(ov_on, 3), "overlap_noring": round(ov_off, 3),
                                      "stuck_share_ring": round(st_on, 4),
                                      "pair_ring": round(mean([a.get("waiters_pair_dist_mean", 0) for a in on]), 1),
                                      "pair_noring": round(mean([a.get("waiters_pair_dist_mean", 0) for a in noring]), 1),
                                      "nn_ring": round(mean([a.get("waiters_nn_mean", 0) for a in on]), 2),
                                      "nn_noring": round(mean([a.get("waiters_nn_mean", 0) for a in noring]), 2),
                                      "stuck_ring": sum(a.get("stuck", 0) for a in on), "stuck_noring": sum(a.get("stuck", 0) for a in noring)}
        g["holders_reach_slots"] = {"pass": all(a.get("holder_false_arrive", 0) == 0 for a in on) and
                                    mean([a.get("holder_in", 0) / max(1, a.get("holder_in", 0) + a.get("holder_out", 0)) for a in on]) >= 0.8,
                                    "holder_in_share": round(mean([a.get("holder_in", 0) / max(1, a.get("holder_in", 0) + a.get("holder_out", 0)) for a in on]), 3),
                                    "false_arrive": sum(a.get("holder_false_arrive", 0) for a in on),
                                    "flank_arrived": sum(a.get("flank_arrived", 0) for a in on), "flank_failed": sum(a.get("flank_failed", 0) for a in on)}
    mon = [m for m in base["morale"] if m.get("arm_kind") == "morale_on"]
    moff = [m for m in base["morale"] if m.get("arm_kind") == "morale_off_control"]
    if mon and moff:
        # a retreat must arrive: every member breaks within 3 s AND every member is home (within 2 tiles of it, or
        # regrouping) 5 s after the kill AND none gave up after 20 s (home_moved). The stance alone passed while the
        # members stood at the keep-out edge (s3-grid, s5-route); how far home lies from the fight is the scenario's,
        # so the distance gain is reported, not gated (s10: a home 4-5 tiles out allows about +20 units).
        gain = [round(m["dist_at_3s"] - m["dist_at_kill"], 1) for m in mon]
        g["morale_retreat"] = {"pass": all(m["retreating_at_3s"] == m["members_at_kill"] and m.get("home_at_5s", -1) == m["members_at_kill"]
                                           and m["home_moved"] == 0 for m in mon)
                               and all(m["retreating_at_3s"] == 0 for m in moff),
                               "home_at_5s": [f'{m.get("home_at_5s", -1)}/{m["members_at_kill"]}' for m in mon],
                               "retreating_at_3s": [f'{m["retreating_at_3s"]}/{m["members_at_kill"]}' for m in mon],
                               "dist_gain_3s": gain, "home_moved": [m["home_moved"] for m in mon],
                               "first_regroup_s": [m["first_regroup_s"] for m in mon],
                               "control_retreating": [m["retreating_at_3s"] for m in moff]}
    hon = [h for h in base["help"] if h.get("arm_kind") == "help_on"]
    hoff = [h for h in base["help"] if h.get("arm_kind") == "help_off_nopacks"]
    if hon and hoff:
        # judged at window_s: twice pack B's walk from its spot plus 2 s (the game logs it); later arrivals in the
        # control are monsters wandering into the fight on their own (s5-route: 2 of 4 at 26.3 s), kept as *_end
        w = lambda h: h.get("b_within4_window", h["b_within4_end"])
        # the pack comes (3 or 4 of 4) only when called; a control may see one wanderer (s9-1: one helper wandered into
        # sight at 8.1 s and arrived at 11.4 s with no call; the called pack: 4 of 4)
        g["calls_for_help"] = {"pass": all(h["calls_answered"] >= 1 and w(h) >= 3 for h in hon)
                               and all(0 <= w(h) <= 1 for h in hoff),
                               "window_s": [h.get("window_s") for h in hon + hoff],
                               "on_within4": [w(h) for h in hon], "off_within4": [w(h) for h in hoff],
                               "on_within4_end": [h["b_within4_end"] for h in hon], "off_within4_end": [h["b_within4_end"] for h in hoff],
                               "off_first_arrival_s": [h["b_first_arrival_s"] for h in hoff],
                               "on_first_arrival_s": [h["b_first_arrival_s"] for h in hon], "spot_steps": [h["spot_steps"] for h in hon]}
    fl = [a for k, rows in by.items() if k.startswith("flow:") for a in rows]
    nf = [a for k, rows in by.items() if k.startswith("noflow:") for a in rows]
    if fl and nf:
        fpm = mean([a["flow_path_us_mean"] for a in fl if a.get("flow_paths")])
        apm = mean([a["astar_path_us_mean"] for a in nf if a.get("astar_paths")])
        tick = max(a["tick_us_p99"] for a in fl)
        g["flow_cost"] = {"astar_over_flow_per_path": round(apm / fpm, 2) if fpm else None, "flow_path_us_mean": round(fpm, 1),
                          "astar_path_us_mean": round(apm, 1), "pass_ratio": bool(fpm and apm / fpm >= 3.0),
                          "flow_tick_us_p99": round(tick, 1), "pass_tick": (tick < 2000) if args.mode == "exclusive" else None,
                          "timing_valid": args.mode == "exclusive", "creatures": fl[0].get("creatures")}
    pr = [a for k, rows in by.items() if k == "probe" or k.startswith("probe:") for a in rows]
    if pr:
        ptick = max(a["tick_us_p99"] for a in pr)
        # CONTROL: every knob on plus a 1 ms busy-wait per decide must break the 2000 us budget, or the tick timer
        # cannot see a slow decide and a passing flow_tick_us_p99 proves nothing
        g["timing_probe_control"] = {"pass": (ptick >= 2000) if args.mode == "exclusive" else None,
                                     "probe_tick_us_p99": round(ptick, 1), "creatures": pr[0].get("creatures"),
                                     "timing_valid": args.mode == "exclusive"}
    ft = base["flowtest"]
    if ft:
        both = sum(r["both_ok"] for r in ft)
        g["flow_field_vs_astar"] = {"pairs": sum(r["pairs"] for r in ft), "both_ok": both,
                                    "steps_equal": sum(r["steps_equal"] for r in ft), "steps_within1": sum(r["steps_within1"] for r in ft),
                                    "field_dearer": sum(r["cost_field_dearer"] for r in ft), "astar_only": sum(r["astar_only"] for r in ft),
                                    "door_pairs": sum(r["door_pairs"] for r in ft), "door_within1": sum(r["door_within1"] for r in ft),
                                    "chase_astar_fail": sum(r["chase_astar_fail"] for r in ft),
                                    "build_us_p99_max": max(r["build_us_p99"] for r in ft),
                                    "pass": sum(r["cost_field_dearer"] for r in ft) == 0 and sum(r["astar_only"] for r in ft) == 0
                                    and max(r["build_us_p99"] for r in ft) < 500 if args.mode == "exclusive" else None}
    return g


def regate(paths, args):
    """Recompute the summary and gates from finished run folders (their apx/*.jsonl rows); prints the gates."""
    results = []
    for d in paths:
        def rows(name):
            out = []
            for root, _, files in os.walk(d):
                if name in files:
                    with open(os.path.join(root, name), encoding="utf-8") as fh:
                        out += [json.loads(x) for x in fh if x.strip()]
            return out
        results.append({"arms": rows("bf3-arms.jsonl"), "morale": rows("bf3-morale.jsonl"), "help": rows("bf3-help.jsonl"),
                        "flowtest": rows("bf3-flowtest.jsonl"), "code": 0, "wall_s": 0})
    base = summarise(results, args)
    for k, v in base["gates"].items():
        print(f"{k}: {json.dumps(v)}")
    return base


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--exe", default="build/vs2022/Release/barony.exe")
    ap.add_argument("--runs", type=int, default=1)
    ap.add_argument("--tag", default="limits3x")
    ap.add_argument("--seed", type=int, default=1514995713)
    ap.add_argument("--arms", default=None, help="APX_BF3_ARMS for the game (default: off,on,on,off,off,on,on,off)")
    ap.add_argument("--control", default="off", help="arm kind paired with 'on' in the pair table")
    ap.add_argument("--ref-dmg", type=float, default=268.3,
                    help="D71 reference: damage per 20 s arm with limits on at floor 1, 12 skeletons (MEASURED full3, 02:49)")
    ap.add_argument("--env", action="append", default=[], help="extra KEY=VALUE for the game")
    ap.add_argument("--mode", choices=("slot", "exclusive"), default="slot",
                    help="slot: D70 correctness slot, windowed (damage, counts); exclusive: the whole machine "
                         "(report decide_us / tick_us only from exclusive runs)")
    ap.add_argument("--timeout", type=float, default=1500)
    ap.add_argument("--wait-limit", type=float, default=4 * 3600)
    ap.add_argument("--poll", type=float, default=10.0, help="seconds between lock checks")
    ap.add_argument("--build-hash", default="unknown")
    ap.add_argument("--regate", nargs="+", default=None, help="run folders: recompute and print the gates, launch nothing")
    args = ap.parse_args()
    if args.regate:
        regate(args.regate, args)
        return 0
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
            for row in r["arms"] + r["morale"] + r.get("help", []) + r.get("flowtest", []) + r["run"]:
                row = dict(row)
                row["run_dir"] = r["run_dir"]
                f.write(json.dumps(row, ensure_ascii=True) + "\n")
        f.write(json.dumps(base, ensure_ascii=True) + "\n")
    print(json.dumps({k: v for k, v in base.items() if k not in ("morale", "help", "flowtest", "gates")}, indent=1)[:4000])
    print("[bf3] gates:")
    for name, gate in base["gates"].items():
        print(f"  {name}: {json.dumps(gate)}")
    for m in base["morale"]:
        print("morale", m.get("arm_kind"), "retreating_at_3s", m.get("retreating_at_3s"), "of", m.get("members_at_kill"),
              "dist", m.get("dist_at_kill"), "->", m.get("dist_at_3s"), "regroups", m.get("regroups"), "first_regroup_s", m.get("first_regroup_s"))
    print(f"[bf3] rows + baseline: {out}")
    bad = [r for r in results if r["code"] != 0 or r["finish"] != "exit"]
    failed = [n for n, gate in base["gates"].items() if gate.get("pass") is False or gate.get("pass_ratio") is False or gate.get("pass_tick") is False]
    if bad or base["flagged_rows"] or failed:
        print(f"[bf3] NOT CLEAN: failed runs {len(bad)}, flagged rows {base['flagged_rows']}, failed gates {failed}")
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
