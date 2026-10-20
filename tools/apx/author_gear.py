#!/usr/bin/env python3
"""tools/apx/author_gear.py -- source of truth for the authored F6 gear content (D78).

Edit the tables here, then run:  python tools/apx/author_gear.py
It APPENDS to data/apx/materials.json and data/apx/forms.json (existing rows are never reordered or changed)
and writes data/apx/kinds.json, recipes.json, formbook.json, salvage_materials.json. ASCII, sorted keys.
Check with: python tests/apx/test_gear_content.py   and   build-tests\\Release\\parts_test.exe data\\apx tests\\apx\\itemtypes.txt
All numbers are ASSUMED (tuned later); densities, melt points are real values.
"""
import json
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
D = ROOT / "data" / "apx"

# ---- materials: appended rows (id, name, density kg/m3, hardness, tier, value_per_kg, melt_c, flammable, element, salvage)
NEW_MATERIALS = [
    ("tin", "Tin", 7300, 2, 1, 6, 232, 0, 0, "metal"),
    ("lead", "Lead", 11340, 1, 1, 3, 327, 0, 3, "metal"),
    ("pewter", "Pewter", 7280, 2, 2, 12, 230, 0, 0, "metal"),
    ("brass", "Brass", 8500, 3, 2, 11, 930, 0, 0, "metal"),
    ("blackiron", "Blackiron", 7900, 8, 6, 60, 1600, 0, 7, "metal"),
    ("ash", "Ash", 690, 3, 2, 3, 0, 3, 6, "none"),
    ("yew", "Yew", 670, 3, 3, 6, 0, 3, 6, "none"),
    ("flint", "Flint", 2600, 7, 1, 2, 0, 0, 3, "none"),
    ("quartz", "Quartz", 2650, 7, 3, 12, 1700, 0, 5, "magic"),
    ("chitin", "Chitin", 1300, 4, 3, 14, 0, 1, 3, "none"),
    ("horn", "Horn", 1300, 3, 2, 6, 0, 1, 6, "none"),
    ("ivory", "Ivory", 1850, 3, 3, 40, 0, 1, 6, "none"),
    ("scale", "Scale", 1500, 5, 4, 25, 0, 0, 3, "none"),
    ("sinew", "Sinew", 1100, 1, 1, 5, 0, 2, 6, "none"),
    ("linen", "Linen", 450, 1, 1, 4, 0, 3, 0, "none"),
    ("wool", "Wool", 350, 1, 1, 3, 0, 3, 0, "none"),
    ("silk", "Silk", 300, 1, 3, 40, 0, 3, 5, "none"),
]

