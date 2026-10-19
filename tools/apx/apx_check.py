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

LOCK_DIR = Path(r"C:\Apocrypha\locks\barony")   # machine-wide shared Barony lock folder (all lanes)
P.LOCK_NAME = "barony-f1-instruments.LOCK"
LOCK_ROOT = Path(r"C:\Apocrypha\locks")             # f2 keeps barony-f2-gate.LOCK here: honour and hold both folders


SLOT_DIR = Path(r"C:\Apocrypha\locks\barony-slots")
_held = []


def acquire(exclusive=False):
    """D70 run rule. Correctness runs (hashes, gates, bot smoke) take a free slot file slot1..3.LOCK in barony-slots while
    C:\\Apocrypha\\locks\\barony is empty (up to 3 at once). Timing measurements pass exclusive=True: they take
    barony\\barony-f1-instruments.LOCK only when no slot file and no barony.exe exist. Polls every second."""
    t0 = time.monotonic()
    announced = None
    SLOT_DIR.mkdir(parents=True, exist_ok=True)
    while True:
        excl_dir = [x.name for x in LOCK_DIR.glob("*.LOCK")] if LOCK_DIR.exists() else []
        slots = [x for x in SLOT_DIR.glob("slot*.LOCK")]
        busy = []
        if exclusive:
            busy = P.barony_running() + excl_dir + [x.name for x in slots] + P.foreign_locks([LOCK_ROOT])
            if not busy:
                P.hold_lock([LOCK_DIR], True)
                time.sleep(1.5)
                again = P.barony_running() + [x.name for x in SLOT_DIR.glob("slot*.LOCK")]
                if not again:
                    _held.append(LOCK_DIR / P.LOCK_NAME)
                    return
                P.hold_lock([LOCK_DIR], False)
                busy = again
        else:
            busy = excl_dir + P.foreign_locks([LOCK_ROOT])
            if not busy:
                for n in (1, 2, 3):
                    f = SLOT_DIR / f"slot{n}.LOCK"
                    try:
                        with open(f, "x", encoding="ascii") as fh:      # atomic create: fails if another lane took it
                            fh.write(f"{datetime.datetime.now().astimezone().isoformat(timespec=chr(115))} barony-f1-instruments {os.getpid()}\n")
                        _held.append(f)
                        time.sleep(1.0)
                        if [x for x in LOCK_DIR.glob("*.LOCK")]:         # an exclusive holder appeared meanwhile: back off
                            f.unlink(missing_ok=True)
                            _held.remove(f)
                            busy = ["exclusive holder appeared"]
                            break
                        return
                    except FileExistsError:
                        continue
                else:
                    busy = ["all 3 slots taken"]
        if time.monotonic() - t0 > 86400:
            sys.exit("Barony stayed busy for 24 h; giving up.")
        if busy and busy != announced:
            print(f"[apx] waiting for {chr(44).join(busy)} ...", flush=True)
            announced = busy
        time.sleep(1.0)


def release():
    for f in list(_held):
        f.unlink(missing_ok=True)
        _held.remove(f)
    time.sleep(5)


def launch(exe, seed, floors, tag, telem="1", datadir=P.STEAM_DIR, extra=(), settle=4.0, timeout=240, env_extra=None, after=None):
    """One game run: descend `floors` levels. Returns dict(run_dir, out_dir, gen_lines, flags, exit_code)."""
    run_dir = DIAG / "runs" / f"{datetime.datetime.now():%Y%m%d-%H%M%S}-{tag}"
    run_dir.mkdir(parents=True)
    out_dir = run_dir / "apx-out"
    env = os.environ.copy()
    env["BARONY_DATADIR"] = str(datadir)
    env["APX_TELEM"] = telem
    env.update(env_extra or {})
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
        if after:
            after(hwnd, log, proc, deadline)
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

    acquire()
    try:
        r1 = launch(exe, a.seed, a.floors, f"floors-s{a.seed}-a")
        r2 = launch(exe, a.seed, a.floors, f"floors-s{a.seed}-b")
        r3 = launch(exe, a.seed + 1, 1, f"floors-s{a.seed + 1}-ctl")
        r4 = launch(exe, a.seed, 1, f"floors-s{a.seed}-off", telem="0")
    finally:
        release()
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


