"""gen_hollow_content.py -- writes data/apx/hollow/*.json for every H1b class that does not need BF6
(parts gear) or BF7 (alchemy); those six classes are written as empty STUB files (manifest marks
them). Deterministic: a fixed LCG (seed 20261019), no other randomness. Content is placeholder-grade
on purpose (card BH1b, Stop and ask 2: Apocky reviews names and text).

    python -I tools/apx/gen_hollow_content.py [--apxgen C:/Users/Apocky/source/repos/apx-gen]
"""
import argparse
import json
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
OUT = ROOT / "data/apx/hollow"


class LCG:
    def __init__(self, seed):
        self.s = seed & 0xFFFFFFFF

    def next(self, n):
        self.s = (1664525 * self.s + 1013904223) & 0xFFFFFFFF
        return (self.s >> 8) % n


def items():
    return (ROOT / "tests/apx/itemtypes.txt").read_text(encoding="ascii").split()


def write(name, rows, sort=True):
    if sort:
        rows = sorted(rows, key=lambda r: str(r["id"]))
    (OUT / name).write_text(json.dumps(rows, indent=1, sort_keys=True) + "\n", encoding="ascii", newline="\n")
    return len(rows)


def label(t):
    return t.split("_", 1)[1].replace("_", " ").lower() if "_" in t else t.lower()


EFFECT = {"POTION_WATER": "plain water, quenches thirst", "POTION_BOOZE": "drunk: brave and clumsy", "POTION_JUICE": "a sweet drink, small heal",
          "POTION_SICKNESS": "poisons the drinker", "POTION_CONFUSION": "confuses the drinker", "POTION_EXTRAHEALING": "heals a great deal",
          "POTION_HEALING": "heals wounds", "POTION_CUREAILMENT": "cures poison and illness", "POTION_BLINDNESS": "blinds the drinker",
          "POTION_RESTOREMAGIC": "restores magic", "POTION_INVISIBILITY": "turns the drinker unseen", "POTION_LEVITATION": "floats over pits and water",
          "POTION_SPEED": "moves and acts faster", "POTION_ACID": "burns whoever it touches", "POTION_PARALYSIS": "freezes the drinker in place",
          "POTION_POLYMORPH": "changes the drinker's form", "POTION_FIRESTORM": "bursts into flame when thrown", "POTION_ICESTORM": "bursts into frost when thrown",
          "POTION_THUNDERSTORM": "bursts into lightning when thrown", "POTION_STRENGTH": "raises strength for a while"}


def consumables():
    it = items()
    rows = []
    pots = sorted(t for t in it if t.startswith("POTION_") and t != "POTION_EMPTY")
    foods = ["FOOD_BREAD", "FOOD_CREAMPIE", "FOOD_CHEESE", "FOOD_APPLE", "FOOD_MEAT", "FOOD_FISH", "FOOD_TIN", "FOOD_TOMALLEY", "FOOD_RATION",
             "FOOD_RATION_SPICY", "FOOD_RATION_SOUR", "FOOD_RATION_BITTER"]
    scrolls = sorted(t for t in it if t.startswith("SCROLL_"))[:10]
    throw = ["TOOL_BOMB", "TOOL_SLEEP_BOMB", "TOOL_FREEZE_BOMB", "TOOL_TELEPORT_BOMB", "TOOL_DECOY", "TOOL_DETONATOR_CHARGE", "TOOL_BEARTRAP"]
    tools = ["TOOL_TORCH", "TOOL_LANTERN", "TOOL_PICKAXE", "TOOL_LOCKPICK", "TOOL_SKELETONKEY", "TOOL_MIRROR", "TOOL_TINKERING_KIT", "TOOL_ALEMBIC"]
    i = 0
    for cls, names in (("potion", pots), ("food", foods), ("scroll", scrolls), ("throwable", throw), ("tool", tools)):
        for t in names:
            eff = EFFECT.get(t) or {"food": "eaten to stave off hunger", "scroll": f"reads as {label(t)}", "throwable": f"thrown: {label(t)}",
                                    "tool": f"used as a {label(t)}"}.get(cls, f"vanilla {label(t)}")
            rows.append({"id": t.lower(), "class": cls, "base_type": t, "label": label(t), "depth": i % 6, "effect": eff[:60]})
            i += 1
    for fid, base, eff in (("flask_oil", "POTION_FIRESTORM", "thrown: spreads burning oil"), ("flask_brine", "POTION_WATER", "thrown: salt water, puts out fire"),
                           ("flask_acid", "POTION_ACID", "thrown: acid that eats metal")):
        rows.append({"id": fid, "class": "throwable", "base_type": base, "label": fid.replace("_", " "), "depth": i % 6, "effect": eff})
        i += 1
    return rows


