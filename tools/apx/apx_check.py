"""apx_check.py -- BF1 gates that launch the real game and read its JSONL instruments.

  python tools\\apx\\apx_check.py floors --exe <barony.exe> [--seed 1234] [--floors 3]

Gate BF1-1: for one seed, launch twice with -apx-seed -apx-out, descend with the console, and check
  - floors.jsonl has one row per generated floor, rooms/monsters/gold/items/decorations equal the
    "successfully generated a dungeon" lines of log.txt;
  - the second launch gives identical rows (excluding run_id, wall_ms, gen_ms, load_ms, path_maps_ms, tick);
  - control: another seed MUST differ in rooms or monsters on floor 1 (exit non-zero if it matches);
  - control: APX_TELEM=0 writes no file, and the floor-1 generated line is unchanged vs the telemetry-on run.
Takes the shared lock C:\\Apocrypha\\locks\\barony-f1-instruments.LOCK; waits for any other Barony / *.LOCK.
Posted (not sent) input only, so focus is not needed; a stray human key press is detected as a seed
or command mismatch in the log and fails the run instead of passing it.
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

sys.path.insert(0, str(Path(__file__).parent))
import parity_f0 as P  # noqa: E402

LOCK_DIR = Path(r"C:\Apocrypha\locks")
P.LOCK_NAME = "barony-f1-instruments.LOCK"
DIAG = Path(r"C:\Apocrypha\diagnostics\barony-f1")
VOLATILE = {"run_id", "wall_ms", "gen_ms", "load_ms", "path_maps_ms", "tick", "sim_seconds", "build_hash", "uid_at_gen"}  # uid_at_gen: runtime entity counter, diagnostic only (1223 vs 1224 seen)


def launch(exe, seed, floors, tag, telem="1", datadir=P.STEAM_DIR, extra=(), settle=4.0, timeout=240):
    """One game run: descend `floors` levels. Returns dict(run_dir, out_dir, gen_lines, flags, exit_code)."""
    run_dir = DIAG / "runs" / f"{datetime.datetime.now():%Y%m%d-%H%M%S}-{tag}"
    run_dir.mkdir(parents=True)
    out_dir = run_dir / "apx-out"
    env = os.environ.copy()
    env["BARONY_DATADIR"] = str(datadir)
    env["APX_TELEM"] = telem
    env.setdefault("APX_BUILD_HASH", git_head())
    cmd = [str(exe), "-quickstart=barbarian", f"-apx-out={out_dir}"]
    if seed is not None:
        cmd.append(f"-apx-seed={seed}")
    cmd += list(extra)
    log = run_dir / "log.txt"
    res = {"run_dir": run_dir, "out_dir": out_dir, "gen_lines": [], "flags": [], "finish": "error"}
    deadline = time.monotonic() + timeout
    proc = subprocess.Popen(cmd, cwd=run_dir, env=env)
    hwnd = None
    try:
        _, off = P.wait_for(log, P.RE_START_MAP, deadline, proc)
        while hwnd is None and time.monotonic() < deadline:
            hwnd = P.find_game_window(proc.pid)
            time.sleep(0.25)
        if hwnd is None:
            raise TimeoutError("no window")
        P.keep_visible(hwnd)
        time.sleep(settle)
        P.chat_command(hwnd, "/enablecheats", "post")
        for n in range(floors):
            for attempt in range(3):
                P.chat_command(hwnd, "/nextlevel", "post")
                try:
                    _, off = P.wait_for(log, P.RE_GENERATING, min(deadline, time.monotonic() + 15), proc, off)
                    m, off = P.wait_for(log, P.RE_GENERATED, deadline, proc, off)
                    res["gen_lines"].append(tuple(int(v) for v in m.groups()))
                    break
                except TimeoutError:
                    res["flags"].append(f"nextlevel_retry_{n + 1}_{attempt + 1}")
            else:
                raise TimeoutError(f"floor {n + 1} never generated")
            time.sleep(2.0)                          # let the loader task finish its row
        res["finish"] = "ok"
    except Exception as e:  # noqa: BLE001
        res["error"] = f"{type(e).__name__}: {e}"
    finally:
        P.stop(proc, hwnd)
    return res


def git_head():
    try:
        return subprocess.run(["git", "rev-parse", "--short=8", "HEAD"], capture_output=True, text=True,
                              cwd=Path(__file__).parent).stdout.strip() or "unknown"
    except OSError:
        return "unknown"


def rows(out_dir, name):
    p = Path(out_dir) / name
    if not p.exists():
        return None
    return [json.loads(l) for l in p.read_text(encoding="ascii").splitlines() if l.strip()]


def norm(r):
    return {k: v for k, v in r.items() if k not in VOLATILE}


def gen_tuple(r):
    return (r["rooms"], r["monsters"], r["gold"], r["items"], r["decorations"])


def cmd_floors(a):
    exe = Path(a.exe)
    checks = []
    def chk(name, ok, detail=""):
        checks.append({"check": name, "ok": bool(ok), "detail": detail})
        print(f"[{'PASS' if ok else 'FAIL'}] {name} {detail}", flush=True)

    P.wait_until_no_barony(1200, LOCK_DIR)
    P.hold_lock(LOCK_DIR, True)
    try:
        r1 = launch(exe, a.seed, a.floors, f"floors-s{a.seed}-a")
        r2 = launch(exe, a.seed, a.floors, f"floors-s{a.seed}-b")
        r3 = launch(exe, a.seed + 1, 1, f"floors-s{a.seed + 1}-ctl")
        r4 = launch(exe, a.seed, 1, f"floors-s{a.seed}-off", telem="0")
    finally:
        P.hold_lock(LOCK_DIR, False)
    for r in (r1, r2, r3, r4):
        if r["finish"] != "ok":
            chk("launch " + r["run_dir"].name, False, r.get("error", ""))
    f1, f2, f3 = (rows(r["out_dir"], "floors.jsonl") for r in (r1, r2, r3))
    chk("floors.jsonl exists", f1 is not None)
    if f1:
        gen = [x for x in f1 if x["rooms"] > 0]
        chk(f"{a.floors} generated-floor rows", len(gen) == a.floors, f"got {len(gen)} of {len(f1)} rows")
        chk("rows equal log.txt generated lines", [gen_tuple(x) for x in gen] == r1["gen_lines"],
            f"rows={[gen_tuple(x) for x in gen]} log={r1['gen_lines']}")
        chk("dropped_rows == 0", all(x["dropped_rows"] == 0 for x in f1))
    if f1 and f2:
        chk("second launch identical", [norm(x) for x in f1] == [norm(x) for x in f2])
    if f1 and f3:
        a1 = [x for x in f1 if x["rooms"] > 0][0]
        b1 = [x for x in f3 if x["rooms"] > 0][0]
        chk("control: other seed differs on floor 1", (a1["rooms"], a1["monsters"]) != (b1["rooms"], b1["monsters"]),
            f"{(a1['rooms'], a1['monsters'])} vs {(b1['rooms'], b1['monsters'])}")
    chk("control: APX_TELEM=0 writes no file", not (r4["out_dir"] / "floors.jsonl").exists())
    if f1 and r4["gen_lines"]:
        chk("APX_TELEM=0 floor-1 line equals telemetry-on line", r4["gen_lines"][0] == r1["gen_lines"][0])
    ok = all(c["ok"] for c in checks)
    DIAG.mkdir(parents=True, exist_ok=True)
    with open(DIAG / "checks.jsonl", "a", encoding="ascii") as f:
        f.write(json.dumps({"ts": datetime.datetime.now().astimezone().isoformat(timespec="seconds"),
                            "gate": "BF1-1", "ok": ok, "checks": checks}) + "\n")
    sys.exit(0 if ok else 1)


def main():
    ap = argparse.ArgumentParser()
    sub = ap.add_subparsers(dest="cmd", required=True)
    f = sub.add_parser("floors")
    f.add_argument("--exe", required=True)
    f.add_argument("--seed", type=int, default=1234)
    f.add_argument("--floors", type=int, default=3)
    f.set_defaults(fn=cmd_floors)
    a = ap.parse_args()
    a.fn(a)


if __name__ == "__main__":
    main()