def hashes(r):
    rs = rows(r["out_dir"], "floors.jsonl") or []
    return [x["floor_hash"] for x in rs if x["rooms"] > 0]


def cmd_hash(a):
    exe = Path(a.exe)
    checks = []

    def chk(name, ok, detail=""):
        checks.append({"check": name, "ok": bool(ok), "detail": detail})
        print(f"[{ok and chr(80) + chr(65) + chr(83) + chr(83) or chr(70) + chr(65) + chr(73) + chr(76)}] {name} {detail}", flush=True)

    seeds = [a.seed0 + i for i in range(a.seeds)]
    acquire()
    runs = {}
    try:
        for s in seeds:
            runs[s] = []
            for k in "ab":
                r = launch(exe, s, a.floors, f"hash-s{s}-{k}")
                if len(hashes(r)) < a.floors:      # one retry for a lost posted key press
                    r = launch(exe, s, a.floors, f"hash-s{s}-{k}-retry")
                runs[s].append(r)
        ctl_seed = seeds[0]
        poke = launch(exe, ctl_seed, a.floors, f"hash-s{ctl_seed}-poke", env_extra={"APX_HASH_POKE": "1"})
        bad = [launch(exe, ctl_seed, a.floors, f"hash-s{ctl_seed}-bad{k}", env_extra={"APX_HASH_BAD": "1"}) for k in "ab"]
    finally:
        release()
    pairs = equal = 0
    for s, (ra, rb) in runs.items():
        ha, hb = hashes(ra)[:a.floors], hashes(rb)[:a.floors]
        for x, y in zip(ha, hb):
            pairs += 1
            equal += (x == y)
        if len(ha) != a.floors or len(hb) != a.floors:
            chk(f"seed {s} produced {a.floors} hashes in both launches", False, f"{len(ha)}/{len(hb)}")
    chk(f"all {pairs} pairs hash-equal", pairs == a.seeds * a.floors and equal == pairs, f"equal {equal} of {pairs}")
    f1 = {hashes(ra)[0] for ra, _ in runs.values() if hashes(ra)}
    chk("floor-1 hashes distinct across seeds", len(f1) >= a.seeds - 1, f"{len(f1)} distinct of {a.seeds}")
    base = hashes(runs[ctl_seed][0])
    chk("control: poked wall tile changes the hash", hashes(poke)[:a.floors] != base[:a.floors] and len(hashes(poke)) >= a.floors,
        f"{hashes(poke)[:1]} vs {base[:1]}")
    badpairs = sum(x == y for x, y in zip(hashes(bad[0])[:a.floors], hashes(bad[1])[:a.floors]))
    chk("control: ticks mixed into the hash fails the repeat gate", badpairs < a.floors, f"{badpairs} of {a.floors} equal")
    ok = all(c["ok"] for c in checks)
    with open(DIAG / "checks.jsonl", "a", encoding="ascii") as f:
        f.write(json.dumps({"ts": datetime.datetime.now().astimezone().isoformat(timespec="seconds"),
                            "gate": "BF1-2", "ok": ok, "checks": checks}) + "\n")
    sys.exit(0 if ok else 1)


