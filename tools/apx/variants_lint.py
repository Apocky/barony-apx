"""Lint for data/apx/variants/*.json (card BV1 section 3). Pure python, seconds.
    python -I tools/apx/variants_lint.py [--dir data/apx/variants]
Prints one line per violation: RULE: message. Exit 0 only when there are none.
Rules: R1 parent exists, R2 no cycle, R3 depth <= 4 and level order, R4 sibling rarity sum <= 1, R5 rare rarity <= 0.01 and
<= 2 per subtype, R6 loot resolves non-empty, R7 loot ids exist, R8 tier_shift and group size (D72), R9 traits known,
R10 material exists, R11 depth window nests, R12 stats and look sane."""
import json
import re
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
LEVELS = ["base", "type", "subtype", "rare"]
TRAITS = {"curse_on_hit", "rise_once", "brittle", "ethereal_pass", "death_burst", "ignite_on_hit", "chill_on_hit", "shock_chain",
          "whisper_aura", "bone_spurs", "fear_on_hit", "ember_trail", "frost_nova", "static_field"}


def load(d):
    nodes = {}
    for f in sorted(Path(d).glob("*.json")):
        doc = json.load(open(f))
        for n in doc["nodes"]:
            n["_file"] = f.name
            nodes.setdefault(n["id"], []).append(n)
    return nodes


def resolve_loot(nodes, nid, seen=None):
    """The node's table: the parent's resolved table (via loot.extends), reweighted, plus the node's additions."""
    seen = seen or set()
    if nid in seen or nid not in nodes:
        return None
    seen = seen | {nid}
    n = nodes[nid][0]
    loot = n.get("loot", {})
    if "table" in loot:
        return {k: v for k, v in loot["table"]}
    base = resolve_loot(nodes, loot.get("extends"), seen)
    if base is None:
        return None
    t = dict(base)
    for k, m in loot.get("reweight", {}).items():
        if k in t:
            t[k] = round(t[k] * m, 6)
    for k, v in loot.get("add", []):
        t[k] = max(t.get(k, 0.0), v)
    return t


