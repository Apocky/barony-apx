"""props_lint.py -- lint for data/apx/props.json (card BP1). Exit 1 on any failure.
    python -I tools/apx/props_lint.py [--file PATH]
Checks: ids unique and ^[a-z0-9_]{3,40}$; match.behavior furniture (type -1..4) or collider (a Barony
collider name); verb in the known set; per prop the outcome chances 0-1000 summing to at most 1000;
item names resolve (tests/apx/itemtypes.txt); D72: a spawn is 1-3 creatures, tier fodder unless the
row says regular, never elite or champion; spawn kinds resolve (tests/apx/monstertypes.txt); rest
params in range; ASCII labels of at most 40 chars.
"""
import argparse
import json
import re
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
VERBS = {"rest", "sit", "bench", "read", "break", "destroyed"}
COLLIDERS = {"hanging_cage_skel", "hanging_cage_empty", "candelabra", "iron_maiden"}


def lint(path):
    fails = []
    items = set((ROOT / "tests/apx/itemtypes.txt").read_text(encoding="ascii").split())
    mons = set((ROOT / "tests/apx/monstertypes.txt").read_text(encoding="ascii").split())
    try:
        doc = json.loads(Path(path).read_text(encoding="ascii"))
    except (ValueError, UnicodeDecodeError) as e:
        return [f"not ASCII JSON: {e}"]
    seen = set()
    for p in doc.get("props", []):
        pid = p.get("id", "?")
        f = lambda m: fails.append(f"{pid}: {m}")  # noqa: E731
        if not re.fullmatch(r"[a-z0-9_]{3,40}", str(pid)) or pid in seen:
            f("bad or duplicate id")
        seen.add(pid)
        m = p.get("match", {})
        if m.get("behavior") == "furniture":
            if not isinstance(m.get("type"), int) or not -1 <= m["type"] <= 4:
                f("furniture type must be -1..4")
        elif m.get("behavior") == "collider":
            if m.get("type") not in COLLIDERS:
                f(f"collider {m.get('type')!r} not a known Barony collider")
        else:
            f(f"behavior {m.get('behavior')!r}")
        if p.get("verb") not in VERBS:
            f(f"verb {p.get('verb')!r}")
        lab = p.get("label", "")
        if not isinstance(lab, str) or not lab.isascii() or len(lab) > 40:
            f("label: ASCII, at most 40 chars")
        if p.get("verb") in ("rest", "sit", "bench", "read") and not lab:
            f("a use verb needs a label (the world prompt, D81)")
        par = p.get("params", {})
        if p.get("verb") == "rest":
            ok = 50 <= par.get("ticks", 0) <= 3000 and 1 <= par.get("heal_every", 0) <= 200 and 0 <= par.get("hunger", -1) <= 200 \
                and 0 <= par.get("encounter_pm", -1) <= 1000 and 1 <= par.get("encounter_max", 0) <= 3
            if not ok:
                f("rest params out of range (encounter_max at most 3: D72)")
        total = 0
        for o in p.get("outcomes", []):
            pm = o.get("pm")
            if not isinstance(pm, int) or not 0 <= pm <= 1000:
                f("outcome pm must be 0..1000")
                continue
            total += pm
            for it in o.get("items", []):
                if it not in items:
                    f(f"item {it!r} does not resolve")
            y = o.get("yield", "")
            if y and not re.fullmatch(r"stub:F[67]:[a-z_]+", y) and not re.fullmatch(r"pouch:[a-z_]+:[1-9][0-9]{0,5}", y) and y not in items:
                f(f"yield {y!r}")
            s = o.get("spawn")
            if s:
                if s.get("kind") not in mons:
                    f(f"spawn kind {s.get('kind')!r} does not resolve")
                if not 1 <= s.get("n", 0) <= 3:
                    f("spawn n must be 1-3 (D72)")
                if s.get("tier") not in ("fodder", "regular"):
                    f("spawn tier must be fodder or regular (D72: no elite outside a pack)")
        if total > 1000:
            f(f"outcome chances sum to {total} > 1000")
    return fails


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--file", default=str(ROOT / "data/apx/props.json"))
    a = ap.parse_args()
    fails = lint(a.file)
    for x in fails:
        print(x)
    print(f"{'FAIL' if fails else 'PASS'}: {len(fails)} failures")
    sys.exit(1 if fails else 0)


if __name__ == "__main__":
    main()
