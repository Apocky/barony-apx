"""tests/apx/test_variants.py -- the variants lint and its controls (card BV1 V1-1). python -I tests/apx/test_variants.py
The real data passes; every control mutates a copy and must FAIL with the named rule."""
import json
import shutil
import sys
import tempfile
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / "tools/apx"))
import variants_lint as vl  # noqa: E402

SRC = ROOT / "data/apx/variants"


def mutated(fn):
    d = Path(tempfile.mkdtemp())
    for f in SRC.glob("*.json"):
        shutil.copy(f, d / f.name)
    doc = json.load(open(d / "rat.json"))
    fn(doc["nodes"])
    json.dump(doc, open(d / "rat.json", "w"))
    return vl.lint(d)


def node(nodes, nid):
    return next(n for n in nodes if n["id"] == nid)


class T(unittest.TestCase):
    def test_real_data_passes(self):
        self.assertEqual(vl.lint(SRC), [])

    def test_broken_parent_fails_R1(self):
        e = mutated(lambda ns: node(ns, "rat.undead.accursed").__setitem__("parent", "rat.nope"))
        self.assertTrue(any(x.startswith("R1:") and "rat.nope" in x for x in e), e)

    def test_empty_loot_fails_R6(self):
        def f(ns):
            n = node(ns, "rat")
            n["loot"] = {"table": []}
        e = mutated(f)
        self.assertTrue(any(x.startswith("R6:") for x in e), e)

    def test_cycle_fails_R2(self):
        e = mutated(lambda ns: node(ns, "rat.undead").__setitem__("parent", "rat.undead.accursed"))
        self.assertTrue(any(x.startswith("R2:") for x in e), e)

    def test_sibling_rarity_over_one_fails_R4(self):
        e = mutated(lambda ns: node(ns, "rat.undead").__setitem__("rarity", 0.99) or node(ns, "rat.elemental").__setitem__("rarity", 0.5))
        self.assertTrue(any(x.startswith("R4:") for x in e), e)

    def test_rare_too_common_fails_R5(self):
        e = mutated(lambda ns: node(ns, "rat.undead.accursed.gravewhisker").__setitem__("rarity", 0.2))
        self.assertTrue(any(x.startswith("R5:") for x in e), e)

    def test_unknown_item_fails_R7(self):
        def f(ns):
            node(ns, "rat.undead")["loot"]["add"].append(["item:NOT_AN_ITEM", 0.1])
        e = mutated(f)
        self.assertTrue(any(x.startswith("R7:") for x in e), e)

    def test_elite_in_big_group_fails_R8(self):
        e = mutated(lambda ns: node(ns, "rat.undead.accursed.gravewhisker").__setitem__("max_group", 12))
        self.assertTrue(any(x.startswith("R8:") for x in e), e)

    def test_too_deep_fails_R3(self):
        def f(ns):
            ns.append({"id": "rat.undead.accursed.gravewhisker.x", "parent": "rat.undead.accursed.gravewhisker", "level": "rare", "rarity": 0.01,
                       "depth_min": 6, "depth_max": 15, "material": "bone", "stats": {"hp": 1, "dmg": 1, "speed": 1, "poise": 1}, "tier_shift": 0,
                       "traits": [], "look": {"tint": "#000000", "scale": 1.0, "light": "none"}, "loot": {"extends": "rat.undead.accursed.gravewhisker"}})
        e = mutated(f)
        self.assertTrue(any(x.startswith("R3:") for x in e), e)

    def test_unknown_trait_fails_R9(self):
        def f(ns):
            node(ns, "rat.undead")["traits"].append("steal_gear:100%")
        e = mutated(f)
        self.assertTrue(any(x.startswith("R9:") for x in e), e)


if __name__ == "__main__":
    unittest.main(verbosity=1)