def lint(d, root=ROOT):
    errs = []
    nodes = load(d)
    mats = {m["id"] for m in json.load(open(root / "data/apx/materials.json"))["materials"]}
    items = {l.strip() for l in open(root / "tests/apx/itemtypes.txt") if l.strip()}
    monsters = {l.strip() for l in open(root / "tests/apx/monstertypes.txt") if l.strip()}
    for nid, lst in nodes.items():
        if len(lst) > 1:
            errs.append("R1: duplicate id %s" % nid)
    children = {}
    for nid, lst in nodes.items():
        n = lst[0]
        lvl = n.get("level")
        if lvl not in LEVELS:
            errs.append("R3: %s has unknown level %r" % (nid, lvl))
            continue
        par = n.get("parent")
        if lvl == "base":
            if par:
                errs.append("R1: base %s must not have a parent" % nid)
            if n.get("monster") not in monsters:
                errs.append("R7: base %s monster %r not a monster type" % (nid, n.get("monster")))
        else:
            if par not in nodes:
                errs.append("R1: %s names a missing parent %r" % (nid, par))
                continue
            children.setdefault(par, []).append(n)
            if LEVELS.index(nodes[par][0].get("level", "base")) + 1 != LEVELS.index(lvl):
                errs.append("R3: %s (%s) is not one level below its parent %s (%s)" % (nid, lvl, par, nodes[par][0].get("level")))
    # R2 cycles and depth
    for nid in nodes:
        seen, cur, depth = set(), nid, 0
        while cur and cur in nodes:
            if cur in seen:
                errs.append("R2: cycle through %s" % nid)
                break
            seen.add(cur)
            depth += 1
            cur = nodes[cur][0].get("parent")
        if depth > 4:
            errs.append("R3: %s is %d deep (max 4)" % (nid, depth))
    # R4 / R5 siblings
    for par, kids in children.items():
        tot = sum(k.get("rarity", 0.0) for k in kids)
        if tot > 1.0 + 1e-9:
            errs.append("R4: children of %s sum to %.3f (> 1)" % (par, tot))
        rares = [k for k in kids if k.get("level") == "rare"]
        if len(rares) > 2:
            errs.append("R5: %s has %d rare variants (max 2)" % (par, len(rares)))
        for k in rares:
            if k.get("rarity", 1.0) > 0.01 + 1e-9:
                errs.append("R5: rare %s rarity %.4f > 0.01 of its subtype" % (k["id"], k["rarity"]))
    for nid, lst in nodes.items():
        n = lst[0]
        if n.get("level") == "base":
            lt = resolve_loot(nodes, nid)
        else:
            lt = resolve_loot(nodes, nid)
        # R6 / R7
        if not lt:
            errs.append("R6: loot of %s resolves empty (or its extends chain is broken)" % nid)
        else:
            for k, v in lt.items():
                if not (0.0 < v <= 1.0):
                    errs.append("R6: %s loot %s chance %r outside (0,1]" % (nid, k, v))
                kind, _, ident = k.partition(":")
                if kind == "mat" and ident not in mats:
                    errs.append("R7: %s loot %s: unknown material" % (nid, k))
                elif kind == "item" and ident not in items:
                    errs.append("R7: %s loot %s: unknown item type" % (nid, k))
                elif kind == "comp":
                    # D86: components are first-class items (rarity, prefix/suffix); data-only until F6's component item lands.
                    # format comp:<material>@<rarity>[+<affix word>]
                    mm = re.fullmatch(r"([a-z]+)@(common|uncommon|rare|epic|legendary)(\+[a-z]+)?", ident)
                    if not mm or mm.group(1) not in mats:
                        errs.append("R7: %s loot %s: component must be comp:<material>@<rarity>[+affix] with a known material" % (nid, k))
                elif kind not in ("mat", "item"):
                    errs.append("R7: %s loot %s: id must start mat:, item: or comp:" % (nid, k))
        if n.get("level") != "base":
            for k in n.get("loot", {}).get("reweight", {}):
                pt = resolve_loot(nodes, n["loot"].get("extends")) or {}
                if k not in pt:
                    errs.append("R6: %s reweights %s which its parent table lacks" % (nid, k))
            # R8
            ts = n.get("tier_shift", 0)
            if ts < 0 or ts > 2:
                errs.append("R8: %s tier_shift %r out of 0..2" % (nid, ts))
            if ts > 0 and n.get("max_group", 99) > 4:
                errs.append("R8: %s shifts tier but max_group %r > 4 (D72: elites only in packs of 4 or fewer)" % (nid, n.get("max_group")))
            # R9
            for t in n.get("traits", []):
                if t.split(":")[0] not in TRAITS:
                    errs.append("R9: %s unknown trait %r" % (nid, t))
            # R10
            if n.get("material") not in mats:
                errs.append("R10: %s material %r not in materials.json" % (nid, n.get("material")))
            # R11
            lo, hi = n.get("depth_min", 1), n.get("depth_max", 15)
            if not (1 <= lo <= hi <= 15):
                errs.append("R11: %s depth window %r-%r invalid" % (nid, lo, hi))
            pn = nodes.get(n.get("parent"), [{}])[0]
            if pn.get("level") != "base" and lo < pn.get("depth_min", 1):
                errs.append("R11: %s starts at depth %d before its parent (%d)" % (nid, lo, pn.get("depth_min", 1)))
            # R12
            st = n.get("stats", {})
            for k in ("hp", "dmg", "speed", "poise"):
                if not (0.2 <= st.get(k, 0) <= 4.0):
                    errs.append("R12: %s stats.%s %r out of 0.2..4" % (nid, k, st.get(k)))
            lk = n.get("look", {})
            if not re.fullmatch(r"#[0-9a-fA-F]{6}", lk.get("tint", "")) or not (0.5 <= lk.get("scale", 0) <= 2.0):
                errs.append("R12: %s look.tint/scale invalid" % nid)
            if not (0.0 < n.get("rarity", 0) <= 1.0):
                errs.append("R4: %s rarity %r outside (0,1]" % (nid, n.get("rarity")))
    return errs


def main():
    d = ROOT / "data/apx/variants"
    if "--dir" in sys.argv:
        d = Path(sys.argv[sys.argv.index("--dir") + 1])
    errs = lint(d)
    for e in errs:
        print(e)
    n = sum(len(v) for v in load(d).values())
    print("LINT: %d nodes, %d violations" % (n, len(errs)))
    return 1 if errs else 0


if __name__ == "__main__":
    sys.exit(main())
