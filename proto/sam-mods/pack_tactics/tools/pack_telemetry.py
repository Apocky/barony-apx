"""pack_telemetry.py -- sam_log.txt rows of the pack_tactics PROTOTYPE -> JSONL + a baseline.

Reads the "pack_tactics: ENCOUNTER ...", "pack_tactics: TRIAL ..." and "pack_tactics: TRIALSUM ..."
lines that main.lua writes, one per encounter / self-test arm / trial verdict, and writes
(workspace rule 12):

  <out>/rows.jsonl     one row per line found: point, kind, run, dims, timings, metrics, finish,
                       flags; then ONE baseline row per run (kind "baseline") with the aggregates
                       per configuration. Appending is idempotent: a run already present is skipped.
  <out>/baseline.json  the latest baseline per configuration, for regression comparisons.

Default <out> is C:\\Apocrypha\\diagnostics\\pack_tactics. Default input is the Barony install's
sam_log.txt; pass log files to parse others (sam_logs\\ keeps older sessions).

Exit 1 when a TRIALSUM shows the control failing (tokens off did NOT take more damage than tokens
on), when the self-test reported a failed check, or when the session had script errors.

Usage:
  python pack_telemetry.py [--out DIR] [--dry-run] [LOG ...]
"""
import argparse
import json
import os
import re
import statistics
import sys
import time

BARONY = r"C:\Program Files (x86)\Steam\steamapps\common\Barony"
DEFAULT_OUT = r"C:\Apocrypha\diagnostics\pack_tactics"

RE_SESSION = re.compile(r"Session #(\d+) - (\d{4}-\d{2}-\d{2}) (\d{2}:\d{2}:\d{2})")
RE_ROW = re.compile(r"pack_tactics: (ENCOUNTER|TRIAL|TRIALSUM) (.*)$")
RE_RESULT = re.compile(r"PACK_TACTICS RESULT: (\d+) passed, (\d+) FAILED")
RE_SCRIPT_ERR = re.compile(r"Script errors:\s+(\d+)")
RE_EXIT = re.compile(r"Test run finished with exit code (\d+)")
# S.A.M's session summary counts every hook; a key press in a headless self-test means someone was
# playing the test character (sessions 22 and 31 on 2026-10-18), which contaminates the trial
RE_PRESSES = re.compile(r"^\s+on_action_pressed\s+(\d+)\s*$")
RE_KV = re.compile(r"(\w+)=(\S+)")

ENC_DIMS = ("floor", "pack", "kind", "size", "players", "spawned", "tactics", "tokens", "token_cap", "verbs",
            "population", "test")
ENC_TIMINGS = ("dur_s", "ttk_s")
# TRIAL v=1 compared the token cap (tokens=on|off); v=2 compares D62's natural limits (limits=on|off)
TRIAL_DIMS = ("v", "arm", "tokens", "limits", "token_cap", "creatures", "population", "kind", "weapon", "floor", "seed")
TRIAL_TIMINGS = ("window_s",)


def num(v):
    if v in ("na", "nan"):
        return None
    if v in ("true", "false"):
        return v == "true"
    try:
        return int(v)
    except ValueError:
        try:
            return float(v)
        except ValueError:
            return v


def parse_log(path):
    run, results, script_errors, exit_code = None, None, None, None
    rows = []
    presses = 0
    with open(path, encoding="utf8", errors="replace") as fh:
        for line in fh:
            m = RE_PRESSES.search(line)
            if m:
                presses = max(presses, int(m.group(1)))
            m = RE_SESSION.search(line)
            if m and run is None:
                run = "sam-session-%s-%sT%s" % (m.group(1), m.group(2).replace("-", ""), m.group(3).replace(":", ""))
            m = RE_RESULT.search(line)
            if m:
                results = (int(m.group(1)), int(m.group(2)))
            m = RE_SCRIPT_ERR.search(line)
            if m:
                script_errors = int(m.group(1))
            m = RE_EXIT.search(line)
            if m:
                exit_code = int(m.group(1))
            m = RE_ROW.search(line)
            if m:
                kv = {k: num(v) for k, v in RE_KV.findall(m.group(2))}
                rows.append((m.group(1), kv))
    if run is None:
        run = "file-%s-%d" % (os.path.basename(path), int(os.path.getmtime(path)))
    return run, rows, results, script_errors, exit_code, presses