# ---- D66 damage-kind properties per material (0-100): resistance as a target/armour layer to the three kinds,
# brittle = shatter share under impact, edge = how well a strike part holds an edge, poise = armour-mass poise weight (D75).
KINDS = {  # crush, slash, pierce, brittle, edge, poise
    "stone": (30, 50, 45, 40, 10, 60), "granite": (35, 60, 55, 45, 10, 70), "obsidian": (10, 40, 35, 80, 95, 50),
    "clay": (15, 20, 20, 50, 0, 30), "brick": (25, 45, 40, 35, 0, 55), "iron": (45, 70, 65, 5, 60, 70),
    "steel": (55, 85, 80, 3, 90, 72), "bronze": (45, 60, 60, 5, 55, 72), "copper": (35, 45, 45, 3, 40, 75),
    "silver": (30, 40, 40, 3, 45, 85), "gold": (20, 30, 30, 2, 25, 100), "wood": (30, 25, 20, 10, 5, 25),
    "oak": (40, 35, 30, 8, 10, 30), "bone": (20, 45, 40, 60, 35, 35), "hide": (35, 40, 25, 0, 5, 15),
    "cloth": (10, 15, 10, 0, 0, 5), "glass": (5, 35, 30, 90, 85, 45), "ice": (10, 25, 25, 85, 50, 25),
    "salt": (5, 15, 15, 60, 0, 35), "sulfur": (5, 10, 10, 50, 0, 35), "coal": (10, 15, 15, 45, 0, 20),
    "aethite": (65, 95, 90, 5, 100, 50), "flesh": (20, 15, 10, 0, 0, 15), "water": (0, 0, 0, 0, 0, 10),
    "tin": (25, 35, 30, 5, 25, 60), "lead": (40, 25, 35, 2, 5, 95), "pewter": (30, 40, 38, 4, 30, 68),
    "brass": (40, 55, 55, 4, 45, 70), "blackiron": (60, 90, 85, 4, 95, 74), "ash": (35, 28, 22, 8, 8, 27),
    "yew": (35, 30, 25, 8, 10, 28), "flint": (10, 45, 40, 75, 90, 45), "quartz": (15, 50, 45, 80, 80, 50),
    "chitin": (40, 60, 50, 25, 40, 25), "horn": (35, 45, 40, 15, 35, 30), "ivory": (30, 50, 45, 30, 55, 40),
    "scale": (45, 65, 60, 10, 35, 35), "sinew": (25, 20, 15, 0, 0, 8), "linen": (10, 15, 10, 0, 0, 5),
    "wool": (20, 20, 12, 0, 0, 8), "silk": (8, 20, 8, 0, 0, 3),
}
# kinds a family's moves deliver, share in percent (04 section 3.1; sums to 100). Armour-like families deliver none.
FAMILY_KINDS = {
    "Blades": (0, 70, 30), "Daggers": (0, 25, 75), "Hafts": (45, 55, 0), "Polearms": (5, 35, 60), "Staves": (85, 5, 10),
    "Fists": (90, 0, 10), "Bows": (0, 0, 100), "Crossbows": (0, 0, 100), "Thrown": (0, 20, 80), "Tools": (60, 20, 20),
    "Shields": (100, 0, 0),
}  # order: crush, slash, pierce

# ---- forms.json additions: new slots and the 8 new families (17 in all, 05 section 3)
NEW_SLOTS = [  # id, role, volume cm^3
    ("Point", "strike", 80), ("Langet", "other", 60), ("Ferrule", "other", 30), ("Cap", "other", 40),
    ("Stock", "structural", 500), ("Prod", "strike", 350), ("Trigger", "other", 15), ("Tip", "strike", 20),
    ("Cord", "grip", 10), ("Shell", "structural", 700), ("Liner", "other", 150), ("Crest", "balance", 30),
    ("Nasal", "other", 20), ("Upper", "structural", 200), ("Sole", "structural", 150), ("Palm", "structural", 80),
    ("Cuff", "grip", 60),
]
NEW_FAMILIES = [  # id, required, optional[(slot, q)]
    ("Daggers", ["Point", "Grip"], [("Guard", 3), ("Pommel", 4), ("Edge", 5), ("Fuller", 8), ("Graft", 9)]),
    ("Polearms", ["Head", "Shaft", "Binding"], [("Langet", 3), ("Spike", 4), ("Grip", 5), ("Graft", 8)]),
    ("Staves", ["Shaft", "Binding"], [("Cap", 2), ("Ferrule", 3), ("Grip", 4), ("Graft", 8)]),
    ("Crossbows", ["Stock", "Prod", "String"], [("Trigger", 2), ("Nock", 4), ("Riser", 5), ("Graft", 8)]),
    ("Thrown", ["Tip", "Fletching"], [("Cord", 4), ("Graft", 8)]),
    ("Helms", ["Shell", "Liner"], [("Nasal", 2), ("Visor", 3), ("Crest", 4), ("Graft", 8)]),
    ("Boots", ["Upper", "Sole"], [("Strap", 3), ("Trim", 4), ("Plate", 5), ("Graft", 8)]),
    ("Gloves", ["Palm", "Cuff"], [("Plate", 3), ("Knuckle", 4), ("Graft", 8)]),
]


