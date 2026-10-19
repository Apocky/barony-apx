#!/usr/bin/env python3
"""tools/apx/parity_f0.py -- slice F0 parity gate: same seed, same dungeon, our build vs Steam vanilla.

Every run gets an empty run folder as its working directory. On Windows Barony writes log.txt, config,
saves and compiled books to its working directory, so the Steam install and Apocky's saves in it are
never written. Two ways to give a run the game data:
  datadir  BARONY_DATADIR=<Steam install> in the environment (barony-apx reads every base data file
           from there at run time; upstream half-honours -datadir=, so this needs our build);
  mirror   the run folder itself looks like the install: junctions into the install for the big
           folders the game never writes (images, music, sound, themes), junctions into one private
           copy for the folders it reads through "./" paths (data, models, items, maps, lang, fonts),
           a fresh copy of books/ (the game compiles books there) and the root name lists. This is
           how barony_vanilla.exe can run as installed without touching the install.
For every (config, seed, repeat) the driver starts the exe with -quickstart=barbarian (a new
singleplayer game on the fixed start map, level 0), waits for "LoadMap ...start.lmp", types
    /enablecheats    /setmapseed <seed>    /nextlevel
into the game window (checking that the window is in front before every key; /setmapseed sets
forceMapSeed, which the next level transition uses as the map seed), waits for "successfully generated
a dungeon with N rooms, M monsters, G gold, I items, D decorations." for level 1, presses F6 (the game's
own framebuffer screenshot, saved into the run folder) and closes the game. One JSONL row per run;
every run has a wall-time bound and is killed when it runs over.
At the end: parity (all configs agree per seed), control (different seeds differ), determinism
(repeats agree), data-load health (no base file failed to load), install unchanged (snapshot of the
install's data folders before and after). summary.json and baseline.json go next to the JSONL.

Usage:
  python tools/apx/parity_f0.py --ours build/vs2022/Release/barony.exe
"""

import argparse
import ctypes
import ctypes.wintypes as wt
import datetime
import hashlib
import json
import os
import re
import shutil
import subprocess
import sys
import time
from pathlib import Path

STEAM_DIR = Path(r"C:\Program Files (x86)\Steam\steamapps\common\Barony")
STEAM_APPID = "371970"

MIRROR_JUNCTION_INSTALL = ["images", "music", "sound", "themes"]           # big, never written by the game
MIRROR_SHARED_COPY = ["data", "models", "items", "maps", "lang", "fonts"]  # read through ./ paths
MIRROR_RUN_COPY = ["books"]                                                # compiled into ./books at start
MIRROR_ROOT_FILES = ["gamecontrollerdb.txt", "playernames-male.txt", "playernames-female.txt",
                     "npcnames-male.txt", "npcnames-female.txt"]
SNAPSHOT_DIRS = MIRROR_JUNCTION_INSTALL + MIRROR_SHARED_COPY + MIRROR_RUN_COPY

RE_START_MAP = re.compile(r"LoadMap .*maps[/\\]start\.lmp")
RE_GENERATING = re.compile(r"generating a dungeon from level set '([^']*)'.*\(seed (\d+)\)")
RE_GENERATED = re.compile(
    r"successfully generated a dungeon with (\d+) rooms, (\d+) monsters, (\d+) gold, (\d+) items, (\d+) decorations\.")
RE_CAMERA = re.compile(r"Camera at (-?\d+), (-?\d+), (-?\d+)")
RE_LOAD_ERROR = re.compile(r"(unable to (load|locate) language file|Could not (open|locate) json file|"
                           r"failed to load|unable to (open|load|locate))", re.I)
# a fresh profile has no saves, config or scores yet; those misses are expected in every run
RE_OUTPUT_FILE = re.compile(r"savegames|[/\\]config[/\\]|[/\\]scores", re.I)
RE_TIMESTAMP = re.compile(r"^\[\d\d-\d\d-\d\d\]\s*")


