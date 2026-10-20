"""input_gate.py -- D93 phase C0 gate for apx::in, with real input through the QA driver (-apxqa=<dir>).

  python tools/apx/input_gate.py --exe build/ninja-ccache/barony.exe matrix  [--modes new,old] [--paths start,floor]
  python tools/apx/input_gate.py --exe ... ui                      hovered ImGui window blocks game clicks; text field; owner switch
  python tools/apx/input_gate.py --exe ... replay                  60 s recorded, replayed, rows compared
  python tools/apx/input_gate.py --exe ... all

Matrix: press lengths x frame caps x entry paths x mode (new = apx::in, old = APX_INPUT_OLD=1, the CONTROL).
Each cell injects N presses of Attack (mouse button 1) and N of Sprint (Left Shift) with the QA `act` command and
reads counters back with `mark`. A press is "lost" when a consumer never saw it:
  taken   what apx::in handed to the sim through take()            (new mode)
  legacy  rising edges Barony's own bindings saw at sim ticks      (both modes: the old consumers, fed by the compat feed in new mode)
Rows -> C:/Apocrypha/diagnostics/input/<run>/rows.jsonl, baseline.json, verdict.txt. One launch per (path, mode),
D70 slot lock held per launch, windowed non-activating, the user's mouse is never touched.
"""
import argparse
import datetime
import json
import os
import shutil
import subprocess
import sys
import time
from pathlib import Path

HERE = Path(__file__).resolve().parent
sys.path.insert(0, str(HERE))
import apx_locks  # noqa: E402
import parity_f0 as pf  # noqa: E402

ROOT = HERE.parents[1]
DATADIR = r"C:\Program Files (x86)\Steam\steamapps\common\Barony"
OUTROOT = Path(r"C:\Apocrypha\diagnostics\input")
LENGTHS = [5, 16, 33, 60, 120, 500]
CAPS = {"30": 30, "60": 60, "uncapped": 1200}


class Launch:
    def __init__(self, exe, run, path, mode, extra_env=None, extra_args=None, tag=""):
        self.exe, self.path, self.mode = exe, path, mode
        self.dir = run / f"{tag or 'launch'}-{path}-{mode}"
        self.qa = self.dir / "qa"
        self.qa.mkdir(parents=True)
        (self.qa / "cmd.txt").write_text("", encoding="ascii")
        env = dict(os.environ, BARONY_DATADIR=DATADIR, APX_HOLLOW_DIR=str(ROOT / "data/apx/hollow"),
                   APX_INPUT_DIR=str(ROOT / "data/apx/input"))
        if mode == "old":
            env["APX_INPUT_OLD"] = "1"
        env.update(extra_env or {})
        args = [str(Path(exe).resolve()), "-windowed", "-quickstart=barbarian", f"-apxqa={self.qa}"]
        if path == "floor":
            args += ["-apxfloor=1", "-apxmapseed=101"]
        args += extra_args or []
        self.lock = apx_locks.take_slot(f"input-gate {path}-{mode}", 6 * 3600)
        self.proc = subprocess.Popen(args, cwd=self.dir, env=env)
        self.t0 = time.monotonic()

    def send(self, *lines):
        with open(self.qa / "cmd.txt", "a", encoding="ascii") as f:
            for l in lines:
                f.write(l + "\n")

    def status(self):
        p = self.qa / "status.txt"
        d = {}
        try:
            for line in p.read_text(errors="replace").splitlines():
                k, _, v = line.partition(" ")
                d[k] = v
        except OSError:
            pass
        return d

    def marks(self):
        p = self.qa / "marks.jsonl"
        out = {}
        try:
            for line in p.read_text(errors="replace").splitlines():
                if line.strip():
                    j = json.loads(line)
                    out[j["label"]] = j
        except (OSError, ValueError):
            pass
        return out

    def wait_ingame(self, limit=180):
        t = time.monotonic()
        while time.monotonic() - t < limit:
            if self.proc.poll() is not None:
                raise RuntimeError(f"barony exited early ({self.proc.returncode}); see {self.dir}/log.txt")
            if self.status().get("ingame") == "1":
                time.sleep(2.0)
                return time.monotonic() - t
            time.sleep(0.5)
        raise RuntimeError("never in game")

    def wait_mark(self, label, limit):
        t = time.monotonic()
        while time.monotonic() - t < limit:
            m = self.marks()
            if label in m:
                return m[label]
            if self.proc.poll() is not None:
                raise RuntimeError("barony exited while waiting for " + label)
            time.sleep(0.25)
        raise RuntimeError("timeout waiting for mark " + label)

    def close(self):
        try:
            self.send("quit")
            try:
                self.proc.wait(20)
            except subprocess.TimeoutExpired:
                pf.stop(self.proc)
        finally:
            try:
                self.lock.unlink(missing_ok=True)
            except OSError:
                pass


