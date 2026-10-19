#!/usr/bin/env python3
"""tools/apx/gate_f2.py -- slice F2 gates for the apx-gen floor generator inside barony-apx.

Every session is one game process in its own run folder (BARONY_DATADIR = the Steam install), driven
with posted chat commands like parity_f0.py: /enablecheats, /god, then per floor
    /setmapseed <seed>   /nextlevel
and the game's own log lines are the measurements ("apxgen: ..." from src/apx/apx_gen_bridge.cpp).
Pictures are F6: the game's framebuffer read (glReadPixels), so a covered window still yields pixels.

  sweep  W2 + W5: N seeds, K floors per process; rows per floor (W2 verdict, hash, ms, counts)
  w1     load ms, FPS p50/p99 in the floor's largest room (/apxhall), save size, F6 pictures,
         for each --sizes value (and 'vanilla' = the control generator)

Rows go to <out>/f2.jsonl. One Barony process at a time: honours *.LOCK in --lock-dir and holds its own.
"""

import argparse
import datetime
import json
import os
import re
import sys
import time
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
import parity_f0 as pf  # noqa: E402

pf.LOCK_NAME = "barony-f2-gate.LOCK"

RE_W2 = re.compile(r"apxgen: W2 (PASS|FAIL) connected (\d) \((\d+)/(\d+) walk tiles\) locks (\d) \((\d+)/(\d+) opened\) "
                   r"exit (\d) keys-behind-own-lock (\d+) control (\d+) locks-unsealed (\d+)")
RE_HASH = re.compile(r"apxgen: hash ([0-9a-f]{16}) apx-floor ([0-9a-f]{16}) apx-dress ([0-9a-f]{16}) client (\d) "
                     r"ms gen ([\d.]+) load ([\d.]+) entities ([\d.]+)")
RE_FLOOR = re.compile(r"apxgen: floor seed (\d+) level (-?\d+) set '([^']*)' size (\d+)x(\d+) theme (\S+) attempt (\d+) "
                      r"rooms (\d+) corridors (\d+) tiles; textures floor (\d+) wall (\d+) ceiling (\d+)")
RE_ENTS = re.compile(r"apxgen: entities (\d+): (.*)")
RE_FAIL = re.compile(r"apxgen: (FAILED|FALLBACK).*")
RE_VANILLA = re.compile(r"apx: vanilla floor (\d+)x(\d+) walk tiles (\d+)")
RE_FPS = re.compile(r"apxfps: frames (\d+) seconds ([\d.]+) frame-ms p50 ([\d.]+) p99 ([\d.]+) max ([\d.]+) "
                    r"fps-p50 ([\d.]+) fps-p99 ([\d.]+) map (\d+)x(\d+)")
RE_HALL = re.compile(r"apxhall: at (\d+),(\d+) \(room (\d+) floor tiles\)|apxhall: no hall")
RE_LEVEL_DONE = re.compile(r"successfully generated a dungeon with (\d+) rooms, (\d+) monsters, (\d+) gold, (\d+) items, (\d+) decorations\.")
RE_LOADMAP = re.compile(r"LoadMap (\S+)")


def start_game(exe, run_dir, extra, datadir, args):
    env = os.environ.copy()
    for k in list(env):
        if k.startswith("APXGEN"):
            env.pop(k)
    env["BARONY_DATADIR"] = str(datadir)
    env.update(extra.get("env", {}))
    cmd = [str(exe), "-quickstart=barbarian"] + extra.get("args", [])
    proc = pf.subprocess.Popen(cmd, cwd=run_dir, env=env)
    return proc, cmd


def boot(proc, log, deadline, args):
    _, off = pf.wait_for(log, pf.RE_START_MAP, deadline, proc)
    hwnd = None
    while hwnd is None and time.monotonic() < deadline:
        hwnd = pf.find_game_window(proc.pid)
        time.sleep(0.25)
    if hwnd is None:
        raise TimeoutError("no SDL window")
    pf.keep_visible(hwnd)
    time.sleep(args.settle)
    for attempt in range(4):
        pf.chat_command(hwnd, "/enablecheats")
        pf.chat_command(hwnd, "/god")
        try:
            _, off = pf.wait_for(log, re.compile(r"[Cc]heats? (enabled|on)|God mode|godmode", re.I),
                                 min(deadline, time.monotonic() + 5), proc, off)
            break
        except TimeoutError:
            pass
    return hwnd, off


