#!/usr/bin/env python3
"""tools/apx/gate_f6.py -- the F6 batched launch (T3 gates: save format, host = client, vanilla).

Runs, in one job (lane protocol v2; D76 stop rule: each run is a deterministic gate, run once,
stop at the first decisive failure unless --all):
  save        APX_BF6_PROBE=save, floor 1 of mapseed 101: 40 rolled items through saveGame /
              getSaveGameInfo / loadGame; the probe exits 0 (pass) or 3 (fail)
  save-strip  control APX_SAVE_STRIP=1: parts must be lost on every item with parts
  save-vanilla control APX_PARTS=0: no parts, no sidecar; the floor line equals the parts-on run
  net         host (-apxhost, APX_BF6_PROBE=net) + client (-apxjoin=127.0.0.1): the host gives the
              client 40 rolled items through itemPickup; every blob the host sent arrives equal
  net-strip   control APX_NET_STRIP=1 on the host: the client gets no blob (40/40)
  net-vanilla control APX_PARTS=0 on the host: ITEM packets stay 29 bytes, no blob
Locks (rule 1, D70): single runs take a slot in C:\\Apocrypha\\locks\\barony-slots; the two-process
runs take C:\\Apocrypha\\locks\\barony exclusively; every launch waits for barony-want\\*.WANT.
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

sys.path.insert(0, str(Path(__file__).resolve().parent))
import parity_f0 as pf  # noqa: E402

LOCKS = Path(r"C:\Apocrypha\locks")
EXCL, SLOTS, WANT = LOCKS / "barony", LOCKS / "barony-slots", LOCKS / "barony-want"


def acquire(exclusive, limit_s):
    for d in (EXCL, SLOTS, WANT):
        d.mkdir(parents=True, exist_ok=True)
    stamp = datetime.datetime.now().astimezone().isoformat(timespec="seconds") + " f6 gate_f6.py\n"
    t0 = time.monotonic()
    while time.monotonic() - t0 < limit_s:
        busy = any(WANT.glob("*.WANT")) or any(EXCL.glob("*.LOCK"))
        if exclusive:
            busy = busy or any(SLOTS.glob("slot*.LOCK")) or bool(pf.barony_running())
        if not busy:
            if exclusive:
                p = EXCL / "f6.LOCK"
                p.write_text(stamp, encoding="ascii")
                time.sleep(1)
                if not [q for q in EXCL.glob("*.LOCK") if q != p] and not list(SLOTS.glob("slot*.LOCK")):
                    return p
                p.unlink()
            else:
                for n in (1, 2, 3):
                    p = SLOTS / f"slot{n}.LOCK"
                    try:
                        with open(p, "x", encoding="ascii") as fh:
                            fh.write(stamp)
                        if not any(EXCL.glob("*.LOCK")):
                            return p
                        p.unlink()
                        break
                    except FileExistsError:
                        continue
        time.sleep(1)
    sys.exit(f"no Barony slot for {limit_s:.0f} s")


def launch(exe, run_dir, args, env_extra, datadir):
    env = os.environ.copy()
    for k in list(env):
        if k.startswith(("APX_", "APXGEN")):
            env.pop(k)
    env["BARONY_DATADIR"] = str(datadir)
    env.update(env_extra)
    env.setdefault("APX_HEADLESS", "1")  # parent 2026-10-19: headless only (hidden window, never focused)
    return subprocess.Popen([str(exe)] + args, cwd=run_dir, env=env)


def log_text(run_dir):
    return pf.read_log(run_dir / "log.txt")


def run_save(name, exe, out, env_extra, args):
    stamp = datetime.datetime.now().strftime("%Y%m%d-%H%M%S")
    d = out / "runs" / f"{stamp}-{name}"
    d.mkdir(parents=True)
    rows = d / "probe.jsonl"
    env = {"APX_BF6_PROBE": "save,salvage,craft,bundle", "APX_BF6_OUT": str(rows)}
    env.update(env_extra)
    lock = acquire(False, args.wait)
    t0 = time.monotonic()
    try:
        p = launch(exe, d, ["-windowed", "-quickstart=barbarian", "-apxfloor=1", f"-apxmapseed={args.mapseed}"], env, args.datadir)
        try:
            code = p.wait(args.timeout)
        except subprocess.TimeoutExpired:
            pf.stop(p, pf.find_game_window(p.pid))
            code = "timeout"
    finally:
        lock.unlink(missing_ok=True)
    text = log_text(d)
    probe, salvage, bundle, comps, craft = None, None, None, None, None
    if rows.exists():
        for l in rows.read_text().splitlines():
            if l.strip():
                j = json.loads(l)
                if j.get("probe") == "save":
                    probe = j
                elif j.get("probe") == "salvage":
                    salvage = j
                elif j.get("probe") == "bundle":
                    bundle = j
                elif j.get("probe") == "components":
                    comps = j
                elif j.get("probe") == "craft":
                    craft = j
    gen = re.findall(r"successfully generated a dungeon with [^\n]*", text)
    sidecars = sorted(str(x) for x in d.rglob("*.apxparts"))
    return {"name": name, "env": env_extra, "exit": code, "seconds": round(time.monotonic() - t0, 1), "probe": probe, "salvage": salvage, "bundle": bundle, "components": comps, "craft": craft,
            "generated": gen[-1] if gen else None, "sidecars": sidecars, "run_dir": str(d),
            "data_line": (re.findall(r"apxparts: data [^\n]*", text) or [None])[-1]}


def crop_mean(png):
    """Mean RGB of the first-person blade column (x 60-70 %, y 45-85 % of the view; measured on the 1280x720 shots:
    the wider 55-98 % box was diluted by background and popups, 2.9 % tinted vs 1.6 % untinted)."""
    from PIL import Image
    im = Image.open(png).convert("RGB")
    w, h = im.size
    box = im.crop((int(w * 0.60), int(h * 0.45), int(w * 0.70), int(h * 0.85)))
    px = list(box.get_flattened_data()) if hasattr(box, "get_flattened_data") else list(box.getdata())
    n = len(px) or 1
    return [sum(c[i] for c in px) / n for i in range(3)]


def run_visual(name, exe, out, env_extra, args):
    stamp = datetime.datetime.now().strftime("%Y%m%d-%H%M%S")
    d = out / "runs" / f"{stamp}-{name}"
    d.mkdir(parents=True)
    env = {"APX_BF6_PROBE": "visual", "APX_BF6_OUT": str(d / "probe.jsonl")}
    env.update(env_extra)
    lock = acquire(False, args.wait)
    try:
        p = launch(exe, d, ["-windowed", "-quickstart=barbarian", "-apxfloor=1", f"-apxmapseed={args.mapseed}"], env, args.datadir)
        try:
            code = p.wait(args.timeout)
        except subprocess.TimeoutExpired:
            pf.stop(p, pf.find_game_window(p.pid))
            code = "timeout"
    finally:
        lock.unlink(missing_ok=True)
    a, b = d / "visual-a.png", d / "visual-b.png"
    row = {"name": name, "env": env_extra, "exit": code, "run_dir": str(d), "shots": [str(a), str(b)]}
    if a.exists() and b.exists():
        ma, mb = crop_mean(a), crop_mean(b)
        row.update(mean_iron=[round(x, 1) for x in ma], mean_gold=[round(x, 1) for x in mb],
                   distance_pct=round(100 * max(abs(x - y) for x, y in zip(ma, mb)) / 255, 2))
    return row


def run_net(name, exe, out, host_env, args):
    stamp = datetime.datetime.now().strftime("%Y%m%d-%H%M%S")
    dirs = {r: out / "runs" / f"{stamp}-{name}-{r}" for r in ("host", "client")}
    for v in dirs.values():
        v.mkdir(parents=True)
    henv = {"APX_BF6_PROBE": "net", "APX_BF6_OUT": str(dirs["host"] / "probe.jsonl")}
    henv.update(host_env)
    lock = acquire(True, args.wait)
    t0 = time.monotonic()
    procs = {}
    row = {"name": name, "host_env": host_env, "finish": "error"}
    try:
        procs["host"] = launch(exe, dirs["host"], ["-windowed", "-apxhost"], henv, args.datadir)
        pf.wait_for(dirs["host"] / "log.txt", re.compile(r"apxlobby: hosting"), time.monotonic() + 90, procs["host"])
        procs["client"] = launch(exe, dirs["client"], ["-windowed", "-apxjoin=127.0.0.1"],
                                 {"APX_BF6_PROBE": "netclient", "APX_BF6_OUT": str(dirs["client"] / "probe.jsonl")}, args.datadir)
        deadline = time.monotonic() + args.timeout
        pf.wait_for(dirs["host"] / "log.txt", re.compile(r"apxprobe: \{\"probe\":\"net\""), deadline, procs["host"])
        # the client logs one "net recv" per ITEM packet; stop once 40 arrived or the budget ends
        want = 40
        while time.monotonic() < deadline:
            if len(re.findall(r"apxparts: net recv ", log_text(dirs["client"]))) >= want:
                break
            time.sleep(0.5)
        # the client's half: break 3 (FBOK), drop 5 (DROP), equip 1 (EQUI), logged once its 40 items arrived
        cl_rows = dirs["client"] / "probe.jsonl"
        while time.monotonic() < deadline and not cl_rows.exists() and not host_env:  # controls: the client gets no parts, so no client half
            time.sleep(0.5)
        time.sleep(4)  # the packets the client just sent reach the host
        sent = re.findall(r"apxparts: net sent type (\d+) len (\d+) blob ([0-9a-f]+)", log_text(dirs["host"]))
        recv = re.findall(r"apxparts: net recv type (\d+) len (\d+) blob ([0-9a-f]+|none)", log_text(dirs["client"]))
        c_sent = re.findall(r"apxparts: net sent type (\d+) len (\d+) blob ([0-9a-f]+)", log_text(dirs["client"]))
        h_recv = re.findall(r"apxparts: net recv type (\d+) len (\d+) blob ([0-9a-f]+|none)", log_text(dirs["host"]))
        b_absorbed = re.findall(r"apxparts: bundle absorbed player \d+ \S+ \d+ g appearance ([0-9a-f]+)", log_text(dirs["client"]))
        c_floor = re.findall(r"apxparts: component on floor \"[^\"]*\" blob ([0-9a-f]+)", log_text(dirs["host"]))
        b_floor = re.findall(r"apxparts: bundle on floor \S+ \d+ g appearance ([0-9a-f]+)", log_text(dirs["host"]))
        fb_sent = re.findall(r"apxparts: formbook sent player \d+ forms \d+ hash ([0-9a-f]+)", log_text(dirs["client"]))
        fb_recv = re.findall(r"apxparts: formbook recv player \d+ forms \d+ hash ([0-9a-f]+)", log_text(dirs["host"]))
        hr = {(t, b) for t, _, b in h_recv}
        row.update(bundles_absorbed_on_client=b_absorbed, bundles_on_host_floor=b_floor, components_on_host_floor=c_floor,
                   client_sent=len(c_sent), client_sent_found_on_host=sum(1 for t, _, b in c_sent if (t, b) in hr),
                   formbook_sent=fb_sent[-1] if fb_sent else None, formbook_recv=fb_recv[-1] if fb_recv else None,
                   client_probe=(json.loads(cl_rows.read_text().splitlines()[-1]) if cl_rows.exists() else None))
        hp = [json.loads(l) for l in (dirs["host"] / "probe.jsonl").read_text().splitlines() if l.strip()] if (dirs["host"] / "probe.jsonl").exists() else []
        # matched by (type, blob) as a multiset: blob-less packets (the D85 bundle) interleave, so position proves nothing
        from collections import Counter
        rc = Counter((t, b) for t, _, b in recv if b != "none")
        equal = sum((Counter((t, b) for t, _, b in sent) & rc).values())
        row.update(finish="ok", sent=len(sent), recv=len(recv), equal_blobs=equal, recv_none=sum(r[2] == "none" for r in recv),
                   recv_lens=sorted({int(r[1]) for r in recv}), host_probe=hp[-1] if hp else None,
                   seconds=round(time.monotonic() - t0, 1), run_dirs={k: str(v) for k, v in dirs.items()})
    except Exception as e:  # noqa: BLE001
        row["error"] = f"{type(e).__name__}: {e}"
    finally:
        for p in procs.values():
            pf.stop(p, pf.find_game_window(p.pid) if p.poll() is None else None)
        lock.unlink(missing_ok=True)
    return row


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--exe", type=Path, required=True)
    ap.add_argument("--datadir", type=Path, default=pf.STEAM_DIR)
    ap.add_argument("--out", type=Path, default=Path(r"C:\Apocrypha\diagnostics\bf6"))
    ap.add_argument("--mapseed", type=int, default=101)
    ap.add_argument("--timeout", type=float, default=150.0)
    ap.add_argument("--wait", type=float, default=7200.0)
    ap.add_argument("--only", default="save,save-strip,save-vanilla,visual,visual-off,net,net-strip,net-vanilla")
    ap.add_argument("--all", action="store_true", help="do not stop at the first failing gate")
    args = ap.parse_args()
    args.out.mkdir(parents=True, exist_ok=True)
    stamp = datetime.datetime.now().strftime("%Y%m%d-%H%M%S")
    jl = args.out / f"matrix-{stamp}.jsonl"
    only = args.only.split(",")
    results, gates = {}, []

    def emit(row):
        with open(jl, "a", encoding="utf-8") as fh:
            fh.write(json.dumps(row) + "\n")

    def gate(name, ok, detail):
        gates.append({"gate": name, "pass": bool(ok), "detail": detail})
        print(f"[f6] {name}: {'PASS' if ok else 'FAIL'} {detail}", flush=True)
        emit({"gate": name, "pass": bool(ok), "detail": detail})
        return ok or args.all

    if "save" in only:
        r = results["save"] = run_save("save", args.exe, args.out, {}, args)
        emit(r)
        p = r["probe"] or {}
        if not gate("data loaded (parts on without APX_PARTS)", p.get("parts_on") == 1, f"parts_on {p.get('parts_on')} ({r.get('data_line')})"):  # a build without data/apx must not pass
            return 1
        if not gate("forced 6-part roll in the save", (p.get("six_parts") or 0) >= 1, f"six-part items {p.get('six_parts')}, max parts {p.get('max_parts')}"):
            return 1
        if not gate("Formbook: 5 breaks master a Form; survives save/load", p.get("broke") == 5 and p.get("blades_successes") == 5 and p.get("formbook_equal") == 1,
                    f"broke {p.get('broke')}, successes {p.get('blades_successes')}, book {p.get('formbook_before')} -> {p.get('formbook_after')}"):
            return 1
        sv = r.get("salvage") or {}
        if not gate("salvage live: station 10 + bench keep-part 10", sv.get("pass") is True,
                    f"station {sv.get('station_ok')}/10, bench {sv.get('bench_ok')}/10, kept one part {sv.get('bench_kept_one_part')}, metal {sv.get('metal_scrap')}, "
                    f"magic {sv.get('magic_scrap')}, aura gems {sv.get('aura_gems')} (counted {sv.get('aura_counted')}), pouch {sv.get('pouch_before')} g -> {sv.get('pouch_after')} g after save/load, carry weight {sv.get('carry_weight')} = items {sv.get('item_weight')} + gold + pouch {sv.get('pouch_units')} ({'ok' if sv.get('weight_ok') else 'WRONG'}), inventory {sv.get('inventory_before')} -> {sv.get('inventory_after')}, grams out {sv.get('grams_out')} = paid {sv.get('grams_paid')} + ledger {sv.get('ledger_gain')} ({'conserved' if sv.get('conserved') else 'LOST'})"):
            return 1
        cm = r.get("components") or {}
        if not gate("D86 components: salvage hands the chosen part over as an item; crafted = every part; save/load keeps them", cm.get("pass") is True,
                    f"expected {cm.get('expected')}, gained {cm.get('gained')}, given {cm.get('given')}, crafted {cm.get('crafted_components')}/{cm.get('crafted_parts')} parts, "
                    f"kept after load {cm.get('kept_after_load')}/{cm.get('before_load')}, e.g. \"{cm.get('name')}\""):
            return 1
        cf = r.get("craft") or {}
        if not gate("C4/C5 live: make from the pouch, assemble a known Form, swap a part (old back intact), looted non-quick refused", cf.get("pass") is True,
                    f"form {cf.get('form')}: made {cf.get('made')}/{cf.get('required')}, pouch spent {cf.get('pouch_spent')} g = cost {cf.get('cost')} g, "
                    f"assembled {cf.get('assembled')} \"{cf.get('assembled_name')}\" IP {cf.get('power')} {cf.get('why') or ''}, swap {cf.get('swap_ok')}, looted refused {cf.get('looted_non_quick_refused')}"):
            return 1
        bd = r.get("bundle") or {}
        if not gate("D85 bundles: draw from pouch, survive save/load, pickup returns to pouch", bd.get("pass") is True,
                    f"drawn {bd.get('drawn')} {bd.get('names')}, pouch -{bd.get('pouch_drop')} g (= {bd.get('drawn_grams')}), kept after load {bd.get('kept_after_load')}/2, "
                    f"pickup gain {bd.get('pickup_gain')} g (want 250), bundles left {bd.get('bundles_in_inventory')}"):
            return 1
        if not gate("S5 save round trip (parts on)", r["exit"] == 0 and p.get("pass") and p.get("parts_kept") == p.get("parts_items") and p.get("parts_items", 0) > 0 and r["sidecars"],
                    f"exit {r['exit']}, items {p.get('items_equal')}/{p.get('inventory_before')} equal, parts {p.get('parts_kept')}/{p.get('parts_items')}, "
                    f"six-part items {p.get('six_parts')}, max parts {p.get('max_parts')}, save_blob_dropped {p.get('save_blob_dropped')}, sidecars {len(r['sidecars'])}"):
            return 1
    if "save-strip" in only:
        r = results["save-strip"] = run_save("save-strip", args.exe, args.out, {"APX_SAVE_STRIP": "1"}, args)
        emit(r)
        p = r["probe"] or {}
        if not gate("S5 control APX_SAVE_STRIP=1 (parts must be lost)", p.get("parts_items", 0) > 0 and p.get("parts_kept") == 0,
                    f"parts kept {p.get('parts_kept')}/{p.get('parts_items')}"):
            return 1
    if "save-vanilla" in only:
        r = results["save-vanilla"] = run_save("save-vanilla", args.exe, args.out, {"APX_PARTS": "0"}, args)
        emit(r)
        p = r["probe"] or {}
        same_floor = results.get("save", {}).get("generated") == r["generated"] and r["generated"]
        if not gate("P vanilla APX_PARTS=0 (no parts, no sidecar, same floor)", r["exit"] == 0 and p.get("with_parts") == 0 and not r["sidecars"] and same_floor,
                    f"exit {r['exit']}, with_parts {p.get('with_parts')}, sidecars {len(r['sidecars'])}, floor line equal {bool(same_floor)}: {r['generated']}"):
            return 1
    if os.environ.get("APX_HEADLESS") == "1" and ("visual" in only or "visual-off" in only):
        # headless renders no framebuffer (every pixel 0): the tint cannot be measured, so the steps are
        # NOT RUN (named, never counted as a pass); run them windowed under the f6 lock
        print("[f6] V palette tint + control: NOT RUN (APX_HEADLESS=1 has no framebuffer)", flush=True)
        only = [o for o in only if o not in ("visual", "visual-off")]
    if "visual" in only:
        r = results["visual"] = run_visual("visual", args.exe, args.out, {}, args)
        emit(r)
        if not gate("V palette tint: iron vs gold blade differ (> 3 % of a channel; measured 6.2 vs 0.5 off)", (r.get("distance_pct") or 0) > 3,
                    f"exit {r['exit']}, iron {r.get('mean_iron')}, gold {r.get('mean_gold')}, distance {r.get('distance_pct')} %"):
            return 1
    if "visual-off" in only:
        r = results["visual-off"] = run_visual("visual-off", args.exe, args.out, {"APX_PARTS_VIS": "0"}, args)
        emit(r)
        lit = any(sum(r.get(k) or [0]) > 0 for k in ("mean_iron", "mean_gold"))
        if not gate("V control APX_PARTS_VIS=0: no tint (< 1.5 %, frame not blank)", lit and r.get("distance_pct") is not None and r["distance_pct"] < 1.5,
                    f"exit {r['exit']}, distance {r.get('distance_pct')} %"):
            return 1
    if "net" in only:
        r = results["net"] = run_net("net", args.exe, args.out, {}, args)
        emit(r)
        if not gate("S5 host -> client parts (40 items + 1 D86 component)", r.get("finish") == "ok" and r.get("sent") == 41 and r.get("equal_blobs") == 41,
                    f"sent {r.get('sent')}, received {r.get('recv')}, equal {r.get('equal_blobs')}, lens {r.get('recv_lens')}, {r.get('error', '')}"):
            return 1
        cp = r.get("client_probe") or {}
        if not gate("S5 client -> host parts (DROP x5, EQUI x1, D86 component DROP x1)", cp.get("dropped") == 5 and cp.get("equipped") == 1 and r.get("client_sent") == 7
                    and r.get("client_sent_found_on_host") == 7,
                    f"client sent {r.get('client_sent')} blobs, found on host {r.get('client_sent_found_on_host')}, probe {cp}"):
            return 1
        hp_ = r.get("host_probe") or {}
        if not gate("D85 bundle host -> client (client pouch absorbs it)", hp_.get("bundle_sent") not in (None, "00000000") and hp_.get("bundle_sent") in r.get("bundles_absorbed_on_client", []),
                    f"host sent {hp_.get('bundle_sent')}, client absorbed {r.get('bundles_absorbed_on_client')}"):
            return 1
        if not gate("D85 bundle client -> host (dropped bundle on the host's floor)", cp.get("bundle_dropped") not in (None, "00000000") and cp.get("bundle_dropped") in r.get("bundles_on_host_floor", []),
                    f"client dropped {cp.get('bundle_dropped')}, host floor {r.get('bundles_on_host_floor')}"):
            return 1
        if not gate("D86 component host -> client (byte-equal blob)", hp_.get("component_sent") not in (None, "0" * 16) and hp_.get("component_sent") == cp.get("component_received"),
                    f"host sent {hp_.get('component_sent')}, client holds {cp.get('component_received')} \"{cp.get('component_name')}\""):
            return 1
        if not gate("D86 component client -> host (dropped, on the host floor byte-equal)", cp.get("component_dropped") not in (None, "0" * 16) and cp.get("component_dropped") in r.get("components_on_host_floor", []),
                    f"client dropped {cp.get('component_dropped')}, host floor {r.get('components_on_host_floor')}"):
            return 1
        if not gate("Formbook client -> host (FBOK)", cp.get("broke") == 3 and r.get("formbook_sent") and r.get("formbook_sent") == r.get("formbook_recv"),
                    f"broke {cp.get('broke')}, client hash {r.get('formbook_sent')}, host hash {r.get('formbook_recv')}"):
            return 1
    if "net-strip" in only:
        r = results["net-strip"] = run_net("net-strip", args.exe, args.out, {"APX_NET_STRIP": "1"}, args)
        emit(r)
        if not gate("S5 control APX_NET_STRIP=1 (client gets no blob)", r.get("finish") == "ok" and r.get("recv") == 42 and r.get("recv_none") == 42,
                    f"received {r.get('recv')} (40 items + the D85 bundle + the D86 component), without blob {r.get('recv_none')}, {r.get('error', '')}"):
            return 1
    if "net-vanilla" in only:
        r = results["net-vanilla"] = run_net("net-vanilla", args.exe, args.out, {"APX_PARTS": "0"}, args)
        emit(r)
        if not gate("P vanilla APX_PARTS=0 host (ITEM stays 29 bytes)", r.get("finish") == "ok" and r.get("recv") == 40 and r.get("recv_lens") == [29] and r.get("sent") == 0,
                    f"received {r.get('recv')}, lens {r.get('recv_lens')}, host blobs sent {r.get('sent')}, {r.get('error', '')}"):
            return 1
    failed = [g for g in gates if not g["pass"]]
    print(f"[f6] gates {len(gates)} failed {len(failed)} rows {jl}", flush=True)
    return 1 if failed else 0


if __name__ == "__main__":
    sys.exit(main())
