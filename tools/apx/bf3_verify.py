"""bf3_verify.py -- the BF3 verification chain as ONE background job (lane protocol v2, rule 4: one waiting job).

1. A D70 slot run (bf3_trial.py --mode slot): the movement, morale, help and flow self-test batch.
2. Only if its movement gates pass: the exclusive timing run (flow cost, tick p99, the probe control that must fail).
3. Only if that ran: F0 parity on a slot (parity_f0.py --slot, 7 runs).
Each step takes its own lock through the D70 tools and waits its turn; nothing here polls the caller.
Writes C:/Apocrypha/runs/bf3-<tag>-verify.json (every step's exit code, wall time and gates) and prints a summary.

Usage: python tools/apx/bf3_verify.py --tag s9 [--build-hash s9] [--skip-exclusive] [--skip-parity]
"""
import argparse
import json
import subprocess
import sys
import time
from pathlib import Path

REPO = Path(__file__).resolve().parents[2]
DIAG = Path(r"C:\Apocrypha\diagnostics")
RUNS = Path(r"C:\Apocrypha\runs")
SLOT_ARMS = ("on,off,on,slots5,noring,on,off,vanilla,on,slots5,noring,off,on,vanilla,on,on,nodiag,nodiag"
             ",on:12:rat,off:12:rat,on:12:rat,off:12:rat,on:12:rat,vanilla:12:rat,vanilla:12:rat"  # D72 fodder crowd
             ",flow:150,flow:150")  # 150 creatures chasing: stuck counts in big crowds (timing only from --mode exclusive)
EXCL_ARMS = "on,noring,noring,on,flow:150,noflow:150,noflow:150,flow:150,probe:150,off"
# the gates that decide whether the build is final enough to spend exclusive and parity minutes on; damage and the
# jam control wait for Apocky's reference decision and do not block
MOVEMENT_GATES = ("reach_overflow_zero", "holders_reach_slots", "ring_control_bunching", "morale_retreat", "calls_for_help")
# D74 (--d74): 12 sword skeletons and 12 rats tagged fodder (rule on) vs fodder0 (rule forced off in the arm: the
# in-process vanilla control), interleaved x3, then regular (must stay vanilla) and wild (untagged: the floor's tier)
D74_ARMS = ",".join(["on:12:skeleton:fodder", "on:12:skeleton:fodder0", "on:12:rat:fodder", "on:12:rat:fodder0"] * 3
                    + ["on:12:skeleton:regular", "on:12:rat:regular", "on:12:skeleton:wild", "on:12:rat:wild"])
# the knob control: a second launch with APX_TIER_DAMAGE=0, the same fodder-tagged arms (must land above the band)
D74_ENV0_ARMS = ",".join(["on:12:skeleton:fodder", "on:12:rat:fodder"] * 2)
D74_GATES = ("d74_fodder_per_hit_1_to_2", "d74_skeleton_fodder_in_band", "d74_controls_above_band", "d74_regular_is_vanilla",
             "d74_fodder_no_stagger_lock")
# the F2 generator's own gate (C:/Apocrypha/diagnostics/barony-f2, build a90e21c3, 2026-10-19 08:40-09:00): apx-gen
# floors for these seeds must hash the same after the band rank moved into tier_rules.hpp (placement unchanged)
F2_RECORD = Path(r"C:\Apocrypha\diagnostics\barony-f2\f2.jsonl")
F2_SEEDS = (2001, 4)  # first, count: floors 1-4 (bands of 2 and 4 types, ties included)


def rows_of(p):
    out = []
    if p.exists():
        for x in p.read_text(encoding="utf-8-sig").splitlines():
            x = x.strip()
            if x:
                try:
                    out.append(json.loads(x))
                except ValueError:
                    pass
    return out


def run_dir_of(tag):
    for r in rows_of(DIAG / f"bf3-{tag}.jsonl"):
        if r.get("run_dir"):
            return r["run_dir"]
    return None


def f2_floors(rows, since=None):
    """(seed, level) -> hash of apx-gen size-128 floors, host side, no control."""
    out = {}
    for r in rows:
        env = r.get("env") or {}
        if r.get("kind") != "floor" or r.get("client") != 0 or env.get("APXGEN_SIZE") != "128" or env.get("APXGEN_CONTROL"):
            continue
        if since and r.get("ts", "") < since:
            continue
        out[(r.get("seed_asked"), r.get("level"))] = r.get("hash")
    return out


