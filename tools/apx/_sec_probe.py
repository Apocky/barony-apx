import json, glob, os

labels = ["0 damage calc","1 rule stages","2 modHP","3 consequences","4 sweep scan",
          "5 getAttack","6 AC","7 damage table","8 AC effectiveness","9 tier",
          "10 effects","11 messages","12 enemy bar","13 sound","14 alert","15 object pass"]

runs = sorted(glob.glob(r"C:\Apocrypha\diagnostics\combat\runs\*"))
focus = ["20261020-161639-gate-perf-floor1",   # fail, cache on
         "20261021-172905-gate-perf-ctl-floor1", # control (typo guard)
         "20261019-234829-gate-perf-floor1"]    # old-code 0.58 spike

for d in runs:
    s = os.path.basename(d)
    if s not in focus and "gate-perf" not in s:
        continue
    if not any(f.startswith(s[:14]) for f in focus):
        continue
    try:
        summary = json.load(open(os.path.join(d, "summary.json")))
    except Exception:
        continue
    perf = [r for r in (summary.get("perf") or []) if r.get("ticks",0) >= 600]
    if not perf:
        continue
    worst = max((r["p99_ms"] for r in perf))
    # take the worst window's sec_ms
    w = max(perf, key=lambda r: r.get("p99_ms",0))
    sm = w.get("sec_ms") or []
    print(f"\n{s}  worst_p99={worst:.4f}")
    tot = sum(sm[:16])
    top = sorted([(sm[i], labels[i]) for i in range(min(16,len(sm)))], reverse=True)[:5]
    for v, lab in top:
        print(f"   {lab:20s} {v:8.3f} ms/s  ({100*v/tot:4.1f}% of tick)")