def run_sim(exe, scenario, tag, env_extra=None, wall_s=1800, datadir=P.STEAM_DIR, extra=(), cls="barbarian"):
    """One unattended sim run. Returns dict(exit, wall_s, out_dir, run_dir, rows)."""
    run_dir = DIAG / "runs" / f"{datetime.datetime.now():%Y%m%d-%H%M%S}-{tag}"
    run_dir.mkdir(parents=True)
    out_dir = run_dir / "apx-out"
    env = os.environ.copy()
    env["BARONY_DATADIR"] = str(datadir)
    env["APX_TELEM"] = "1"
    env["APX_SIM_WALL_S"] = str(wall_s)
    env.setdefault("APX_BUILD_HASH", git_head())
    env.update(env_extra or {})
    cmd = [str(exe), f"-quickstart={cls}", f"-apx-out={out_dir}", f"-apx-scenario={scenario}", "-nosound"] + list(extra)
    t0 = time.monotonic()
    proc = subprocess.Popen(cmd, cwd=run_dir, env=env)
    try:
        code = proc.wait(wall_s + 120)
    except subprocess.TimeoutExpired:
        proc.kill()
        proc.wait(30)
        code = -9
    wall = time.monotonic() - t0
    return {"exit": code, "wall_s": wall, "out_dir": out_dir, "run_dir": run_dir,
            "rows": rows(out_dir, "encounters.jsonl") or []}


def fights(n):
    def run(hwnd, log, proc, deadline):
        for i in range(n):
            P.chat_command(hwnd, "/summonall skeleton", "post")
            for _ in range(2):
                time.sleep(4.5)
                P.chat_command(hwnd, "/heal", "post")
            time.sleep(2.0)
            P.chat_command(hwnd, "/killmonsters", "post")
            time.sleep(8.0)                          # encounter closes 5 s after the last engaged hostile
    return run


def cmd_enc(a):
    exe = Path(a.exe)
    checks = []

    def chk(name, ok, detail=""):
        checks.append({"check": name, "ok": bool(ok), "detail": detail})
        print(f"[{chr(80) + chr(65) + chr(83) + chr(83) if ok else chr(70) + chr(65) + chr(73) + chr(76)}] {name} {detail}", flush=True)

    acquire()
    try:
        sc = str(Path(__file__).parent / "scenarios" / "enc20.json")
        good = run_sim(exe, sc, "enc-good", wall_s=600)
        ctl = run_sim(exe, sc, "enc-ctl", wall_s=600, env_extra={"APX_BREAK_KIND": a.control_kind})
    finally:
        release()
    er = good["rows"]
    chk("launch ok", good["exit"] == 0, f"exit {good[chr(101)+chr(120)+chr(105)+chr(116)]}")
    chk("20 encounters", len(er) == 20, f"{len(er)} rows")
    bad = [r["enc_id"] for r in er if r["unattributed_hp"] != 0]
    chk("unattributed_hp == 0 in every row", not bad, f"bad enc_ids {bad}")
    summ = [r["enc_id"] for r in er if sum(r["dmg_in_by_source"].values()) != r["dmg_in_total"]]
    chk("sum(dmg_in_by_source) == dmg_in_total", not summ, f"bad {summ}")
    second = [r["enc_id"] for r in er
              if sum(h0 - h1 for h0, h1 in zip(r["party_hp_start"], r["party_hp_end"])) + r["heals_total"] != r["dmg_in_total"]]
    chk("hp_start - hp_end + heals == dmg_in_total (second reading)", not second, f"bad {second}")
    chk("some melee damage attributed", sum(r["dmg_in_by_source"]["melee"] for r in er) > 0)
    chk("kills > 0", sum(r["kills"] for r in er) > 0, f"kills={sum(r[chr(107) + chr(105) + chr(108) + chr(108) + chr(115)] for r in er)}")
    chk("dropped_rows == 0", all(r["dropped_rows"] == 0 for r in er))
    cr = ctl["rows"]
    cu = sum(r["unattributed_hp"] for r in cr)
    cb = sum(r["dmg_in_by_source"][a.control_kind] for r in cr)
    chk(f"control: APX_BREAK_KIND={a.control_kind} shows unattributed_hp > 0 and {a.control_kind}=0",
        cu > 0 and cb == 0, f"unattributed={cu} {a.control_kind}={cb} rows={len(cr)}")
    ok = all(c["ok"] for c in checks)
    with open(DIAG / "checks.jsonl", "a", encoding="ascii") as f:
        f.write(json.dumps({"ts": datetime.datetime.now().astimezone().isoformat(timespec="seconds"),
                            "gate": "BF1-3", "ok": ok, "checks": checks}) + "\n")
    sys.exit(0 if ok else 1)


