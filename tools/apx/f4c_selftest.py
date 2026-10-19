"""f4c_selftest.py -- the F4c integration gate: ONE waiting job, two launches (gate and control).

Each launch: -quickstart=barbarian -apxfloor=1 -apxmapseed=<seed> with APX_F4C_SELFTEST=1 and
APX_DOWNED_FORCE_COOP=1; the self-test in src/apx_combat.hpp spawns four tiers beside player 0,
drives the real hit hook and modHP, prints "[apx-f4c] check ..." rows and a summary, and quits.
The control turns the three features off (APX_POISE=0 APX_ZONES=0 APX_DOWNED=0) and must fail.
Uses the shared Barony lock convention of parity_f0.py. Rows -> C:\\Apocrypha\\diagnostics\\f4c.

    python tools/apx/f4c_selftest.py --exe build/vs2022/Release/barony.exe [--seed 1234]
Exit 0 only when the gate passes every check and the control does not pass.
"""
import argparse
import datetime
import json
import os
import re
import subprocess
import sys
import time
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
import parity_f0 as pf  # noqa: E402  (lock, stop and process helpers)

pf.LOCK_NAME = "barony-f4c.LOCK"
DATADIR = r"C:\Program Files (x86)\Steam\steamapps\common\Barony"
OUT = Path(r"C:\Apocrypha\diagnostics\f4c")


def launch(exe, seed, env_extra, tag, timeout_s):
    stamp = datetime.datetime.now().strftime("%Y%m%d-%H%M%S")
    run_dir = OUT / "runs" / f"{stamp}-{tag}-s{seed}"
    run_dir.mkdir(parents=True)
    env = dict(os.environ)
    env.update({"BARONY_DATADIR": DATADIR, "APX_F4C_SELFTEST": "1", "APX_DOWNED_FORCE_COOP": "1"})
    env.update(env_extra)
    t0 = time.monotonic()
    proc = subprocess.Popen([str(exe), "-quickstart=barbarian", "-apxfloor=1", f"-apxmapseed={seed}"], cwd=run_dir, env=env)
    log = run_dir / "log.txt"
    summary = None
    while time.monotonic() - t0 < timeout_s and proc.poll() is None:
        time.sleep(2)
        if log.exists() and "[apx-f4c] summary" in log.read_text(errors="replace"):
            summary = True
            time.sleep(2)
            break
    ended = pf.stop(proc)
    text = log.read_text(errors="replace") if log.exists() else ""
    checks = [m.groups() for m in re.finditer(r"\[apx-f4c\] check (PASS|FAIL) (\S+) got=(\S+) want=(\S+)", text)]
    m = re.search(r"\[apx-f4c\] summary pass=(\d+) fail=(\d+)", text)
    row = {
        "tag": tag, "seed": seed, "env": env_extra, "secs": round(time.monotonic() - t0, 1), "ended": ended,
        "summary": bool(m), "pass": int(m.group(1)) if m else sum(c[0] == "PASS" for c in checks),
        "fail": int(m.group(2)) if m else sum(c[0] == "FAIL" for c in checks),
        "checks": [{"ok": c[0] == "PASS", "what": c[1], "got": float(c[2]), "want": float(c[3])} for c in checks],
        "hits": len(re.findall(r"\[apx-f4c\] hit ", text)), "run_dir": str(run_dir),
        "exe_sha256": pf.sha256(exe),
    }
    row["ok"] = row["summary"] and row["fail"] == 0 and row["pass"] > 0
    return row


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--exe", required=True)
    ap.add_argument("--seed", type=int, default=1234)
    ap.add_argument("--timeout", type=float, default=180)
    a = ap.parse_args()
    exe = Path(a.exe).resolve()
    OUT.mkdir(parents=True, exist_ok=True)
    waited = pf.wait_until_no_barony(3600, [pf.SHARED_LOCK_DIR])
    pf.hold_lock([pf.SHARED_LOCK_DIR], True)
    rows = []
    try:
        rows.append(launch(exe, a.seed, {}, "gate", a.timeout))
        rows.append(launch(exe, a.seed, {"APX_POISE": "0", "APX_ZONES": "0", "APX_DOWNED": "0"}, "control", a.timeout))
    finally:
        pf.hold_lock([pf.SHARED_LOCK_DIR], False)
    with open(OUT / "f4c.jsonl", "a", encoding="ascii") as f:
        for r in rows:
            f.write(json.dumps(r) + "\n")
    gate, control = rows
    ok = gate["ok"] and not control["ok"]
    for r in rows:
        print(f"{r['tag']}: summary={r['summary']} pass={r['pass']} fail={r['fail']} secs={r['secs']} ended={r['ended']}")
        for c in r["checks"]:
            print(f"   {'PASS' if c['ok'] else 'FAIL'} {c['what']} got={c['got']} want={c['want']}")
    print(json.dumps({"f4c_gate": "PASS" if ok else "FAIL", "waited_s": waited, "gate_ok": gate["ok"], "control_ok": control["ok"]}))
    sys.exit(0 if ok else 1)


if __name__ == "__main__":
    main()
