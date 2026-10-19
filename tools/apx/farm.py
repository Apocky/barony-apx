#!/usr/bin/env python3
"""tools/apx/farm.py -- the Barony test farm: one runner, a job queue, capacity limits, the D70 rules.

Lanes drop job files into C:/Apocrypha/barony-jobs/queue; one runner starts them first in, first out under
capacity (headless 6, windowed 3, exclusive 1) and the machine's run protocol (Apockalypsis
specs/barony-fork/11-MASTER-PLAN.md section 5 rule 1, tools/apx/apx_locks.py):
  - nothing new starts while Apocky plays, while any barony-want/*.WANT exists, while another lane holds an
    exclusive barony/*.LOCK, or while free commit memory is below --min-free-gb;
  - windowed jobs hold a D70 slot file (barony-slots/slotN.LOCK) while they run;
  - an exclusive job, once it is first in the queue, writes barony-want/farm-<id>.WANT, waits until no Barony
    runs and no slot or exclusive lock is held, then runs alone holding barony/farm-<id>.LOCK.
Each job runs in its own working folder (runs/<id>, so its log.txt, config and saves stay there), is closed
when its `until` regex appears in a file of that folder (WM_CLOSE, then kill), or killed at its timeout.
done/<id>.json records exit code, wall time, how it ended and where its logs are.

Job file (JSON): {"exe": path, "args": [...], "mode": "headless"|"windowed"|"exclusive", "timeout_s": n,
  "lane": name, "env": {...}, "cwd": optional folder, "datadir": optional (BARONY_DATADIR, default the Steam
  install unless the exe lives there), "until": optional regex, "until_file": "log.txt", "until_grace_s": 1.0, "close_grace_s": 15}

Commands:
  farm.py run <job.json> | run --exe X --mode M --timeout S [--lane L] [--until RE] -- <game args>
      submit, start the runner if none runs, wait for the result, print it, exit with the job's exit code
  farm.py submit ... (same options, does not wait; prints the job id)
  farm.py runner      the runner loop (started hidden on demand; exits after --idle-exit-s with nothing to do)
  farm.py status      queue, running and done counts
"""

import argparse
import ctypes
import ctypes.wintypes as wt
import datetime
import json
import os
import re
import subprocess
import sys
import time
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
import apx_locks  # noqa: E402  (D70 lock folders, Barony process list, Apocky-playing check)

FARM = Path(os.environ.get("APX_FARM_DIR", r"C:\Apocrypha\barony-jobs"))
DIRS = {k: FARM / k for k in ("queue", "running", "done", "logs", "runs")}
CAPS = {"headless": 6, "windowed": 3, "exclusive": 1}
MODES = tuple(CAPS)
STEAM_DIR = apx_locks.STEAM_DIR
RUNNER_PID = FARM / "runner.pid"
RUNNER_LOG = FARM / "runner.log"

# ---------------------------------------------------------------- pure logic (tested in test_farm.py)


def validate(job):
    """Return a list of problems with a job dict (empty when it is valid)."""
    bad = []
    if not job.get("exe"):
        bad.append("exe missing")
    if job.get("mode") not in MODES:
        bad.append(f"mode must be one of {MODES}")
    t = job.get("timeout_s")
    if not isinstance(t, (int, float)) or t <= 0:
        bad.append("timeout_s must be a positive number")
    if not isinstance(job.get("args", []), list):
        bad.append("args must be a list")
    if job.get("until") is not None:
        try:
            re.compile(job["until"])
        except re.error as e:
            bad.append(f"until is not a regex: {e}")
    return bad