def next_floor(hwnd, proc, log, seed, off, deadline):
    """setmapseed + nextlevel; returns (log offset after the level, text of that level's lines)."""
    re_seed = re.compile(rf"Set next map seed to: {seed}\b")
    for _ in range(4):
        pf.chat_command(hwnd, f"/setmapseed {seed}")
        try:
            _, off = pf.wait_for(log, re_seed, min(deadline, time.monotonic() + 6), proc, off)
            break
        except TimeoutError:
            pass
    else:
        raise TimeoutError("setmapseed not confirmed")
    t0 = time.monotonic()
    for _ in range(3):
        pf.chat_command(hwnd, "/nextlevel")
        try:
            m, off2 = pf.wait_for(log, RE_LOADMAP, min(deadline, time.monotonic() + 15), proc, off)
            break
        except TimeoutError:
            pass
    else:
        raise TimeoutError("nextlevel did not load a map")
    # a generated floor ends with the 'successfully generated' line; a fixed floor has none
    try:
        m, end = pf.wait_for(log, RE_LEVEL_DONE, min(deadline, time.monotonic() + 30), proc, off)
    except TimeoutError:
        end = off2
    wall_s = time.monotonic() - t0
    return end, pf.read_log(log)[off:end], wall_s


def parse_floor(text):
    row = {}
    m = RE_FLOOR.search(text)
    if m:
        g = m.groups()
        row.update(seed=int(g[0]), level=int(g[1]), levelset=g[2], W=int(g[3]), H=int(g[4]), theme=g[5],
                   attempt=int(g[6]), rooms=int(g[7]), corridor_tiles=int(g[8]),
                   tex=[int(g[9]), int(g[10]), int(g[11])])
    m = RE_ENTS.search(text)
    if m:
        row["entities"] = int(m.group(1))
        row["counts"] = {k: int(v) for k, v in re.findall(r"([a-z-]+) (\d+)", m.group(2))}
    m = RE_W2.search(text)
    if m:
        g = m.groups()
        row["w2"] = dict(verdict=g[0], connected=int(g[1]), reached=int(g[2]), walk=int(g[3]), locks=int(g[4]),
                         opened=int(g[5]), lock_doors=int(g[6]), exit=int(g[7]), keys_behind_own_lock=int(g[8]), locks_unsealed=int(g[10]),
                         control=int(g[9]))
    m = RE_HASH.search(text)
    if m:
        g = m.groups()
        row.update(hash=g[0], apx_floor=g[1], apx_dress=g[2], client=int(g[3]),
                   ms_gen=float(g[4]), ms_load=float(g[5]), ms_entities=float(g[6]))
    m = RE_VANILLA.search(text)
    if m:
        row["vanilla"] = dict(W=int(m.group(1)), H=int(m.group(2)), walk=int(m.group(3)))
    m = RE_LEVEL_DONE.search(text)
    if m:
        row["generated"] = dict(zip(("rooms", "monsters", "gold", "items", "decorations"), map(int, m.groups())))
    fails = [x.group(0) for x in RE_FAIL.finditer(text)]
    if fails:
        row["apx_fail"] = fails
    lm = RE_LOADMAP.findall(text)
    row["loadmaps"] = lm[-2:]
    return row


def f6(hwnd, run_dir, name):
    before = set(run_dir.glob("*.png"))
    pf.press(hwnd, pf.VK_F6, pf.SC_F6, "post")
    t = time.monotonic() + 10
    while time.monotonic() < t:
        new = sorted(set(run_dir.glob("*.png")) - before)
        if new:
            time.sleep(0.6)
            dst = run_dir / f"{name}.png"
            new[0].replace(dst)
            return str(dst)
        time.sleep(0.25)
    return None


