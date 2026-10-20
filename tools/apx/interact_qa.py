"""interact_qa.py -- the apx::interact gate matrix (D93, PROPS lane). Every prop type is placed straight ahead at a centre
distance (APX_PROPS_QA=<name>:<d>), the player's Use is pressed with real input (QA driver `act Use`, through apx::in) and the
log must show a prompt and a use. Out-of-reach cases must show neither. Controls (APX_INTERACT_OLD=1) run Barony's old path:
colliders (and furniture beyond Barony's 24-unit tooltip range) must show neither prompt nor use.
    python -I tools/apx/interact_qa.py --exe build/vs2022/Release/barony.exe [--only chair:30,...] [--control]
Rows go to C:/Apocrypha/diagnostics/interact/matrix-<stamp>.jsonl; exit code 1 if any row fails its expectation.
"""
import argparse
import datetime
import json
import os
import re
import subprocess
import sys
from pathlib import Path

HERE = Path(__file__).resolve().parent
OUT = Path(r"C:\Apocrypha\diagnostics\interact")
# name:centre distance, expectation (True = prompt and use), kind
NEW = [("chair:30", True), ("table:30", True), ("bed:30", True), ("bunkbed:30", True), ("podium:30", True), ("torch:30", True),
       ("cage:30", True), ("candelabra:30", True), ("maiden:30", True),
       ("chair:40", False), ("candelabra:44", False), ("maiden:46", False)]
CONTROL = [("chair:30", False), ("candelabra:30", False), ("maiden:30", False), ("cage:30", False)]


def run_case(exe, script, prop, old):
    env = dict(os.environ)
    env.pop("APX_INTERACT_OLD", None)
    if old:
        env["APX_INTERACT_OLD"] = "1"
    p = subprocess.run([sys.executable, "-I", str(HERE / "props_qa.py"), "--exe", exe, "--script", script, "--prop", prop],
                       capture_output=True, text=True, env=env, timeout=900)
    m = re.search(r"^run (\S+) ended=(\S+)", p.stdout, re.M)
    run = Path(m.group(1)) if m else None
    log = (run / "log.txt").read_text(errors="replace") if run and (run / "log.txt").exists() else ""
    prompts = [l for l in log.splitlines() if "[apx-interact] prompt" in l and "cleared" not in l]
    uses = [l for l in log.splitlines() if "[apx-interact] use " in l]
    placed = [l for l in log.splitlines() if "[apx-props] qa placed" in l]
    return {"run": str(run) if run else None, "prompts": len(prompts), "uses": len(uses), "placed": placed[-1].split("] ", 2)[-1] if placed else None,
            "first_prompt": prompts[0].split("] ", 2)[-1] if prompts else None, "first_use": uses[0].split("] ", 2)[-1] if uses else None,
            "shots": sorted(str(s) for s in (run / "qa").glob("*.png")) if run else [], "ended": m.group(2) if m else None}


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--exe", required=True)
    ap.add_argument("--script", default=str(HERE / "qa" / "interact_use.txt"))
    ap.add_argument("--only", default="")
    ap.add_argument("--control", action="store_true", help="run the APX_INTERACT_OLD=1 control cases instead of the new path")
    a = ap.parse_args()
    cases = CONTROL if a.control else NEW
    if a.only:
        want = set(a.only.split(","))
        cases = [c for c in cases if c[0] in want]
    OUT.mkdir(parents=True, exist_ok=True)
    out = OUT / f"matrix-{'control' if a.control else 'new'}-{datetime.datetime.now():%Y%m%d-%H%M%S}.jsonl"
    bad = 0
    for prop, expect in cases:
        r = run_case(a.exe, a.script, prop, a.control)
        got = r["prompts"] > 0 and r["uses"] > 0
        gotNone = r["prompts"] == 0 and r["uses"] == 0
        ok = got if expect else gotNone
        r.update({"prop": prop, "expect": "prompt+use" if expect else "neither", "control": a.control, "pass": ok})
        bad += 0 if ok else 1
        with out.open("a") as f:
            f.write(json.dumps(r) + "\n")
        print(f"{'PASS' if ok else 'FAIL'} {'control ' if a.control else ''}{prop} expect={r['expect']} prompts={r['prompts']} uses={r['uses']} {r['placed'] or ''}", flush=True)
    print(f"rows -> {out}; failures {bad}")
    sys.exit(1 if bad else 0)


if __name__ == "__main__":
    main()
