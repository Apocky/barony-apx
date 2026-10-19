"""tools/apx/apx_locks.py -- this machine's Barony run protocol (D70, Apockalypsis specs/barony-fork/11-MASTER-PLAN.md
section 5 rule 1), shared by the apx tools.

CORRECTNESS runs (hashes, parity, gates) run up to 3 at once. Each launch holds a slot file
C:/Apocrypha/locks/barony-slots/slot1..3.LOCK, taken only while C:/Apocrypha/locks/barony is empty and no
C:/Apocrypha/locks/barony-want/*.WANT exists, and released when that launch ends (so an exclusive run that
wants the machine waits for running launches only, never for a slot held between launches). Windowed, own
working folder.
EXCLUSIVE runs (timing/perf measurements, the Steam S.A.M exe, input-sensitive tests) first write
barony-want/<lane>.WANT, wait until no slot file, no other exclusive lock and no Barony process exist, take
barony/<lane>.LOCK and delete the .WANT.
Nobody launches while Apocky plays (barony.exe running from the Steam install).
"""
import ctypes
import ctypes.wintypes as wt
import os
import time
from pathlib import Path

ROOT = Path(r"C:\Apocrypha\locks")
EXCL = ROOT / "barony"
SLOTS = ROOT / "barony-slots"
WANT = ROOT / "barony-want"
STEAM_DIR = Path(r"C:\Program Files (x86)\Steam\steamapps\common\Barony")
NSLOTS = 3
GAME_EXES = ("barony.exe", "barony_vanilla.exe", "editor.exe")

_psapi = ctypes.WinDLL("psapi")
_kernel32 = ctypes.WinDLL("kernel32")
PROCESS_QUERY_LIMITED_INFORMATION = 0x1000


def processes():
    """(pid, image path) of every process whose image can be read."""
    arr = (wt.DWORD * 8192)()
    got = wt.DWORD()
    if not _psapi.EnumProcesses(arr, ctypes.sizeof(arr), ctypes.byref(got)):
        return []
    out = []
    for pid in arr[: got.value // ctypes.sizeof(wt.DWORD)]:
        h = _kernel32.OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, False, pid)
        if not h:
            continue
        try:
            buf = ctypes.create_unicode_buffer(1024)
            size = wt.DWORD(1024)
            if _kernel32.QueryFullProcessImageNameW(h, 0, buf, ctypes.byref(size)):
                out.append((int(pid), buf.value))
        finally:
            _kernel32.CloseHandle(h)
    return out


def barony_processes():
    return [(pid, p) for pid, p in processes() if Path(p).name.lower() in GAME_EXES]


def apocky_playing():
    """The Steam install's barony.exe is running (Apocky's S.A.M build): nobody launches."""
    steam = str(STEAM_DIR).lower()
    return [p for _, p in barony_processes() if str(Path(p).parent).lower() == steam and Path(p).name.lower() == "barony.exe"]


def _names(d, pattern):
    return sorted(p.name for p in d.glob(pattern)) if d.is_dir() else []


def _create(path, text):
    """Atomic create; False if the file already exists."""
    try:
        fd = os.open(str(path), os.O_CREAT | os.O_EXCL | os.O_WRONLY)
    except FileExistsError:
        return False
    with os.fdopen(fd, "w", encoding="ascii") as f:
        f.write(text)
    return True


def _stamp(tag):
    return f"{time.strftime('%Y-%m-%dT%H:%M:%S')} {tag} pid {os.getpid()}\n"


def take_slot(tag, limit_s, poll_s=3.0, log=print):
    """Block until a slot is ours; return its path. Raise TimeoutError after limit_s."""
    t0, last = time.time(), None
    while True:
        busy = []
        excl, wants, play = _names(EXCL, "*.LOCK"), _names(WANT, "*.WANT"), apocky_playing()
        if excl:
            busy.append("exclusive " + ",".join(excl))
        if wants:
            busy.append("wanted by " + ",".join(wants))
        if play:
            busy.append("Apocky is playing")
        if not busy:
            SLOTS.mkdir(parents=True, exist_ok=True)
            for i in range(1, NSLOTS + 1):
                p = SLOTS / f"slot{i}.LOCK"
                if not _create(p, _stamp(tag)):
                    continue
                if _names(EXCL, "*.LOCK") or _names(WANT, "*.WANT"):   # lost the race to an exclusive run
                    p.unlink(missing_ok=True)
                    break
                return p
            else:
                busy.append("all slots busy (" + ",".join(_names(SLOTS, "*.LOCK")) + ")")
        if time.time() - t0 > limit_s:
            raise TimeoutError(f"no Barony slot in {limit_s:.0f} s: {'; '.join(busy)}")
        if busy != last:
            log(f"[locks] waiting for a slot: {'; '.join(busy)}")
            last = busy
        time.sleep(poll_s)


def take_exclusive(name, limit_s, poll_s=3.0, log=print):
    """Write our .WANT, wait for the machine, take barony/<name> (must end in .LOCK), drop the .WANT."""
    if not name.endswith(".LOCK"):
        raise ValueError("lock names end in .LOCK")
    WANT.mkdir(parents=True, exist_ok=True)
    EXCL.mkdir(parents=True, exist_ok=True)
    want = WANT / (name[:-5] + ".WANT")
    want.write_text(_stamp(name[:-5]), encoding="ascii")
    t0, last = time.time(), None
    try:
        while True:
            slots = _names(SLOTS, "*.LOCK")
            excl = [n for n in _names(EXCL, "*.LOCK") if n != name]
            procs = [Path(p).name for _, p in barony_processes()]
            busy = (["slots " + ",".join(slots)] if slots else []) + (["exclusive " + ",".join(excl)] if excl else []) \
                + (["running " + ",".join(procs)] if procs else [])
            if not busy:
                lock = EXCL / name
                if _create(lock, _stamp(name[:-5])):
                    if _names(SLOTS, "*.LOCK") or barony_processes():   # a launch slipped in: back off
                        lock.unlink(missing_ok=True)
                    else:
                        return lock
            if time.time() - t0 > limit_s:
                raise TimeoutError(f"no exclusive Barony slot in {limit_s:.0f} s: {'; '.join(busy)}")
            if busy != last:
                log(f"[locks] waiting for the machine: {'; '.join(busy)}")
                last = busy
            time.sleep(poll_s)
    finally:
        want.unlink(missing_ok=True)