def d74_chain(args, bh, py, out, save):
    t1, t2 = args.tag, args.tag + "-env0"
    common = ["--exe", args.exe, "--runs", "1", "--mode", "slot", "--env", "APX_BF3_MORALE=0", "--env", "APX_BF3_HELP=0",
              "--timeout", "3600", "--poll", "3", "--build-hash", bh]
    code, wall = run([py, "-u", "tools/apx/bf3_trial.py", "--tag", t1, "--arms", D74_ARMS] + common, RUNS / f"bf3-{t1}.out.txt")
    out["steps"].append({"step": "d74-slot", "exit": code, "wall_s": wall, "run_dir": run_dir_of(t1)})
    save()
    code, wall = run([py, "-u", "tools/apx/bf3_trial.py", "--tag", t2, "--arms", D74_ENV0_ARMS, "--env", "APX_TIER_DAMAGE=0"] + common,
                     RUNS / f"bf3-{t2}.out.txt")
    out["steps"].append({"step": "d74-env0", "exit": code, "wall_s": wall, "run_dir": run_dir_of(t2)})
    save()
    dirs = [d for d in (run_dir_of(t1), run_dir_of(t2)) if d]
    comb = DIAG / f"bf3-{args.tag}-d74.jsonl"
    code, wall = run([py, "-u", "tools/apx/bf3_trial.py", "--regate"] + dirs + ["--regate-out", str(comb)], RUNS / f"bf3-{args.tag}-d74-regate.out.txt")
    rows = rows_of(comb)
    base = rows[-1] if rows and rows[-1].get("kind") == "baseline" else {}
    g = base.get("gates", {})
    out["steps"].append({"step": "d74-gates", "exit": code, "wall_s": wall, "runs": dirs, "combined": str(comb),
                         "pass": {k: (g.get(k) or {}).get("pass") for k in D74_GATES}, "gates": {k: v for k, v in g.items() if k.startswith("d74")},
                         "d74": base.get("d74")})
    save()
    # the generator's placement unchanged: the F2 gate's own sweep for 4 seeds, hashes against its record
    fdir = DIAG / f"barony-bf3-f2check-{args.tag}"
    code, wall = run([py, "-u", "tools/apx/gate_f2.py", "sweep", "--exe", args.exe, "--size", "128", "--first", str(F2_SEEDS[0]),
                      "--count", str(F2_SEEDS[1]), "--per-process", str(F2_SEEDS[1]), "--out", str(fdir)], RUNS / f"bf3-{args.tag}-f2check.out.txt")
    rec = f2_floors(rows_of(F2_RECORD), since="2026-10-19T08:40")
    mine = f2_floors(rows_of(fdir / "f2.jsonl"))
    cmp_ = {f"{s}/{l}": {"recorded": rec.get((s, l)), "now": h, "same": rec.get((s, l)) == h} for (s, l), h in sorted(mine.items())}
    out["steps"].append({"step": "f2-hashes", "exit": code, "wall_s": wall, "floors": cmp_,
                         "pass": bool(cmp_) and len(cmp_) == F2_SEEDS[1] and all(v["same"] for v in cmp_.values())})
    save()
    golden_step(args, out, save)


# D75 (--d75) under D76 (protocol rule 12: run until the critical numbers are in). Each launch lists CELLS; the game
# repeats them round-robin until each cell's 95 percent half-width is under its tolerance (2 arms at least, 4 at most;
# tolerances in bf3trial.cpp stopTolerance and the plan file handovers/plans/bf3-d75.md). Only the cells a decision needs:
#   A physical poise: robed vs armoured under fodder skeletons and wild rats (staggers), armoured under regular
#     skeletons (locks); the jam in the open arena: vanilla, bodies colliding, limits on with bodies colliding (dmg).
#     Floor-1 wild rats and skeletons must be fodder (a deterministic check at spawn: exit 3 stops the chain).
#   B the control APX_POISE_PHYSICAL=0: the armoured cells again (does the lock come back).
#   C the jam in a corridor: vanilla, bodies colliding, limits on (dmg).
D75_A = ["on:12:skeleton:fodder:robed", "on:12:skeleton:fodder:armoured", "on:12:rat:wild:robed", "on:12:rat:wild:armoured",
         "on:12:skeleton:regular:armoured@locks", "vanilla:12:skeleton@dmg", "vcollide:12:skeleton@dmg", "oncollide:12:skeleton@dmg"]
