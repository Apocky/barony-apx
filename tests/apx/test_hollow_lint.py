"""tests/apx/test_hollow_lint.py -- the H1b lint and counter and their controls (card BH1b-0).
    python -I tests/apx/test_hollow_lint.py      (seconds; -I keeps the cwd off sys.path)
The real data passes; every control below mutates a copy and must FAIL with the named gate.
"""
import json
import shutil
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
DATA = ROOT / "data/apx/hollow"
LINT = ROOT / "tools/apx/hollow_lint.py"
COUNT = ROOT / "tools/apx/count_manifest.py"


def run(tool, d, *extra):
    p = subprocess.run([sys.executable, "-I", str(tool), "--dir", str(d), *extra], capture_output=True, text=True)
    return p.returncode, p.stdout


class HollowLint(unittest.TestCase):
    def setUp(self):
        self.tmp = Path(tempfile.mkdtemp())
        shutil.copytree(DATA, self.tmp / "h")
        self.d = self.tmp / "h"

    def tearDown(self):
        shutil.rmtree(self.tmp, ignore_errors=True)

    def edit(self, name, fn):
        p = self.d / name
        rows = json.loads(p.read_text(encoding="ascii"))
        rows = fn(rows) or rows
        p.write_text(json.dumps(rows), encoding="ascii")

    def expect_fail(self, gate, tool=LINT, *extra):
        code, out = run(tool, self.d, *extra)
        self.assertEqual(code, 1, out)
        if gate:
            self.assertIn(gate, out, out)
        return out

    def test_real_data_passes(self):
        code, out = run(LINT, self.d)
        self.assertEqual(code, 0, out)
        code, out = run(COUNT, self.d)
        self.assertEqual(code, 0, out)

    def test_empty_manifest_fails(self):
        for f in self.d.glob("*.json"):
            if f.name != "manifest.json":
                f.write_text("[]", encoding="ascii")
        self.expect_fail("G3")
        self.expect_fail("", COUNT)

    def test_g1_string_where_number(self):
        self.edit("wands.json", lambda r: r[0].__setitem__("tier", "two"))
        self.expect_fail("G1 wands")

    def test_g2_dangling_ref(self):
        self.edit("creatures.json", lambda r: r[0].__setitem__("kind", "NOT_A_MONSTER"))
        self.expect_fail("G2 creatures")

    def test_g3_thin(self):
        self.expect_fail("", COUNT, "--thin", "0.9")

    def test_g4_duplicate_id_across_files(self):
        self.edit("bosses.json", lambda r: r[0].__setitem__("id", "npc_smith"))
        self.edit("bosses.json", lambda r: sorted(r, key=lambda x: x["id"]))
        self.expect_fail("G4")

    def test_g5_quest_predicate_vocabulary(self):
        self.edit("quests.json", lambda r: r[0]["steps"].append({"pred": "teleport", "args": ["x"]}))
        self.expect_fail("G1 quests")

    def test_g5_secret_room_must_be_an_apxgen_program(self):
        def f(r):
            next(x for x in r if x["kind"] == "room")["ref"] = "no-such-room"
        self.edit("secrets.json", f)
        self.expect_fail("G5 secrets")

    def test_g6_species(self):
        self.edit("ecology.json", lambda r: r[0].__setitem__("species", "human"))
        self.expect_fail("G1 ecology")

    def test_g6_banned_token(self):
        self.edit("history.json", lambda r: r[0].__setitem__("text", r[0]["text"][:200] + " torture"))
        self.expect_fail("G6 history")

    def test_g7_merchant_buys_other_trade(self):
        self.edit("merchants.json", lambda r: r[0].__setitem__("buys", "fence"))
        self.expect_fail("G7 merchants")

    def test_g7_finished_product_on_sale(self):
        self.edit("merchants.json", lambda r: r[0]["sells"].append({"kind": "material", "ref": "IRON_SWORD"}))
        self.expect_fail("G7 merchants")

    def test_g8_asymmetric_stance(self):
        def f(r):
            for x in r:
                if x["id"] == "garrison":
                    x["stance"]["cult"] = 1
        self.edit("factions.json", f)
        self.expect_fail("G8 factions")


if __name__ == "__main__":
    unittest.main()