def cmd_sim(a):
    exe = Path(a.exe).resolve()
    sc = Path(__file__).parent / "scenarios"
    checks = []

    def chk(name, ok, detail=""):
        checks.append({"check": name, "ok": bool(ok), "detail": detail})
        print(f"[{chr(80) + chr(65) + chr(83) + chr(83) if ok else chr(70) + chr(65) + chr(73) + chr(76)}] {name} {detail}", flush=True)

    acquire(exclusive=True)   # sim speed is a timing measurement
    try:
        f1 = run_sim(exe, str(sc / "skel4-f1.json"), "sim-f1", wall_s=1500)
        f2 = run_sim(exe, str(sc / "skel4-f2.json"), "sim-f2", wall_s=1500)
        d1 = run_sim(exe, str(sc / "det100.json"), "sim-det-a", wall_s=900)
        d2 = run_sim(exe, str(sc / "det100.json"), "sim-det-b", wall_s=900)
        z = run_sim(exe, str(sc / "skel4-f1-skill0.json"), "sim-skill0", wall_s=900)
        wl = run_sim(exe, str(sc / "skel4-f1.json"), "sim-walllimit", wall_s=60)
    finally:
        release()
    rows_all = f1["rows"] + f2["rows"]
    wall = f1["wall_s"] + f2["wall_s"]
    chk("1000 encounters finished", len(rows_all) >= 1000 and f1["exit"] == 0 and f2["exit"] == 0, f"{len(rows_all)} rows exit {f1[chr(101)+chr(120)+chr(105)+chr(116)]}/{f2[chr(101)+chr(120)+chr(105)+chr(116)]}")
    chk("under 25 wall minutes", wall < 1500, f"{wall:.0f} s")
    last = f1["rows"][-1] if f1["rows"] else None
    if last:
        rate = (last["tick"] / 50.0) / (last["wall_ms"] / 1000.0)
        chk("sim speed >= 20 sim s per wall s", rate >= 20, f"{rate:.1f} (f1 run, includes startup)")
    stuck = (f1["rows"][-1]["bots_stuck"] if f1["rows"] else 0) + (f2["rows"][-1]["bots_stuck"] if f2["rows"] else 0)
    chk("bots_stuck rate < 2 percent", rows_all and stuck / len(rows_all) < 0.02, f"{stuck} of {len(rows_all)}")
    chk("unattributed_hp == 0 in all rows", all(r["unattributed_hp"] == 0 for r in rows_all), f"{sum(1 for r in rows_all if r[chr(117)+chr(110)+chr(97)+chr(116)+chr(116)+chr(114)+chr(105)+chr(98)+chr(117)+chr(116)+chr(101)+chr(100)+chr(95)+chr(104)+chr(112)] != 0)} rows")
    vol = VOLATILE | {"encounter_wall_ms", "uid_at_gen", "t_open", "t_close", "sim_seconds", "tick"}   # absolute ticks differ with startup timing; durations stay compared
    n = lambda rs: [{k: v for k, v in r.items() if k not in vol} for r in rs]
    chk("two launches row-identical", len(d1["rows"]) == 100 and n(d1["rows"]) == n(d2["rows"]),
        f"{len(d1[chr(114)+chr(111)+chr(119)+chr(115)])}/{len(d2[chr(114)+chr(111)+chr(119)+chr(115)])} rows")
    zr = z["rows"]
    bad = sum(1 for r in zr if r["outcome"] in ("wipe", "flee"))
    chk("control: skill 0.0 gives wipe/flee in >= 90 percent", zr and bad / len(zr) >= 0.9, f"{bad} of {len(zr)}, kills {sum(r[chr(107)+chr(105)+chr(108)+chr(108)+chr(115)] for r in zr)}")
    chk("control: wall limit exits non-zero", wl["exit"] != 0, f"exit {wl[chr(101)+chr(120)+chr(105)+chr(116)]}")
    ok = all(c["ok"] for c in checks)
    with open(DIAG / "checks.jsonl", "a", encoding="ascii") as f:
        f.write(json.dumps({"ts": datetime.datetime.now().astimezone().isoformat(timespec="seconds"),
                            "gate": "BF1-4", "ok": ok, "checks": checks}) + "\n")
    sys.exit(0 if ok else 1)


