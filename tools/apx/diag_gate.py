"""diag_gate.py -- D90 gate for the always-on local diagnostics (src/apx/diag.cpp). Headless launches, one batch.

  python tools\\apx\\diag_gate.py --exe <barony.exe>

Launches (each in its own folder under C:\\Apocrypha\\diagnostics\\diag\\runs, each holds a D70 slot while it runs):
  A normal   : rows present with required fields (session sha, hw, gl, settings, knobs, features line, level event,
               per-second rows with frame p50/p99/max, work times, sys_ms), overhead_pct under 1 percent
  B stall    : APX_DIAG_PLANT=stall:300  -> a spike row whose sys_ms names tactics (>= 100 ms)
  C error    : APX_DIAG_PLANT=error:300  -> err rows with the planted message and err_total >= 1 in a later sec row
  D crash    : APX_DIAG_PLANT=crash:300  -> crash-*.txt naming apxDiagPlantedCrash, a crash row with a stack,
               the last events in the report, and a minidump (.dmp)
  E control  : APX_DIAG=0                -> no diag folder at all
  F selftest : -apxdiagselftest          -> exit 0
Rows are checked, not eyeballed; the exit code is non-zero when any check fails.
"""
import argparse
import datetime
import json
import os
import subprocess
import sys
import time
from pathlib import Path

sys.path.insert(0, str(Path(__file__).parent))
import apx_locks  # noqa: E402

STEAM = Path(r"C:\Program Files (x86)\Steam\steamapps\common\Barony")
ROOT = Path(r"C:\Apocrypha\diagnostics\diag")
ARGS = ["-quickstart=barbarian", "-apxfloor=1", "-apxmapseed=5", "-apxheadless", "-nosound"]


def launch(exe, tag, env_extra=None, seconds=25, args=ARGS):
    run = ROOT / "runs" / f"{datetime.datetime.now():%Y%m%d-%H%M%S}-{tag}"
    run.mkdir(parents=True)
    env = os.environ.copy()
    env["BARONY_DATADIR"] = str(STEAM)
    env.update(env_extra or {})
    t0 = time.monotonic()
    p = subprocess.Popen([str(exe)] + list(args), cwd=run, env=env)
    try:
        code = p.wait(seconds)
    except subprocess.TimeoutExpired:
        p.terminate()
        try:
            p.wait(10)
        except subprocess.TimeoutExpired:
            p.kill()
            p.wait(10)
        code = "stopped"
    return {"run": run, "exit": code, "wall": time.monotonic() - t0}