KINDS = [("RAT", "sewer"), ("BAT_SMALL", "cave"), ("SPIDER", "cave"), ("SKELETON", "crypt"), ("GHOUL", "crypt"), ("REVENANT_SKULL", "crypt"),
         ("GOBLIN", "mine"), ("KOBOLD", "mine"), ("BUGBEAR", "mine"), ("SLIME", "sewer"), ("MYCONID", "cave"), ("CRAB", "sewer"),
         ("SALAMANDER", "cave"), ("EARTH_ELEMENTAL", "mine"), ("SHADOW", "temple"), ("HUMAN", "fortress")]
THEME_FACTION = {"fortress": "garrison", "crypt": "cult", "sewer": "cult", "temple": "cult", "mine": "warren", "cave": "warren"}
ADJ = ["rusty", "gaunt", "feral", "ashen", "hollow", "grim", "swift", "brute"]
VARIANT_TIERS = ["fodder", "fodder", "regular", "regular", "regular", "elite", "elite", "champion"]
ARMED = {"HUMAN", "SKELETON", "GOBLIN", "KOBOLD", "BUGBEAR"}


def melee():
    import re
    pat = re.compile(r"(SWORD|AXE|MACE|SPEAR|HALBERD|TRIDENT|QUARTERSTAFF)")
    return sorted(t for t in items() if pat.search(t) and not t.startswith(("TOOL_", "SPELLBOOK_")))


def creatures():
    mw = melee()
    rows = []
    for ki, (kind, theme) in enumerate(KINDS):
        for vi in range(8):
            tier = VARIANT_TIERS[vi]
            n = {"fodder": 0, "regular": 1, "elite": 2, "champion": 3}[tier] if kind in ARMED else 0
            eq = [mw[(ki + vi + j) % len(mw)] for j in range(n)]
            rows.append({"id": f"{kind.lower()}_{ADJ[vi]}", "kind": kind, "tier": tier, "faction": THEME_FACTION[theme], "theme": theme,
                         "equipment": eq, "name": f"{ADJ[vi]} {kind.lower().replace('_', ' ')}"})
    return rows


def bosses():
    return [{"id": "warden", "kind": "HUMAN", "tier": "boss", "faction": "garrison", "name": "the Warden",
             "phase_note": "stagger-heavy: below half HP calls the garrison and fights shield-first"},
            {"id": "high_cultist", "kind": "HUMAN", "tier": "boss", "faction": "cult", "name": "the High Cultist",
             "phase_note": "ranged with cover play: below half HP floods the nave sump"},
            {"id": "cave_matriarch", "kind": "SPIDER", "tier": "boss", "faction": "warren", "name": "the Cave Matriarch",
             "phase_note": "webs the exits; below half HP calls her brood from the vent"}]


FACTIONS = ["caravan", "cult", "garrison", "warren"]
STANCE = {("garrison", "cult"): -2, ("garrison", "warren"): -1, ("cult", "warren"): -1}


def factions():
    seat = {"garrison": "special-stronghold-hall", "cult": "temple-rotunda", "warren": "mine-ore-cavern", "caravan": "special-haven-shrine"}
    names = {"garrison": "Warden's Garrison", "cult": "Hollow Choir", "warren": "Deep Warren", "caravan": "Lantern Caravan"}
    rows = []
    for f in FACTIONS:
        st = {}
        for o in FACTIONS:
            if o == f:
                st[o] = 2
            elif "caravan" in (f, o):
                st[o] = 1
            else:
                st[o] = STANCE.get((f, o), STANCE.get((o, f), 0))
        rows.append({"id": f, "name": names[f], "seat_room_kind": seat[f], "stance": st})
    return rows


NPC = [("merchant", "HUMAN", "caravan"), ("smith", "HUMAN", "garrison"), ("alchemist", "HUMAN", "caravan"), ("scholar", "HUMAN", "caravan"),
       ("prisoner", "HUMAN", "garrison"), ("deserter", "HUMAN", "garrison"), ("cultist_defector", "HUMAN", "cult"), ("goblin_envoy", "GOBLIN", "warren"),
       ("hermit", "HUMAN", "caravan"), ("ghost", "REVENANT_SKULL", "cult"), ("caravan_master", "HUMAN", "caravan"), ("guard_a", "HUMAN", "garrison"),
       ("guard_b", "HUMAN", "garrison"), ("warden_prewar", "HUMAN", "garrison")]