def print_window_luma(hwnd, out_png, flags=3):
    """PrintWindow capture of the game window (the control: a fullscreen GL window comes back black)."""
    import ctypes
    from ctypes import wintypes as wt
    from PIL import Image
    u, g = ctypes.windll.user32, ctypes.windll.gdi32
    r = wt.RECT()
    u.GetClientRect(hwnd, ctypes.byref(r))
    w, h = r.right - r.left, r.bottom - r.top
    hdc = u.GetDC(hwnd)
    mem = g.CreateCompatibleDC(hdc)
    bmp = g.CreateCompatibleBitmap(hdc, w, h)
    g.SelectObject(mem, bmp)
    u.PrintWindow(hwnd, mem, flags)
    buf = ctypes.create_string_buffer(w * h * 4)
    class BIH(ctypes.Structure):
        _fields_ = [("s", wt.DWORD), ("w", wt.LONG), ("h", wt.LONG), ("p", wt.WORD), ("b", wt.WORD), ("c", wt.DWORD),
                    ("i", wt.DWORD), ("x", wt.LONG), ("y", wt.LONG), ("u", wt.DWORD), ("m", wt.DWORD)]
    bi = BIH(ctypes.sizeof(BIH), w, -h, 1, 32, 0, 0, 0, 0, 0, 0)
    g.GetDIBits(mem, bmp, 0, h, buf, ctypes.byref(bi), 0)
    img = Image.frombuffer("RGBA", (w, h), buf, "raw", "BGRA", 0, 1).convert("RGB")
    img.save(out_png)
    g.DeleteObject(bmp); g.DeleteDC(mem); u.ReleaseDC(hwnd, hdc)
    import numpy as np
    return float(np.asarray(img.convert("L")).mean())


def shot_stats(png):
    import numpy as np
    from PIL import Image
    im = np.asarray(Image.open(png).convert("RGB")).astype("float32")
    l = im @ np.array([0.2126, 0.7152, 0.0722], dtype="float32")
    h, w = l.shape
    hud = l[int(h * 0.85):, : int(w * 0.35)]
    world = l[int(h * 0.30): int(h * 0.60), int(w * 0.30): int(w * 0.70)]
    hh = np.histogram(hud, bins=16, range=(0, 255))[0] / hud.size
    hw = np.histogram(world, bins=16, range=(0, 255))[0] / world.size
    return {"mean": float(l.mean()), "std": float(l.std()), "bytes": Path(png).stat().st_size,
            "hud_world_l1": float(np.abs(hh - hw).sum())}


def shot_after(name, hdr, control=False, minimize=False, f6=False):
    def run(hwnd, log, proc, deadline):
        out = Path(log).parent / "apx-out"
        if hdr == 0:
            P.chat_command(hwnd, "/hdr_enabled 0", "post")
        time.sleep(2.5)
        if minimize:
            P.user32.ShowWindow(hwnd, 6)                  # SW_MINIMIZE
            time.sleep(1.5)
        if control:
            try:
                l0 = print_window_luma(hwnd, Path(log).parent / "printwindow_plain.png", 0)
                l3 = print_window_luma(hwnd, Path(log).parent / "printwindow_full.png", 3)
                (Path(log).parent / "printwindow.txt").write_text(f"plain {l0:.3f} renderfull {l3:.3f}")
            except Exception as e:  # noqa: BLE001
                (Path(log).parent / "printwindow.txt").write_text(f"error {e}")
        (out / "shot.req").write_text(name)
        t0 = time.monotonic()
        while (out / "shot.req").exists() and time.monotonic() - t0 < 8:
            time.sleep(0.2)
        time.sleep(1.0)
        if f6:
            P.press(hwnd, P.VK_F6, P.SC_F6, "post")
            time.sleep(2.0)
    return run