def normalize_error(line, roots):
    """Strip the timestamp and the data/run folder prefixes so the same miss compares equal across runs."""
    s = RE_TIMESTAMP.sub("", line.strip())
    for r in sorted(roots, key=len, reverse=True):
        s = s.replace(r, "<root>")
    s = s.replace("\\", "/")
    s = re.sub(r"(<root>|\./)/*", "<root>/", s)
    return re.sub(r"/{2,}", "/", s)

# ---------------------------------------------------------------- Win32 input (ctypes, no extra packages)

user32 = ctypes.WinDLL("user32", use_last_error=True)
kernel32 = ctypes.WinDLL("kernel32", use_last_error=True)

INPUT_KEYBOARD = 1
KEYEVENTF_KEYUP = 0x0002
KEYEVENTF_UNICODE = 0x0004
VK_RETURN, SC_RETURN = 0x0D, 0x1C
VK_F6, SC_F6 = 0x75, 0x40
SW_RESTORE = 9


class KEYBDINPUT(ctypes.Structure):
    _fields_ = [("wVk", wt.WORD), ("wScan", wt.WORD), ("dwFlags", wt.DWORD), ("time", wt.DWORD),
                ("dwExtraInfo", ctypes.c_size_t)]


class MOUSEINPUT(ctypes.Structure):
    _fields_ = [("dx", wt.LONG), ("dy", wt.LONG), ("mouseData", wt.DWORD), ("dwFlags", wt.DWORD),
                ("time", wt.DWORD), ("dwExtraInfo", ctypes.c_size_t)]


class HARDWAREINPUT(ctypes.Structure):
    _fields_ = [("uMsg", wt.DWORD), ("wParamL", wt.WORD), ("wParamH", wt.WORD)]


class _INPUTUNION(ctypes.Union):
    _fields_ = [("ki", KEYBDINPUT), ("mi", MOUSEINPUT), ("hi", HARDWAREINPUT)]


class INPUT(ctypes.Structure):
    _fields_ = [("type", wt.DWORD), ("u", _INPUTUNION)]


user32.SendInput.argtypes = (wt.UINT, ctypes.POINTER(INPUT), ctypes.c_int)
user32.GetWindowThreadProcessId.argtypes = (wt.HWND, ctypes.POINTER(wt.DWORD))
WNDENUMPROC = ctypes.WINFUNCTYPE(wt.BOOL, wt.HWND, wt.LPARAM)


class FocusLost(RuntimeError):
    pass


def window_pid(hwnd):
    pid = wt.DWORD()
    user32.GetWindowThreadProcessId(hwnd, ctypes.byref(pid))
    return pid.value


def find_game_window(pid):
    found = []

    def cb(hwnd, _):
        if window_pid(hwnd) == pid and user32.IsWindowVisible(hwnd):
            cls = ctypes.create_unicode_buffer(64)
            user32.GetClassNameW(hwnd, cls, 64)
            if cls.value == "SDL_app":
                found.append(hwnd)
        return True

    user32.EnumWindows(WNDENUMPROC(cb), 0)
    return found[0] if found else None


def bring_to_front(hwnd):
    if user32.GetForegroundWindow() == hwnd:
        return True
    user32.ShowWindow(hwnd, SW_RESTORE)
    me = kernel32.GetCurrentThreadId()
    fg_tid = user32.GetWindowThreadProcessId(user32.GetForegroundWindow(), None)
    tgt_tid = user32.GetWindowThreadProcessId(hwnd, None)
    for t in (fg_tid, tgt_tid):
        user32.AttachThreadInput(me, t, True)
    user32.BringWindowToTop(hwnd)
    user32.SetForegroundWindow(hwnd)
    for t in (fg_tid, tgt_tid):
        user32.AttachThreadInput(me, t, False)
    time.sleep(0.25)
    return user32.GetForegroundWindow() == hwnd