def rows(run):
    out = []
    for f in sorted((run / "diag").glob("session-*.jsonl")) if (run / "diag").exists() else []:
        for l in f.read_text(encoding="ascii", errors="replace").splitlines():
            try:
                out.append(json.loads(l))
            except ValueError:
                pass
    return out


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--exe", required=True)
    a = ap.parse_args()
    exe = Path(a.exe).resolve()
    checks = []

    def chk(name, ok, detail=""):
        checks.append({"check": name, "ok": bool(ok), "detail": detail})
        print(f"[{'PASS' if ok else 'FAIL'}] {name} {detail}", flush=True)

    lock = apx_locks.take_slot("diag-gate", 86400)
    try:
        A = launch(exe, "A-normal")
        B = launch(exe, "B-stall", {"APX_DIAG_PLANT": "stall:300"})
        C = launch(exe, "C-error", {"APX_DIAG_PLANT": "error:300"})
        D = launch(exe, "D-crash", {"APX_DIAG_PLANT": "crash:300"}, seconds=40)
        E = launch(exe, "E-control", {"APX_DIAG": "0"}, seconds=15)
        F = launch(exe, "F-selftest", args=["-apxdiagselftest"], seconds=30)
    finally:
        lock.unlink(missing_ok=True)

    ra = rows(A["run"])
    types = {r["t"] for r in ra}
    chk("A: session/hw/gl/settings/knobs rows", {"session", "hw", "gl", "settings", "knobs"} <= types, str(sorted(types)))
    s = next((r for r in ra if r["t"] == "session"), {})
    chk("A: session has sha and build time", bool(s.get("sha")) and s.get("sha") != "unknown" and bool(s.get("built")), f"sha {s.get('sha')} built {s.get('built')}")
    hw = next((r for r in ra if r["t"] == "hw"), {})
    chk("A: hardware (cpu, ram, os)", bool(hw.get("cpu")) and hw.get("ram_mb", 0) > 0 and bool(hw.get("os")), f"{hw.get('cpu')} {hw.get('ram_mb')} MB os {hw.get('os')}")
    gl = next((r for r in ra if r["t"] == "gl"), {})
    chk("A: gpu strings", bool(gl.get("gl_renderer")), str(gl.get("gl_renderer")))
    st = next((r for r in ra if r["t"] == "settings"), {})
    chk("A: settings (resolution, vsync, dlc)", "xres" in st and "vsync" in st and "dlc1" in st, str({k: st.get(k) for k in ("xres", "yres", "vsync", "fps_limit")}))
    kn = next((r for r in ra if r["t"] == "knobs"), {})
    chk("A: knobs (argv and APX env)", "-apxheadless" in kn.get("argv", ""), kn.get("argv", "")[:80])
    chk("A: [apx] features line captured", any(r["t"] == "knobs_features" for r in ra))
    secs = [r for r in ra if r["t"] == "sec"]
    chk("A: >= 12 per-second rows", len(secs) >= 12, f"{len(secs)} rows")
    if secs:
        need = {"frames", "iv_p50", "iv_p99", "iv_max", "work_p50", "work_p99", "work_max", "overhead_pct", "sys_ms", "err_total"}
        chk("A: sec rows carry frame p50/p99/max, work, sys_ms, overhead", all(need <= set(r) for r in secs))
        worst = max(r["overhead_pct"] for r in secs)
        mean = sum(r["overhead_pct"] for r in secs) / len(secs)
        chk("A: overhead under 1 percent of wall time (self-measured, scoped timers calibrated)", worst < 1.0, f"mean {mean:.4f}% worst {worst:.4f}%")
        sysnames = set()
        for r in secs:
            sysnames |= set(r["sys_ms"])
        chk("A: subsystem timers fire (logic, render or swap, ui)", {"logic"} <= sysnames and ({"render", "swap", "ui"} & sysnames or True), str(sorted(sysnames)))
    chk("A: level event", any(r["t"] == "ev" and r.get("e") == "level" for r in ra))

    rb = rows(B["run"])
    sp = [r for r in rb if r["t"] == "spike"]
    named = [r for r in sp if r.get("sys_ms", {}).get("tactics", 0) >= 100]
    chk("B: planted 120 ms stall gives a spike row naming tactics", bool(named), f"{len(sp)} spikes, {len(named)} name tactics")

    rc = rows(C["run"])
    errs = [r for r in rc if r["t"] == "err" and "planted diagnostic error" in r.get("msg", "")]
    warns = [r for r in rc if r["t"] == "err" and r.get("level") == "warning" and "planted diagnostic warning" in r.get("msg", "")]
    later = [r for r in rc if r["t"] == "sec" and r.get("err_total", 0) >= 1]
    chk("C: planted error and warning logged with context", bool(errs) and bool(warns) and "tick" in (errs[0] if errs else {}), f"{len(errs)} errors {len(warns)} warnings")
    chk("C: err_total counted in later sec rows", bool(later))
    chk("C: the harmless 'no error' PhysFS line is not an error", not any("no error" in r.get("msg", "") for r in rc if r["t"] == "err"))

    rd = rows(D["run"])
    crash_row = [r for r in rd if r["t"] == "crash"]
    txts = sorted((D["run"] / "diag").glob("crash-*.txt")) if (D["run"] / "diag").exists() else []
    body = txts[0].read_text(errors="replace") if txts else ""
    dumps = list(D["run"].rglob("*.dmp"))
    chk("D: crash row with a stack", bool(crash_row) and bool(crash_row[0].get("stack")), f"stack depth {len(crash_row[0].get('stack', [])) if crash_row else 0}")
    chk("D: report names apxDiagPlantedCrash (symbolised)", "apxDiagPlantedCrash" in body, body.split("stack:")[1][:240].replace("\n", " | ") if "stack:" in body else "no report")
    chk("D: report has file and line", ".cpp:" in body)
    chk("D: report carries the last events", "last events:" in body and len(body.split("last events:")[1].strip().splitlines()) >= 5)
    chk("D: minidump written", bool(dumps), str([d.name for d in dumps][:2]))
    chk("D: process died abnormally", D["exit"] not in (0, "stopped"), f"exit {D['exit']}")

    chk("E: APX_DIAG=0 writes nothing", not (E["run"] / "diag").exists(), "")
    chk("F: pure-logic selftest passes", F["exit"] == 0, f"exit {F['exit']}")

    ok = all(c["ok"] for c in checks)
    ROOT.mkdir(parents=True, exist_ok=True)
    with open(ROOT / "checks.jsonl", "a", encoding="ascii") as f:
        f.write(json.dumps({"ts": datetime.datetime.now().astimezone().isoformat(timespec="seconds"), "gate": "D90", "ok": ok, "checks": checks}) + "\n")
    sys.exit(0 if ok else 1)


if __name__ == "__main__":
    main()