# ---- affix pools for the new slots (appended; existing pools and affix order never change). (name, pct)
NEW_POOLS = {
    "Point": [("piercing", 12), ("needle", 8), ("slip", 6), ("poise", 5), ("anchor", 4)],
    "Shaft": [("reach", 10), ("straight", 8), ("spring", 6), ("flex", 5), ("long", 7)],
    "Prod": [("draw weight", 10), ("snap", 8), ("range", 10), ("steady", 6), ("quiet", 5)],
    "Stock": [("steady aim", 8), ("recoil", 6), ("light", 6), ("sturdy", 8)],
    "Shell": [("deflection", 10), ("lightness", 8), ("bulwark", 12), ("padded", 6), ("sheen", 4)],
    "Liner": [("cushion", 10), ("warmth", 8), ("breathing", 6), ("absorb", 10)],
    "Upper": [("grip", 8), ("quiet step", 8), ("warmth", 6), ("water-shedding", 6)],
    "Sole": [("traction", 10), ("quiet step", 8), ("spring", 6), ("long wear", 8)],
    "Palm": [("sure grip", 8), ("quick hands", 6), ("padding", 6), ("feel", 5)],
    "Cuff": [("seal", 6), ("wrap", 5), ("guard", 8), ("flex", 5)],
    "Guard": [("catch", 8), ("deflect", 10), ("balance", 6), ("wide", 5)],
    "Pommel": [("counterweight", 8), ("bash", 6), ("balance", 8), ("charm", 3)],
    "String": [("tight", 8), ("silent", 6), ("durable", 8), ("fast", 6)],
    "Visor": [("clear sight", 8), ("deflection", 8), ("hush", 4), ("shade", 5)],
}

# ---- recipes: per family (bench, form_needed, main materials, secondary materials); slot lists derive from forms.json
RECIPE_FAMILIES = {
    "Blades": ("forge", 1, ["iron", "steel", "bronze", "copper", "blackiron"], ["wood", "oak", "hide", "cloth"]),
    "Daggers": ("forge", 1, ["iron", "steel", "bronze", "flint", "bone"], ["wood", "hide", "cloth"]),
    "Hafts": ("forge", 2, ["iron", "steel", "bronze", "stone", "flint", "bone"], ["wood", "oak", "ash", "hide", "sinew"]),
    "Polearms": ("forge", 2, ["iron", "steel", "bronze", "flint"], ["wood", "ash", "yew", "hide", "sinew"]),
    "Staves": ("workbench", 1, ["wood", "oak", "ash", "yew"], ["hide", "sinew", "linen", "iron", "bronze"]),
    "Fists": ("forge", 2, ["iron", "steel", "bronze", "brass"], ["hide", "cloth"]),
    "Bows": ("workbench", 2, ["yew", "ash", "oak", "wood", "horn"], ["sinew", "linen", "silk", "hide"]),
    "Crossbows": ("workbench", 3, ["oak", "ash", "yew", "iron", "steel"], ["sinew", "linen", "hide"]),
    "Thrown": ("forge", 1, ["iron", "steel", "blackiron", "flint", "bone"], ["wood", "linen", "hide"]),
    "Armour": ("forge", 2, ["iron", "steel", "bronze", "blackiron", "scale", "chitin", "bone"], ["cloth", "wool", "linen", "hide"]),
    "Helms": ("forge", 2, ["iron", "steel", "bronze", "blackiron", "bone", "hide"], ["cloth", "wool", "linen", "hide"]),
    "Boots": ("workbench", 1, ["hide", "scale", "iron"], ["hide", "cloth", "wool"]),
    "Gloves": ("workbench", 1, ["hide", "cloth", "iron", "steel"], ["hide", "cloth", "linen"]),
    "Cloth": ("loom", 1, ["cloth", "linen", "wool", "silk", "hide"], ["bronze", "brass", "iron", "bone", "horn"]),
    "Shields": ("forge", 2, ["wood", "oak", "iron", "steel", "bronze", "bone"], ["hide", "cloth", "iron"]),
    "Tools": ("any", 1, ["iron", "bronze", "brass", "wood"], ["wood", "hide", "sinew"]),
}
RECIPE_MAX_Q = 3   # optional slots with q <= 3 are part of the base recipe; higher ones are grafted/added by quality