def to_row(run, src, idx, kind, kv):
    if kind == "ENCOUNTER":
        dims = {k: kv.get(k) for k in ENC_DIMS}
        timings = {k: kv.get(k) for k in ENC_TIMINGS}
        metrics = {k: v for k, v in kv.items() if k not in ENC_DIMS and k not in ENC_TIMINGS and k not in ("v", "outcome")}
        finish = kv.get("outcome")
        flags = []
        if kv.get("tokens") == "on":
            flags.append("SUBSTITUTE_TOKENS")      # target clearing, not the fork's attack-state gate
        if kv.get("tactics") == "on":
            flags.append("SUBSTITUTE_FLANK_ROUTE")
        if (kv.get("dur_s") or 0) < 5:
            flags.append("SHORT")
        if kv.get("test") is True:
            # the self-test negates incoming damage (the character must not die), so player.on_damage_taken
            # never fires and these rows read 0; the TRIAL rows carry the damage instead
            flags += ["SELFTEST", "DAMAGE_NEGATED_IN_TEST"]
        point = "encounter:f%s:p%s" % (kv.get("floor"), kv.get("pack"))
    elif kind == "TRIAL":
        dims = {k: kv.get(k) for k in TRIAL_DIMS}
        timings = {k: kv.get(k) for k in TRIAL_TIMINGS}
        metrics = {k: v for k, v in kv.items() if k not in TRIAL_DIMS and k not in TRIAL_TIMINGS and k != "v"}
        finish = "measured"
        flags = ["SELFTEST", "HEADLESS_PASSIVE_PLAYER"]
        if kv.get("tokens") == "on" or kv.get("token_cap") == "on":
            flags.append("SUBSTITUTE_TOKENS")
        if kv.get("limits") == "on":
            flags.append("SUBSTITUTE_PACIFY_HOLD")   # EFF_PACIFY holds the swing, not the fork's motor gate
        if (kv.get("leaks") or 0) > 0:
            flags.append("LEAKS")
        if (kv.get("foreign_peak") or 0) > 0:
            flags.append("CONTAMINATED")           # a creature not spawned by the trial came near
        if (kv.get("player_input") or 0) > 0 or (kv.get("player_attacks") or 0) > 0:
            flags.append("PLAYER_INPUT")           # someone played the test character during this arm
        if "limits" in kv:
            point = "trial:arm%s:limits_%s" % (kv.get("arm"), kv.get("limits"))
        else:
            point = "trial:arm%s:tokens_%s" % (kv.get("arm"), kv.get("tokens"))
    else:  # TRIALSUM
        dims = {"population": kv.get("population"), "creatures": kv.get("creatures"), "arms": kv.get("arms")}
        timings = {}
        metrics = {k: v for k, v in kv.items() if k not in dims and k != "v"}
        finish = "control_ok" if kv.get("control_more_damage") is True else "CONTROL_FAILED"
        flags = ["SELFTEST"] + ([] if kv.get("control_more_damage") is True else ["CONTROL_FAILED"])
        point = "trialsum:pop%s" % kv.get("population")
    return {"point": point, "kind": kind.lower(), "run": run, "source": src, "index": idx,
            "dims": dims, "timings": timings, "metrics": metrics, "finish": finish, "flags": flags}


def config_key(row):
    d = row["dims"]
    if row["kind"] == "trial":   # the arena is the floor the test stands on: keep floors apart
        if d.get("limits") is not None:
            return "trial|f%s|pop%s|n%s|limits=%s" % (d.get("floor"), d.get("population"), d.get("creatures"), d.get("limits"))
        return "trial|f%s|pop%s|n%s|tokens=%s" % (d.get("floor"), d.get("population"), d.get("creatures"), d.get("tokens"))
    if row["kind"] == "encounter":
        return "encounter|pop%s|tokens=%s|tactics=%s|test=%s" % (d.get("population"), d.get("tokens"), d.get("tactics"), d.get("test"))
    return None


def aggregate(rows):
    groups = {}
    for r in rows:
        k = config_key(r)
        if k:
            groups.setdefault(k, []).append(r)
    out = {}
    for k, rs in sorted(groups.items()):
        if rs[0]["kind"] == "trial":
            vals = [r["metrics"].get("dmg_pm") or 0.0 for r in rs]
            out[k] = {"n": len(rs), "dmg_pm_mean": round(statistics.fmean(vals), 2),
                      "dmg_pm_median": round(statistics.median(vals), 2),
                      "dmg_total": sum(r["metrics"].get("dmg") or 0 for r in rs),
                      "holds_mean": round(statistics.fmean([r["metrics"].get("holds") or 0 for r in rs]), 1),
                      "contact_mean": round(statistics.fmean([r["metrics"].get("contact_mean") or 0 for r in rs]), 2)}
        else:
            vals = [r["metrics"].get("dmg_ppm") or 0.0 for r in rs]
            ttk = [r["timings"].get("ttk_s") for r in rs if r["timings"].get("ttk_s") is not None]
            out[k] = {"n": len(rs), "dmg_ppm_median": round(statistics.median(vals), 2),
                      "dmg_ppm_mean": round(statistics.fmean(vals), 2),
                      "ttk_s_median": round(statistics.median(ttk), 2) if ttk else None,
                      "player_deaths": sum(r["metrics"].get("player_deaths") or 0 for r in rs),
                      "retreats": sum(r["metrics"].get("retreats") or 0 for r in rs),
                      "flank_arrivals": sum(r["metrics"].get("flank_arrivals") or 0 for r in rs),
                      "outcomes": dict(sorted({o: sum(1 for r in rs if r["finish"] == o) for o in {r["finish"] for r in rs}}.items()))}
    return out