D75_B = ["on:12:skeleton:fodder:armoured", "on:12:rat:wild:armoured", "on:12:skeleton:regular:armoured@locks"]
D75_C = ["vanilla:12:skeleton@dmg", "vcollide:12:skeleton@dmg", "on:12:skeleton@dmg"]
D75_GATES = ("d75_armour_staggers_less", "d75_control_restores_lock", "d75_floor1_rats_fodder")
F2_OLD = Path(r"C:\Apocrypha\diagnostics\barony-f2")
STOP = ["--env", "APX_BF3_STOP=min=2,max=4", "--env", "APX_BF3_STOP_METRIC=staggers"]


def regate_gates(dirs, outpath, tag, py):
    code, wall = run([py, "-u", "tools/apx/bf3_trial.py", "--regate"] + dirs + ["--regate-out", str(outpath)], RUNS / f"bf3-{tag}-regate.out.txt")
    rows = rows_of(outpath)
    base = rows[-1] if rows and rows[-1].get("kind") == "baseline" else {}
    return code, wall, base


def d75_chain(args, bh, py, out, save):
    def stop(why):
        out["stopped"] = why
        save()
    code = subprocess.call([py, "tools/apx/gen_strength.py", "--check"], cwd=REPO)
    out["steps"].append({"step": "strength-table-current", "exit": code, "wall_s": 0, "pass": code == 0})
    save()
    if code != 0:
        return stop("monster_strength.inc stale")
    common = ["--exe", args.exe, "--runs", "1", "--mode", "slot", "--env", "APX_BF3_MORALE=0", "--env", "APX_BF3_HELP=0",
              "--timeout", "3600", "--poll", "3", "--build-hash", bh] + STOP
    dirs = []

    def launch(tag, cells, extra):
        code, wall = run([py, "-u", "tools/apx/bf3_trial.py", "--tag", tag, "--arms", ",".join(cells)] + extra + common, RUNS / f"bf3-{tag}.out.txt")
        d = run_dir_of(tag)
        if d:
            dirs.append(d)
        out["steps"].append({"step": f"d75-{tag}", "exit": code, "wall_s": wall, "run_dir": d})
        save()
        return code
    # A, then the first decisive check: the tiers (exit 3) and the armour gate
    if launch(args.tag, D75_A, ["--env", "APX_BF3_EXPECT_TIERS=rat=fodder,skeleton=fodder"]) == 3:
        return stop("floor-1 tier expectation failed at spawn (exit 3)")
    _, _, baseA = regate_gates(list(dirs), DIAG / f"bf3-{args.tag}-d75-A.jsonl", args.tag + "-A", py)
    gA = (baseA.get("gates") or {}).get("d75_armour_staggers_less") or {}
    out["steps"].append({"step": "d75-gate-A", "exit": 0, "wall_s": 0, "armour_gate": gA})
    save()
    if gA.get("pass") is False:
        return stop("armoured not at most half the robed staggers: decisive failure after launch A")
    launch(args.tag + "-f4c", D75_B, ["--env", "APX_POISE_PHYSICAL=0"])
    launch(args.tag + "-corridor", D75_C, ["--env", "APX_BF3_ARENA=corridor"])
    comb = DIAG / f"bf3-{args.tag}-d75.jsonl"
    code, wall, base = regate_gates(list(dirs), comb, args.tag, py)
    g = base.get("gates", {})
    out["steps"].append({"step": "d75-gates", "exit": code, "wall_s": wall, "runs": dirs, "combined": str(comb),
                         "pass": {k: (g.get(k) or {}).get("pass") for k in D75_GATES},
                         "gates": {k: v for k, v in g.items() if k.startswith("d75")}, "d75": base.get("d75")})
    save()
    # the apx-gen recheck, deterministic, once: floors 1-4 at size 128 (W2, D72 and the new hashes), then host and client
    fout = DIAG / f"barony-f2-{args.tag}"
    fout.mkdir(parents=True, exist_ok=True)
    (fout / "REASON.txt").write_text(
        "apx-gen baseline re-recorded for D75 (Apocky, 2026-10-19): band tiers are strength-ordered (weakest types up to\n"
        "about 60 percent of a floor's spawns are fodder; the strongest of two or more left elite), src/apx/tier_rules.hpp\n"
        "bandTiers with apx/monster_strength.inc; the band is sampled 4000 times instead of 400. D72 placement picks types\n"
        "by tier, so floors whose band changed hash differently (floor 1: rats move from regular to fodder).\n"
        "D76 (run until the critical numbers are in): floors 1-4 at size 128 (seeds 2001-2004) and host+client at seed\n"
        "3001 size 128 are re-recorded once; other seeds and sizes of the previous baseline are re-recorded when needed.\n"
        f"Previous baseline: {F2_OLD} (build a90e21c3). Build under test: {bh}.\n", encoding="ascii")
    code, wall = run([py, "-u", "tools/apx/gate_f2.py", "sweep", "--exe", str(Path(args.exe).resolve()), "--size", "128", "--first", "2001",
                      "--count", "4", "--per-process", "4", "--out", str(fout), "--wait-foreign", "7200"], RUNS / f"bf3-{args.tag}-f2sweep.out.txt")
    rows = [r for r in rows_of(fout / "f2.jsonl") if r.get("kind") == "floor"]
    bad = [f'{r.get("seed_asked")}/{r.get("level")}' for r in rows if r.get("finish") != "ok" or any("FAIL" in str(f) for f in r.get("flags", []))]
    old = f2_floors(rows_of(F2_OLD / "f2.jsonl"), since="2026-10-19T08:40")
    new = f2_floors(rows_of(fout / "f2.jsonl"))
    out["steps"].append({"step": "f2-sweep-floors-1-4", "exit": code, "wall_s": wall, "floors": len(rows), "bad": bad,
                         "hashes": {f"{s_}/{l_}": {"old": old.get((s_, l_)), "new": h} for (s_, l_), h in sorted(new.items())},
                         "pass": code == 0 and len(rows) == 4 and not bad})
    save()
    if bad or len(rows) != 4:
        return stop("apx-gen floors failed W2/D72 or did not generate: decisive failure")
    code, wall = run([py, "-u", "tools/apx/gate_f2.py", "mp", "--exe", str(Path(args.exe).resolve()), "--seeds", "3001", "--sizes", "128",
                      "--out", str(fout), "--wait-foreign", "7200"], RUNS / f"bf3-{args.tag}-f2mp.out.txt")
    mp = [r for r in rows_of(fout / "f2.jsonl") if r.get("kind") == "mp"]
    last = mp[-1] if mp else {}
    out["steps"].append({"step": "f2-host-client", "exit": code, "wall_s": wall, "same_hash": last.get("same_hash"),
                         "same_counts": last.get("same_counts"), "host": (last.get("host") or {}).get("hash"),
                         "client": (last.get("client") or {}).get("hash"),
                         "pass": bool(last) and last.get("same_hash") is True and last.get("same_counts") is True})
    save()
    golden_step(args, out, save)
    f4c_step(args, py, out, save)  # apx_combat.hpp changed (player poise): F4c's own in-game gate, once