def cmd_shot(a):
    exe = Path(a.exe).resolve()
    checks = []

    def chk(name, ok, detail=""):
        checks.append({"check": name, "ok": bool(ok), "detail": detail})
        print(f"[{chr(80) + chr(65) + chr(83) + chr(83) if ok else chr(70) + chr(65) + chr(73) + chr(76)}] {name} {detail}", flush=True)

    import numpy as np
    from PIL import Image
    acquire()
    runs = {}
    try:
        for mode in ("windowed", "borderless", "fullscreen"):
            for hdr in (1, 0):
                nm = f"{mode}_hdr{hdr}"
                runs[nm] = launch(exe, 1234, 1, f"shot-{nm}", extra=[f"-apx-window={mode}"], env_extra={"APX_SHOT_TWICE": "1"},
                                  after=shot_after(nm, hdr, control=(mode == "fullscreen" and hdr == 1), f6=(nm == "windowed_hdr1")))
        runs["minimized"] = launch(exe, 1234, 1, "shot-minimized", extra=["-apx-window=windowed"],
                                   env_extra={"APX_SHOT_TWICE": "1"}, after=shot_after("minimized", 1, minimize=True))
        runs["old_site"] = launch(exe, 1234, 1, "shot-old", extra=["-apx-window=windowed"], env_extra={"APX_SHOT_OLD": "1", "APX_SHOT_TWICE": "1"},
                                  after=shot_after("old", 1, f6=True))
    finally:
        release()
    for nm, r in runs.items():
        sd = r["out_dir"] / "shots"
        pa, pb = sd / f"{nm}_a.png", sd / f"{nm}_b.png"
        if nm == "old_site":
            print(f"[INFO] old_site (APX_SHOT_OLD=1 only changes the in-game key site; tool path unchanged) shots exist: {pa.exists()}")
            continue
        if not pa.exists():
            if nm == "minimized":
                print(f"[INFO] minimized window: no shot written (recorded, not assumed)")
            else:
                chk(f"{nm} shot written", False, r.get("error", ""))
            continue
        s = shot_stats(pa)
        if nm == "minimized":
            print(f"[INFO] minimized window: shot written, luma {s[chr(109)+chr(101)+chr(97)+chr(110)]:.1f} std {s[chr(115)+chr(116)+chr(100)]:.1f}")
            continue
        ok = s["mean"] > 20 and s["std"] > 15 and s["bytes"] > 100_000 and s["hud_world_l1"] > 0.1
        chk(f"{nm}: luma>20, std>15, >100KB, HUD!=world", ok, str({k: round(v, 2) for k, v in s.items()}))
        if pb.exists():
            ia = np.asarray(Image.open(pa).convert("RGB")).astype(int)
            ib = np.asarray(Image.open(pb).convert("RGB")).astype(int)
            same = (np.abs(ia - ib) <= 1).mean() if ia.shape == ib.shape else 0.0
            chk(f"{nm}: two reads of one frame agree within 1 level", same >= 0.999, f"{same * 100:.3f}% of values (same swap, same tick)")
    t = runs["fullscreen_hdr1"]["run_dir"] / "printwindow.txt"
    pw = t.read_text() if t.exists() else "missing"
    print(f"[INFO] PrintWindow of the fullscreen window: {pw}")
    try:
        lumas = [float(x) for x in pw.split() if x.replace(".", "", 1).isdigit()]
        chk("control: PrintWindow of the fullscreen window is black (luma < 2)", min(lumas) < 2, pw)
    except ValueError:
        chk("control: PrintWindow of the fullscreen window is black (luma < 2)", False, pw)
    f6s = sorted((runs["windowed_hdr1"]["out_dir"] / "shots").glob("Screenshot*.png"))
    print("[INFO] F6 key through the hook, windowed:", [(x.name, round(shot_stats(x)["mean"], 1)) for x in f6s])
    o6 = sorted(runs["old_site"]["run_dir"].glob("Screenshot*.png"))
    print("[INFO] F6 key at the OLD call site (APX_SHOT_OLD=1):", [(x.name, round(shot_stats(x)["mean"], 1)) for x in o6])
    ok = all(c["ok"] for c in checks)
    with open(DIAG / "checks.jsonl", "a", encoding="ascii") as f:
        f.write(json.dumps({"ts": datetime.datetime.now().astimezone().isoformat(timespec="seconds"),
                            "gate": "BF1-5", "ok": ok, "checks": checks}) + "\n")
    sys.exit(0 if ok else 1)