# ---- starting Formbook (vanilla ItemType names). "default" is known by everyone; origins add to it (D58).
FORMBOOK = {
    "default": {"known": ["IRON_DAGGER", "LEATHER_BOOTS", "QUARTERSTAFF", "SLING", "TOOL_TORCH", "TUNIC", "WOODEN_SHIELD"], "mastered": []},
    "origins": {
        "barbarian": {"known": ["IRON_AXE", "IRON_MACE", "LEATHER_BREASTPIECE"], "mastered": ["IRON_AXE"]},
        "warrior": {"known": ["IRON_BREASTPIECE", "IRON_SHIELD", "IRON_SPEAR", "IRON_SWORD"], "mastered": ["IRON_SWORD"]},
        "healer": {"known": ["TOOL_ALEMBIC", "TUNIC"], "mastered": ["TOOL_ALEMBIC"]},
        "rogue": {"known": ["SHORTBOW", "TOOL_LOCKPICK"], "mastered": ["IRON_DAGGER"]},
        "wanderer": {"known": ["LEATHER_HELM", "SHORTBOW"], "mastered": ["SHORTBOW"]},
        "cleric": {"known": ["BRONZE_MACE", "BRONZE_SHIELD"], "mastered": ["BRONZE_MACE"]},
        "merchant": {"known": ["TOOL_LANTERN", "TOOL_TINKERING_KIT"], "mastered": ["TOOL_TINKERING_KIT"]},
        "wizard": {"known": ["HAT_WIZARD", "WIZARD_DOUBLET"], "mastered": ["QUARTERSTAFF"]},
        "hunter": {"known": ["CROSSBOW", "LONGBOW"], "mastered": ["LONGBOW"]},
    },
}

# ---- salvage yields per material category: (unit, grams_per_unit, break_loss_pct)
SALVAGE_CATS = {
    "metal": ("ingot", 400, 0), "wood": ("plank", 500, 0), "hide": ("pelt", 300, 0), "fibre": ("bolt", 200, 0),
    "bone": ("bone", 150, 10), "stone": ("rubble", 600, 20), "shard": ("shards", 150, 30), "other": ("lump", 250, 10),
}
SALVAGE_OF = {
    "metal": ["iron", "steel", "bronze", "copper", "silver", "gold", "tin", "lead", "pewter", "brass", "blackiron", "aethite"],
    "wood": ["wood", "oak", "ash", "yew"], "hide": ["hide", "scale", "flesh"], "fibre": ["cloth", "linen", "wool", "silk", "sinew"],
    "bone": ["bone", "horn", "ivory", "chitin"], "stone": ["stone", "granite", "brick", "clay"],
    "shard": ["glass", "obsidian", "flint", "quartz", "ice"], "other": ["salt", "sulfur", "coal", "water"],
}

