#!/usr/bin/env python3
"""tools/apx/spawn_gate.py -- gate: a player standing still at the floor-1 start survives.

Regression gate for the QA s1/s2 spawn deaths (seed 4242: about 30 creatures brawled at the start, a
kobold struck the player for 25-31 with head hits x2.5 = 78 on a 40 HP character, the solo player was
downed and bled out with no killer named; seed 777: the player was stagger-locked).

Each run is headless: -quickstart=barbarian -apxfloor=1 -apxmapseed=<seed> -apxheadless with
APX_SPAWN_GATE=<seconds> (src/apx_combat.hpp: the game counts for N game seconds, prints one
"[apx-spawngate] summary" row and a "fixes" counter row, and quits). Scenarios:
  stand (x --trials per seed; headless runs are not tick-deterministic): the player never moves. PASS: alive at the
    end, no shopkeeper hit, creature-on-creature hits near the start <= --max-brawl and creatures in such fights on the whole floor
    <= --max-brawlers (the QA brawl had about 30), no bleed-out, no "dies randomly", stagger locks <= --max-locks.
  fight (APX_SPAWN_GATE_FIGHT=1, one per seed): 3 kobolds (fodder) and 2 goblins (regular) are set on the passive
    player 5 s in. The player may die, by the rules: fodder blows <= the D74 depth cap after zones, regular blows
    <= a third of max HP, a solo down gets back up, the death names its killer, no shopkeeper hit, no lock.
  fight-coop (plus APX_DOWNED_FORCE_COOP=1): a co-op bleed-out names its killer.
CONTROL runs set APX_SPAWN_LEGACY=1 (every old behaviour back: per-member D72 kinds, no territory or arrival
clearance, untagged director spawns, no director arrival quiet, variant after tier, uncapped zone multiplier on
players, solo bleed-out, no killer) and must FAIL; the gate fails if a control passes.
Launch rules: a free slot file C:\\Apocrypha\\locks\\barony-slots\\slotN.LOCK is created exclusively and
deleted afterwards; no launch while C:\\Apocrypha\\locks\\barony has a .LOCK or barony-want a .WANT.
Rows go to C:\\Apocrypha\\diagnostics\\spawn-gate\\<stamp>.jsonl.

  python tools/apx/spawn_gate.py --exe build/ninja-ccache/barony.exe [--seeds 4242,777] [--seconds 90]
"""

import argparse
import json
import os
import re
import subprocess
import sys
import tempfile
import time
from pathlib import Path

LOCKS = Path(r"C:\Apocrypha\locks")
DATADIR = os.environ.get("BARONY_DATADIR", r"C:\Program Files (x86)\Steam\steamapps\common\Barony")
RE_SUM = re.compile(r"\[apx-spawngate\] summary (.*)")


def take_slot():
    if any((LOCKS / "barony").glob("*.LOCK")) or any((LOCKS / "barony-want").glob("*.WANT")):
        return None
    d = LOCKS / "barony-slots"
    d.mkdir(parents=True, exist_ok=True)
    for n in range(1, 9):
        p = d / f"slot{n}.LOCK"
        try:
            fd = os.open(p, os.O_CREAT | os.O_EXCL | os.O_WRONLY)
            os.write(fd, f"spawn_gate pid={os.getpid()} {time.strftime('%Y-%m-%dT%H:%M:%S')}".encode())
            os.close(fd)
            return p
        except FileExistsError:
            continue
    return None


def parse(line):
    out = {}
    for tok in line.split():
        if "=" in tok:
            k, v = tok.split("=", 1)
            try:
                out[k] = float(v) if "." in v else int(v)
            except ValueError:
                out[k] = v
    return out