def schedule(pending, running, cond, caps=CAPS):
    """Decide which queued jobs start now. Pure: no files, no clock.

    pending: queued jobs, oldest first (dicts with "id" and "mode").
    running: jobs of ours that run now (dicts with "mode").
    cond: dict with apocky_playing, low_memory (bool), foreign_wants, foreign_excl, foreign_slots (int),
          foreign_barony (int: Barony processes that are not ours).
    Returns (start, want): start = jobs to launch now, in order; want = id of an exclusive job that is first
    in line and needs its .WANT file (or None).
    Rules: per-mode first in, first out; capacity per mode; windowed jobs also need a free D70 slot;
    an exclusive job is a barrier for everything queued after it, and once it is first in line it waits
    for the machine to empty, then runs alone; nothing new starts while Apocky plays, while memory is low,
    or (for non-exclusive jobs) while another lane wants or holds the machine.
    """
    if cond.get("apocky_playing") or cond.get("low_memory"):
        return [], None
    budget = cond.get("start_budget")                    # how many launches memory allows this pass
    counts = {m: 0 for m in caps}
    for r in running:
        counts[r["mode"]] = counts.get(r["mode"], 0) + 1
    start, blocked_modes = [], set()
    for i, job in enumerate(pending):
        mode = job["mode"]
        if mode == "exclusive":
            if i > 0 or start:
                break                                    # barrier: wait until it is first in line
            machine_busy = (sum(counts.values()) > 0 or cond.get("foreign_slots", 0) > 0
                            or cond.get("foreign_excl", 0) > 0 or cond.get("foreign_barony", 0) > 0)
            if machine_busy:
                return [], job["id"]                       # hold the .WANT, start nothing else
            return [job], job["id"]
        if cond.get("foreign_wants", 0) or cond.get("foreign_excl", 0):
            break                                        # another lane wants or holds the machine
        if mode in blocked_modes:
            continue                                     # keep per-mode order
        free = counts[mode] < caps[mode]
        if mode == "windowed":
            free = free and (counts["windowed"] + cond.get("foreign_slots", 0)) < apx_locks.NSLOTS
        if not free:
            blocked_modes.add(mode)
            continue
        if budget is not None and len(start) >= budget:
            break
        counts[mode] += 1
        start.append(job)
    return start, None


def done_record(job, exit_code, started, finished, how, cwd, stdout_path, until_line=None):
    """The done/<id>.json content for a finished job."""
    rec = dict(job)
    rec.update({
        "exit_code": exit_code, "started": started, "finished": finished,
        "wall_s": round(finished - started, 3) if started else None,
        "ended_by": how,                   # exit | until | timeout | launch_error
        "killed": how == "timeout",
        "cwd": str(cwd) if cwd else None,
        "log": str(Path(cwd) / "log.txt") if cwd else None,
        "stdout": str(stdout_path) if stdout_path else None,
    })
    if until_line is not None:
        rec["until_line"] = until_line
    return rec

# ---------------------------------------------------------------- Win32 helpers


user32 = ctypes.WinDLL("user32", use_last_error=True)
kernel32 = ctypes.WinDLL("kernel32", use_last_error=True)
WNDENUMPROC = ctypes.WINFUNCTYPE(wt.BOOL, wt.HWND, wt.LPARAM)
user32.GetWindowThreadProcessId.argtypes = (wt.HWND, ctypes.POINTER(wt.DWORD))
user32.PostMessageW.argtypes = (wt.HWND, wt.UINT, wt.WPARAM, wt.LPARAM)
WM_CLOSE = 0x0010


def windows_of(pid):
    """The game's SDL windows of a process, visible or hidden (a headless game window is hidden)."""
    found = []

    def cb(hwnd, _):
        p = wt.DWORD()
        user32.GetWindowThreadProcessId(hwnd, ctypes.byref(p))
        if p.value == pid:
            cls = ctypes.create_unicode_buffer(64)
            user32.GetClassNameW(hwnd, cls, 64)
            if cls.value == "SDL_app":
                found.append(hwnd)
        return True

    user32.EnumWindows(WNDENUMPROC(cb), 0)
    return found