def session(label, exe, seeds, extra, args, rows_out, w1=False):
    stamp = datetime.datetime.now().strftime("%Y%m%d-%H%M%S")
    run_dir = args.out / "runs" / f"{stamp}-{label}"
    run_dir.mkdir(parents=True)
    log = run_dir / "log.txt"
    t0 = time.monotonic()
    deadline = t0 + args.run_timeout + 25 * len(seeds)
    proc, cmd = start_game(exe, run_dir, extra, args.datadir, args)
    hwnd = None
    base = {"ts": datetime.datetime.now().astimezone().isoformat(timespec="seconds"), "label": label,
            "exe": str(exe), "cmd": cmd, "env": extra.get("env", {}), "run_dir": str(run_dir)}
    try:
        hwnd, off = boot(proc, log, deadline, args)
        for i, seed in enumerate(seeds):
            row = dict(base, kind="w1" if w1 else "floor", seed_asked=seed, index=i, finish="error", flags=[])
            try:
                off, text, wall_s = next_floor(hwnd, proc, log, seed, off, deadline)
                row.update(parse_floor(text))
                row["level_wall_s"] = round(wall_s, 2)
                if w1:
                    time.sleep(args.shot_delay)
                    row["shot_arrive"] = f6(hwnd, run_dir, f"s{seed}-arrive")
                    if "hash" in row:
                        pf.chat_command(hwnd, "/apxhall")
                        try:
                            hm, off = pf.wait_for(log, RE_HALL, min(deadline, time.monotonic() + 6), proc, off)
                            row["hall"] = hm.group(0)
                        except TimeoutError:
                            row["flags"].append("no_hall")
                        time.sleep(2.5)
                    pf.chat_command(hwnd, "/apxfps_begin")
                    time.sleep(args.fps_seconds)
                    pf.chat_command(hwnd, "/apxfps_end")
                    try:
                        fm, off = pf.wait_for(log, RE_FPS, min(deadline, time.monotonic() + 6), proc, off)
                        g = fm.groups()
                        row["fps"] = dict(frames=int(g[0]), seconds=float(g[1]), ms_p50=float(g[2]), ms_p99=float(g[3]),
                                          ms_max=float(g[4]), fps_p50=float(g[5]), fps_p99=float(g[6]))
                    except TimeoutError:
                        row["flags"].append("no_fps")
                    row["shot_hall"] = f6(hwnd, run_dir, f"s{seed}-hall")
                    saves = sorted((run_dir / "savegames").glob("*")) if (run_dir / "savegames").exists() else []
                    row["saves"] = {p.name: p.stat().st_size for p in saves if p.is_file()}
                row["finish"] = "ok"
            except Exception as e:  # noqa: BLE001
                row["error"] = f"{type(e).__name__}: {e}"
                row["finish"] = "timeout" if isinstance(e, TimeoutError) else "error"
            rows_out.append(row)
            with open(args.out / "f2.jsonl", "a", encoding="utf-8") as fh:
                fh.write(json.dumps(row) + "\n")
            print(f"[f2] {label} seed {seed}: {row['finish']} w2={row.get('w2', {}).get('verdict')} "
                  f"hash={row.get('hash')} lvl={row.get('level')} ms={row.get('ms_gen')}/{row.get('ms_load')} "
                  f"fps={row.get('fps')} err={row.get('error', '')}", flush=True)
            if row["finish"] != "ok" and row["finish"] == "timeout":
                break
    finally:
        how = pf.stop(proc, hwnd)
        print(f"[f2] {label}: stopped ({how}) after {time.monotonic() - t0:.0f} s", flush=True)
        cache = run_dir / "models.cache"
        if cache.exists():
            cache.unlink()


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("mode", choices=["sweep", "w1", "one"])
    ap.add_argument("--exe", type=Path, required=True)
    ap.add_argument("--datadir", type=Path, default=pf.STEAM_DIR)
    ap.add_argument("--out", type=Path, default=Path(r"C:\Apocrypha\diagnostics\barony-f2"))
    ap.add_argument("--seeds", default="1001")
    ap.add_argument("--first", type=int, default=0, help="sweep: seeds first..first+count-1 instead of --seeds")
    ap.add_argument("--count", type=int, default=0)
    ap.add_argument("--per-process", type=int, default=4)
    ap.add_argument("--size", type=int, default=128)
    ap.add_argument("--sizes", default="128,48,vanilla")
    ap.add_argument("--control", type=int, default=0)
    ap.add_argument("--fps-seconds", type=float, default=10.0)
    ap.add_argument("--run-timeout", type=float, default=150.0)
    ap.add_argument("--settle", type=float, default=4.0)
    ap.add_argument("--shot-delay", type=float, default=4.0)
    ap.add_argument("--lock-dir", type=Path, default=None)
    ap.add_argument("--wait-foreign", type=float, default=1800.0)
    args = ap.parse_args()
    args.out.mkdir(parents=True, exist_ok=True)
    seeds = list(range(args.first, args.first + args.count)) if args.count else [int(s) for s in args.seeds.split(",")]
    rows = []
    pf.wait_until_no_barony(args.wait_foreign, args.lock_dir)
    pf.hold_lock(args.lock_dir, True)
    try:
        if pf.foreign_locks(args.lock_dir) or len(pf.barony_running()) > 0:
            sys.exit("someone else took Barony at the same moment; lock released")
        if args.mode in ("sweep", "one"):
            env = {"APXGEN": "1", "APXGEN_SIZE": str(args.size)}
            if args.control:
                env["APXGEN_CONTROL"] = str(args.control)
            for i in range(0, len(seeds), args.per_process):
                session(f"apx{args.size}-c{args.control}-{seeds[i]}", args.exe, seeds[i:i + args.per_process],
                        {"env": env}, args, rows)
        else:
            for s in args.sizes.split(","):
                extra = {"env": {}} if s == "vanilla" else {"env": {"APXGEN": "1", "APXGEN_SIZE": s}}
                session(f"w1-{s}", args.exe, seeds, extra, args, rows, w1=True)
    finally:
        pf.hold_lock(args.lock_dir, False)
    ok = sum(r["finish"] == "ok" for r in rows)
    print(f"[f2] rows {len(rows)} ok {ok}")


if __name__ == "__main__":
    main()
