"""Authors data/apx/variants/*.json (creature variant trees, card BV1). Every number lives here.
Run: python -I tools/apx/author_variants.py   (rewrites the files; then tools/apx/variants_lint.py and gen_variants.py)
All names of rare variants are PLACEHOLDERS until Apocky reviews them (card section 8)."""
import json
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
OUT = ROOT / "data/apx/variants"

# base: monster type (src/monster.hpp enum name), default loot (each entry: id, chance per kill)
BASES = {
    "rat": dict(monster="RAT", name="rat", loot=[["mat:hide", 0.6], ["mat:sinew", 0.2], ["mat:bone", 0.15]]),
    "skeleton": dict(monster="SKELETON", name="skeleton", loot=[["mat:bone", 0.9], ["item:GEM_ROCK", 0.1], ["mat:iron", 0.08]]),
}

# types: (id, name, depth_min, rarity among the base's children, material, stats, traits, tint, loot add)
TYPES = {
    "undead": dict(name="Undead", rarity=0.07, dmin=1, dmax=15, material="bone",
                   stats=dict(hp=1.1, dmg=1.0, speed=0.95, poise=1.0), traits=["rise_once:30%"],
                   look=dict(tint="#8fa88a", scale=1.0, light="none"),
                   add=[["mat:bone", 0.25], ["item:GEM_JETSTONE", 0.02]]),
    "elemental": dict(name="Elemental", rarity=0.07, dmin=2, dmax=15, material="obsidian",
                      stats=dict(hp=1.2, dmg=1.1, speed=1.0, poise=1.1), traits=["death_burst:6:12"],
                      look=dict(tint="#c08a50", scale=1.05, light="dim_amber"),
                      add=[["mat:obsidian", 0.2], ["mat:coal", 0.15]]),
}

# subtypes per type: id -> row; "rare" is the one named variant under it
SUBTYPES = {
    "undead": {
        "accursed": dict(name="Accursed", rarity=0.28, dmin=3, material="bone", tint="#5a2a6a", light="dim_violet",
                         stats=dict(hp=1.4, dmg=1.1, speed=0.9, poise=1.2), traits=["curse_on_hit:10%:30s", "rise_once:50%"],
                         add=[["mat:ash", 0.35], ["item:SCROLL_REMOVECURSE", 0.03], ["comp:bone@uncommon+cursed", 0.2]], reweight={},
                         rare=dict(name="Gravewhisker", rarity=0.01, dmin=5, tier_shift=1, scale=1.15,
                                   trait="whisper_aura:6:slow", add=[["item:GEM_AMETHYST", 0.4], ["comp:bone@rare+whispering", 0.35]])),
        "skeletal": dict(name="Skeletal", rarity=0.28, dmin=1, material="bone", tint="#d8d2b8", light="none",
                         stats=dict(hp=0.9, dmg=1.15, speed=1.05, poise=0.8), traits=["brittle:impact:1.5"],
                         add=[["mat:bone", 0.5], ["comp:bone@common", 0.3]], reweight={},
                         rare=dict(name="Clatterking", rarity=0.01, dmin=4, tier_shift=1, scale=1.2,
                                   trait="bone_spurs:3:pierce", add=[["mat:ivory", 0.3], ["comp:ivory@rare+clattering", 0.3]])),
        "spectral": dict(name="Spectral", rarity=0.28, dmin=4, material="aethite", tint="#7fb0d8", light="dim_blue",
                         stats=dict(hp=0.8, dmg=1.2, speed=1.1, poise=0.6), traits=["ethereal_pass:physical:0.5"],
                         add=[["mat:glass", 0.3], ["mat:quartz", 0.15], ["comp:glass@uncommon+ethereal", 0.2]], reweight={},
                         rare=dict(name="Wailing Shade", rarity=0.01, dmin=6, tier_shift=1, scale=1.0,
                                   trait="fear_on_hit:8%:3s", add=[["item:GEM_OPAL", 0.3], ["comp:aethite@rare+wailing", 0.3]])),
    },
    "elemental": {
        "fire": dict(name="Fire", rarity=0.28, dmin=2, material="coal", tint="#d8502a", light="warm_red",
                     stats=dict(hp=1.0, dmg=1.3, speed=1.05, poise=1.0), traits=["ignite_on_hit:15%:4s"],
                     add=[["mat:sulfur", 0.35], ["mat:coal", 0.3], ["comp:coal@uncommon+smoldering", 0.2]], reweight={"mat:hide": 0.3},
                     rare=dict(name="Cinderback", rarity=0.01, dmin=5, tier_shift=1, scale=1.15,
                               trait="ember_trail:2:burn", add=[["item:GEM_RUBY", 0.35], ["comp:coal@rare+cinder", 0.3]])),
        "frost": dict(name="Frost", rarity=0.28, dmin=2, material="ice", tint="#8fd0f0", light="cold_cyan",
                      stats=dict(hp=1.2, dmg=1.0, speed=0.9, poise=1.3), traits=["chill_on_hit:20%:4s"],
                      add=[["mat:ice", 0.4], ["mat:salt", 0.2], ["comp:ice@uncommon+rimed", 0.2]], reweight={"mat:hide": 0.4},
                      rare=dict(name="Rimeheart", rarity=0.01, dmin=5, tier_shift=1, scale=1.1,
                                trait="frost_nova:24:slow", add=[["item:GEM_AQUAMARINE", 0.35], ["comp:ice@rare+rimeheart", 0.3]])),
        "storm": dict(name="Storm", rarity=0.28, dmin=3, material="quartz", tint="#b8a8f0", light="flicker_white",
                      stats=dict(hp=0.9, dmg=1.2, speed=1.2, poise=0.9), traits=["shock_chain:2:10"],
                      add=[["mat:quartz", 0.35], ["mat:glass", 0.2], ["comp:quartz@uncommon+charged", 0.2]], reweight={"mat:hide": 0.4},
                      rare=dict(name="Thunderpelt", rarity=0.01, dmin=6, tier_shift=1, scale=1.1,
                                trait="static_field:30:shock", add=[["item:GEM_FLUORITE", 0.35], ["comp:quartz@rare+thunder", 0.3]])),
    },
}


