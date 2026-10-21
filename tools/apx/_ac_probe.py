import json, glob, os

# sec_ms indices: 0 damage calc 1 rule stages 2 modHP 3 consequences 4 sweep scan
#                  5 getAttack 6 AC 7 damage table 8 AC effectiveness 9 tier
#                  10 effects 11 messages 12 enemy bar 13 sound 14 alert 15 object pass
runs = sorted(glob.glob(r"C:\Apocrypha\diagnostics\combat\runs\*"))
for d in runs:
    s = os.path.basename(d)
    if "gate-perf" not in s:
        continue
    try:
        summary = json.load(open(os.path.join(d, "summary.json")))
    except Exception:
        continue
    perf = summary.get("perf") or []
    pr = [r for r in perf if r.get("ticks", 0) >= 600]
    worst = max((r["p99_ms"] for r in pr), default=None)
    # aggregate sec_ms across the big windows (per-second ms sums)
    nwin = len(perf)
    agg = [0.0]*16
    for r in perf:
        sm = r.get("sec_ms") or []
        for i, v in enumerate(sm):
            if i < 16:
                agg[i] += v
    ac = agg[6] if len(agg) > 6 else 0.0
    tot = sum(agg)
    print(f"{s}  worst_p99={worst:.4f}  AC_ms/s={ac:.3f}  total_sec_ms={tot:.3f}")