def free_commit_gb():
    class MEMORYSTATUSEX(ctypes.Structure):
        _fields_ = [("dwLength", wt.DWORD), ("dwMemoryLoad", wt.DWORD), ("ullTotalPhys", ctypes.c_ulonglong),
                    ("ullAvailPhys", ctypes.c_ulonglong), ("ullTotalPageFile", ctypes.c_ulonglong),
                    ("ullAvailPageFile", ctypes.c_ulonglong), ("ullTotalVirtual", ctypes.c_ulonglong),
                    ("ullAvailVirtual", ctypes.c_ulonglong), ("ullAvailExtendedVirtual", ctypes.c_ulonglong)]
    m = MEMORYSTATUSEX()
    m.dwLength = ctypes.sizeof(m)
    kernel32.GlobalMemoryStatusEx(ctypes.byref(m))
    return m.ullAvailPageFile / 1e9                    # commit still available, bytes -> GB


def pid_alive(pid):
    h = kernel32.OpenProcess(0x1000, False, pid)      # PROCESS_QUERY_LIMITED_INFORMATION
    if not h:
        return False
    code = wt.DWORD()
    ok = kernel32.GetExitCodeProcess(h, ctypes.byref(code))
    kernel32.CloseHandle(h)
    return bool(ok) and code.value == 259             # STILL_ACTIVE

# ---------------------------------------------------------------- files


def now():
    return time.time()


def stamp():
    return datetime.datetime.now().strftime("%Y%m%d-%H%M%S-%f")


def ensure_dirs():
    for d in DIRS.values():
        d.mkdir(parents=True, exist_ok=True)


def write_json(path, obj):
    tmp = path.with_suffix(path.suffix + ".tmp")
    tmp.write_text(json.dumps(obj, indent=1), encoding="utf-8")
    os.replace(tmp, path)                             # atomic: readers never see half a file


def read_json(path):
    try:
        return json.loads(path.read_text(encoding="utf-8"))
    except (OSError, ValueError):
        return None


def submit(job):
    bad = validate(job)
    if bad:
        raise ValueError("; ".join(bad))
    ensure_dirs()
    job = dict(job)
    job.setdefault("args", [])
    job.setdefault("lane", "unnamed")
    job["id"] = f"{stamp()}-{re.sub(r'[^A-Za-z0-9_-]', '_', job['lane'])}"
    job["submitted"] = now()
    write_json(DIRS["queue"] / f"{job['id']}.json", job)
    return job["id"]


def log(msg):
    line = f"{datetime.datetime.now().strftime('%Y-%m-%d %H:%M:%S')} {msg}"
    try:
        with open(RUNNER_LOG, "a", encoding="utf-8") as f:
            f.write(line + "\n")
    except OSError:
        pass

# ---------------------------------------------------------------- runner


class Job:
    """A job of ours that is running: its process and where it stands."""

    def __init__(self, proc, job, started, cwd, out_path, hold, out):
        self.proc, self.job, self.started, self.cwd = proc, job, started, cwd
        self.out_path, self.hold, self.out = out_path, hold, out
        self.until_hit = None       # time the until regex matched
        self.until_line = None
        self.closing = None         # (reason, time WM_CLOSE was posted, kill deadline)