# D80: bodies collide by default; stuck near 0 with limits on; the crowd control in both places (open, corridor).
# Default launch (apx-gen floors, impact, bodies colliding) and a -apxvanilla launch for the vanilla control (D80).
D80_DEFAULT = ["on:12:skeleton@dmg", "nocollide:12:skeleton@dmg", "on:12:skeleton!corridor@dmg", "nocollide:12:skeleton!corridor@dmg"]
D80_VANILLA = ["vanilla:12:skeleton@dmg", "vanilla:12:skeleton!corridor@dmg"]


def d80_chain(args, bh, py, out, save):
    common = ["--exe", args.exe, "--runs", "1", "--mode", "slot", "--env", "APX_BF3_MORALE=0", "--env", "APX_BF3_HELP=0",
              "--timeout", "3600", "--poll", "3", "--build-hash", bh] + STOP
    dirs = []
    for tag, cells, extra in ((args.tag, D80_DEFAULT, []), (args.tag + "-vanilla", D80_VANILLA, ["--game-arg=-apxvanilla"])):
        code, wall = run([py, "-u", "tools/apx/bf3_trial.py", "--tag", tag, "--arms", ",".join(cells)] + extra + common, RUNS / f"bf3-{tag}.out.txt")
        d = run_dir_of(tag)
        if d:
            dirs.append(d)
        out["steps"].append({"step": f"d80-{tag}", "exit": code, "wall_s": wall, "run_dir": d})
        save()
    comb = DIAG / f"bf3-{args.tag}-d80.jsonl"
    code, wall, base = regate_gates(list(dirs), comb, args.tag, py)
    g = base.get("gates", {})
    out["steps"].append({"step": "d80-gates", "exit": code, "wall_s": wall, "runs": dirs, "combined": str(comb),
                         "pass": {"d80_stuck_near_zero": (g.get("d80_stuck_near_zero") or {}).get("pass")},
                         "stuck": g.get("d80_stuck_near_zero"), "jam": (base.get("d75") or {}).get("jam")})
    save()
    golden_step(args, out, save)  # T3: vanilla parity (bodies colliding change no generation; -apxvanilla keeps them off)


