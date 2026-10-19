"""hollow_lint.py -- the H1b content lint over data/apx/hollow (card BH1b gates H1).

    python -I tools/apx/hollow_lint.py [--dir DIR]      (APX_HOLLOW_DIR also sets DIR)
Prints every failure as "G<n> <class> <id>: why" and exits 1 on any. Gates:
  G1 schema   types, enums, ranges, ASCII, id regex (hollow_schema.py is the only schema)
  G2 refs     every ref resolves (manifest ids, tests/apx/itemtypes.txt, monstertypes.txt,
              apxgen_room_programs.txt); refs into a STUB class are DEFERRED (counted, not failed)
  G3 counts   rows at or above min, sub-counts exact (count_manifest.py prints the table)
  G4 unique   ids unique across all files
  G5 coverage quest predicates in the vocabulary; every secret resolves by kind; creatures 8 per
              kind over 16 kinds (2 fodder, 3 regular, 2 elite, 1 champion); one quest per kind;
              NPC roles exactly the 14; history rows fill the 12 templates 5 times each
  G6 safety   animal species allowed; no banned token (tests/apx/banned_tokens.txt) in any text
  G7 trade    D73: a merchant buys only its own trade; sells materials, components, reagents,
              services and provisions only (no finished weapon or armour); at least one service
  G8 stance   faction stance complete, symmetric, self +2
"""
import argparse
import json
import os
import re
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
import hollow_schema as S  # noqa: E402

ROOT = Path(__file__).resolve().parents[2]
ID_RE = re.compile(r"^[a-z0-9_]{3,40}$")
PROVISIONS = re.compile(r"^(FOOD_[A-Z_]+|TOOL_TORCH|TOOL_LANTERN)$")
FINISHED = re.compile(r"(SWORD|AXE|MACE|SPEAR|HALBERD|TRIDENT|QUARTERSTAFF|SHIELD|HELM|HAT|HOOD|BOOTS|GLOVES|GAUNTLETS|BRACERS|BREASTPIECE|MAIL|ARMOR|CLOAK|BOW|CROSSBOW|RING_|AMULET_)")


def load_lines(rel):
    p = ROOT / rel
    return set(p.read_text(encoding="ascii").split()) if p.exists() else set()


