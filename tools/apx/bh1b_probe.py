"""bh1b_probe.py -- the H1b batched engine check: ONE waiting job, two launches (card BH1b gate H3/H4).

gate:    APX_BH_PROBE=content on data/apx/hollow: the engine loads every class, makes every consumable,
         spawns every creature and boss, tags tiers; the row's counters must be 0, exit 0, and
         rows_loaded must equal the lint's row counts.
control: the same on a copy where one creature kind is renamed NOT_A_MONSTER: must exit 3.
Stop rule (D76): two launches, each stops at the probe's exit or 180 s. Rows go to
C:\\Apocrypha\\diagnostics\\bh1b\\content-<time>.jsonl.
    python tools/apx/bh1b_probe.py --exe build/vs2022/Release/barony.exe [--seed 101]
"""
import argparse
import datetime
import json
import os
import shutil
import subprocess
import sys
import time
from pathlib import Path

HERE = Path(__file__).resolve().parent
sys.path.insert(0, str(HERE))
import parity_f0 as pf  # noqa: E402

ROOT = HERE.parents[1]
pf.LOCK_NAME = "barony-h1b.LOCK"
DATADIR = r"C:\Program Files (x86)\Steam\steamapps\common\Barony"
OUT = Path(r"C:\Apocrypha\diagnostics\bh1b")


def lint_counts(d):
    man = json.loads((Path(d) / "manifest.json").read_text(encoding="ascii"))
    return {c["file"][:-5]: len(json.loads((Path(d) / c["file"]).read_text(encoding="ascii"))) for c in man["classes"]}


def launch(exe, seed, hollow_dir, out_file, tag, timeout_s):
    run_dir = OUT / "runs" / f"{datetime.datetime.now():%Y%m%d-%H%M%S}-{tag}"
    run_dir.mkdir(parents=True)
    env = dict(os.environ, BARONY_DATADIR=DATADIR, APX_BH_PROBE="content", APX_HOLLOW_DIR=str(hollow_dir), APX_BH_OUT=str(out_file))
    t0 = time.monotonic()
    proc = subprocess.Popen([str(exe), "-windowed", "-quickstart=barbarian", "-apxfloor=1", f"-apxmapseed={seed}"], cwd=run_dir, env=env)
    try:
        code = proc.wait(timeout_s)
        ended = "exited"
    except subprocess.TimeoutExpired:
        ended = pf.stop(proc)
        code = None
    log = (run_dir / "log.txt").read_text(errors="replace") if (run_dir / "log.txt").exists() else ""
    row = None
    for line in log.splitlines():
        if "[apx-bh1b] {" in line:
            row = json.loads(line[line.index("{"):])
    return {"tag": tag, "exit": code, "ended": ended, "secs": round(time.monotonic() - t0, 1), "row": row, "run_dir": str(run_dir)}


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--exe", required=True)
    ap.add_argument("--seed", type=int, default=101)
    ap.add_argument("--timeout", type=float, default=180)
    a = ap.parse_args()
    exe = Path(a.exe).resolve()
    OUT.mkdir(parents=True, exist_ok=True)
    out_file = OUT / f"content-{datetime.datetime.now():%Y%m%d-%H%M%S}.jsonl"
    data = ROOT / "data/apx/hollow"
    ctrl = OUT / "control-hollow"
    shutil.rmtree(ctrl, ignore_errors=True)
    shutil.copytree(data, ctrl)
    rows = json.loads((ctrl / "creatures.json").read_text(encoding="ascii"))
    for r in rows:
        if r["kind"] == "RAT":
            r["kind"] = "NOT_A_MONSTER"
    (ctrl / "creatures.json").write_text(json.dumps(rows), encoding="ascii")
    waited = pf.wait_until_no_barony(4 * 3600, [pf.SHARED_LOCK_DIR])
    pf.hold_lock([pf.SHARED_LOCK_DIR], True)
    try:
        gate = launch(exe, a.seed, data, out_file, "gate", a.timeout)
        control = launch(exe, a.seed, ctrl, out_file, "control", a.timeout)
    finally:
        pf.hold_lock([pf.SHARED_LOCK_DIR], False)
    want = lint_counts(data)
    got = (gate["row"] or {}).get("rows_loaded", {})
    mismatch = {k: (got.get(k), v) for k, v in want.items() if got.get(k) != v}
    gate_ok = gate["exit"] == 0 and gate["row"] is not None and gate["row"]["ok"] and not mismatch
    control_ok = control["exit"] == 3 and control["row"] is not None and control["row"]["rows_unresolved"] > 0
    for r in (gate, control):
        print(json.dumps(r))
    summary = {"bh1b_content": "PASS" if gate_ok and control_ok else "FAIL", "gate_ok": gate_ok, "control_failed_as_required": control_ok,
               "rows_loaded_mismatch": len(mismatch), "mismatch": mismatch, "waited_s": waited, "exe_sha256": pf.sha256(exe)}
    print(json.dumps(summary))
    with open(OUT / "bh1b.jsonl", "a", encoding="ascii") as f:
        f.write(json.dumps({"gate": gate, "control": control, "summary": summary}) + "\n")
    sys.exit(0 if gate_ok and control_ok else 1)


if __name__ == "__main__":
    main()