def presses_for(length):
    return 8 if length >= 500 else 16


def run_matrix(exe, run, path, mode, lengths, caps, rows):
    L = Launch(exe, run, path, mode, tag="matrix")
    try:
        print(f"[gate] {path}/{mode}: in game after {L.wait_ingame():.1f} s", flush=True)
        for cname, cap in caps.items():
            L.send(f"console /fps {cap}", "wait 1500")
            for length in lengths:
                n = presses_for(length)
                gap = 110 if length < 120 else 150
                lab = f"{path}_{mode}_{cname}_{length}"
                cmds = [f"mark {lab}_a"]
                for i in range(n):
                    cmds += [f"act Attack {length}", f"wait {gap + (i * 7) % 23}", f"act Sprint {length}", f"wait {gap + (i * 11) % 29}"]
                cmds += ["wait 400", f"mark {lab}_b"]
                L.send(*cmds)
                a = L.wait_mark(f"{lab}_a", 60)
                b = L.wait_mark(f"{lab}_b", 120)
                fr, ms = b["frames"] - a["frames"], b["wall_ms"] - a["wall_ms"]
                row = {
                    "gate": "matrix", "path": path, "mode": mode, "cap": cname, "len_ms": length, "injected_each": n,
                    "frames": fr, "wall_ms": ms, "fps_measured": round(fr * 1000.0 / ms, 1) if ms else 0,
                    "ticks": b["tick"] - a["tick"],
                }
                for k in ("taken_attack", "taken_sprint", "legacy_attack", "legacy_lshift", "gated", "lost"):
                    row[k] = b[k] - a[k]
                row["lost_taken"] = (2 * n - row["taken_attack"] - row["taken_sprint"]) if mode == "new" else None
                row["lost_legacy"] = 2 * n - row["legacy_attack"] - row["legacy_lshift"]
                row["in_lost_counter"] = b["lost"]
                rows.append(row)
                with open(run / "rows.jsonl", "a", encoding="ascii") as f:
                    f.write(json.dumps(row) + "\n")
                print(f"  cap={cname:8} len={length:3} fps={row['fps_measured']:6} ticks={row['ticks']:4} inj={2*n} "
                      f"taken={row['taken_attack']}+{row['taken_sprint']} legacy={row['legacy_attack']}+{row['legacy_lshift']} "
                      f"lost_taken={row['lost_taken']} lost_legacy={row['lost_legacy']}", flush=True)
    finally:
        L.close()