NPC_NAMES = ["Odda the trader", "Brann the smith", "Mirel the alchemist", "Tolly the scholar", "Cass the prisoner", "Fenn the deserter",
             "Sister Ilse", "Grik the envoy", "old Haskel", "the drowned choirmaster", "Mother Vey", "Corporal Ames", "Private Dunne", "Warden Orsk (before)"]


def npcs(hist_ids):
    rows = []
    for i, (role, kind, fac) in enumerate(NPC):
        n = len(hist_ids)
        rows.append({"id": f"npc_{role}", "role": role, "kind": kind, "faction": fac, "name": NPC_NAMES[i],
                     "talk_rows": [hist_ids[(i * 2) % n], hist_ids[(i * 2 + 1) % n]]})
    return rows


def quests(hist_ids):
    return [
        {"id": "q_cure", "kind": "cure", "giver": "npc_alchemist", "reward": "POTION_EXTRAHEALING", "text": "Mirel needs a brewed cure for the fever in the camp.",
         "steps": [{"pred": "has_item", "args": ["POTION_CUREAILMENT", "1"]}, {"pred": "talked", "args": ["npc_alchemist", "cure"]}]},
        {"id": "q_key", "kind": "key", "giver": "npc_smith", "reward": "TOOL_SKELETONKEY", "text": "Brann lost the vault key somewhere on the second floor.",
         "steps": [{"pred": "flag", "args": ["vault_key_found"]}, {"pred": "gave", "args": ["npc_smith", "TOOL_SKELETONKEY"]}]},
        {"id": "q_rescue", "kind": "rescue", "giver": "npc_prisoner", "reward": "FOOD_RATION", "text": "Cass is held in the garrison cells; walk her out.",
         "steps": [{"pred": "in_room", "args": ["special-stronghold-hall"]}, {"pred": "flag", "args": ["prisoner_freed"]}]},
        {"id": "q_delivery", "kind": "delivery", "giver": "npc_merchant", "reward": "TOOL_LANTERN", "text": "Odda wants three loaves carried to the caravan camp.",
         "steps": [{"pred": "has_item", "args": ["FOOD_BREAD", "3"]}, {"pred": "gave", "args": ["npc_caravan_master", "FOOD_BREAD"]}]},
        {"id": "q_flood", "kind": "flood", "giver": "npc_scholar", "reward": "SCROLL_MAGICMAPPING" if "SCROLL_MAGICMAPPING" in items() else "TOOL_LANTERN",
         "text": "Tolly wants the sump drained so the old plaques can be read.",
         "steps": [{"pred": "built", "args": ["sluice"]}, {"pred": "water_below", "args": ["sewer-cistern", "1"]}]},
        {"id": "q_wall", "kind": "wall", "giver": "npc_hermit", "reward": "FOOD_RATION_HERBAL", "text": "Haskel asks for a wall to keep the warren out of his grotto.",
         "steps": [{"pred": "built", "args": ["stone_wall"]}, {"pred": "talked", "args": ["npc_hermit", "wall"]}]},
        {"id": "q_beast", "kind": "beast", "giver": "npc_goblin_envoy", "reward": "TOOL_PICKAXE", "text": "Grik wants the Matriarch driven off, not killed.",
         "steps": [{"pred": "fled", "args": ["cave_matriarch"]}, {"pred": "talked", "args": ["npc_goblin_envoy", "matriarch"]}]},
        {"id": "q_chronicle", "kind": "chronicle", "giver": "npc_scholar", "reward": "SPELLBOOK_LIGHT" if "SPELLBOOK_LIGHT" in items() else "TOOL_LANTERN",
         "text": "Tolly pays for five true pages of the Hollow's history.",
         "steps": [{"pred": "read", "args": hist_ids[:5]}]},
    ]