def golden_step(args, out, save):
    """Vanilla floors unchanged: golden parity against the F0 lines recorded from Steam vanilla (its own slot)."""
    code, wall = run(["pwsh", "-NoProfile", "-File", "tools/apx/golden_parity.ps1", "-Exe", str(Path(args.exe).resolve()), "-Tag", f"bf3-{args.tag}"],
                     RUNS / f"bf3-{args.tag}-golden.out.txt")
    txt = (RUNS / f"bf3-{args.tag}-golden.out.txt").read_text(encoding="utf-8", errors="replace")
    out["steps"].append({"step": "golden-parity", "exit": code, "wall_s": wall, "tail": txt.strip().splitlines()[-3:] if txt.strip() else []})
    save()


def f4c_step(args, py, out, save):
    """F4c's own in-game gate (tools/apx/f4c_selftest.py: gate + control launch) on this build. It needs the whole
    machine and waits for it without a .WANT, so the D70 exclusive lock is taken first under its own lock name
    (barony/barony-f4c.LOCK: the .WANT drains the slots; f4c_selftest.py sees its own lock, runs, and removes it)."""
    sys.path.insert(0, str(REPO / "tools" / "apx"))
    import apx_locks
    t0 = time.time()
    lock = apx_locks.take_exclusive("barony-f4c.LOCK", 4 * 3600, 3.0, lambda t: print(t, flush=True))
    waited = round(time.time() - t0, 1)
    try:
        code, wall = run([py, "-u", "tools/apx/f4c_selftest.py", "--exe", str(Path(args.exe).resolve())], RUNS / f"bf3-{args.tag}-f4c.out.txt")
    finally:
        lock.unlink(missing_ok=True)
    txt = (RUNS / f"bf3-{args.tag}-f4c.out.txt").read_text(encoding="utf-8", errors="replace")
    verdict = None
    for line in txt.splitlines():
        if line.startswith("{") and "f4c_gate" in line:
            try:
                verdict = json.loads(line)
            except ValueError:
                pass
    out["steps"].append({"step": "f4c-gate", "exit": code, "wall_s": wall, "waited_s": waited, "verdict": verdict,
                         "pass": bool(verdict) and verdict.get("f4c_gate") == "PASS" and code == 0})
    save()


def run(cmd, log):
    t0 = time.time()
    with open(log, "w", encoding="utf-8") as fh:
        code = subprocess.call(cmd, cwd=REPO, stdout=fh, stderr=subprocess.STDOUT)
    return code, round(time.time() - t0, 1)


def baseline(tag):
    p = DIAG / f"bf3-{tag}.jsonl"
    if not p.exists():
        return {}
    rows = [json.loads(x) for x in p.read_text(encoding="utf-8").splitlines() if x.strip()]
    base = [r for r in rows if r.get("kind") == "baseline"]
    return base[-1] if base else {}


