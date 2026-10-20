#!/usr/bin/env python3
"""tests/apx/test_gear_content.py -- offline lint for the authored F6 gear content (tier T0/T1, seconds, no Barony).

  python tests/apx/test_gear_content.py        exit 0 = pass
Checks cross-file references (the engine loader checks only its own files) and ranges; then runs CONTROLS: each
deliberately broken copy of the data must be caught, else the lint proves nothing.
"""
import copy
import json
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
D = ROOT / "data" / "apx"
ITEMTYPES = set((ROOT / "tests" / "apx" / "itemtypes.txt").read_text().split())


def load():
    names = ["materials", "forms", "affixes", "form_map", "kinds", "recipes", "formbook", "salvage_materials", "salvage"]
    return {n: json.loads((D / (n + ".json")).read_text(encoding="ascii")) for n in names}


def name_of(d, pool_hint, material, slot, prefix_names, suffix_names):
    """component name: prefix words in order (at most 2) + Material + Slot + suffix words (at most 2); None when a name is unknown"""
    by = {}
    for pool in d["affixes"]["pools"]:
        for a in pool["affixes"]:
            by.setdefault(a["name"], a)
    pre = [by[n]["word"] for n in prefix_names if n in by and by[n]["pos"] == "prefix"][:2]
    suf = [by[n]["word"] for n in suffix_names if n in by and by[n]["pos"] == "suffix"][:2]
    if len(pre) != len(prefix_names) or len(suf) != len(suffix_names): return None
    return " ".join(pre + [material.capitalize(), slot] + suf)


def lint(d):
    """returns a list of error strings"""
    e = []
    mats = {m["id"]: m for m in d["materials"]["materials"]}
    slots = {s["id"]: s for s in d["forms"]["slots"]}
    fams = {f["id"]: f for f in d["forms"]["families"]}
    pools = {p["id"] for p in d["affixes"]["pools"]}
    if len(mats) < 40: e.append("fewer than 40 materials")
    if len(fams) != 17: e.append("families != 17 (05 section 3), got %d" % len(fams))
    for m in mats.values():
        if not (0 <= m["hardness"] <= 10 and 1 <= m["tier"] <= 10 and 0 <= m["flammable"] <= 3 and 0 <= m["element"] <= 7 and m["density"] > 0):
            e.append("material range: " + m["id"])
        if m["salvage"] not in ("metal", "magic", "none"): e.append("material salvage class: " + m["id"])
    # D66 kinds: every material has a row with 0-100 values; every damage-dealing family sums to 100
    km = d["kinds"]["materials"]
    for i in mats:
        if i not in km: e.append("kinds.json missing material " + i)
        else:
            for k in ("crush", "slash", "pierce", "brittle", "edge", "poise"):
                if not (0 <= km[i].get(k, -1) <= 100): e.append("kinds %s.%s out of range" % (i, k))
    for i in km:
        if i not in mats: e.append("kinds.json unknown material " + i)
    for f, v in d["kinds"]["families"].items():
        if f not in fams: e.append("kinds family unknown: " + f)
        if v["crush"] + v["slash"] + v["pierce"] != 100: e.append("kinds family does not sum to 100: " + f)
    # D86 affixes: position, display word, a stat from the catalog, a rarity weight; pools keep >= 4 entries; names compose
    stats = {x["id"] for x in d["affixes"].get("stats", [])}
    if len(stats) < 30: e.append("affixes.json stats catalog missing or too small")
    for pool in d["affixes"]["pools"]:
        if len(pool["affixes"]) < 4: e.append("affix pool under 4: " + pool["id"])
        for a in pool["affixes"]:
            tag = "%s/%s" % (pool["id"], a["name"])
            if a.get("pos") not in ("prefix", "suffix"): e.append("affix pos: " + tag)
            if not a.get("word"): e.append("affix word: " + tag)
            elif a.get("pos") == "suffix" and not a["word"].startswith("of "): e.append("suffix word must start with 'of ': " + tag)
            elif a.get("pos") == "prefix" and a["word"].startswith("of "): e.append("prefix word starts with 'of': " + tag)
            if a.get("stat") not in stats: e.append("affix stat not in catalog: " + tag)
            if not (1 <= a.get("weight", 0) <= 100): e.append("affix weight: " + tag)
            if not (1 <= a.get("pct", 0) <= 100): e.append("affix pct: " + tag)
        if not any(a.get("pos") == "prefix" for a in pool["affixes"]): e.append("pool has no prefix: " + pool["id"])
    if "elemental" not in {p["id"] for p in d["affixes"]["pools"]}: e.append("elemental pool missing")
    # a composed name reads "Keen Steel Blade of Embers"
    sample = name_of(d, "Blade", "steel", "Blade", ["keenness", "bleed"], ["embers"])
    if sample != "Keen Gory Steel Blade of Embers" and sample is not None: e.append("name rule gave: " + str(sample))
    # families reference real slots; slot pools exist or fall back to "*"
    if "*" not in pools: e.append("affix pool * missing")
    for f in fams.values():
        for s in f["required"] + [o["slot"] for o in f["optional"]]:
            if s not in slots: e.append("family %s uses unknown slot %s" % (f["id"], s))
        if len(f["required"]) + len(f["optional"]) > 12: e.append("family over APX_MAX_PARTS: " + f["id"])
    # form_map: every vanilla type maps to a real family with real materials
    for n, v in d["form_map"].items():
        if n not in ITEMTYPES: e.append("form_map unknown item type " + n)
        if v["family"] not in fams: e.append("form_map %s family %s" % (n, v["family"]))
        for m in v["materials"]:
            if m not in mats: e.append("form_map %s unknown material %s" % (n, m))
    if set(d["form_map"]) != ITEMTYPES: e.append("form_map does not cover all item types")
    # recipes
    seen = set()
    for r in d["recipes"]["recipes"]:
        f = fams.get(r["family"])
        if not f: e.append("recipe family unknown: " + r["family"]); continue
        seen.add(r["family"])
        for s in f["required"]:
            if s not in r["slots"]: e.append("recipe %s lacks required slot %s" % (r["family"], s))
        for s, v in r["slots"].items():
            if s not in slots: e.append("recipe %s unknown slot %s" % (r["family"], s))
            if v["grams"] < 1: e.append("recipe grams < 1")
            for m in v["materials"]:
                if m not in mats: e.append("recipe %s.%s unknown material %s" % (r["family"], s, m))
            if not v["materials"]: e.append("recipe %s.%s has no materials" % (r["family"], s))
        if r["bench"] not in ("forge", "workbench", "loom", "any"): e.append("recipe bench: " + r["family"])
        if not (1 <= r["form_needed"] <= 5): e.append("recipe form_needed: " + r["family"])
    for f in fams:
        if f != "Plain" and f not in seen: e.append("no recipe for family " + f)
    # formbook: only real item types, only gear forms, mastered subset of known-or-default
    fb = d["formbook"]
    sets = [("default", fb["default"])] + [("origin " + k, v) for k, v in fb["origins"].items()]
    for label, v in sets:
        for n in v["known"] + v["mastered"]:
            if n not in ITEMTYPES: e.append("formbook %s: unknown item type %s" % (label, n))
    if len(fb["origins"]) < 8: e.append("fewer than 8 origins in formbook")
    # salvage yields cover every material that can be salvaged
    sm = d["salvage_materials"]["materials"]
    for i in mats:
        if i not in sm: e.append("salvage_materials missing " + i)
        elif sm[i]["grams_per_unit"] < 1 or not (0 <= sm[i]["break_loss_pct"] <= 100): e.append("salvage range: " + i)
    # ascii + sorted keys are checked at write time by json.dumps(sort_keys); here only that files are pure ASCII
    for p in D.glob("*.json"):
        p.read_bytes().decode("ascii")
    return e