def ensure_front(hwnd, tries=8):
    for _ in range(tries):
        if bring_to_front(hwnd):
            return
        time.sleep(0.5)
    raise FocusLost("game window is not in front; no keys sent")


def _send(inputs):
    arr = (INPUT * len(inputs))(*inputs)
    if user32.SendInput(len(inputs), arr, ctypes.sizeof(INPUT)) != len(inputs):
        raise OSError(ctypes.get_last_error(), "SendInput failed")


def key(hwnd, vk, sc, hold=0.05):
    ensure_front(hwnd)
    _send([INPUT(INPUT_KEYBOARD, _INPUTUNION(ki=KEYBDINPUT(vk, sc, 0, 0, 0)))])
    time.sleep(hold)
    _send([INPUT(INPUT_KEYBOARD, _INPUTUNION(ki=KEYBDINPUT(vk, sc, KEYEVENTF_KEYUP, 0, 0)))])
    time.sleep(0.05)


def type_text(hwnd, text):
    for ch in text:
        ensure_front(hwnd)
        down = INPUT(INPUT_KEYBOARD, _INPUTUNION(ki=KEYBDINPUT(0, ord(ch), KEYEVENTF_UNICODE, 0, 0)))
        up = INPUT(INPUT_KEYBOARD, _INPUTUNION(ki=KEYBDINPUT(0, ord(ch), KEYEVENTF_UNICODE | KEYEVENTF_KEYUP, 0, 0)))
        _send([down, up])
        time.sleep(0.03)


WM_KEYDOWN, WM_KEYUP, WM_CHAR = 0x0100, 0x0101, 0x0102
user32.PostMessageW.argtypes = (wt.HWND, wt.UINT, wt.WPARAM, wt.LPARAM)
HWND_TOPMOST = wt.HWND(-1)
SWP_NOSIZE, SWP_NOMOVE, SWP_NOACTIVATE, SWP_SHOWWINDOW = 0x0001, 0x0002, 0x0010, 0x0040
user32.SetWindowPos.argtypes = (wt.HWND, wt.HWND, ctypes.c_int, ctypes.c_int, ctypes.c_int, ctypes.c_int, wt.UINT)
# Barony samples key state once per frame (Input::update reads Input::keys, which SDL_KEYDOWN sets and
# SDL_KEYUP clears, game.cpp:4494). A press whose down and up land in the same frame is never seen,
# which happened when a covered, throttled game window ran slow frames. Hold keys across many frames.
KEY_HOLD_S = 0.35


def keep_visible(hwnd):
    """Keep the game window on top of other windows without giving it the keyboard: an uncovered
    window is not throttled, and its framebuffer is what F6 saves."""
    return bool(user32.SetWindowPos(hwnd, HWND_TOPMOST, 0, 0, 0, 0,
                                    SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE | SWP_SHOWWINDOW))


def post_key(hwnd, vk, sc):
    """Key press posted straight to the game window: no focus needed, so nothing typed elsewhere on
    the desktop can mix in, and the user's own typing stays in their window. SDL 2.0.22 reads the
    scancode from lParam bits 16-23."""
    user32.PostMessageW(hwnd, WM_KEYDOWN, vk, 1 | (sc << 16))
    time.sleep(KEY_HOLD_S)
    user32.PostMessageW(hwnd, WM_KEYUP, vk, 1 | (sc << 16) | (1 << 30) | (1 << 31))
    time.sleep(KEY_HOLD_S)


def post_text(hwnd, text):
    for ch in text:                       # WM_CHAR only: text input, no key bindings fire
        user32.PostMessageW(hwnd, WM_CHAR, ord(ch), 1)
        time.sleep(0.02)


def press(hwnd, vk, sc, method):
    if method == "post":
        post_key(hwnd, vk, sc)
    else:
        key(hwnd, vk, sc)