def flow_ok(base):
    ft = base.get("flowtest") or []
    if not ft:
        return None
    return sum(r.get("cost_field_dearer", 0) for r in ft) == 0 and sum(r.get("astar_only", 0) for r in ft) == 0


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--tag", required=True)
    ap.add_argument("--build-hash", default=None)
    ap.add_argument("--skip-exclusive", action="store_true")
    ap.add_argument("--skip-parity", action="store_true")
    ap.add_argument("--exe", default="build/vs2022/Release/barony.exe", help="the build under test (build.ps1 -Cache ccache: build/ninja-ccache/barony.exe)")
    ap.add_argument("--d74", action="store_true", help="D74 tier damage: fodder vs both vanilla controls, F2 hashes, golden parity")
    ap.add_argument("--d80", action="store_true", help="D80: steering with colliding bodies (stuck), the crowd control open and corridor")
    ap.add_argument("--d75", action="store_true", help="D75: physical poise (robed vs armoured, F4c control), jam arms (open, corridor,"
                    " bodies colliding), F2's gate matrix as the new apx-gen baseline, golden parity")
    ap.add_argument("--skip-golden", action="store_true", help="with --merge-gates: golden parity already ran on this build")
    ap.add_argument("--merge-gates", action="store_true",
                    help="after the slot run: F4c's in-game gate (f4c_selftest.py) and golden parity on this build (F4c merged in)")
    args = ap.parse_args()
    bh = args.build_hash or args.tag
    py = sys.executable
    out = {"tag": args.tag, "build_hash": bh, "started": time.strftime("%Y-%m-%d %H:%M:%S"), "steps": []}
    report = RUNS / f"bf3-{args.tag}-verify.json"

    def save():
        report.write_text(json.dumps(out, indent=1), encoding="utf-8")

    if args.d74 or args.d75 or args.d80:
        (d80_chain if args.d80 else d75_chain if args.d75 else d74_chain)(args, bh, py, out, save)
        out["finished"] = time.strftime("%Y-%m-%d %H:%M:%S")
        save()
        for s in out["steps"]:
            print(s["step"], "exit", s["exit"], "wall_s", s["wall_s"], json.dumps(s.get("pass")))
        print("report", report)
        return 0

    code, wall = run([py, "-u", "tools/apx/bf3_trial.py", "--exe", args.exe, "--runs", "1", "--tag", args.tag, "--mode", "slot", "--arms", SLOT_ARMS,
                      "--env", "APX_BF3_DUMP=1", "--env", "APX_BF3_FLOWTEST=200", "--timeout", "3600", "--poll", "3",
                      "--build-hash", bh], RUNS / f"bf3-{args.tag}.out.txt")
    base = baseline(args.tag)
    g = base.get("gates", {})
    move = {k: (g.get(k) or {}).get("pass") for k in MOVEMENT_GATES}
    fok = flow_ok(base)
    # the runner exits 1 whenever any gate fails, the damage and jam gates included (they wait for Apocky's reference
    # decision): a completed run is exit 0 or 1 with a baseline row (s11: every movement gate passed, exit 1)
    go_on = code in (0, 1) and bool(base) and all(v is True for v in move.values()) and fok is True
    out["steps"].append({"step": "slot", "exit": code, "wall_s": wall, "movement_gates": move, "flow_selftest_ok": fok,
                         "gates": g, "proceed": go_on})
    save()
    if args.merge_gates:
        f4c_step(args, py, out, save)
        if not args.skip_golden:
            golden_step(args, out, save)
    if go_on and not args.skip_exclusive:
        tag2 = args.tag + "-excl"
        code, wall = run([py, "-u", "tools/apx/bf3_trial.py", "--exe", args.exe, "--runs", "1", "--tag", tag2, "--mode", "exclusive", "--arms", EXCL_ARMS,
                          "--env", "APX_BF3_FLOWTEST=200", "--env", "APX_BF3_MORALE=0", "--env", "APX_BF3_HELP=0", "--timeout", "3600",
                          "--poll", "3", "--build-hash", bh], RUNS / f"bf3-{tag2}.out.txt")
        out["steps"].append({"step": "exclusive", "exit": code, "wall_s": wall, "gates": baseline(tag2).get("gates", {})})
        save()
        if not args.skip_parity:
            pdir = DIAG / f"barony-bf3-parity-{args.tag}"
            code, wall = run([py, "-u", "tools/apx/parity_f0.py", "--ours", args.exe, "--slot", "--exe-arg=-windowed",
                              "--lock-name", "barony-bf3-parity.LOCK", "--out", str(pdir), "--wait-foreign", "10800"],
                             RUNS / f"bf3-{args.tag}-parity.out.txt")
            summary = pdir / "summary.json"
            out["steps"].append({"step": "parity", "exit": code, "wall_s": wall,
                                 "summary": json.loads(summary.read_text(encoding="utf-8")) if summary.exists() else None})
            save()
    out["finished"] = time.strftime("%Y-%m-%d %H:%M:%S")
    save()
    if out["steps"] and out["steps"][0].get("step") == "slot":
        out["by_monster"] = baseline(args.tag).get("by_monster")
        save()
    for s in out["steps"]:
        print(s["step"], "exit", s["exit"], "wall_s", s["wall_s"], json.dumps({k: v for k, v in s.items() if k in ("movement_gates", "flow_selftest_ok", "proceed")}))
    print("report", report)
    return 0


if __name__ == "__main__":
    sys.exit(main())