TEMPLATES = [
    ("t01", "In {year} the {faction} dug into the {place} and found {thing}."), ("t02", "{npc} sealed the {place} in {year}; the key was never named."),
    ("t03", "The {place} flooded in {year}; the {faction} blamed {npc}."), ("t04", "{thing} left the {place} in {year} and was not seen again."),
    ("t05", "Fire took the {place} in {year}; only {thing} survived."), ("t06", "{npc} wrote that the {faction} feared {thing} below the {place}."),
    ("t07", "In {year} the {faction} and {npc} traded {thing} at the {place}."), ("t08", "The {place} fell silent in {year}; {npc} warned of {thing}."),
    ("t09", "Rats took the {place} in {year}; the {faction} burned {thing}."), ("t10", "{npc} mapped the {place} in {year} and hid {thing} there."),
    ("t11", "The {faction} swore off {thing} in {year}."), ("t12", "After {year} only {npc} entered the {place}, and took {thing}."),
]
PLACES = ["crypt", "mine", "sewer", "temple", "fortress", "cave", "sump", "vent", "plaza"]
THINGS = ["a silver bell", "the iron rod", "a sealed ledger", "black salt", "a drowned choir", "the first map", "a lantern that never went out", "copper coins",
          "a broken key", "the vent's warmth", "a nest of moths", "the old charter", "a stone saint", "a cask of oil", "the sluice gate", "a goat skull",
          "seven candles", "a warden's oath", "green glass", "the deep spring"]


def history():
    r = LCG(20261019)
    fac = [f for f in FACTIONS]
    npc_names = NPC_NAMES
    rows = []
    idx = 0
    for tid, text in TEMPLATES:
        for k in range(5):
            year = -400 + (idx * 7) % 399
            f = fac[r.next(len(fac))]
            ni = r.next(len(NPC))
            place = PLACES[r.next(len(PLACES))]
            thing = THINGS[r.next(len(THINGS))]
            t = text.format(year=f"{-year} before the Fall", faction=dict(factions_names())[f], npc=npc_names[ni], place=place, thing=thing)
            t = t[0].upper() + t[1:]
            actors = []
            if "{faction}" in text:
                actors.append(f)
            if "{npc}" in text:
                actors.append(f"npc_{NPC[ni][0]}")
            if not actors:
                actors.append(f)
            rows.append({"id": f"h{idx + 1:03d}", "year": year, "kind": tid, "template": tid, "actors": actors, "place": place, "text": t})
            idx += 1
    return rows


def factions_names():
    return [(r["id"], r["name"]) for r in factions()]


def secrets(hist_ids):
    rooms = ["crypt-ossuary-niches", "mine-drift-end", "sewer-overflow-cell", "temple-chapel", "fortress-guardroom", "cave-pocket"]
    rows = [{"id": f"sec_room_{i + 1}", "kind": "room", "ref": rooms[i], "hint": hist_ids[(i * 9) % len(hist_ids)]} for i in range(6)]
    rows += [{"id": f"sec_recipe_{i + 1}", "kind": "recipe", "ref": r, "hint": hist_ids[(i * 7 + 3) % len(hist_ids)]} for i, r in enumerate(["r28", "r31", "r39"])]
    rows += [{"id": f"sec_spell_{i + 1}", "kind": "spell", "ref": w, "hint": hist_ids[(i * 11 + 5) % len(hist_ids)]} for i, w in enumerate(["wand_03", "wand_07", "wand_11"])]
    return rows


VERBS = ["bolt", "wall", "pit", "burst", "beam", "ward", "lift", "pull", "bind", "mend", "sense", "veil"]
SHAPES = ["point", "line", "cone", "sphere", "ring", "plane", "trail"]
ELEMENTS = ["fire", "ice", "stone", "iron", "acid", "lightning", "light", "shadow", "void", "life"]
MODS = ["none", "trigger_on_hit", "timer", "chain2", "echo", "silent", "anchored", "inverted"]


def spells():
    rows = []
    for pick, names in (("verb", VERBS), ("shape", SHAPES), ("element", ELEMENTS), ("modifier", MODS)):
        rows += [{"id": f"{pick}_{n}", "pick": pick, "name": n.replace("_", " ")} for n in names]
    wands = []
    for i in range(12):
        n = 1 + i % 4
        sp = [{"verb": f"verb_{VERBS[(i + k) % 12]}", "shape": f"shape_{SHAPES[(i + 2 * k) % 7]}", "element": f"element_{ELEMENTS[(i + 3 * k) % 10]}",
               "modifier": f"modifier_{MODS[(i + k) % 8]}"} for k in range(n)]
        wands.append({"id": f"wand_{i:02d}", "tier": 1 + i % 4, "depth": i % 6, "spells": sp})
    return rows, wands


