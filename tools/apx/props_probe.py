"""props_probe.py -- the BP1-1 headless check: ONE waiting job, two launches (-apxheadless, lane rule 14).

gate:    APX_PROPS_PROBE=1: a bed, chair, table and podium beside player 0 are used through the real
         use path, 200 bone cages are broken through the real destroy path; the row must show every
         verb ran, prompt labels set, cage shares within 3 standard errors of 25/45/30 (D76 stop rule:
         200 breaks, standard error 3.1 points at 25 percent), spawned skeletons tagged fodder.
control: the same with APX_PROPS=0 (vanilla): must fail the gate.
    python tools/apx/props_probe.py --exe build/vs2022/Release/barony.exe
"""
import argparse
import datetime
import json
import math
import os
import subprocess
import sys
import time
from pathlib import Path

HERE = Path(__file__).resolve().parent
sys.path.insert(0, str(HERE))
import parity_f0 as pf  # noqa: E402

ROOT = HERE.parents[1]
pf.LOCK_NAME = "barony-props.LOCK"
DATADIR = r"C:\Program Files (x86)\Steam\steamapps\common\Barony"
OUT = Path(r"C:\Apocrypha\diagnostics\props")


def launch(exe, env_extra, tag, timeout_s):
    run_dir = OUT / "runs" / f"{datetime.datetime.now():%Y%m%d-%H%M%S}-{tag}"
    run_dir.mkdir(parents=True)
    out = run_dir / "row.jsonl"
    env = dict(os.environ, BARONY_DATADIR=DATADIR, APX_PROPS_PROBE="1", APX_PROPS_OUT=str(out), APX_HOLLOW_DIR=str(ROOT / "data/apx/hollow"))
    env.update(env_extra)
    t0 = time.monotonic()
    proc = subprocess.Popen([str(exe), "-apxheadless", "-quickstart=barbarian", "-apxfloor=1", "-apxmapseed=101"], cwd=run_dir, env=env)
    try:
        code, ended = proc.wait(timeout_s), "exited"
    except subprocess.TimeoutExpired:
        code, ended = None, pf.stop(proc)
    row = json.loads(out.read_text().splitlines()[-1]) if out.exists() and out.read_text().strip() else None
    return {"tag": tag, "exit": code, "ended": ended, "secs": round(time.monotonic() - t0, 1), "row": row, "run_dir": str(run_dir)}


def checks(r):
    if not r:
        return [("row written", False, None)]
    n = max(1, r["breaks"])
    se = lambda p: 3 * math.sqrt(p * (1 - p) / n)  # noqa: E731
    sk, tr = r["skeletons"] / n, r["trinkets"] / n
    return [
        ("labels are verbs", r["labels"] == "Rest|Sit|Field bench: salvage, repair|Read|", r["labels"]),
        ("rest ran and healed", r["rests_done"] == 1 and r["healed"] > 0 and r["hp1"] > r["hp0"], [r["rests_done"], r["healed"], r["hp0"], r["hp1"]]),
        ("sat with stamina regen", r["sits"] >= 1 and r["sit_ticks"] > 0, [r["sits"], r["sit_ticks"], r["stamina_after_sit"]]),
        ("table is a FIELD bench", r["benches"] == 1 and r["field_bench_near"] == 1, [r["benches"], r["field_bench_near"]]),
        ("podium read a history row", r["reads"] == 1 and r["read_id"].startswith("h"), [r["reads"], r["read_id"]]),
        ("200 cages broke", r["cages"] == 200 and r["breaks"] == 200, [r["cages"], r["breaks"]]),
        ("skeleton share 25 +- 3 SE", abs(sk - 0.25) <= se(0.25), round(sk * 100, 1)),
        ("trinket share 45 +- 3 SE", abs(tr - 0.45) <= se(0.45), round(tr * 100, 1)),
        ("spawned skeletons tagged fodder (D74)", r["skeletons"] > 0 and r["skeletons_fodder"] == r["skeletons"], [r["skeletons"], r["skeletons_fodder"]]),
        ("rest group from the floor's fodder band", r.get("enc_creatures", 0) > 0 and r.get("enc_fodder_band") == r.get("enc_creatures") and r.get("fodder_band_kinds", 0) > 0,
         [r.get("enc_creatures"), r.get("enc_fodder_band"), r.get("fodder_band_kinds")]),
        ("table offers Flip with a hostile in sight", r.get("flip_label") == "Flip for cover" and r.get("flips") == 1, [r.get("flip_label"), r.get("flips")]),
        ("flipped table blocks the sight line", r.get("los_before") == 0 and r.get("los_after") == 1, [r.get("los_before"), r.get("los_after")]),
        ("torch taken leaves a dark sconce, relit with a torch", r.get("torch_found") == 1 and r.get("torch_kept") == 1 and r.get("torch_dark") == 1 and r.get("got_torch") == 1 and r.get("relit") == 1,
         [r.get("torch_found"), r.get("torch_kept"), r.get("torch_dark"), r.get("got_torch"), r.get("relit")]),
        ("repair one step through F6 (ledger grams), from the bench Salvage tab row", r.get("repair_row") == 1 and r.get("repair_ok") == 1 and r.get("status_after") == r.get("status_before", -9) + 1 and r.get("repair_grams", 0) > 0,
         [r.get("repair_ok"), r.get("status_before"), r.get("status_after"), r.get("repair_grams"), r.get("repair_why")]),
    ]


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--exe", required=True)
    ap.add_argument("--timeout", type=float, default=180)
    a = ap.parse_args()
    exe = Path(a.exe).resolve()
    OUT.mkdir(parents=True, exist_ok=True)
    waited = pf.wait_until_no_barony(4 * 3600, [pf.SHARED_LOCK_DIR])
    pf.hold_lock([pf.SHARED_LOCK_DIR], True)
    try:
        gate = launch(exe, {}, "gate", a.timeout)
        control = launch(exe, {"APX_PROPS": "0"}, "control", a.timeout)
    finally:
        pf.hold_lock([pf.SHARED_LOCK_DIR], False)
    res = {}
    for r in (gate, control):
        cs = checks(r["row"])
        r["checks"] = [{"what": w, "ok": ok, "got": g} for w, ok, g in cs]
        res[r["tag"]] = all(ok for _, ok, _ in cs)
        print(f"{r['tag']}: exit={r['exit']} secs={r['secs']} pass={sum(ok for _, ok, _ in cs)}/{len(cs)}")
        for w, ok, g in cs:
            print(f"   {'PASS' if ok else 'FAIL'} {w}: {g}")
    ok = res["gate"] and not res["control"]
    summary = {"props_gate": "PASS" if ok else "FAIL", "gate_ok": res["gate"], "control_failed": not res["control"], "waited_s": waited, "exe_sha256": pf.sha256(exe)}
    print(json.dumps(summary))
    with open(OUT / "props.jsonl", "a", encoding="ascii") as f:
        f.write(json.dumps({"gate": gate, "control": control, "summary": summary}) + "\n")
    sys.exit(0 if ok else 1)


if __name__ == "__main__":
    main()
