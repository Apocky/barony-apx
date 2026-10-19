"""count_manifest.py -- H1b manifest counter (card BH1b gate H2).

    python -I tools/apx/count_manifest.py [--dir DIR] [--thin FRACTION]
Prints rows, min and PASS / FAIL / STUB per class and the sub-counts; --thin keeps that fraction
of every class (in id order) to prove the counter can fail. Exit 1 on any FAIL. STUB classes
(F6 parts gear, F7 alchemy, not merged) are reported, never passed.
"""
import argparse
import json
import os
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
import hollow_schema as S  # noqa: E402

ROOT = Path(__file__).resolve().parents[2]


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--dir", default=os.environ.get("APX_HOLLOW_DIR", str(ROOT / "data/apx/hollow")))
    ap.add_argument("--thin", type=float, default=1.0)
    a = ap.parse_args()
    d = Path(a.dir)
    man = json.loads((d / "manifest.json").read_text(encoding="ascii"))
    bad = 0
    print(f"{'class':22} {'rows':>6} {'min':>6}  result")
    for c in man["classes"]:
        stem = c["file"][:-5]
        rows = json.loads((d / c["file"]).read_text(encoding="ascii")) if (d / c["file"]).exists() else []
        rows = sorted(rows, key=lambda r: str(r.get("id")))[: int(len(rows) * a.thin)]
        stub = S.CLASSES.get(stem, {}).get("stub")
        ok = len(rows) >= c["min"]
        subs = []
        for field, want in S.CLASSES.get(stem, {}).get("sub", {}).items():
            for k, n in want.items():
                got = sum(1 for r in rows if r.get(field) == k)
                subs.append(f"{k} {got}/{n}")
                ok = ok and got == n
        res = f"STUB ({stub})" if stub else ("PASS" if ok else "FAIL")
        bad += 0 if (stub or ok) else 1
        print(f"{stem:22} {len(rows):6} {c['min']:6}  {res}{'  ' + ', '.join(subs) if subs else ''}")
    print(f"{'FAIL' if bad else 'PASS'}: {bad} classes under their minimum{' (thin ' + str(a.thin) + ')' if a.thin != 1.0 else ''}")
    sys.exit(1 if bad else 0)


if __name__ == "__main__":
    main()