def existing_runs(path):
    runs = set()
    if os.path.exists(path):
        with open(path, encoding="utf8") as fh:
            for line in fh:
                try:
                    runs.add(json.loads(line).get("run"))
                except json.JSONDecodeError:
                    pass
    return runs


def main():
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("logs", nargs="*", default=[os.path.join(BARONY, "sam_log.txt")])
    ap.add_argument("--out", default=DEFAULT_OUT)
    ap.add_argument("--dry-run", action="store_true", help="parse and print, write nothing")
    a = ap.parse_args()

    os.makedirs(a.out, exist_ok=True)
    rows_path = os.path.join(a.out, "rows.jsonl")
    base_path = os.path.join(a.out, "baseline.json")
    seen = existing_runs(rows_path)
    bad = False
    all_new = []
    baseline = {}
    if os.path.exists(base_path):
        with open(base_path, encoding="utf8") as fh:
            baseline = json.load(fh)

    for log in a.logs:
        run, found, results, script_errors, exit_code, presses = parse_log(log)
        rows = [to_row(run, os.path.basename(log), i, k, kv) for i, (k, kv) in enumerate(found)]
        if presses and results is not None:        # a self-test session someone played in
            for r in rows:
                r["flags"].append("PLAYER_INPUT_IN_SESSION")
                if r["kind"] == "trialsum":
                    r["finish"] = "contaminated"
        agg = aggregate([r for r in rows if "PLAYER_INPUT_IN_SESSION" not in r["flags"]])
        flags = ["PLAYER_INPUT_IN_SESSION"] if presses and results is not None else []
        if results and results[1] > 0:
            flags.append("SELFTEST_FAILED")
        if script_errors:
            flags.append("SCRIPT_ERRORS")
        sums = [r for r in rows if r["kind"] == "trialsum"]
        if any("CONTROL_FAILED" in r["flags"] for r in sums):
            flags.append("CONTROL_FAILED")
        base_row = {"point": "baseline", "kind": "baseline", "run": run, "source": os.path.basename(log),
                    "dims": {"configs": len(agg)}, "timings": {"parsed_at": time.strftime("%Y-%m-%dT%H:%M:%S%z")},
                    "metrics": {"configs": agg, "selftest": list(results) if results else None,
                                "script_errors": script_errors, "test_exit_code": exit_code,
                                "trialsum": sums[-1]["metrics"] if sums else None},
                    "finish": "ok" if not flags else "flagged", "flags": flags}
        print("%s: %d rows (%d encounter, %d trial, %d trialsum) selftest=%s script_errors=%s exit=%s flags=%s" % (
            run, len(rows), sum(r["kind"] == "encounter" for r in rows), sum(r["kind"] == "trial" for r in rows),
            len(sums), results, script_errors, exit_code, ",".join(flags) or "-"))
        for k, v in agg.items():
            print("   %-48s %s" % (k, json.dumps(v)))
        for r in sums:
            m = r["metrics"]
            print("   TRIALSUM dmg_off=%s dmg_on=%s per_min off=%s on=%s ratio=%s a2_plus50=%s -> %s" % (
                m.get("dmg_off"), m.get("dmg_on"), m.get("dmg_pm_off"), m.get("dmg_pm_on"), m.get("ratio"),
                m.get("a2_plus50"), r["finish"]))
        if flags:
            bad = True
        if run in seen:
            print("   (run already in %s; not appended again)" % rows_path)
            continue
        all_new.extend(rows + [base_row])
        for k, v in agg.items():
            baseline[k] = dict(v, run=run)

    if not a.dry_run and all_new:
        with open(rows_path, "a", encoding="utf8") as fh:
            for r in all_new:
                fh.write(json.dumps(r, sort_keys=True) + "\n")
        with open(base_path, "w", encoding="utf8") as fh:
            json.dump(baseline, fh, indent=1, sort_keys=True)
        print("wrote %d rows to %s; baseline %s" % (len(all_new), rows_path, base_path))
    return 1 if bad else 0


if __name__ == "__main__":
    sys.exit(main())