def cmd_party(a):
    exe = Path(a.exe).resolve()
    sc = Path(__file__).parent / "scenarios"
    checks = []

    def chk(name, ok, detail=""):
        checks.append({"check": name, "ok": bool(ok), "detail": detail})
        print(f"[{chr(80) + chr(65) + chr(83) + chr(83) if ok else chr(70) + chr(65) + chr(73) + chr(76)}] {name} {detail}", flush=True)

    acquire()
    try:
        res = {n: run_sim(exe, str(sc / f"party{n}.json"), f"party{n}", wall_s=900) for n in (1, 2, 3, 4)}
    finally:
        release()
    for n, r in res.items():
        rs = r["rows"]
        chk(f"party {n}: 50 encounters, exit 0, no crash", r["exit"] == 0 and len(rs) == 50, f"exit {r[chr(101)+chr(120)+chr(105)+chr(116)]} rows {len(rs)} wall {r[chr(119)+chr(97)+chr(108)+chr(108)+chr(95)+chr(115)]:.0f} s")
        chk(f"party {n}: rows carry party_n and one hp entry per player", rs and all(x["party_n"] == n and len(x["party_hp_start"]) in (0, n) for x in rs),
            f"party_n {sorted({x[chr(112)+chr(97)+chr(114)+chr(116)+chr(121)+chr(95)+chr(110)] for x in rs})} hp lens {sorted({len(x[chr(112)+chr(97)+chr(114)+chr(116)+chr(121)+chr(95)+chr(104)+chr(112)+chr(95)+chr(115)+chr(116)+chr(97)+chr(114)+chr(116)]) for x in rs})}")
        chk(f"party {n}: unattributed_hp == 0", all(x["unattributed_hp"] == 0 for x in rs))
    ok = all(c["ok"] for c in checks)
    with open(DIAG / "checks.jsonl", "a", encoding="ascii") as f:
        f.write(json.dumps({"ts": datetime.datetime.now().astimezone().isoformat(timespec="seconds"),
                            "gate": "BF1-4-party", "ok": ok, "checks": checks}) + "\n")
    sys.exit(0 if ok else 1)


def main():
    ap = argparse.ArgumentParser()
    sub = ap.add_subparsers(dest="cmd", required=True)
    f = sub.add_parser("floors")
    f.add_argument("--exe", required=True)
    f.add_argument("--seed", type=int, default=1234)
    f.add_argument("--floors", type=int, default=3)
    f.set_defaults(fn=cmd_floors)
    e = sub.add_parser("enc")
    e.add_argument("--exe", required=True)
    e.add_argument("--seed", type=int, default=77)
    e.add_argument("--fights", type=int, default=20)
    e.add_argument("--control-kind", default="melee")
    e.set_defaults(fn=cmd_enc)
    sh = sub.add_parser("shot")
    sh.add_argument("--exe", required=True)
    sh.set_defaults(fn=cmd_shot)
    pa = sub.add_parser("party")
    pa.add_argument("--exe", required=True)
    pa.set_defaults(fn=cmd_party)
    s = sub.add_parser("sim")
    s.add_argument("--exe", required=True)
    s.set_defaults(fn=cmd_sim)
    h = sub.add_parser("hash")
    h.add_argument("--exe", required=True)
    h.add_argument("--seed0", type=int, default=101)
    h.add_argument("--seeds", type=int, default=20)
    h.add_argument("--floors", type=int, default=5)
    h.set_defaults(fn=cmd_hash)
    a = ap.parse_args()
    a.fn(a)


if __name__ == "__main__":
    main()