def chat_command(hwnd, command, method="post"):
    press(hwnd, VK_RETURN, SC_RETURN, method)     # "Chat" is bound to Return by default
    time.sleep(0.4)
    if method == "post":
        post_text(hwnd, command)
    else:
        type_text(hwnd, command)
    time.sleep(0.2)
    press(hwnd, VK_RETURN, SC_RETURN, method)
    time.sleep(0.6)

# ---------------------------------------------------------------- files, junctions, snapshots


def sha256(path):
    h = hashlib.sha256()
    with open(path, "rb") as f:
        for chunk in iter(lambda: f.read(1 << 20), b""):
            h.update(chunk)
    return h.hexdigest()


def junction(link, target):
    r = subprocess.run(["cmd", "/c", "mklink", "/J", str(link), str(target)], capture_output=True, text=True)
    if r.returncode != 0 or not os.path.isjunction(link):
        raise RuntimeError(f"mklink /J {link} -> {target} failed: {r.stdout} {r.stderr}")


def remove_junctions(folder):
    """Remove the junctions in a run folder (the links only; targets are untouched)."""
    for p in Path(folder).iterdir():
        if os.path.isjunction(p):
            os.rmdir(p)


def snapshot(root, dirs):
    snap = {}
    for d in dirs:
        for dp, _, files in os.walk(root / d):
            for f in files:
                p = Path(dp) / f
                st = p.stat()
                snap[str(p.relative_to(root))] = (st.st_size, st.st_mtime_ns)
    return snap


def build_shared_mirror(datadir, shared):
    if (shared / ".complete").exists():
        return
    if shared.exists():
        shutil.rmtree(shared)
    shared.mkdir(parents=True)
    for d in MIRROR_SHARED_COPY:
        shutil.copytree(datadir / d, shared / d)
    shutil.copy2(datadir / "models.cache", shared / "models.cache")
    (shared / ".complete").write_text("copied from the Steam install for parity runs; safe to delete\n")


def prepare_mirror(run_dir, datadir, shared):
    for d in MIRROR_JUNCTION_INSTALL:
        junction(run_dir / d, datadir / d)
    for d in MIRROR_SHARED_COPY:
        junction(run_dir / d, shared / d)
    for d in MIRROR_RUN_COPY:
        shutil.copytree(datadir / d, run_dir / d)
    for f in MIRROR_ROOT_FILES:
        shutil.copy2(datadir / f, run_dir / f)
    os.link(shared / "models.cache", run_dir / "models.cache")   # hardlink to our private copy


def read_log(path):
    try:
        return path.read_text(encoding="utf-8", errors="replace")
    except FileNotFoundError:
        return ""


def wait_for(path, regex, deadline, proc, start=0):
    """Poll a log file until regex matches after offset `start`; return (match, offset of its end)."""
    while time.monotonic() < deadline:
        text = read_log(path)
        m = regex.search(text, start)
        if m:
            return m, m.end()
        if proc.poll() is not None:
            raise RuntimeError(f"game exited with code {proc.returncode} before '{regex.pattern}'")
        time.sleep(0.25)
    raise TimeoutError(f"timed out waiting for '{regex.pattern}'")


WM_CLOSE = 0x0010


def stop(proc, hwnd=None, grace=15):
    """Close the game the way a player does first: WM_CLOSE becomes SDL_QUIT and Barony leaves its
    main loop cleanly. A forced TerminateProcess left the process stuck in teardown for up to two
    minutes (GPU resources), which blocked the next launch. Returns how it ended."""
    if proc.poll() is not None:
        return "exited"
    if hwnd:
        user32.PostMessageW(hwnd, WM_CLOSE, 0, 0)
        try:
            proc.wait(grace)
            return "closed"
        except subprocess.TimeoutExpired:
            pass
    proc.terminate()
    try:
        proc.wait(10)
    except subprocess.TimeoutExpired:
        proc.kill()
        proc.wait(10)
    return "terminated"


