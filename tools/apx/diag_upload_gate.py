"""diag_upload_gate.py -- D91 gate: the opt-in upload (src/apx/diag_upload.cpp). Headless; real network only with --live.

  python tools\\apx\\diag_upload_gate.py --exe <barony.exe> [--live]

Dry-run launches write the request bodies to diag/upload-dryrun instead of sending them, so every rule is checked on the
exact bytes that would leave the machine:
  U0 consent unset           -> nothing queued or written for upload
  U1 consent No              -> nothing queued
  U2 consent Yes (test id)   -> one session row, event batch at exit; sizes within the table caps; no cwd, pid or account name
  U3+U4 planted crash, then a second launch in the same folder -> the crash report is uploaded on the NEXT launch, once
  L  (--live) the same session sent to the real Supabase with the TEST install id; the retry queue must end empty
Exit code non-zero when any check fails.
"""
import argparse
import datetime
import json
import os
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).parent))
import apx_locks  # noqa: E402
import diag_gate as G  # noqa: E402

TEST_ID = "00000000-0000-4000-8000-000000000d91"
BASE = {"APX_DIAG_VERSION": "0.0.0-test"}


def bodies(run, table):
    d = run / "diag" / "upload-dryrun"
    out = []
    for f in sorted(d.glob(f"*-{table}.json")) if d.exists() else []:
        out.append(json.loads(f.read_text(encoding="ascii", errors="replace")))
    return out


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--exe", required=True)
    ap.add_argument("--live", action="store_true")
    a = ap.parse_args()
    exe = Path(a.exe).resolve()
    checks = []

    def chk(name, ok, detail=""):
        checks.append({"check": name, "ok": bool(ok), "detail": detail})
        print(f"[{'PASS' if ok else 'FAIL'}] {name} {detail}", flush=True)

    lock = apx_locks.take_slot("diag-upload-gate", 86400)
    try:
        dry = {"APX_DIAG_UPLOAD_DRYRUN": "1"}
        yes = dict(dry, APX_DIAG_UPLOAD="yes", APX_DIAG_INSTALL_ID=TEST_ID, **BASE)
        U0 = G.launch(exe, "U0-unset", dry, seconds=20)
        U1 = G.launch(exe, "U1-no", dict(dry, APX_DIAG_UPLOAD="no"), seconds=20)
        U2 = G.launch(exe, "U2-yes", dict(yes, APX_DIAG_PLANT="exit:900"), seconds=60)
        U3 = G.launch(exe, "U3-crash", dict(yes, APX_DIAG_PLANT="crash:300"), seconds=40)
        U4 = G.launch_in(exe, U3["run"], yes, seconds=20)
        L = G.launch(exe, "L-live", dict(APX_DIAG_UPLOAD="yes", APX_DIAG_INSTALL_ID=TEST_ID, APX_DIAG_PLANT="exit:900", **BASE), seconds=60) if a.live else None
    finally:
        lock.unlink(missing_ok=True)

    user = os.environ.get("USERNAME", "")
    chk("U0: consent unset -> nothing queued for upload", not (U0["run"] / "diag" / "upload-dryrun").exists() and not (U0["run"] / "diag" / "upload-queue").exists())
    cj = U0["run"] / "diag" / "consent.json"
    chk("U0: an anonymous random install id is stored locally, consent stays unset", cj.exists() and '"consent":"unset"' in cj.read_text() and len(json.loads(cj.read_text())["install_id"]) == 36)
    chk("U1: consent No -> nothing queued", not (U1["run"] / "diag" / "upload-dryrun").exists())
    s = bodies(U2["run"], "labyrinth_sessions")
    e = bodies(U2["run"], "labyrinth_events")
    chk("U2: one session body with install id, session id, version, sha", len(s) == 1 and s[0]["install_id"] == TEST_ID and s[0]["version"] == "0.0.0-test"
        and bool(s[0]["build_sha"]) and len(s[0]["session_id"]) == 36, f"{len(s)} bodies")
    if s:
        raw = json.dumps(s[0])
        size = len(json.dumps(s[0]["info"]))
        chk("U2: session info within 16 KB with hardware and gpu", size <= 16384 and "gl_renderer" in raw and '"cpu"' in raw, f"{size} bytes")
        chk("U2: no working directory, process id or account name in the session body", '"cwd"' not in raw and '"pid"' not in raw and (not user or user.lower() not in raw.lower()), f"account {user!r} absent")
    nrows = sum(len(b["batch"]) for b in e)
    chk("U2: event batch queued at exit, within 64 KB", len(e) >= 1 and all(len(json.dumps(b["batch"])) <= 65536 for b in e), f"{len(e)} batches, {nrows} rows")
    if e:
        chk("U2: batch rows are performance/error/event rows, no account name",
            all(r.get("t") in ("sec", "spike", "ev", "err", "errsum") for b in e for r in b["batch"]) and (not user or user.lower() not in json.dumps(e).lower()))
    c = bodies(U4["run"], "labyrinth_crashes")
    chk("U4: the NEXT launch uploads the crash report once", len(c) == 1 and c[0]["install_id"] == TEST_ID and "apxDiagPlantedCrash" in c[0]["stack"], f"{len(c)} bodies")
    if c:
        ev = c[0]["last_events"]
        chk("U4: stack within 32 KB; last_events a list with no raw log lines and no account name",
            len(c[0]["stack"]) <= 32768 and isinstance(ev, list) and not any(str(x).startswith("log:") for x in ev) and (not user or user.lower() not in json.dumps(c[0]).lower()),
            f"stack {len(c[0]['stack'])} bytes, {len(ev)} events")
        chk("U4: crash body keeps the crashed build's sha and session id", bool(c[0]["build_sha"]) and len(c[0]["session_id"]) == 36)
    chk("U4: sent marker written so the report is not uploaded twice", bool(list((U3["run"] / "diag").glob("crash-*.txt.sent"))))
    if L:
        qd = L["run"] / "diag" / "upload-queue"
        q = list(qd.glob("*.json")) if qd.exists() else []
        chk("L: live upload with the TEST install id left nothing in the retry queue", not q, f"{len(q)} spooled")
        ups = [r for r in G.rows(L["run"]) if r["t"] == "upload"]
        got = {r["table"]: r["status"] for r in ups}
        chk("L: the server accepted the session and the event batch (HTTP 201)", got.get("labyrinth_sessions") == 201 and got.get("labyrinth_events") == 201, str(got))

    ok = all(c["ok"] for c in checks)
    G.ROOT.mkdir(parents=True, exist_ok=True)
    with open(G.ROOT / "checks.jsonl", "a", encoding="ascii") as f:
        f.write(json.dumps({"ts": datetime.datetime.now().astimezone().isoformat(timespec="seconds"), "gate": "D91", "ok": ok, "checks": checks}) + "\n")
    sys.exit(0 if ok else 1)


if __name__ == "__main__":
    main()