# ---- D86: affix metadata. Every affix gets pos (prefix|suffix), word (shown in the component name), stat (id below) and weight.
STATS = [  # id, unit, note
    ("dmg", "pct", "damage dealt"), ("speed", "pct", "attack and move speed"), ("reach", "pct", "weapon reach"), ("bleed", "pct", "chance to cause bleeding"),
    ("stagger", "pct", "poise damage dealt"), ("stun", "pct", "chance to stun on a heavy hit"), ("edge", "pct", "edge holding"),
    ("durability", "pct", "integrity lost per use, less"), ("mass", "pct", "mass, less"), ("block", "pct", "damage blocked"), ("armor", "pct", "armour value"),
    ("poise", "pct", "poise gained"), ("pierce_armor", "pct", "armour ignored"), ("resist_crush", "pct", "crushing resistance"), ("resist_slash", "pct", "slashing resistance"),
    ("resist_pierce", "pct", "piercing resistance"), ("resist_fire", "pct", "fire resistance"), ("resist_frost", "pct", "frost resistance"), ("resist_storm", "pct", "storm resistance"),
    ("resist_arcane", "pct", "arcane resistance"), ("resist_death", "pct", "death resistance"), ("resist_life", "pct", "life resistance"),
    ("dmg_fire", "pct", "fire damage added"), ("dmg_frost", "pct", "frost damage added"), ("dmg_storm", "pct", "storm damage added"),
    ("dmg_arcane", "pct", "arcane damage added"), ("dmg_death", "pct", "death damage added"), ("dmg_life", "pct", "life damage added"),
    ("stamina", "pct", "stamina cost, less"), ("range", "pct", "ranged distance"), ("accuracy", "pct", "aim accuracy"), ("quiet", "pct", "noise made, less"),
    ("light", "pct", "light radius or glint"), ("regen_hp", "pct", "health regeneration"), ("regen_mp", "pct", "mana regeneration"), ("luck", "pct", "luck"),
    ("grip", "pct", "resistance to being disarmed"), ("carry", "pct", "carry mass"), ("trade", "pct", "barter prices"), ("sockets", "count", "extra sockets"),
]
ELEMENTAL_PCT = {"dmg_": 12, "resist_": 15}


def load_affix_meta():
    meta = {}
    for line in (ROOT / "tools" / "apx" / "affix_meta.txt").read_text(encoding="ascii").splitlines():
        line = line.strip()
        if not line or line.startswith("#"):
            continue
        pool, name, pos, word, stat = [x.strip() for x in line.split("|")]
        meta[(pool, name)] = (pos, word, stat)
    return meta


def dump(path, obj):
    path.write_text(json.dumps(obj, indent=1, sort_keys=True, ensure_ascii=True) + "\n", encoding="ascii")


