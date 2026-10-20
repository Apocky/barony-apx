"""combat_gate.py -- the whole D93 C1 gate in one go (each step is one combat_qa.py launch, ~20-100 s).

    python tools/apx/combat_gate.py --exe build/vs2022/Release/barony.exe [--no-shots]

Steps and what must hold:
  normal       all 8 families x (quick, left, overhead, low, charged) by real clicks on the normal start: 40/40 swing, cost = table, hit
  floor1       the same on -apxfloor=1 (seed 1234567, a dry start)
  floor1-lava  sword on -apxfloor=1 seed 101: the player starts swimming in lava, where Barony's HUD weapon returns before its swing check
               (the named regression gate): the core must swing and hit
  control      APX_COMBAT_OLD=1 on that seed: Barony's old path must show 0 swings (the control FAILS)
  perf150      150 standing creatures (+ the 7 test dummies, + the floor's own): combat cost p99 per sim tick (< 0.3 ms)
  perf150fight 150 creatures that fight back (their hits go through applyHit too)
  shots        a sweep through three creatures and a charged move, PNGs in C:\\Apocrypha\\diagnostics\\combat\\shots
Prints one table and exits 0 when every must-hold row holds. Rows go to C:\\Apocrypha\\diagnostics\\combat\\c1-gate-*.jsonl.
"""
import argparse
import json
import re
import subprocess
import sys
import shutil
from pathlib import Path

HERE = Path(__file__).resolve().parent
OUT = Path(r"C:\Apocrypha\diagnostics\combat")


def run(args):
    p = subprocess.run([sys.executable, str(HERE / "combat_qa.py")] + args, capture_output=True, text=True)
    text = p.stdout + p.stderr
    m = re.search(r"run (\S+) ended", text)
    run_dir = Path(m.group(1)) if m else None
    summ = {}
    if run_dir and (run_dir / "summary.json").exists():
        summ = json.loads((run_dir / "summary.json").read_text())
    return p.returncode, summ, text


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--exe", required=True)
    ap.add_argument("--no-shots", action="store_true")
    a = ap.parse_args()
    rows, ok_all = [], True

    def step(name, args, must, note=lambda s: ""):
        nonlocal ok_all
        code, s, text = run(["--exe", a.exe] + args)
        good = must(code, s)
        ok_all = ok_all and good
        perf = (s.get("perf") or [{}])[-1]
        rows.append((name, "PASS" if good else "FAIL", f"{s.get('ok', '-')}/{s.get('swings_expected', '-')} swings ok", note(s), s.get("run", "")))
        return s

    step("normal", ["--mode", "normal", "--tag", "gate"], lambda c, s: c == 0)
    step("floor1", ["--mode", "floor1", "--seed", "1234567", "--tag", "gate"], lambda c, s: c == 0)
    step("floor1-lava", ["--mode", "floor1", "--seed", "101", "--families", "sword", "--tag", "gate-lava"], lambda c, s: c == 0)
    step("control", ["--mode", "floor1", "--seed", "101", "--families", "sword", "--old", "--tag", "gate-lava"],
         lambda c, s: s.get("legacy_swings", 1) == 0, lambda s: f"legacy swings {s.get('legacy_swings')} (must be 0: the control fails)")

    def p99(s):
        pr = [r for r in s.get("perf", []) if r.get("ticks", 0) >= 600]
        return max((r["p99_ms"] for r in pr), default=9.9)   # the worst window of the run
    fams = "sword,axe,mace,spear,staff,dagger"
    step("perf150", ["--mode", "floor1", "--seed", "1234567", "--crowd", "150", "--still", "--perf", "--families", fams, "--tag", "gate-perf"],
         lambda c, s: p99(s) < 0.3 and s.get("ok", 0) == s.get("swings_expected", 1), lambda s: f"p99 {p99(s):.3f} ms per tick, {s['perf'][-1].get('creatures') if s.get('perf') else '?'} creatures")
    step("perf150fight", ["--mode", "floor1", "--seed", "1234567", "--crowd", "150", "--perf", "--families", fams, "--tag", "gate-perf"],
         lambda c, s: p99(s) < 0.3, lambda s: f"p99 {p99(s):.3f} ms per tick, {s['perf'][-1].get('creatures') if s.get('perf') else '?'} creatures (swing verdicts not gated: the crowd fights)")
    if not a.no_shots:
        code, s, text = run(["--exe", a.exe, "--mode", "floor1", "--seed", "1234567", "--shots", "--families", "sword", "--tag", "gate-shots"])
        dest = OUT / "shots"
        dest.mkdir(parents=True, exist_ok=True)
        n = 0
        if s.get("run"):
            for png in (Path(s["run"]) / "qa").glob("s*.png"):
                shutil.copy(png, dest / f"{Path(s['run']).name}-{png.name}")
                n += 1
        rows.append(("shots", "PASS" if n >= 6 else "FAIL", f"{n} PNGs", str(dest), s.get("run", "")))
        ok_all = ok_all and n >= 6
    print("\n{:13s} {:5s} {:22s} {}".format("step", "res", "swings", "note"))
    for r in rows:
        print("{:13s} {:5s} {:22s} {}   [{}]".format(*r))
    print("COMBAT C1 GATE:", "PASS" if ok_all else "FAIL")
    sys.exit(0 if ok_all else 1)


if __name__ == "__main__":
    main()