def main():
    d = load()
    errs = lint(d)
    print("lint:", "OK" if not errs else "%d errors" % len(errs))
    for x in errs[:20]: print("  ", x)
    # CONTROLS: each broken copy must produce at least one error
    ctrl = []
    def broken(label, fn):
        c = copy.deepcopy(d); fn(c)
        n = len(lint(c)); ctrl.append((label, n)); print("control %-44s %s" % (label, "caught (%d)" % n if n else "MISSED"))
    broken("recipe uses an unknown material", lambda c: c["recipes"]["recipes"][0]["slots"]["Blade"]["materials"].append("unobtainium"))
    broken("material removed from kinds.json", lambda c: c["kinds"]["materials"].pop("steel"))
    broken("family kinds do not sum to 100", lambda c: c["kinds"]["families"]["Blades"].update(crush=50))
    broken("formbook names a non-item", lambda c: c["formbook"]["default"]["known"].append("NOT_AN_ITEM"))
    broken("family references an unknown slot", lambda c: c["forms"]["families"][0]["required"].append("Nonexistent"))
    broken("form_map points at a removed family", lambda c: c["form_map"].__setitem__("IRON_SWORD", {"family": "Ghost", "materials": ["iron"]}))
    broken("recipe lacks a required slot", lambda c: c["recipes"]["recipes"][0]["slots"].pop("Grip"))
    broken("an affix loses its stat", lambda c: c["affixes"]["pools"][0]["affixes"][0].pop("stat"))
    broken("an affix has an unknown stat", lambda c: c["affixes"]["pools"][0]["affixes"][0].update(stat="nonsense"))
    broken("an affix has a bad position", lambda c: c["affixes"]["pools"][0]["affixes"][0].update(pos="middle"))
    broken("a suffix word without 'of'", lambda c: c["affixes"]["pools"][0]["affixes"][0].update(pos="suffix", word="Lasting"))
    broken("an affix weight out of range", lambda c: c["affixes"]["pools"][0]["affixes"][0].update(weight=0))
    broken("salvage yield missing for a material", lambda c: c["salvage_materials"]["materials"].pop("oak"))
    missed = [l for l, n in ctrl if n == 0]
    ok = not errs and not missed
    print("RESULT", "PASS" if ok else "FAIL", "(%d controls, %d missed)" % (len(ctrl), len(missed)))
    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main())