def barony_running():
    out = subprocess.run(["tasklist", "/FO", "CSV", "/NH"], capture_output=True, text=True).stdout.lower()
    return [n for n in ("barony.exe", "barony_vanilla.exe", "editor.exe") if f'"{n}"' in out]


LOCK_NAME = "barony-f0-parity.LOCK"


def foreign_locks(lock_dir):
    if not lock_dir:
        return []
    return sorted(p.name for p in Path(lock_dir).glob("*.LOCK") if p.name != LOCK_NAME)


def wait_until_no_barony(limit_s, lock_dir=None):
    """Our own runs are always stopped before the next starts, so any Barony process here belongs to
    someone else (Apocky playing, or another session's runs). Wait for it to close, and for other
    sessions' *.LOCK files in lock_dir to disappear, instead of colliding with them."""
    t0 = time.monotonic()
    announced = None
    while True:
        running = barony_running()
        locks = foreign_locks(lock_dir)
        if not running and not locks:
            return round(time.monotonic() - t0, 1)
        if time.monotonic() - t0 > limit_s:
            sys.exit(f"Barony stayed busy ({', '.join(running + locks)}) for {limit_s:.0f} s; giving up.")
        state = ", ".join(running + locks)
        if state != announced:
            print(f"[parity] waiting for {state} ...", flush=True)
            announced = state
        time.sleep(10)


def hold_lock(lock_dir, on):
    if not lock_dir:
        return
    p = Path(lock_dir) / LOCK_NAME
    if on:
        p.write_text(datetime.datetime.now().astimezone().isoformat(timespec="seconds") + " barony-apx F0 parity runs\n",
                     encoding="ascii")
    elif p.exists():
        p.unlink()

# ---------------------------------------------------------------- one run


