"""props_qa.py -- BP1 props played like a player through the QA driver (-apxqa=<dir>, real SDL input
events; the window never takes focus or the mouse). APX_PROPS_QA=1 places a chair, a table and a bed in
an arc in front of player 0 and a bone cage behind; the command script (tools/apx/qa/<name>.txt) aims,
uses (right click = Barony's Use) and attacks with real input and takes screenshots.
    python tools/apx/props_qa.py --exe build/vs2022/Release/barony.exe --script tools/apx/qa/props_use.txt
Prints the [apx-props] log lines and the shot paths; one launch, shared Barony lock.
"""
import argparse
import datetime
import os
import shutil
import subprocess
import sys
import time
from pathlib import Path

HERE = Path(__file__).resolve().parent
sys.path.insert(0, str(HERE))
import parity_f0 as pf  # noqa: E402
import apx_locks  # noqa: E402  D70 slots

ROOT = HERE.parents[1]
pf.LOCK_NAME = "barony-props-qa.LOCK"
DATADIR = r"C:\Program Files (x86)\Steam\steamapps\common\Barony"
OUT = Path(r"C:\Apocrypha\diagnostics\props\qa")


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--exe", required=True)
    ap.add_argument("--script", required=True)
    ap.add_argument("--timeout", type=float, default=240)
    ap.add_argument("--prop", default="1", help="APX_PROPS_QA: chair, table, bed, cage, or 1 (all)")
    a = ap.parse_args()
    run = OUT / f"{datetime.datetime.now():%Y%m%d-%H%M%S}-{Path(a.script).stem}-{a.prop.replace(":", "_")}"
    run.mkdir(parents=True)
    qa = run / "qa"
    qa.mkdir()
    shutil.copy(a.script, qa / "cmd.txt")
    env = dict(os.environ, BARONY_DATADIR=DATADIR, APX_PROPS_QA=a.prop, APX_HOLLOW_DIR=str(ROOT / "data/apx/hollow"))
    slot = apx_locks.take_slot("props_qa", 4 * 3600, log=lambda m: print(m, flush=True))  # D70: a slot, other lanes' games run beside us
    try:
        t0 = time.monotonic()
        p = subprocess.Popen([str(Path(a.exe).resolve()), "-windowed", "-quickstart=barbarian", "-apxfloor=1", "-apxmapseed=101", f"-apxqa={qa}"],
                             cwd=run, env=env)
        try:
            p.wait(a.timeout)
            ended = "exited"
        except subprocess.TimeoutExpired:
            ended = pf.stop(p)
    finally:
        slot.unlink(missing_ok=True)
    log = (run / "log.txt").read_text(errors="replace") if (run / "log.txt").exists() else ""
    print(f"run {run} ended={ended} secs={time.monotonic() - t0:.1f}")
    for line in log.splitlines():
        if "[apx-props]" in line or "[apx-interact]" in line or "apxqa" in line.lower() or "[apx-qa]" in line:
            print("  " + line.strip()[:220])
    for s in sorted(qa.glob("*.png")):
        print("  shot", s)
    st = qa / "status.txt"
    if st.exists():
        print("  status:", st.read_text(errors="replace").strip()[-400:])


if __name__ == "__main__":
    main()
