"""gen_spells.py -- data/apx/magic/spells.json: Barony's 223 vanilla spells as apx::magic rows (spec 04b section 7).

Each row maps a vanilla spell (items/items.json, spell_id) to the four picks (VERB x SHAPE x ELEMENT x MODIFIER) that price it, and to a
DELIVERY:
  proj    native: a physical projectile through apx::proj; the hit goes through apx::combat::applyHit and the row's status
  legacy  a named effect verb: the cast pipeline aims, charges mana, and calls Barony's own castSpell() effect code for that spell id
          (trap mode: no animation, no mana, no skill-up there; apx::magic did all three)
The picks are AUTHORED heuristics (keywords of the spell name, then the spell's rangefinder type); OVERRIDES pin the ones that matter.
    python tools/apx/gen_spells.py [--items <items.json>] [--out data/apx/magic/spells.json]
"""
import argparse
import json
from pathlib import Path

ITEMS = r"C:\Program Files (x86)\Steam\steamapps\common\Barony\items\items.json"
ROOT = Path(__file__).resolve().parents[2]

VERBS = ["bolt", "wall", "pit", "burst", "beam", "ward", "lift", "pull", "bind", "mend", "sense", "veil"]
SHAPES = ["point", "line", "cone", "sphere", "ring", "plane", "trail"]
ELEMENTS = ["fire", "ice", "stone", "iron", "acid", "lightning", "light", "shadow", "void", "life"]
MODIFIERS = ["none", "trigger", "timer", "chain", "echo", "silent", "anchored", "inverted"]

# element keywords, first match wins
EL_KEYS = [
    ("void", ["void", "null_", "puncture", "unmake", "taboo", "demesne", "minimise", "numbing"]),
    ("lightning", ["lightning", "arcs", "static", "nexus", "shock", "storm"]),
    ("fire", ["fire", "flame", "burn", "ignite", "meteor", "ember", "scepter", "sun"]),
    ("ice", ["cold", "ice", "snow", "frost", "freeze", "sleep", "slow", "chronomic"]),
    ("acid", ["acid", "grease", "corrode"]),
    ("iron", ["metal", "lead", "mercury", "gear", "splinter", "needle", "forge", "steel", "force", "kinetic", "disarm", "weapon", "arrow", "strike", "shatter_obj", "slam"]),
    ("stone", ["earth", "stone", "rock", "spine", "sand", "dig", "tunnel", "geomancy", "boulder", "roots", "mushroom", "shrub", "bastion"]),
    ("shadow", ["shadow", "shade", "dark", "drain", "curse", "vampiric", "steal", "cowardice", "deep", "dominate", "charm", "fear", "bleed", "blood_waves", "confuse", "incoherence", "dizzy", "spin", "weakness", "demon", "ghost", "revenant", "command", "taint", "desecrate", "vandal"]),
    ("light", ["light", "holy", "sanct", "justice", "purity", "peace", "provid", "divine", "reveal", "detect", "scry", "wonder", "identify", "mapping", "beam", "pinpoint", "penance", "courage", "turn_undead", "eternal", "reflect", "magic_well", "magicmissile", "magic_missile"]),
    ("life", ["heal", "cure", "mend", "troll", "restor", "abundance", "vine", "thorn", "bless", "spore", "regen", "food", "true_blood", "poison", "salvage", "preserve", "revert", "form", "polymorph", "summon", "ally", "elemental", "sprite"]),
]
SCHOOL_EL = {"sorcery": "light", "mysticism": "shadow", "thaumaturgy": "life", "innate": "iron"}