def run_ui(exe, run, path, mode, rows):
    """Hovered ImGui window blocks clicks; clicking away unblocks; a text field takes keys; movement keeps walking."""
    L = Launch(exe, run, path, mode, tag="ui")
    out = {"gate": "ui", "path": path, "mode": mode}
    try:
        L.wait_ingame()
        L.send("console /fps 60", "wait 800", "mark ui_a")
        for i in range(6):
            L.send("act Attack 60", "wait 200")
        L.send("wait 300", "mark ui_open_ref")           # six clicks with the world owning the pointer
        L.send("console /devmenu", "wait 1200", "move 260 70", "wait 600", "mark ui_hover_a")
        for i in range(6):
            L.send("act Attack 60", "wait 200")          # the pointer is over the Dev Menu window
        L.send("wait 300", "mark ui_hover_b", "move 900 500", "wait 600", "act Attack 60", "wait 300", "mark ui_away_b")
        a, r, ha, hb, ab = (L.wait_mark(x, 120) for x in ("ui_a", "ui_open_ref", "ui_hover_a", "ui_hover_b", "ui_away_b"))
        out.update({
            "world_clicks_seen": r["taken_attack"] - a["taken_attack"], "world_clicks_legacy": r["legacy_attack"] - a["legacy_attack"],
            "hover_owner_mouse": ha["owner_mouse"], "hover_clicks_taken": hb["taken_attack"] - ha["taken_attack"],
            "hover_clicks_legacy": hb["legacy_attack"] - ha["legacy_attack"], "hover_gated": hb["gated"] - ha["gated"],
            "away_taken": ab["taken_attack"] - hb["taken_attack"], "away_owner_mouse": ab["owner_mouse"],
        })
        # text field: Return opens vanilla chat (text input); W typed must not walk; Escape closes
        L.send("console /devmenu", "wait 800", "mark ui_text_a", "key Return 80", "wait 500", "mark ui_text_on",
               "act Sprint 80", "wait 300", "mark ui_text_sprint", "key Escape 80", "wait 500", "act Sprint 80", "wait 300", "mark ui_text_off")
        ta, to, ts, tf = (L.wait_mark(x, 60) for x in ("ui_text_a", "ui_text_on", "ui_text_sprint", "ui_text_off"))
        out.update({
            "text_owner_key_on": to["owner_key"], "text_sprint_taken": ts["taken_sprint"] - to["taken_sprint"],
            "text_off_owner_key": tf["owner_key"], "text_off_sprint_taken": tf["taken_sprint"] - ts["taken_sprint"],
        })
        rows.append(out)
        with open(run / "rows.jsonl", "a", encoding="ascii") as f:
            f.write(json.dumps(out) + "\n")
        print("  ui:", json.dumps(out), flush=True)
    finally:
        L.close()


def run_replay(exe, run, secs=60):
    rec = Launch(exe, run, "floor", "new", extra_env={"APX_INPUT_LOG": "1", "APX_INPUT_LOG_FILE": str(run / "record.jsonl")}, tag="record")
    try:
        rec.wait_ingame()
        rec.send("mark rec_start")
        t_end = time.monotonic() + secs
        i = 0
        # a scripted 60 s: walk, turn, swing, sprint, with short and long presses
        cmds = []
        total_ms = 0
        while total_ms < secs * 1000 - 3000:
            cmds += [f"hold W {700 + (i * 37) % 400}", "wait 100", f"look {(i * 13) % 40 - 20} {(i * 7) % 10 - 5}", f"act Attack {5 + (i * 29) % 120}", "wait 150",
                     f"act Sprint {20 + (i * 17) % 300}", "wait 200", f"hold D {200 + (i * 11) % 200}", "wait 120"]
            total_ms += 700 + (i * 37) % 400 + 100 + 30 + 150 + 30 + 200 + 30 + 200 + (i * 11) % 200 + 120 + 30
            i += 1
        rec.send(*cmds)
        rec.send("mark rec_end")
        rec.wait_mark("rec_end", secs + 60)
        time.sleep(1.0)
    finally:
        rec.close()
    rec_rows = (run / "record.jsonl").read_text().splitlines()
    rep = Launch(exe, run, "floor", "new", extra_env={"APX_INPUT_LOG": "1", "APX_INPUT_LOG_FILE": str(run / "replay.jsonl")},
                 extra_args=[f"-apxreplay={run / 'record.jsonl'}"], tag="replay")
    try:
        t = time.monotonic()
        while rep.proc.poll() is None and time.monotonic() - t < secs + 120:
            time.sleep(1.0)
    finally:
        rep.close()
    rep_rows = (run / "replay.jsonl").read_text().splitlines() if (run / "replay.jsonl").exists() else []
    return compare_rows(rec_rows, rep_rows)


def strip_pos(line):
    j = json.loads(line)
    pos = j.pop("pos", None)
    return json.dumps(j, sort_keys=True), pos