class Runner:
    def __init__(self, args):
        self.args = args
        self.jobs = {}           # id -> Job
        self.want_files = {}     # job id -> path of our .WANT

    def conditions(self):
        ours = {j.proc.pid for j in self.jobs.values()}
        our_wants = {p.name for p in self.want_files.values()}
        our_locks = {Path(j.hold).name for j in self.jobs.values() if j.hold}
        wants = [n for n in apx_locks._names(apx_locks.WANT, "*.WANT") if n not in our_wants]
        excl = [n for n in apx_locks._names(apx_locks.EXCL, "*.LOCK") if n not in our_locks]
        slots = [n for n in apx_locks._names(apx_locks.SLOTS, "*.LOCK") if n not in our_locks]
        barony = [pid for pid, _ in apx_locks.barony_processes() if pid not in ours]
        free = free_commit_gb()
        budget = max(0, int((free - self.args.min_free_gb) // self.args.job_gb))
        return {"apocky_playing": bool(apx_locks.apocky_playing()), "low_memory": budget == 0,
                "start_budget": budget, "foreign_wants": len(wants), "foreign_excl": len(excl),
                "foreign_slots": len(slots), "foreign_barony": len(barony), "free_commit_gb": round(free, 1)}

    def take_slot_file(self):
        apx_locks.SLOTS.mkdir(parents=True, exist_ok=True)
        for i in range(1, apx_locks.NSLOTS + 1):
            p = apx_locks.SLOTS / f"slot{i}.LOCK"
            if apx_locks._create(p, apx_locks._stamp("farm")):
                return p
        return None

    def launch(self, job):
        cwd = Path(job["cwd"]) if job.get("cwd") else DIRS["runs"] / job["id"]
        cwd.mkdir(parents=True, exist_ok=True)
        hold = None
        if job["mode"] == "windowed":
            hold = self.take_slot_file()
            if hold is None:
                return False                               # lost a race for the slot: try next pass
        elif job["mode"] == "exclusive":
            apx_locks.EXCL.mkdir(parents=True, exist_ok=True)
            hold = apx_locks.EXCL / f"farm-{job['id']}.LOCK"
            if not apx_locks._create(hold, apx_locks._stamp("farm")):
                return False
            w = self.want_files.pop(job["id"], None)
            if w:
                w.unlink(missing_ok=True)
        env = os.environ.copy()
        env.pop("BARONY_DATADIR", None)
        exe = Path(job["exe"])
        in_steam = exe.parent.resolve() == STEAM_DIR.resolve()
        datadir = job.get("datadir", None if in_steam else str(STEAM_DIR))
        if datadir:
            env["BARONY_DATADIR"] = datadir
        env.update({k: str(v) for k, v in (job.get("env") or {}).items()})
        out_path = DIRS["logs"] / f"{job['id']}.out"
        out = open(out_path, "wb")
        started = now()
        try:
            proc = subprocess.Popen([str(exe)] + [str(a) for a in job.get("args", [])], cwd=str(cwd), env=env,
                                    stdout=out, stderr=subprocess.STDOUT, creationflags=0x08000000)  # CREATE_NO_WINDOW
        except OSError as e:
            out.close()
            if hold:
                hold.unlink(missing_ok=True)
            (DIRS["queue"] / f"{job['id']}.json").unlink(missing_ok=True)
            self.finish(job, None, started, cwd, out_path, "launch_error", note=str(e))
            return True
        rec = dict(job, pid=proc.pid, started=started, cwd=str(cwd), hold=str(hold) if hold else None)
        write_json(DIRS["running"] / f"{job['id']}.json", rec)
        (DIRS["queue"] / f"{job['id']}.json").unlink(missing_ok=True)
        self.jobs[job["id"]] = Job(proc, job, started, cwd, out_path, str(hold) if hold else None, out)
        log(f"start {job['id']} {job['mode']} pid {proc.pid}")
        return True

    def request_close(self, j, reason, grace):
        """Ask the game to quit (WM_CLOSE -> SDL_QUIT, a clean exit); kill it if it is still there at the
        deadline. Never blocks the runner."""
        if j.closing is None:
            for hwnd in windows_of(j.proc.pid):
                user32.PostMessageW(hwnd, WM_CLOSE, 0, 0)
            j.closing = (reason, now(), now() + grace)

    def finish(self, job, code, started, cwd, out_path, how, until_line=None, note=None):
        rec = done_record(job, code, started, now(), how, cwd, out_path, until_line)
        if note:
            rec["note"] = note
        write_json(DIRS["done"] / f"{job['id']}.json", rec)
        (DIRS["running"] / f"{job['id']}.json").unlink(missing_ok=True)
        log(f"done {job['id']} {how} exit {code} {rec['wall_s']} s")

    def reap(self):
        t = now()
        for jid in list(self.jobs):
            j = self.jobs[jid]
            if j.proc.poll() is None:
                if j.closing is None and j.job.get("until"):
                    if j.until_hit is None:
                        f = j.cwd / j.job.get("until_file", "log.txt")
                        try:
                            m = re.search(j.job["until"], f.read_text(encoding="utf-8", errors="replace"))
                        except OSError:
                            m = None
                        if m:
                            j.until_hit, j.until_line = t, m.group(0)
                    if j.until_hit is not None and t - j.until_hit >= float(j.job.get("until_grace_s", 1.0)):
                        self.request_close(j, "until", grace=float(j.job.get("close_grace_s", 15.0)))
                if j.closing is None and t - j.started > float(j.job["timeout_s"]):
                    self.request_close(j, "timeout", grace=min(5.0, float(j.job.get("close_grace_s", 5.0))))
                if j.closing is not None and t >= j.closing[2]:
                    j.proc.kill()                          # did not quit in time
                    try:
                        j.proc.wait(10)
                    except subprocess.TimeoutExpired:
                        continue
                else:
                    continue
            how = j.closing[0] if j.closing else "exit"
            j.out.close()
            if j.hold:
                Path(j.hold).unlink(missing_ok=True)
            del self.jobs[jid]
            self.finish(j.job, j.proc.returncode, j.started, j.cwd, j.out_path, how, until_line=j.until_line)

    def recover(self):
        """Jobs a previous runner left behind: close what still runs, record them, free their locks."""
        for p in DIRS["running"].glob("*.json"):
            r = read_json(p)
            if not r:
                p.unlink(missing_ok=True)
                continue
            pid = r.get("pid")
            if pid and pid_alive(pid):
                for hwnd in windows_of(pid):
                    user32.PostMessageW(hwnd, WM_CLOSE, 0, 0)
                time.sleep(5)
                if pid_alive(pid):
                    subprocess.run(["taskkill", "/F", "/PID", str(pid)], capture_output=True)
            if r.get("hold"):
                Path(r["hold"]).unlink(missing_ok=True)
            self.finish(r, None, r.get("started"), r.get("cwd"), DIRS["logs"] / f"{r['id']}.out", "orphaned",
                        note="the runner that started this job died; the job was closed and not retried")
        for w in apx_locks._names(apx_locks.WANT, "farm-*.WANT"):
            (apx_locks.WANT / w).unlink(missing_ok=True)

    def pending(self):
        jobs = []
        for p in sorted(DIRS["queue"].glob("*.json")):
            j = read_json(p)
            if j is None:
                continue
            bad = validate(j)
            if bad:
                p.unlink(missing_ok=True)
                self.finish(j, None, None, None, None, "launch_error", note="; ".join(bad))
                continue
            jobs.append(j)
        return jobs

    def loop(self):
        ensure_dirs()
        self.recover()
        idle_since = now()
        last_cond = None
        while True:
            self.reap()
            pend = self.pending()
            cond = self.conditions()
            start, want = schedule(pend, [j.job for j in self.jobs.values()], cond)
            for jid in [k for k in self.want_files if k != want]:
                self.want_files.pop(jid).unlink(missing_ok=True)
            if want and want not in self.want_files:
                apx_locks.WANT.mkdir(parents=True, exist_ok=True)
                w = apx_locks.WANT / f"farm-{want}.WANT"
                w.write_text(apx_locks._stamp("farm"), encoding="ascii")
                self.want_files[want] = w
            for job in start:
                self.launch(job)
            brief = {k: cond[k] for k in ("apocky_playing", "low_memory", "foreign_wants", "foreign_excl")}
            if pend and not start and brief != last_cond:
                log(f"waiting: {len(pend)} queued, {len(self.jobs)} running, {cond}")
                last_cond = brief
            if pend or self.jobs:
                idle_since = now()
            elif now() - idle_since > self.args.idle_exit_s:
                log("idle: runner exits")
                return
            time.sleep(self.args.poll_s)


def runner_alive():
    pid = None
    try:
        pid = int(RUNNER_PID.read_text().split()[0])
    except (OSError, ValueError, IndexError):
        return False
    return pid_alive(pid)


def run_runner(args):
    ensure_dirs()
    try:
        fd = os.open(RUNNER_PID, os.O_CREAT | os.O_EXCL | os.O_WRONLY)
    except FileExistsError:
        if runner_alive():
            print("a runner is already running", file=sys.stderr)
            return 0
        RUNNER_PID.unlink(missing_ok=True)                 # stale file of a runner that died
        fd = os.open(RUNNER_PID, os.O_CREAT | os.O_EXCL | os.O_WRONLY)
    os.write(fd, f"{os.getpid()} {datetime.datetime.now().isoformat(timespec='seconds')}\n".encode())
    os.close(fd)
    log(f"runner {os.getpid()} up (headless {CAPS['headless']}, windowed {CAPS['windowed']}, exclusive 1)")
    try:
        Runner(args).loop()
    finally:
        RUNNER_PID.unlink(missing_ok=True)
    return 0


def ensure_runner():
    if runner_alive():
        return
    py = Path(sys.executable)
    pyw = py.with_name("pythonw.exe")
    exe = pyw if pyw.exists() else py
    # hidden, detached: no console window (workspace rule: background services never show a console)
    subprocess.Popen([str(exe), str(Path(__file__).resolve()), "runner"], creationflags=0x00000008 | 0x08000000,
                     stdin=subprocess.DEVNULL, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL, close_fds=True)
    t0 = now()
    while not runner_alive() and now() - t0 < 15:
        time.sleep(0.2)


def wait_done(jid, limit_s):
    p = DIRS["done"] / f"{jid}.json"
    t0 = now()
    while not p.exists():
        if now() - t0 > limit_s:
            return None
        if not runner_alive():
            ensure_runner()
        time.sleep(0.5)
    return read_json(p)


def job_from_args(a):
    if a.job:
        job = json.loads(Path(a.job).read_text(encoding="utf-8"))
    else:
        job = {"exe": a.exe, "mode": a.mode, "timeout_s": a.timeout, "lane": a.lane, "args": a.game_args or []}
        if a.until:
            job["until"] = a.until
    return job


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    sub = ap.add_subparsers(dest="cmd", required=True)
    for name in ("run", "submit"):
        s = sub.add_parser(name)
        s.add_argument("job", nargs="?", help="job JSON file")
        s.add_argument("--exe")
        s.add_argument("--mode", choices=MODES, default="headless")
        s.add_argument("--timeout", type=float, default=300.0)
        s.add_argument("--lane", default="unnamed")
        s.add_argument("--until")
    r = sub.add_parser("runner")
    r.add_argument("--poll-s", type=float, default=0.5)
    r.add_argument("--idle-exit-s", type=float, default=300.0)
    r.add_argument("--min-free-gb", type=float, default=3.0, help="free commit memory to keep after launches")
    r.add_argument("--job-gb", type=float, default=2.0, help="commit memory one game takes (ASSUMED until measured)")
    sub.add_parser("status")
    argv, game_args = sys.argv[1:], []
    if "--" in argv:                                 # everything after -- goes to the game untouched
        i = argv.index("--")
        argv, game_args = argv[:i], argv[i + 1:]
    a = ap.parse_args(argv)
    a.game_args = game_args
    if a.cmd == "runner":
        return run_runner(a)
    if a.cmd == "status":
        ensure_dirs()
        print(json.dumps({k: len(list(DIRS[k].glob("*.json"))) for k in ("queue", "running", "done")}
                         | {"runner_alive": runner_alive()}))
        return 0
    job = job_from_args(a)
    jid = submit(job)
    if a.cmd == "submit":
        print(jid)
        ensure_runner()
        return 0
    ensure_runner()
    rec = wait_done(jid, float(job["timeout_s"]) + 3600)    # queue time plus run time
    print(json.dumps(rec, indent=1))
    if rec is None:
        return 2
    return rec["exit_code"] if isinstance(rec.get("exit_code"), int) else 1


if __name__ == "__main__":
    sys.exit(main())