def run_once(build, mode, exe, seed, rep, out_root, datadir, shared, args):
    stamp = datetime.datetime.now().strftime("%Y%m%d-%H%M%S")
    run_dir = out_root / "runs" / f"{stamp}-{build}-{mode}-s{seed}-r{rep}"
    run_dir.mkdir(parents=True)
    env = os.environ.copy()
    env.pop("BARONY_DATADIR", None)
    cmd = [str(exe), "-quickstart=barbarian"]
    if mode == "datadir":
        env["BARONY_DATADIR"] = str(datadir)
    else:
        prepare_mirror(run_dir, datadir, shared)
    if build == "vanilla":
        # The Steam build calls SteamAPI_RestartAppIfNecessary(). Outside Steam that would ask Steam to
        # launch the game's default exe (barony.exe, the S.A.M build) in the install folder. A
        # steam_appid.txt in the working directory plus SteamAppId stops the relaunch.
        (run_dir / "steam_appid.txt").write_text(STEAM_APPID, encoding="ascii")
        env["SteamAppId"] = STEAM_APPID
        env["SteamGameId"] = STEAM_APPID
    row = {
        "ts": datetime.datetime.now().astimezone().isoformat(timespec="seconds"),
        "build": build, "mode": mode, "exe": str(exe), "exe_sha256": sha256(exe), "seed": seed,
        "repeat": rep, "run_dir": str(run_dir), "cmd": cmd,
        "env_BARONY_DATADIR": env.get("BARONY_DATADIR"), "finish": "error", "flags": [],
    }
    log = run_dir / "log.txt"
    prev_fg = user32.GetForegroundWindow()
    t0 = time.monotonic()
    deadline = t0 + args.run_timeout
    proc = subprocess.Popen(cmd, cwd=run_dir, env=env)
    hwnd = None
    try:
        _, off = wait_for(log, RE_START_MAP, deadline, proc)
        row["t_start_map_s"] = round(time.monotonic() - t0, 2)
        while hwnd is None and time.monotonic() < deadline:
            hwnd = find_game_window(proc.pid)
            time.sleep(0.25)
        if hwnd is None:
            raise TimeoutError("no SDL window for the game process")
        if args.input == "post":
            if prev_fg and user32.GetForegroundWindow() == hwnd:
                # posted input needs no focus: hand the keyboard back to whatever the user was using
                if bring_to_front(prev_fg):
                    row["flags"].append("returned_focus")
            if not keep_visible(hwnd):
                row["flags"].append("topmost_failed")
        time.sleep(args.settle)                      # fade-in after the start map loads
        # each step is checked in the log and retried, so a lost or garbled line cannot pass silently
        re_seed_set = re.compile(rf"Set next map seed to: {seed}\b")
        for attempt in range(1, 5):
            chat_command(hwnd, "/enablecheats", args.input)
            chat_command(hwnd, f"/setmapseed {seed}", args.input)
            try:
                _, off = wait_for(log, re_seed_set, min(deadline, time.monotonic() + 6), proc, off)
                break
            except TimeoutError:
                row["flags"].append(f"seed_command_retry_{attempt}")
        else:
            raise TimeoutError("'/setmapseed' never confirmed in the log")
        for attempt in range(1, 4):
            chat_command(hwnd, "/nextlevel", args.input)
            try:
                gen_line, off2 = wait_for(log, RE_GENERATING, min(deadline, time.monotonic() + 15), proc, off)
                break
            except TimeoutError:
                row["flags"].append(f"nextlevel_retry_{attempt}")
        else:
            raise TimeoutError("'/nextlevel' never started a level")
        m, _ = wait_for(log, RE_GENERATED, deadline, proc, off2)
        row["t_generated_s"] = round(time.monotonic() - t0, 2)
        row["levelset"] = gen_line.group(1)
        row["map_seed_logged"] = int(gen_line.group(2))
        row["generating_line"] = gen_line.group(0)
        row["generated_line"] = m.group(0)
        row.update(dict(zip(("rooms", "monsters", "gold", "items", "decorations"), map(int, m.groups()))))
        if row["map_seed_logged"] != seed:
            row["flags"].append("seed_mismatch")
        time.sleep(args.shot_delay)                  # let floor 1 finish its fade-in before the picture
        before = set(run_dir.glob("*.png"))
        press(hwnd, VK_F6, SC_F6, args.input)
        shot = None
        t_shot = time.monotonic() + 10
        while shot is None and time.monotonic() < t_shot:
            new = sorted(set(run_dir.glob("*.png")) - before)
            shot = new[0] if new else None
            time.sleep(0.25)
        if shot:
            time.sleep(0.5)                          # let the PNG writer finish
            row["screenshot"] = str(shot)
        else:
            row["flags"].append("no_screenshot")
        # where the camera stood (logged by the game's own /pos; issued after the picture so its
        # message is not in the frame), so a view difference between two pictures is measured
        chat_command(hwnd, "/pos", args.input)
        try:
            cm, _ = wait_for(log, RE_CAMERA, min(deadline, time.monotonic() + 6), proc, off2)
            row["camera_after_shot"] = [int(v) for v in cm.groups()]
        except TimeoutError:
            row["flags"].append("no_camera_pos")
        row["finish"] = "ok"
    except Exception as e:                            # noqa: BLE001 -- every failure becomes a row
        row["error"] = f"{type(e).__name__}: {e}"
        row["finish"] = "timeout" if isinstance(e, TimeoutError) else "error"
    finally:
        row["stopped"] = stop(proc, hwnd)
        row["t_total_s"] = round(time.monotonic() - t0, 2)
        row["exit_code"] = proc.returncode
        text = read_log(log)
        errs = [l.strip() for l in text.splitlines() if RE_LOAD_ERROR.search(l) and not RE_OUTPUT_FILE.search(l)]
        row["load_errors"] = len(errs)
        row["load_errors_normalized"] = sorted({normalize_error(l, [str(datadir), str(run_dir)]) for l in errs})
        dp = re.search(r"Data path is (.*)", text)
        row["data_path_logged"] = dp.group(1).strip() if dp else None
        if mode == "mirror":
            remove_junctions(run_dir)
        cache = run_dir / "models.cache"
        if cache.exists():                           # hardlink to our copy, or a regenerated cache
            cache.unlink()
    return row