def weather():
    return [{"id": "clear", "light_pct": 100, "wet_per_step": 0, "note": "the surface pocket at its brightest"},
            {"id": "rain", "light_pct": 70, "wet_per_step": 2, "note": "rain soaks the surface and runs into any hole dug through the roof"},
            {"id": "storm", "light_pct": 40, "wet_per_step": 4, "note": "one lightning strike on the ruin's iron rod per storm (built later by H1a and BF7)"}]


def ecology():
    return [{"id": "eco_stream_fish", "species": "fish", "behavior": "shoals in the stream; scatters at footsteps", "den_theme": "surface"},
            {"id": "eco_copse_birds", "species": "bird", "behavior": "roosts in the copse; flees at noise", "den_theme": "surface"},
            {"id": "eco_copse_deer", "species": "deer", "behavior": "grazes at dusk; bolts when hunted", "den_theme": "surface"},
            {"id": "eco_cave_fungus", "species": "fungus", "behavior": "myconid den; spreads in damp mine rooms", "den_theme": "cave"},
            {"id": "eco_rats", "species": "rat", "behavior": "fodder swarms in sewers; flee fire", "den_theme": "sewer"},
            {"id": "eco_bats", "species": "bat", "behavior": "hang in caverns; wake at light", "den_theme": "cave"},
            {"id": "eco_wolf_pair", "species": "wolf", "behavior": "a pair that circles the camp at night", "den_theme": "surface"},
            {"id": "eco_warren_goats", "species": "goat", "behavior": "kept by the warren for milk and hide", "den_theme": "mine"}]


def merchants():
    # D73: one trade per haven merchant; materials, components, reagents, services, provisions
    S = lambda k, r: {"kind": k, "ref": r}  # noqa: E731
    return [
        {"id": "shop_smith", "trade": "smith", "npc": "npc_smith", "floor": 1, "purse_gold": 300, "buy_pct": 33, "buys": "smith",
         "sells": [S("material", "stub:F6:iron_ingot"), S("material", "stub:F6:bronze_ingot"), S("component", "stub:F6:blade_plain"),
                   S("component", "stub:F6:grip_plain"), S("service", "repair"), S("service", "appraisal")]},
        {"id": "shop_alchemist", "trade": "alchemist", "npc": "npc_alchemist", "floor": 2, "purse_gold": 250, "buy_pct": 33, "buys": "alchemist",
         "sells": [S("reagent", "stub:F7:salt_rock"), S("reagent", "stub:F7:herb_bitterleaf"), S("reagent", "stub:F7:fungus_glowcap"),
                   S("service", "healing"), S("service", "cure_curse"), S("provision", "FOOD_APPLE")]},
        {"id": "shop_scribe", "trade": "scribe", "npc": "npc_scholar", "floor": 3, "purse_gold": 200, "buy_pct": 33, "buys": "scribe",
         "sells": [S("material", "stub:F6:vellum"), S("component", "stub:F6:inscription_plain"), S("service", "maps"), S("service", "rumours"),
                   S("service", "appraisal"), S("provision", "TOOL_TORCH")]},
        {"id": "shop_provisioner", "trade": "provisioner", "npc": "npc_merchant", "floor": 4, "purse_gold": 200, "buy_pct": 33, "buys": "provisioner",
         "sells": [S("provision", "FOOD_BREAD"), S("provision", "FOOD_RATION"), S("provision", "TOOL_TORCH"), S("provision", "TOOL_LANTERN"),
                   S("service", "follower_hire"), S("service", "healing")]},
        {"id": "shop_fence", "trade": "fence", "npc": "npc_caravan_master", "floor": 3, "purse_gold": 400, "buy_pct": 25, "buys": "fence",
         "sells": [S("material", "GEM_ROCK"), S("component", "stub:F6:setting_plain"), S("service", "appraisal"), S("service", "rumours")]},
    ]