# explicit picks: id-name -> (verb, shape, element, modifier)
OVERRIDES = {
    "forcebolt": ("bolt", "point", "iron", "none"),
    "magicmissile": ("bolt", "line", "light", "none"),
    "cold": ("bolt", "point", "ice", "none"),
    "fireball": ("bolt", "sphere", "fire", "none"),
    "lightning": ("bolt", "line", "lightning", "none"),
    "acid_spray": ("bolt", "cone", "acid", "none"),
    "poison": ("bolt", "point", "acid", "none"),
    "bleed": ("bolt", "point", "shadow", "none"),
    "sleep": ("bind", "point", "shadow", "none"),
    "slow": ("bind", "point", "ice", "none"),
    "confuse": ("bind", "point", "shadow", "none"),
    "weakness": ("bind", "point", "shadow", "timer"),
    "healing": ("mend", "sphere", "life", "none"),
    "extrahealing": ("mend", "sphere", "life", "echo"),
    "heal_other": ("mend", "point", "life", "none"),
    "heal_minor": ("mend", "point", "life", "none"),
    "heal_pulse": ("mend", "ring", "life", "none"),
    "cureailment": ("mend", "point", "life", "none"),
    "light": ("sense", "sphere", "light", "anchored"),
    "invisibility": ("veil", "point", "shadow", "none"),
    "levitation": ("lift", "point", "light", "timer"),
    "reflect_magic": ("ward", "sphere", "light", "none"),
    "force_shield": ("ward", "sphere", "iron", "none"),
    "reflector": ("ward", "sphere", "iron", "inverted"),
    "magicmapping": ("sense", "sphere", "light", "none"),
    "identify": ("sense", "point", "light", "none"),
    "detect_food": ("sense", "sphere", "life", "none"),
    "fire_wall": ("wall", "plane", "fire", "none"),
    "ice_wave": ("burst", "cone", "ice", "none"),
    "meteor": ("burst", "sphere", "fire", "timer"),
    "meteor_shower": ("bolt", "ring", "fire", "chain"),
    "earth_spines": ("pit", "line", "stone", "none"),
    "disrupt_earth": ("pit", "sphere", "stone", "none"),
    "lightning_bolt": ("bolt", "line", "lightning", "trigger"),
    "slam": ("burst", "ring", "iron", "none"),
    "telepull": ("pull", "line", "void", "none"),
    "vortex": ("pull", "sphere", "void", "none"),
    "kinetic_push": ("lift", "cone", "iron", "none"),
    "lift": ("lift", "point", "iron", "none"),
    "holy_beam": ("beam", "line", "light", "none"),
    "breathe_fire": ("bolt", "cone", "fire", "none"),
    "stoneblood": ("ward", "point", "stone", "none"),
    "teleportation": ("lift", "point", "void", "none"),
    "dig": ("bolt", "line", "stone", "none"),
    "tunnel": ("wall", "plane", "stone", "inverted"),
    "sanctuary": ("ward", "ring", "light", "anchored"),
    "sigil": ("ward", "ring", "shadow", "anchored"),
    "shatter_objects": ("burst", "sphere", "iron", "none"),
    "ignite": ("burst", "sphere", "fire", "none"),
}

# natives: delivered by apx::proj. value = the on-hit status ("" none).
NATIVE = {
    "forcebolt": "push", "magicmissile": "", "cold": "slow", "fireball": "burn", "lightning": "shock", "bleed": "bleed", "poison": "poison",
    "slime_acid": "corrode", "slime_fire": "burn", "slime_tar": "slow", "slime_metal": "",
    "foci_fire": "burn", "foci_snow": "slow", "foci_needles": "bleed", "foci_arcs": "shock", "foci_sandblast": "blind",
    "psychic_spear": "", "lead_bolt": "", "mercury_bolt": "poison", "splinter_gear": "bleed", "blood_waves": "bleed", "scepter_blast": "burn",
    "breathe_fire": "burn", "sleep": "sleep", "confuse": "confuse", "slow": "slow", "weakness": "weakness",
}

TYPE_DEFAULT = {   # spell_type -> (verb, shape)
    "PROJECTILE": ("bolt", "point"), "PROJECTILE_SHORT_X3": ("bolt", "cone"), "SELF": ("ward", "point"), "SELF_SUSTAIN": ("ward", "sphere"),
    "TOUCH_FLOOR": ("burst", "sphere"), "TOUCH_ENEMY": ("bind", "point"), "AREA": ("burst", "sphere"), "DIVINE_TARGET": ("sense", "sphere"),
    "TOUCH_WALL": ("wall", "plane"), "TOUCH_ALLY": ("mend", "point"),
}
VERB_KEYS = [
    ("mend", ["heal", "cure", "restor", "mend", "troll", "regen", "salvage"]),
    ("sense", ["detect", "scry", "identify", "mapping", "seek", "pinpoint", "wonder", "eternal"]),
    ("veil", ["invis", "form", "shade", "polymorph", "mist", "deep_shade", "hologram", "flutter"]),
    ("lift", ["lev", "lift", "jump", "dash", "telekin"]),
    ("pull", ["pull", "vortex", "attract", "return"]),
    ("bind", ["sleep", "slow", "confuse", "dominate", "charm", "fear", "web", "root", "weak", "command", "curse", "disarm", "strip", "paral", "cowardice"]),
    ("ward", ["guard", "ward", "shield", "reflect", "absorb", "armor", "armour", "sanct", "cloak", "protect", "bless", "courage", "prof_"]),
    ("pit", ["spine", "pit", "disrupt", "earth", "booby", "trap", "sabotage", "shatter"]),
    ("wall", ["wall", "gate", "door", "tunnel", "bastion", "barrier"]),
    ("beam", ["beam", "ray"]),
    ("burst", ["burst", "explo", "nova", "shower", "meteor", "bomb", "blast", "wave"]),
]