# ---------------------------------------------------------------- matrix


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--ours", required=True, type=Path)
    ap.add_argument("--vanilla", type=Path, default=STEAM_DIR / "barony_vanilla.exe")
    ap.add_argument("--datadir", type=Path, default=STEAM_DIR)
    ap.add_argument("--out", type=Path, default=Path(r"C:\Apocrypha\diagnostics\barony-f0"))
    ap.add_argument("--seeds", default="1234567,7654321", help="comma list; the first is repeated")
    ap.add_argument("--repeats", type=int, default=2, help="runs of the first seed per config")
    ap.add_argument("--configs", default="ours:datadir,vanilla:mirror",
                    help="build:mode list run over every seed")
    ap.add_argument("--cross", default="ours:mirror",
                    help="build:mode list run once on the first seed (environment cross-check); '' for none")
    ap.add_argument("--run-timeout", type=float, default=240.0, help="wall-time bound per run, seconds")
    ap.add_argument("--settle", type=float, default=4.0)
    ap.add_argument("--shot-delay", type=float, default=5.0)
    ap.add_argument("--wait-foreign", type=float, default=1200.0,
                    help="seconds to wait for someone else's Barony to close before a run")
    ap.add_argument("--keep-mirror", action="store_true", help="keep the private asset copy afterwards")
    ap.add_argument("--lock-dir", type=Path, default=None,
                    help="folder shared with other sessions: wait while their *.LOCK files exist, hold "
                         f"{LOCK_NAME} while the matrix runs")
    ap.add_argument("--input", choices=("post", "send"), default="post",
                    help="post: messages straight to the game window (no focus); send: SendInput with focus checks")
    args = ap.parse_args()

    seeds = [int(s) for s in args.seeds.split(",")]
    if any(not (0 < s < 2**31) for s in seeds):
        sys.exit("seeds must be in 1..2147483647 (/setmapseed uses atoi and needs a seed > 0)")
    exes = {"ours": args.ours.resolve(), "vanilla": args.vanilla}
    configs = [tuple(c.split(":")) for c in args.configs.split(",") if c]
    cross = [tuple(c.split(":")) for c in args.cross.split(",") if c]
    for b, m in configs + cross:
        if b not in exes or m not in ("datadir", "mirror"):
            sys.exit(f"bad config {b}:{m}")
        if b == "vanilla" and m == "datadir":
            sys.exit("vanilla cannot run in datadir mode: upstream reads base files through ./ paths")
        if not exes[b].exists():
            sys.exit(f"missing exe for {b}: {exes[b]}")
    args.out.mkdir(parents=True, exist_ok=True)
    jsonl = args.out / "parity.jsonl"
    shared = args.out / "mirror-assets"
    if any(m == "mirror" for _, m in configs + cross):
        print("[parity] preparing the private asset copy for mirror runs ...", flush=True)
        build_shared_mirror(args.datadir, shared)

    plan = []
    for b, m in configs:
        plan += [(b, m, seeds[0], r) for r in range(1, args.repeats + 1)]
        plan += [(b, m, s, 1) for s in seeds[1:]]
    plan += [(b, m, seeds[0], 1) for b, m in cross]

    snap_before = snapshot(args.datadir, SNAPSHOT_DIRS)
    rows = []
    try:
        for (b, m, s, r) in plan:
            # wait for other sessions' locks and Barony processes, then take our lock (other
            # sessions honour *.LOCK files in the shared notes folder the same way)
            waited = wait_until_no_barony(args.wait_foreign, args.lock_dir)
            hold_lock(args.lock_dir, True)
            if barony_running():                  # someone launched between our check and our lock
                hold_lock(args.lock_dir, False)
                waited += wait_until_no_barony(args.wait_foreign, args.lock_dir)
                hold_lock(args.lock_dir, True)
            print(f"[parity] {b}:{m} seed={s} repeat={r} ...", flush=True)
            row = run_once(b, m, exes[b], s, r, args.out, args.datadir, shared, args)
            row["waited_for_other_barony_s"] = waited
            rows.append(row)
            with open(jsonl, "a", encoding="ascii") as f:
                f.write(json.dumps(row) + "\n")
            print(f"[parity]   {row['finish']} {row.get('generated_line', row.get('error'))} "
                  f"(load errors {row['load_errors']}, {row['t_total_s']} s)", flush=True)
    finally:
        hold_lock(args.lock_dir, False)
    snap_after = snapshot(args.datadir, SNAPSHOT_DIRS)
    changed = sorted(k for k in set(snap_before) | set(snap_after) if snap_before.get(k) != snap_after.get(k))
    if not args.keep_mirror and shared.exists():
        shutil.rmtree(shared)

    ok = [r for r in rows if r["finish"] == "ok"]
    by_seed = {}
    for r in ok:
        by_seed.setdefault(r["seed"], {}).setdefault(f"{r['build']}:{r['mode']}", []).append(r["generated_line"])
    wanted = {f"{b}:{m}" for b, m in configs}
    parity = {s: (len({l for ls in d.values() for l in ls}) == 1 and wanted <= set(d)) for s, d in by_seed.items()}
    repeats_agree = all(len(set(ls)) == 1 for d in by_seed.values() for ls in d.values())
    lines_per_seed = {s: sorted({l for ls in d.values() for l in ls}) for s, d in by_seed.items()}
    control = len(seeds) > 1 and len({tuple(v) for v in lines_per_seed.values()}) == len(lines_per_seed)
    # data-load health: our build may only miss what the shipped game misses on the same data
    vanilla_errs = set().union(*[set(r["load_errors_normalized"]) for r in ok if r["build"] == "vanilla"])
    extra_errs = sorted(set().union(*[set(r["load_errors_normalized"]) for r in ok if r["build"] != "vanilla"])
                        - vanilla_errs)
    clean_loads = not extra_errs
    summary = {
        "written": datetime.datetime.now().astimezone().isoformat(timespec="seconds"),
        "runs": len(rows), "ok": len(ok), "configs": [":".join(c) for c in configs],
        "cross": [":".join(c) for c in cross], "seeds": seeds,
        "parity_per_seed": {str(k): v for k, v in parity.items()},
        "control_different_seeds_differ": control,
        "repeats_agree": repeats_agree,
        "clean_data_loads": clean_loads,
        "load_errors_only_in_ours": extra_errs[:20],
        "load_errors_shared_with_vanilla": sorted(vanilla_errs)[:20],
        "install_data_files_changed": changed[:20],
        "install_data_files_checked": len(snap_after),
        "lines_per_seed": {str(k): v for k, v in lines_per_seed.items()},
        "configs_per_seed": {str(k): sorted(d) for k, d in by_seed.items()},
        "gate": "PASS" if (len(ok) == len(rows) and all(parity.values()) and control and repeats_agree
                           and clean_loads and not changed) else "FAIL",
    }
    (args.out / "summary.json").write_text(json.dumps(summary, indent=2), encoding="ascii")
    base = {str(r["seed"]): r["generated_line"] for r in ok if r["build"] == "vanilla"}
    if base:
        (args.out / "baseline.json").write_text(json.dumps({"source": str(exes["vanilla"]), "lines": base},
                                                           indent=2), encoding="ascii")
    print(json.dumps(summary, indent=2))
    return 0 if summary["gate"] == "PASS" else 1


if __name__ == "__main__":
    sys.exit(main())