def main():
    # materials: append only
    mats = json.loads((D / "materials.json").read_text())
    have = {m["id"] for m in mats["materials"]}
    for (i, n, den, har, tier, vpk, melt, flam, el, salv) in NEW_MATERIALS:
        if i not in have:
            mats["materials"].append({"id": i, "name": n, "density": den, "hardness": har, "tier": tier, "value_per_kg": vpk,
                                      "melt_c": melt, "flammable": flam, "element": el, "salvage": salv})
    dump(D / "materials.json", mats)
    ids = [m["id"] for m in mats["materials"]]
    density = {m["id"]: m["density"] for m in mats["materials"]}

    # forms: append only
    forms = json.loads((D / "forms.json").read_text())
    sl = {s["id"] for s in forms["slots"]}
    for (i, role, vol) in NEW_SLOTS:
        if i not in sl:
            forms["slots"].append({"id": i, "role": role, "volume": vol})
    fam = {f["id"] for f in forms["families"]}
    for (i, req, opt) in NEW_FAMILIES:
        if i not in fam:
            forms["families"].append({"id": i, "required": req, "optional": [{"slot": s, "q": q} for (s, q) in opt]})
    dump(D / "forms.json", forms)
    slot_vol = {s["id"]: s["volume"] for s in forms["slots"]}
    slot_role = {s["id"]: s["role"] for s in forms["slots"]}

    # affixes: append pools for the new slots
    aff = json.loads((D / "affixes.json").read_text())
    have_p = {p["id"] for p in aff["pools"]}
    for pid, lst in NEW_POOLS.items():
        if pid not in have_p:
            aff["pools"].append({"id": pid, "affixes": [{"name": n, "pct": pc} for (n, pc) in lst]})
    # D86 metadata on every entry (keys only; ids and order untouched), plus the appended "elemental" pool and the stats catalog
    meta = load_affix_meta()
    if "elemental" not in {p["id"] for p in aff["pools"]}:
        ents = []
        for (pool, name), (pos, word, stat) in meta.items():
            if pool == "elemental":
                ents.append({"name": name, "pct": ELEMENTAL_PCT["dmg_" if stat.startswith("dmg_") else "resist_"]})
        aff["pools"].append({"id": "elemental", "affixes": ents})
    for pool in aff["pools"]:
        for a in pool["affixes"]:
            m = meta.get((pool["id"], a["name"]))
            if m is None:
                raise SystemExit("affix_meta.txt has no line for %s | %s" % (pool["id"], a["name"]))
            a["pos"], a["word"], a["stat"] = m
            a["weight"] = 15 if pool["id"] == "elemental" else max(5, 100 - 6 * a["pct"])
    aff["stats"] = [{"id": i, "unit": u, "note": n} for (i, u, n) in STATS]
    dump(D / "affixes.json", aff)

    # kinds
    kinds = {"_note": "D66 damage kinds (04 section 3.2). materials: resistance to crushing/slashing/piercing as a target or armour layer 0-100; "
                      "brittle = shatter share under impact; edge = edge holding as a strike part; poise = armour-mass poise weight (D75). "
                      "families: percent of a move's damage delivered as crush/slash/pierce (sum 100). ASSUMED, tuned in F4.",
             "materials": {m: {"crush": v[0], "slash": v[1], "pierce": v[2], "brittle": v[3], "edge": v[4], "poise": v[5]} for m, v in KINDS.items()},
             "families": {f: {"crush": v[0], "slash": v[1], "pierce": v[2]} for f, v in FAMILY_KINDS.items()}}
    dump(D / "kinds.json", kinds)

    # recipes
    recipes = []
    for f in forms["families"]:
        if f["id"] not in RECIPE_FAMILIES:
            continue
        bench, need, main, sec = RECIPE_FAMILIES[f["id"]]
        slots = {}
        names = list(f["required"]) + [o["slot"] for o in f["optional"] if o["q"] <= RECIPE_MAX_Q and o["slot"] != "Graft"]
        for s in names:
            pool = main if slot_role[s] in ("strike", "structural") else sec
            g = max(5, int(round(slot_vol[s] * density[pool[0]] / 1000.0 / 50.0)) * 50) if slot_vol[s] * density[pool[0]] / 1000.0 >= 25 else max(5, int(slot_vol[s] * density[pool[0]] / 1000.0))
            slots[s] = {"grams": g, "materials": pool}
        recipes.append({"family": f["id"], "slots": slots, "bench": bench, "form_needed": need})
    dump(D / "recipes.json", {"_note": "Starting crafting recipes (F6-C). grams = slot volume x density of the first listed material, rounded; "
                                       "form_needed = Formbook successes required. Optional slots above q3 and Graft are added by quality, not by recipe.",
                              "recipes": recipes})

    dump(D / "formbook.json", {"_note": "Starting Formbook (05 section 5). Forms are vanilla ItemType names (form_map.json keys). known = successes 1, mastered = 5. "
                                        "default is known by every character; origins (D58) add to it.", **FORMBOOK})

    sal = {}
    for cat, mlist in SALVAGE_OF.items():
        unit, grams, loss = SALVAGE_CATS[cat]
        for m in mlist:
            sal[m] = {"unit": unit, "grams_per_unit": grams, "break_loss_pct": loss}
    dump(D / "salvage_materials.json", {"_note": "What a salvaged part of each material returns (05 section 5, salvage.json percentages apply on top): "
                                               "unit name, grams per unit, extra loss when the part is broken (brittle materials).", "materials": sal})
    print("materials", len(ids), "families", len(forms["families"]), "slots", len(forms["slots"]), "recipes", len(recipes))


if __name__ == "__main__":
    main()