def compare_rows(a, b):
    n = min(len(a), len(b))
    input_diff = pos_diff = 0
    first = None
    for i in range(n):
        ja, pa = strip_pos(a[i])
        jb, pb = strip_pos(b[i])
        if ja != jb:
            input_diff += 1
            first = first or i
        if pa != pb:
            pos_diff += 1
    return {"gate": "replay", "record_rows": len(a), "replay_rows": len(b), "compared": n, "input_rows_differing": input_diff,
            "pos_rows_differing": pos_diff, "first_input_diff_row": first, "bit_exact_input": len(a) == len(b) and input_diff == 0 and len(a) > 0}


def verdict(rows):
    lines, ok = [], True
    for r in rows:
        if r["gate"] == "matrix":
            if r["mode"] == "new":
                good = r["lost_taken"] == 0 and r["lost_legacy"] == 0 and r["in_lost_counter"] == 0
                ok &= good
                lines.append(f"{'PASS' if good else 'FAIL'} new  {r['path']:5} cap={r['cap']:8} len={r['len_ms']:3} lost_taken={r['lost_taken']} lost_legacy={r['lost_legacy']} counter_lost={r['in_lost_counter']}")
    ctrl = [r for r in rows if r["gate"] == "matrix" and r["mode"] == "old" and r["len_ms"] <= 33]
    if ctrl:
        lost = sum(r["lost_legacy"] for r in ctrl)
        lines.append(f"{'PASS' if lost > 0 else 'FAIL'} control (APX_INPUT_OLD=1) loses short presses: {lost} lost over {len(ctrl)} short cells")
        ok &= lost > 0
    for r in rows:
        if r["gate"] == "ui" and r["mode"] == "new":
            good = (r["hover_clicks_taken"] == 0 and r["hover_clicks_legacy"] == 0 and r["hover_owner_mouse"] == "2" and r["away_taken"] == 1
                    and r["world_clicks_seen"] == 6 and r["text_sprint_taken"] == 0 and r["text_off_sprint_taken"] == 1)
            ok &= good
            lines.append(f"{'PASS' if good else 'FAIL'} ui {r['path']}: {json.dumps(r)}")
        if r["gate"] == "replay":
            ok &= r["bit_exact_input"]
            lines.append(f"{'PASS' if r['bit_exact_input'] else 'FAIL'} replay: {json.dumps(r)}")
    lines.append("C0 GATE: " + ("PASS" if ok else "FAIL"))
    return ok, lines


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--exe", required=True)
    ap.add_argument("cmd", choices=["matrix", "ui", "replay", "all"])
    ap.add_argument("--modes", default="new,old")
    ap.add_argument("--paths", default="start,floor")
    ap.add_argument("--lengths", default=",".join(map(str, LENGTHS)))
    ap.add_argument("--caps", default=",".join(CAPS))
    ap.add_argument("--run", default="")
    a = ap.parse_args()
    run = Path(a.run) if a.run else OUTROOT / f"run-{datetime.datetime.now():%Y%m%d-%H%M%S}"
    run.mkdir(parents=True, exist_ok=True)
    rows = []
    lengths = [int(x) for x in a.lengths.split(",")]
    caps = {k: CAPS[k] for k in a.caps.split(",")}
    exe = a.exe
    if a.cmd in ("matrix", "all"):
        for path in a.paths.split(","):
            for mode in a.modes.split(","):
                run_matrix(exe, run, path, mode, lengths, caps, rows)
    if a.cmd in ("ui", "all"):
        for path in a.paths.split(","):
            run_ui(exe, run, path, "new", rows)
    if a.cmd in ("replay", "all"):
        r = run_replay(exe, run)
        rows.append(r)
        print("  replay:", json.dumps(r), flush=True)
    ok, lines = verdict(rows)
    (run / "verdict.txt").write_text("\n".join(lines) + "\n")
    (run / "baseline.json").write_text(json.dumps({"exe": str(exe), "when": datetime.datetime.now().astimezone().isoformat(timespec="seconds"), "rows": rows}, indent=1))
    print("\n".join(lines))
    print(f"run dir: {run}")
    sys.exit(0 if ok else 1)


if __name__ == "__main__":
    main()