def pick_element(name, school):
    for el, keys in EL_KEYS:
        if any(k in name for k in keys):
            return el
    return SCHOOL_EL.get(school, "iron")


def pick_verb_shape(name, stype):
    verb, shape = TYPE_DEFAULT[stype]
    for v, keys in VERB_KEYS:
        if any(k in name for k in keys):
            verb = v
            break
    if verb in ("ward", "sense", "veil", "mend", "lift", "bind") and stype in ("SELF", "TOUCH_ENEMY", "TOUCH_ALLY", "PROJECTILE"):
        shape = "point" if stype != "SELF_SUSTAIN" else "sphere"
    if verb == "wall":
        shape = "plane"
    if verb == "beam":
        shape = "line"
    if verb == "pit" and shape == "point":
        shape = "sphere"
    return verb, shape


def modifier_for(name, tags, stype):
    if stype == "SELF_SUSTAIN":
        return "timer"
    if "SPELL_LEVEL_SUMMON" in tags:
        return "echo"
    if "TRACK_SPELL_HITS" in tags and stype == "PROJECTILE":
        return "trigger"
    return "none"


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--items", default=ITEMS)
    ap.add_argument("--out", default=str(ROOT / "data" / "apx" / "magic" / "spells.json"))
    a = ap.parse_args()
    src = json.load(open(a.items, encoding="utf-8"))["spells"]
    rows = []
    for key, v in src.items():
        name = key[6:] if key.startswith("spell_") else key
        stype = v["spell_type"]
        tags = v.get("effect_tags", [])
        school = v.get("school", "")
        if name in OVERRIDES:
            verb, shape, el, mod = OVERRIDES[name]
        else:
            verb, shape = pick_verb_shape(name, stype)
            el = pick_element(name, school)
            mod = modifier_for(name, tags, stype)
        deliver = "proj" if name in NATIVE else "legacy"
        row = {"id": v["spell_id"], "key": key, "name": v.get("spell_name", name), "type": stype, "school": school or "innate",
               "picks": {"verb": verb, "shape": shape, "element": el, "modifier": mod}, "deliver": deliver,
               "difficulty": v.get("difficulty", 0), "tags": [t for t in tags if t in ("DAMAGE", "STATUS", "UTILITY", "HEALING", "CURE")]}
        if deliver == "proj":
            row["on_hit"] = NATIVE[name]
            row["damage"] = v.get("damage", 0)
        rows.append(row)
    rows.sort(key=lambda r: r["id"])
    doc = {"version": 1, "source": "items/items.json (Barony 5.0.2), %d spells" % len(rows), "verbs": VERBS, "shapes": SHAPES, "elements": ELEMENTS,
           "modifiers": MODIFIERS, "spells": rows}
    out = Path(a.out)
    out.parent.mkdir(parents=True, exist_ok=True)
    out.write_text(json.dumps(doc, indent=1, ensure_ascii=True) + "\n", encoding="ascii")
    nat = sum(1 for r in rows if r["deliver"] == "proj")
    from collections import Counter
    print("wrote %s: %d spells, %d native (proj), %d legacy; verbs %s" % (out, len(rows), nat, len(rows) - nat, dict(Counter(r["picks"]["verb"] for r in rows))))
    print("shapes", dict(Counter(r["picks"]["shape"] for r in rows)))
    print("elements", dict(Counter(r["picks"]["element"] for r in rows)))


if __name__ == "__main__":
    main()