class Lint:
    def __init__(self, data_dir):
        self.dir = Path(data_dir)
        self.fails = []
        self.deferred = 0
        self.manifest = json.loads((self.dir / "manifest.json").read_text(encoding="ascii"))
        self.rows = {}
        for c in self.manifest["classes"]:
            stem = c["file"][:-5]
            path = self.dir / c["file"]
            try:
                self.rows[stem] = json.loads(path.read_text(encoding="ascii")) if path.exists() else []
            except (ValueError, UnicodeDecodeError) as e:
                self.fail("G1", stem, "-", f"not ASCII JSON: {e}")
                self.rows[stem] = []
        self.ids = {stem: {r.get("id") for r in rows if isinstance(r, dict)} for stem, rows in self.rows.items()}
        self.itemtypes = load_lines("tests/apx/itemtypes.txt")
        self.monsters = load_lines("tests/apx/monstertypes.txt")
        self.rooms = load_lines("tests/apx/apxgen_room_programs.txt")
        self.banned = [t for t in load_lines(S.BANNED_TOKENS_FILE)]

    def fail(self, g, cls, rid, why):
        self.fails.append(f"{g} {cls} {rid}: {why}")

    def stub(self, cls):
        return S.CLASSES.get(cls, {}).get("stub")

    # ---------------------------------------------------------------- G1 + G2
    def ref_ok(self, value, targets):
        for t in targets:
            if t == "itemtype" and value in self.itemtypes:
                return True
            if t == "monster" and value in self.monsters:
                return True
            if t in self.ids and value in self.ids[t]:
                return True
        for t in targets:
            if t in S.CLASSES and self.stub(t):
                self.deferred += 1
                return True
            if t in ("aspect", "material", "liquid"):  # BF6 / BF7 lists, not merged
                self.deferred += 1
                return True
        return False

    def check(self, cls, rid, name, spec, v):
        kind = spec[0]
        where = f"{name}={v!r}"
        if kind == "id":
            if not isinstance(v, str) or not ID_RE.match(v):
                self.fail("G1", cls, rid, f"bad id {where}")
        elif kind == "str":
            lo = spec[2] if len(spec) > 2 else 0
            if not isinstance(v, str) or not v.isascii() or len(v) > spec[1] or len(v) < lo:
                self.fail("G1", cls, rid, f"{name}: ASCII string of {lo}-{spec[1]} chars wanted, got {v!r}")
        elif kind == "int":
            if not isinstance(v, int) or isinstance(v, bool) or not spec[1] <= v <= spec[2]:
                self.fail("G1", cls, rid, f"{name}: int {spec[1]}..{spec[2]} wanted, got {v!r}")
        elif kind == "enum":
            if v not in spec[1]:
                self.fail("G1", cls, rid, f"{where} not in {spec[1]}")
        elif kind == "ref":
            if not isinstance(v, str) or not self.ref_ok(v, spec[1]):
                self.fail("G2", cls, rid, f"{where} does not resolve in {spec[1]}")
        elif kind == "list":
            if not isinstance(v, list) or not spec[2] <= len(v) <= spec[3]:
                self.fail("G1", cls, rid, f"{name}: list of {spec[2]}-{spec[3]} wanted, got {v!r}")
                return
            for e in v:
                self.check(cls, rid, name, spec[1], e)
        elif kind == "obj":
            if not isinstance(v, dict):
                self.fail("G1", cls, rid, f"{name}: object wanted")
                return
            for k, s2 in spec[1].items():
                if k not in v:
                    self.fail("G1", cls, rid, f"{name}.{k} missing")
                else:
                    self.check(cls, rid, f"{name}.{k}", s2, v[k])
            for k in v:
                if k not in spec[1]:
                    self.fail("G1", cls, rid, f"{name}.{k} not in the schema")
        elif kind == "map":
            if not isinstance(v, dict):
                self.fail("G1", cls, rid, f"{name}: map wanted")
                return
            for k, x in v.items():
                self.check(cls, rid, f"{name} key", spec[1], k)
                self.check(cls, rid, f"{name}[{k}]", spec[2], x)

    def schema(self):
        for cls, rows in self.rows.items():
            sch = S.CLASSES.get(cls)
            if sch is None:
                self.fail("G1", cls, "-", "class not in hollow_schema.py")
                continue
            if self.stub(cls):
                continue
            if not isinstance(rows, list):
                self.fail("G1", cls, "-", "file is not a JSON array")
                continue
            ids = [r.get("id") for r in rows if isinstance(r, dict)]
            if ids != sorted(ids, key=str):
                self.fail("G1", cls, "-", "rows not sorted by id")
            for r in rows:
                if not isinstance(r, dict):
                    self.fail("G1", cls, "-", "row is not an object")
                    continue
                rid = r.get("id", "?")
                for k, spec in sch["fields"].items():
                    if k not in r:
                        self.fail("G1", cls, rid, f"{k} missing")
                    else:
                        self.check(cls, rid, k, spec, r[k])
                for k in r:
                    if k not in sch["fields"]:
                        self.fail("G1", cls, rid, f"{k} not in the schema")

    # ---------------------------------------------------------------- G3 G4
    def counts(self):
        for c in self.manifest["classes"]:
            stem = c["file"][:-5]
            if self.stub(stem):
                continue
            rows = self.rows.get(stem, [])
            if len(rows) < c["min"]:
                self.fail("G3", stem, "-", f"{len(rows)} rows under min {c['min']}")
            for field, want in S.CLASSES.get(stem, {}).get("sub", {}).items():
                got = {}
                for r in rows:
                    if isinstance(r, dict):
                        got[r.get(field)] = got.get(r.get(field), 0) + 1
                for k, n in want.items():
                    if got.get(k, 0) != n:
                        self.fail("G3", stem, "-", f"sub-count {field}={k}: {got.get(k, 0)}, want {n}")

    def unique(self):
        seen = {}
        for cls, rows in self.rows.items():
            if self.stub(cls) or cls == "weather":
                continue
            for r in rows if isinstance(rows, list) else []:
                rid = r.get("id") if isinstance(r, dict) else None
                if rid in seen:
                    self.fail("G4", cls, rid, f"id also in {seen[rid]}")
                seen[rid] = cls

    # ---------------------------------------------------------------- G5 G6 G7 G8
    def coverage(self):
        R = self.rows
        cr = R.get("creatures", [])
        kinds = {}
        for r in cr:
            kinds.setdefault(r.get("kind"), []).append(r.get("tier"))
        if cr:
            if len(kinds) != 16:
                self.fail("G5", "creatures", "-", f"{len(kinds)} kinds, want 16")
            for k, tiers in kinds.items():
                want = {"fodder": 2, "regular": 3, "elite": 2, "champion": 1}
                got = {t: tiers.count(t) for t in want}
                if got != want:
                    self.fail("G5", "creatures", k, f"tiers {got}, want {want}")
        q = R.get("quests", [])
        if q and sorted(r.get("kind") for r in q) != sorted(S.CLASSES["quests"]["fields"]["kind"][1]):
            self.fail("G5", "quests", "-", "not one quest per kind")
        npcs = R.get("npcs", [])
        if npcs and sorted(r.get("role") for r in npcs) != sorted(S.NPC_ROLES):
            self.fail("G5", "npcs", "-", "roles are not exactly the 14")
        hist = R.get("history", [])
        if hist:
            per = {}
            for r in hist:
                per[r.get("template")] = per.get(r.get("template"), 0) + 1
            if sorted(per.values()) != [5] * len(R.get("history_templates", [])):
                self.fail("G5", "history", "-", f"template fills {per}, want 5 each")
        for r in R.get("secrets", []):
            ref, kind = r.get("ref", ""), r.get("kind")
            if kind == "room" and ref not in self.rooms:
                self.fail("G5", "secrets", r.get("id"), f"room ref {ref!r} is not an apx-gen room program")
            elif kind == "spell" and ref not in self.ids.get("wands", set()):
                self.fail("G5", "secrets", r.get("id"), f"spell ref {ref!r} is not a wand")
            elif kind == "recipe" and not self.ref_ok(ref, ["reactions_h1b"]):
                self.fail("G5", "secrets", r.get("id"), f"recipe ref {ref!r} unresolved")
        for r in R.get("set_pieces", []):
            if r.get("room_kind") not in self.rooms:
                self.fail("G5", "set_pieces", r.get("id"), f"room_kind {r.get('room_kind')!r} is not an apx-gen room program")

    def safety(self):
        for cls, rows in self.rows.items():
            if self.stub(cls):
                continue
            blob = json.dumps(rows).lower()
            for t in self.banned:
                if t and t in blob:
                    self.fail("G6", cls, "-", f"banned token {t!r}")
        for r in self.rows.get("ecology", []):
            if r.get("species") not in S.SPECIES + ["fungus"]:
                self.fail("G6", "ecology", r.get("id"), f"species {r.get('species')!r}")

    def trade(self):
        for r in self.rows.get("merchants", []):
            rid = r.get("id")
            if r.get("buys") != r.get("trade"):
                self.fail("G7", "merchants", rid, f"buys {r.get('buys')!r} but trades {r.get('trade')!r} (D73: own trade only)")
            sells = r.get("sells", [])
            if not any(s.get("kind") == "service" for s in sells):
                self.fail("G7", "merchants", rid, "no service (D73)")
            for s in sells:
                k, ref = s.get("kind"), s.get("ref", "")
                if k == "service" and ref not in S.SERVICES:
                    self.fail("G7", "merchants", rid, f"service {ref!r} not in {S.SERVICES}")
                if k == "provision" and not (ref in self.itemtypes and PROVISIONS.match(ref)):
                    self.fail("G7", "merchants", rid, f"provision {ref!r} is not food or light")
                if k in ("material", "component", "reagent"):
                    if ref.startswith("stub:"):
                        self.deferred += 1
                    elif ref not in self.itemtypes:
                        self.fail("G2", "merchants", rid, f"{k} {ref!r} does not resolve")
                if ref in self.itemtypes and FINISHED.search(ref):
                    self.fail("G7", "merchants", rid, f"{ref} is a finished product (D73)")

    def stance(self):
        fs = self.rows.get("factions", [])
        ids = [r.get("id") for r in fs]
        for r in fs:
            st = r.get("stance", {})
            if sorted(st) != sorted(ids):
                self.fail("G8", "factions", r.get("id"), "stance does not name every faction")
            if st.get(r.get("id")) != 2:
                self.fail("G8", "factions", r.get("id"), "self stance is not +2")
            for o in fs:
                if st.get(o.get("id")) != o.get("stance", {}).get(r.get("id")):
                    self.fail("G8", "factions", r.get("id"), f"stance with {o.get('id')} not symmetric")

    def run(self):
        self.schema(); self.counts(); self.unique(); self.coverage(); self.safety(); self.trade(); self.stance()
        return self.fails


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--dir", default=os.environ.get("APX_HOLLOW_DIR", str(ROOT / "data/apx/hollow")))
    a = ap.parse_args()
    L = Lint(a.dir)
    fails = L.run()
    for f in fails:
        print(f)
    stubs = [c for c in L.rows if L.stub(c)]
    print(f"{'FAIL' if fails else 'PASS'}: {len(fails)} failures; classes {len(L.rows)}, stubs {len(stubs)} ({', '.join(stubs)}), deferred refs {L.deferred}")
    sys.exit(1 if fails else 0)


if __name__ == "__main__":
    main()
