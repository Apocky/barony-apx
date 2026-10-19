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
    args = ap.parse_args()
    bh = args.build_hash or args.tag
    py = sys.executable
    out = {"tag": args.tag, "build_hash": bh, "started": time.strftime("%Y-%m-%d %H:%M:%S"), "steps": []}
    report = RUNS / f"bf3-{args.tag}-verify.json"

    def save():
        report.write_text(json.dumps(out, indent=1), encoding="utf-8")

    code, wall = run([py, "-u", "tools/apx/bf3_trial.py", "--runs", "1", "--tag", args.tag, "--mode", "slot", "--arms", SLOT_ARMS,
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
    if go_on and not args.skip_exclusive:
        tag2 = args.tag + "-excl"
        code, wall = run([py, "-u", "tools/apx/bf3_trial.py", "--runs", "1", "--tag", tag2, "--mode", "exclusive", "--arms", EXCL_ARMS,
                          "--env", "APX_BF3_FLOWTEST=200", "--env", "APX_BF3_MORALE=0", "--env", "APX_BF3_HELP=0", "--timeout", "3600",
                          "--poll", "3", "--build-hash", bh], RUNS / f"bf3-{tag2}.out.txt")
        out["steps"].append({"step": "exclusive", "exit": code, "wall_s": wall, "gates": baseline(tag2).get("gates", {})})
        save()
        if not args.skip_parity:
            pdir = DIAG / f"barony-bf3-parity-{args.tag}"
            code, wall = run([py, "-u", "tools/apx/parity_f0.py", "--ours", "build/vs2022/Release/barony.exe", "--slot", "--exe-arg=-windowed",
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