def look(tint, scale, light):
    return {"tint": tint, "scale": scale, "light": light}


def main():
    OUT.mkdir(parents=True, exist_ok=True)
    for bid, b in BASES.items():
        nodes = []
        nodes.append({"id": bid, "level": "base", "name": b["name"], "monster": b["monster"], "loot": {"table": b["loot"]}})
        for tid, t in TYPES.items():
            tnode_id = "%s.%s" % (bid, tid)
            nodes.append({"id": tnode_id, "parent": bid, "level": "type", "name": t["name"] + " " + b["name"], "rarity": t["rarity"],
                          "depth_min": t["dmin"], "depth_max": t["dmax"], "stats": t["stats"], "material": t["material"], "tier_shift": 0,
                          "traits": t["traits"], "look": look(t["look"]["tint"], t["look"]["scale"], t["look"]["light"]),
                          "loot": {"extends": bid, "add": t["add"], "reweight": {}}})
            for sid, s in SUBTYPES[tid].items():
                snode_id = "%s.%s" % (tnode_id, sid)
                nodes.append({"id": snode_id, "parent": tnode_id, "level": "subtype", "name": s["name"] + " " + b["name"], "rarity": s["rarity"],
                              "depth_min": s["dmin"], "depth_max": 15, "stats": s["stats"], "material": s["material"], "tier_shift": 0,
                              "traits": s["traits"], "look": look(s["tint"], 1.0, s["light"]),
                              "loot": {"extends": tnode_id, "add": s["add"], "reweight": {k: v for k, v in s["reweight"].items() if any(e[0] == k for e in b["loot"])}}})
                r = s["rare"]
                nodes.append({"id": "%s.%s" % (snode_id, r["name"].lower().replace(" ", "_")), "parent": snode_id, "level": "rare",
                              "name": r["name"] + " (placeholder)", "rarity": r["rarity"], "depth_min": r["dmin"], "depth_max": 15,
                              "stats": {"hp": round(s["stats"]["hp"] * 1.5, 3), "dmg": round(s["stats"]["dmg"] * 1.3, 3),
                                        "speed": s["stats"]["speed"], "poise": round(s["stats"]["poise"] * 1.3, 3)},
                              "material": s["material"], "tier_shift": r["tier_shift"], "max_group": 4, "traits": s["traits"] + [r["trait"]],
                              "look": look(s["tint"], r["scale"], s["light"]),
                              "loot": {"extends": snode_id, "add": r["add"], "reweight": {}}})
        doc = {"_note": "authored by tools/apx/author_variants.py; names of rare variants are placeholders (card BV1 section 8)", "nodes": nodes}
        (OUT / (bid + ".json")).write_text(json.dumps(doc, indent=1, sort_keys=True, ensure_ascii=True) + "\n", encoding="ascii")
        print("wrote", bid, len(nodes), "nodes")


if __name__ == "__main__":
    main()