def set_pieces():
    P = lambda i, lvl, room, st, parts, note: {"id": i, "level": lvl, "room_kind": room, "station": st, "parts": parts, "note": note}  # noqa: E731
    return [
        P("sp_ruin_rod", "surface", "special-nexus-stairhub", "", ["pillar", "rubble", "plaque"], "the ruin's iron rod: storms strike it (rod built later, BF7)"),
        P("sp_camp_fire", "surface", "special-haven-shrine", "", ["campfire", "tent", "crate"], "the caravan camp at the surface; the delivery quest ends here"),
        P("sp_f1_anvil", "floor1", "fortress-armory-hall", "anvil", ["anvil", "rack", "brazier", "barrel"], "crafting depth 1: the garrison anvil (gameplay/20 2.1)"),
        P("sp_f1_cells", "floor1", "fortress-barracks", "", ["bed", "chains", "bench"], "the cells where Cass is held (rescue quest)"),
        P("sp_f1_plaque", "floor1", "special-nexus-stairhub", "", ["plaque", "statue"], "the plaque that names the director (play probe)"),
        P("sp_f2_crucible", "floor2", "temple-nave", "crucible", ["altar", "brazier", "shelf"], "crafting depth 2: crucible and bench (gameplay/20 2.1)"),
        P("sp_f2_sump_wall", "floor2", "temple-courtyard", "", ["rubble", "urn", "chalk_circle"], "the cult room beside the sump wall; breaking it floods the room"),
        P("sp_f3_transmuter", "floor3", "crypt-tomb-chamber", "transmuter", ["altar", "candle", "book"], "crafting depth 3: the transmuter (gameplay/20 2.1)"),
        P("sp_f3_sluice", "floor3", "sewer-cistern", "sluice", ["crate", "barrel", "pipes"], "the flooded sump drains once a sluice is built (flood quest)"),
        P("sp_f4_fungus", "floor4", "mine-ore-cavern", "", ["mushrooms", "ore_pile", "pickaxe"], "fungus that spreads in the mine (living world, 18)"),
        P("sp_f4_envoy", "floor4", "special-stronghold-hall", "", ["table", "bench", "map_scroll"], "the goblin envoy's court (beast quest)"),
        P("sp_caves_vent_forge", "caves", "cave-lava-vent", "vent_forge", ["anvil", "rubble", "brazier"], "crafting depth caves: the vent-forge; the vent cools to basalt"),
    ]


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--apxgen", default=r"C:/Users/Apocky/source/repos/apx-gen")
    a = ap.parse_args()
    OUT.mkdir(parents=True, exist_ok=True)
    # the room programs apx-gen ships (refs for secrets and set pieces)
    progs = sorted(p.stem for p in Path(a.apxgen, "kits/rooms").glob("*.toml"))
    progs += ["crypt-hall", "pillar-hall", "plain-chamber"]  # read in place from Apockalypsis/kits/rooms
    (ROOT / "tests/apx/apxgen_room_programs.txt").write_text("\n".join(sorted(set(progs))) + "\n", encoding="ascii", newline="\n")
    (ROOT / "tests/apx/banned_tokens.txt").write_text("gore\nmutilat\ntorture\nsevered\ndismember\n", encoding="ascii", newline="\n")
    hist = history()
    hist_ids = sorted(r["id"] for r in hist)
    sp, wands = spells()
    counts = {
        "consumables": write("consumables.json", consumables()),
        "creatures": write("creatures.json", creatures()),
        "bosses": write("bosses.json", bosses()),
        "factions": write("factions.json", factions()),
        "npcs": write("npcs.json", npcs(hist_ids)),
        "quests": write("quests.json", quests(hist_ids)),
        "history": write("history.json", hist),
        "history_templates": write("history_templates.json", [{"id": t, "text": x} for t, x in TEMPLATES]),
        "secrets": write("secrets.json", secrets(hist_ids)),
        "weather": write("weather.json", weather()),
        "ecology": write("ecology.json", ecology()),
        "spell_parts": write("spell_parts.json", sp),
        "wands": write("wands.json", wands),
        "merchants": write("merchants.json", merchants()),
        "set_pieces": write("set_pieces.json", set_pieces()),
    }
    import hollow_schema as S
    classes = []
    for stem, sch in S.CLASSES.items():
        if sch.get("stub"):
            (OUT / f"{stem}.json").write_text("[]\n", encoding="ascii", newline="\n")
        classes.append({"file": f"{stem}.json", "min": sch["min"], "schema": stem, "stub": sch.get("stub", "")})
    (OUT / "manifest.json").write_text(json.dumps({"version": 1, "classes": classes}, indent=1) + "\n", encoding="ascii", newline="\n")
    print(json.dumps(counts))


if __name__ == "__main__":
    import sys
    sys.path.insert(0, str(Path(__file__).resolve().parent))
    main()