def run(exe, seed, seconds, legacy, timeout, fight=False, qa=False):
    cwd = Path(tempfile.mkdtemp(prefix=f"spawngate-{seed}-{'ctl' if legacy else 'fix'}-"))
    env = dict(os.environ, APX_SPAWN_GATE=str(seconds), BARONY_DATADIR=DATADIR)
    if legacy is True:
        env["APX_SPAWN_LEGACY"] = "1"
    if legacy in ("wild", "grace"):
        env["APX_WILD_ENMITY"] = "vanilla"  # Barony's enmity table for wild creatures
    if legacy == "grace":
        env["APX_LOAD_GRACE"] = "0"  # and no load grace: fights start while the floor fades in
    if fight:
        env["APX_SPAWN_GATE_FIGHT"] = "1"
    if fight == "coop":
        env["APX_DOWNED_FORCE_COOP"] = "1"  # the co-op downed path: a bleed-out must name its killer
    args = [str(exe), f"-datadir={DATADIR}", "-quickstart=barbarian", "-apxfloor=1", f"-apxmapseed={seed}"]
    if qa:
        # the real input path: the QA driver (src/apx/qadriver.hpp) runs windowed, never touches the OS mouse; the script only
        # waits (the player stands still) and takes a shot at the start and near the end
        qd = cwd / "qa"
        qd.mkdir()
        (qd / "cmd.txt").write_text(f"wait 2000\nshot start\nwait {max(1000, seconds * 1000 - 6000)}\nshot end\n", encoding="utf-8")
        args += ["-windowed", "-size=1280x720", f"-apxqa={qd}"]
    else:
        args.append("-apxheadless")
    t0 = time.time()
    flags = 0x08000000 if os.name == "nt" else 0  # CREATE_NO_WINDOW
    try:
        subprocess.run(args, cwd=cwd, env=env, timeout=timeout, creationflags=flags,
                       stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
        finish = "exit"
    except subprocess.TimeoutExpired:
        finish = "timeout"
    log = cwd / "log.txt"
    text = log.read_text(encoding="utf-8", errors="replace") if log.exists() else ""
    m = RE_SUM.findall(text)
    row = parse(m[-1]) if m else {}
    for fx in re.findall(r"\[apx-spawngate\] (?:fixes|wild) (.*)", text):
        row.update(parse(fx))
    row.update(seed=seed, legacy=legacy if isinstance(legacy, str) else int(legacy), scenario=(("fight-coop" if fight == "coop" else "fight") if fight else "stand") + ("-qa" if qa else ""), wall_s=round(time.time() - t0, 1), finish=finish, cwd=str(cwd),
               shop_hits_lines=text.count("Begone from my shop"), dies_randomly=text.count("dies randomly"))
    return row


def verdict_fight(r, a):
    """A pack is set on the passive player: the player may die, but by the rules: fodder blows within the D74 depth cap after
    zones, regular blows within a third of max HP, a solo down gets back up, a death names its killer, no lock."""
    if not r.get("why"):
        return False, "no summary row"
    bad = []
    if r.get("fight") != 1: bad.append("pack not spawned")
    if r.get("max_fodder_blow", 0) > r.get("fodder_cap", 2): bad.append(f"fodder blow {r.get('max_fodder_blow')} over cap {r.get('fodder_cap')}")
    if r.get("max_regular_blow", 0) > max(1, r.get("max_hp", 40) // 3): bad.append(f"regular blow {r.get('max_regular_blow')} over a third of {r.get('max_hp')} HP")
    if r.get("shopkeeper_hits", 0) > 0: bad.append("shopkeeper hit the player")
    if r.get("dies_randomly", 0) > 0: bad.append("dies randomly (no killer)")
    if r.get("scenario") == "fight-coop":
        if r.get("bleedouts", 0) > 0 and r.get("killers_named", 0) < 1: bad.append("co-op bleed-out with no killer named")
    else:
        if r.get("bleedouts", 0) > 0: bad.append("solo bleed-out (death loop)")
        if r.get("downs", 0) > 0 and r.get("solo_recovers", 0) < 1: bad.append("downed solo without getting up")
    if r.get("locks", 0) > a.max_locks: bad.append(f"stagger locks {r.get('locks')}")
    return not bad, "; ".join(bad) or "ok"


def verdict(r, a):
    if not r.get("why"):
        return False, "no summary row"
    bad = []
    if r.get("alive") != 1: bad.append("player died")
    if r.get("shopkeeper_hits", 0) > 0: bad.append("shopkeeper hit the player")
    if r.get("mm_hits_near", 0) > a.max_brawl: bad.append(f"brawl near start ({r.get('mm_hits_near')} hits)")
    if r.get("brawlers_all", 0) > a.max_brawlers: bad.append(f"floor-start brawl ({r.get('brawlers_all')} creatures, {r.get('mm_hits_all')} creature-on-creature hits)")
    if r.get("bleedouts", 0) > 0: bad.append("bled out")
    if r.get("locks", 0) > a.max_locks: bad.append(f"stagger locks {r.get('locks')}")
    if r.get("dies_randomly", 0) > 0: bad.append("dies randomly (no killer)")
    if r.get("pre_grace_player_hits", 0) > 0: bad.append(f"player hit before the load grace ended ({r.get('pre_grace_player_hits')})")
    if r.get("pre_grace_mm_hits", 0) > 0: bad.append(f"creature fights before the load grace ended ({r.get('pre_grace_mm_hits')} hits)")
    ps, pe = r.get("population_start", -1), r.get("population_end", -1)
    if ps > 0 and pe < ps * a.min_population: bad.append(f"population {pe} of {ps} at the end")
    return not bad, "; ".join(bad) or "ok"


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--exe", required=True, type=Path)
    ap.add_argument("--seeds", default="4242,777")
    ap.add_argument("--seconds", type=int, default=90)
    ap.add_argument("--max-brawl", type=int, default=10)
    ap.add_argument("--max-locks", type=int, default=2)
    ap.add_argument("--min-population", type=float, default=0.9, help="creatures alive at the end over the count at the start")
    ap.add_argument("--max-brawlers", type=int, default=10, help="distinct creatures in creature-on-creature fights, whole floor (the QA brawl: about 30)")
    ap.add_argument("--timeout", type=float, default=400.0)
    ap.add_argument("--no-control", action="store_true")
    ap.add_argument("--qa", action="store_true", help="stand runs windowed through the -apxqa input driver instead of headless (no fight runs)")
    ap.add_argument("--trials", type=int, default=3, help="runs per seed (the game is not tick-deterministic headless)")
    a = ap.parse_args()
    slot = take_slot()
    if not slot:
        print("BLOCKED: a barony .LOCK/.WANT is held or no slot is free; not launching")
        return 2
    out = Path(r"C:\Apocrypha\diagnostics\spawn-gate")
    out.mkdir(parents=True, exist_ok=True)
    rows_path = out / f"{time.strftime('%Y%m%d-%H%M%S')}.jsonl"
    ok_all = True
    try:
        seeds = [int(s) for s in a.seeds.split(",") if s]
        plan = [(s, False, False) for s in seeds for _ in range(a.trials)]
        if not a.qa:
            plan += [(s, False, True) for s in seeds] + [(seeds[0], False, "coop")]
        if not a.no_control:
            plan += [(seeds[0], True, False)] + ([] if a.qa else [(seeds[0], True, True)]) + [(seeds[0], "wild", False), (seeds[0], "grace", False)]
        with rows_path.open("w", encoding="utf-8") as f:
            for seed, legacy, fight in plan:
                r = run(a.exe.resolve(), seed, a.seconds, legacy, a.timeout, fight, a.qa)
                ok, why = (verdict_fight if fight else verdict)(r, a)
                r["pass"], r["reason"] = int(ok), why
                f.write(json.dumps(r) + "\n")
                f.flush()
                tag = (("CONTROL-" + legacy) if isinstance(legacy, str) else "CONTROL" if legacy else "FIX") + ("-" + ("fight-coop" if fight == "coop" else "fight") if fight else "-stand")
                expect = (not ok) if legacy else ok
                ok_all &= expect
                print(f"{tag} seed={seed} {'PASS' if ok else 'FAIL'} ({why}) expected={'FAIL' if legacy else 'PASS'} -> {'ok' if expect else 'GATE FAIL'}")
                print("   " + " ".join(f"{k}={r.get(k)}" for k in ("why", "seconds", "alive", "hp", "min_hp", "player_hits", "player_dmg",
                      "shopkeeper_hits", "director_hits", "max_blow", "max_fodder_blow", "max_regular_blow", "dies_randomly", "mm_hits_near", "brawlers_near", "downs", "bleedouts", "solo_recovers", "staggers", "locks",
                      "mm_hits_all", "brawlers_all", "pre_grace_player_hits", "pre_grace_mm_hits", "population_start", "population_end", "grace_end_s", "mm_pairs", "by_type", "wall_s")))
    finally:
        slot.unlink(missing_ok=True)
    print(f"rows: {rows_path}")
    print("GATE PASS" if ok_all else "GATE FAIL")
    return 0 if ok_all else 1


if __name__ == "__main__":
    sys.exit(main())
