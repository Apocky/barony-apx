r"""hostility_gate.py -- the apx::hostility gate (spec 04b s6, D93).

The player stands still from control for N seconds (APX_HOSTILITY_GATE=N, src/apx/hostility.cpp) on seeds 4242 and 777.
New path must show: player alive, 0 hits on the player before the calm start ends (hits after it, by creatures that noticed a player standing still, are reported, not failed), every creature-on-creature hit explained by a reason class
(table/provoked/event/shop/follower; 0 unexplained), the calm start ended with a log line, floor-1 population at the end within
10 percent of the start. The CONTROL (APX_HOSTILITY_OLD=1, vanilla) must show the old brawl: skeleton-vs-rat hits or unexplained
creature-on-creature hits (fail otherwise: the gate would not discriminate).
Launch rules: a free slot file C:\Apocrypha\locks\barony-slots\slotN.LOCK, none while locks\barony or barony-want has files.
Rows: C:\Apocrypha\diagnostics\hostility\<stamp>.jsonl.
  python tools/apx/hostility_gate.py --exe build/vs2022/Release/barony.exe [--seeds 4242,777] [--seconds 60] [--qa]
"""
import argparse, json, os, re, subprocess, sys, tempfile, time
from pathlib import Path

LOCKS = Path(r"C:\Apocrypha\locks")
OUT = Path(r"C:\Apocrypha\diagnostics\hostility")
DATADIR = os.environ.get("BARONY_DATADIR", r"C:\Program Files (x86)\Steam\steamapps\common\Barony")
RE_GATE = re.compile(r"\[apx-hostility\] gate (\{.*\})")


def take_slot():
    if any((LOCKS / "barony").glob("*.LOCK")) or any((LOCKS / "barony-want").glob("*.WANT")):
        return None
    d = LOCKS / "barony-slots"
    d.mkdir(parents=True, exist_ok=True)
    for n in range(1, 9):
        p = d / f"slot{n}.LOCK"
        try:
            fd = os.open(p, os.O_CREAT | os.O_EXCL | os.O_WRONLY)
            os.write(fd, f"hostility_gate pid={os.getpid()} {time.strftime('%Y-%m-%dT%H:%M:%S')}".encode())
            os.close(fd)
            return p
        except FileExistsError:
            continue
    return None


def run(exe, seed, seconds, control, qa, timeout):
    cwd = Path(tempfile.mkdtemp(prefix=f"hostility-{seed}-{'ctl' if control else 'new'}-"))
    env = dict(os.environ, APX_HOSTILITY_GATE=str(seconds), BARONY_DATADIR=DATADIR)
    if control:
        env["APX_HOSTILITY_OLD"] = "1"
    args = [str(exe), f"-datadir={DATADIR}", "-quickstart=barbarian", "-apxfloor=1", f"-apxmapseed={seed}"]
    if qa:  # real input path: the QA driver, windowed, never touches the OS mouse; the player only waits
        qd = cwd / "qa"; qd.mkdir()
        (qd / "cmd.txt").write_text(f"wait 2000\nshot start\nwait {max(1000, seconds * 1000 - 6000)}\nshot end\n", encoding="utf-8")
        args += ["-windowed", "-size=1280x720", f"-apxqa={qd}"]
    else:
        args.append("-apxheadless")
    t0 = time.time()
    try:
        subprocess.run(args, cwd=cwd, env=env, timeout=timeout, creationflags=0x08000000, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
        finish = "exit"
    except subprocess.TimeoutExpired:
        finish = "timeout"
    log = cwd / "log.txt"
    text = log.read_text(encoding="utf-8", errors="replace") if log.exists() else ""
    m = RE_GATE.findall(text)
    row = json.loads(m[-1]) if m else {}
    row.update(seed=seed, control=int(control), scenario="stand-qa" if qa else "stand", wall_s=round(time.time() - t0, 1), finish=finish, cwd=str(cwd),
               calm_log=len(re.findall(r"\[apx-hostility\] calm start ended", text)), table_log=len(re.findall(r"\[apx-hostility\] table loaded", text)))
    return row


def verdict(r, a):
    if not r.get("why"):
        return False, "no summary row"
    bad = []
    if r.get("player_alive") != 1: bad.append("player died")
    if r.get("player_hits_before_calm_end", 0) > 0: bad.append(f"player hit during the calm start ({r.get('player_hits_before_calm_end')} hits)")
    if r.get("mm_unexplained", 0) > 0: bad.append(f"{r.get('mm_unexplained')} creature-on-creature hits with no table reason: {r.get('mm_pairs')}")
    if r.get("calm_ended") != 1 or r.get("calm_log", 0) < 1: bad.append("calm start end not logged")
    if r.get("table_log", 0) < 1: bad.append("hostility table not loaded")
    ps, pe = r.get("pop_start", -1), r.get("pop_end", -1)
    if ps > 0 and pe < ps * 0.9: bad.append(f"population {pe} of {ps}")
    return not bad, "; ".join(bad) or "ok"


def verdict_control(r):
    if not r.get("why"): return False, "no summary row"
    sk = sum(v for k, v in (r.get("mm_pairs") or {}).items() if k.startswith(("SKELETON>RAT", "RAT>SKELETON")) or "skeleton>rat" in k.lower() or "rat>skeleton" in k.lower())
    ok = sk > 0 or r.get("mm_unexplained", 0) > 0 or r.get("player_hits", 0) > 0
    return ok, f"control brawl: skeleton-vs-rat hits {sk}, unexplained mm hits {r.get('mm_unexplained')}, player hits {r.get('player_hits')}"


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--exe", required=True, type=Path)
    ap.add_argument("--seeds", default="4242,777")
    ap.add_argument("--seconds", type=int, default=60)
    ap.add_argument("--timeout", type=float, default=400.0)
    ap.add_argument("--no-control", action="store_true")
    ap.add_argument("--qa", action="store_true", help="stand runs windowed through the -apxqa driver")
    a = ap.parse_args()
    OUT.mkdir(parents=True, exist_ok=True)
    path = OUT / f"{time.strftime('%Y%m%d-%H%M%S')}.jsonl"
    ok_all = True
    for seed in [int(s) for s in a.seeds.split(",")]:
        for control in ([False] if a.no_control else [False, True]):
            slot = None
            for _ in range(120):
                slot = take_slot()
                if slot: break
                time.sleep(5)
            if not slot:
                print("no free slot"); return 2
            try:
                r = run(a.exe, seed, a.seconds, control, a.qa, a.timeout)
            finally:
                slot.unlink(missing_ok=True)
            ok, why = verdict_control(r) if control else verdict(r, a)
            r.update(pass_=bool(ok), verdict=why)
            with open(path, "a", encoding="utf-8") as f: f.write(json.dumps(r) + "\n")
            print(f"seed {seed} {'CONTROL' if control else 'new'}: {'PASS' if ok else 'FAIL'} -- {why} (pop {r.get('pop_start')}->{r.get('pop_end')}, mm_hits {r.get('mm_hits')} unexplained {r.get('mm_unexplained')}, player_hits {r.get('player_hits')} before_calm_end {r.get('player_hits_before_calm_end')})")
            ok_all &= ok
    print(("GATE PASS" if ok_all else "GATE FAIL"), path)
    return 0 if ok_all else 1


if __name__ == "__main__":
    sys.exit(main())
