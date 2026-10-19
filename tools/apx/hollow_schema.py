"""hollow_schema.py -- the ONLY schema source for data/apx/hollow (card BH1b, H1b manifest).

Field specs:
  ("id",)                      ^[a-z0-9_]{3,40}$, unique across all files
  ("str", maxlen[, minlen])    ASCII text
  ("int", lo, hi)
  ("enum", [values])
  ("ref", [targets])           targets: a class file stem (its ids), "itemtype", "monster",
                               "history_or_quest", "faction_or_npc", "aspect", "material", "liquid"
  ("list", spec, lo, hi)
  ("obj", {field: spec})
  ("map", key_spec, value_spec)
A class with "stub" is deferred to a slice that is not merged (BF6 parts gear, BF7 alchemy): the
counter prints STUB for it, the lint skips its rows and treats refs into it as DEFERRED (counted).
"""

THEMES = ["crypt", "mine", "sewer", "temple", "fortress", "cave", "surface"]
SPECIES = ["rat", "bat", "wolf", "deer", "fish", "bird", "goat", "boar", "spider", "snake", "lizard", "worm", "crab", "beetle", "moth"]
QUEST_PREDICATES = ["has_item", "in_room", "flag", "killed", "read", "built", "water_below", "fled", "gave", "talked"]
NPC_ROLES = ["merchant", "smith", "alchemist", "scholar", "prisoner", "deserter", "cultist_defector", "goblin_envoy", "hermit", "ghost",
             "caravan_master", "guard_a", "guard_b", "warden_prewar"]
TRADES = ["smith", "alchemist", "scribe", "provisioner", "fence"]          # D73: one trade per haven merchant
SERVICES = ["repair", "appraisal", "cure_curse", "healing", "follower_hire", "maps", "rumours"]
SELL_KINDS = ["material", "component", "reagent", "service", "provision"]  # D73: no finished products
TIERS = ["fodder", "regular", "elite", "champion"]

ID = ("id",)

CLASSES = {
    # ---- BF6 parts gear: deferred
    "families": {"min": 17, "stub": "F6", "fields": {"id": ID, "name": ("str", 40), "required": ("list", ("str", 20), 1, 6),
                 "optional": ("list", ("obj", {"slot": ("str", 20), "qgate": ("int", 1, 10)}), 0, 12)}},
    "parts": {"min": 320, "stub": "F6", "fields": {"id": ID, "family": ("ref", ["families"]), "slot": ("str", 20), "variant": ("str", 20),
              "name": ("str", 60), "role": ("enum", ["strike", "structural", "balance", "grip", "other"]), "mass_pct": ("int", 50, 200),
              "ip_pct": ("int", 50, 200), "sprite_item": ("str", 40)}},
    "placed_items": {"min": 400, "stub": "F6", "fields": {"id": ID, "depth": ("int", 0, 5), "source": ("int", 0, 3), "index": ("int", 0, 100000),
                     "family": ("ref", ["families"])}},
    # ---- BF7 alchemy: deferred
    "ingredients": {"min": 80, "stub": "F7", "fields": {"id": ID, "name": ("str", 40), "class": ("enum", ["herb", "fungus", "mineral", "animal", "liquid", "salt"]),
                    "base_type": ("ref", ["itemtype"]), "mass_g": ("int", 1, 5000), "aspects": ("list", ("obj", {"a": ("ref", ["aspect"]), "w": ("int", 1, 3)}), 1, 3),
                    "species": ("enum", SPECIES + [""]), "depth": ("int", 0, 5)}},
    "reactions_h1b": {"min": 28, "stub": "F7", "fields": {"id": ID}},
    "reaction_discovery": {"min": 40, "stub": "F7", "fields": {"id": ID, "discover": ("enum", ["known", "site", "deep"]), "hint": ("str", 40)}},
    # ---- built in H1b now
    "consumables": {"min": 60, "fields": {"id": ID, "class": ("enum", ["potion", "food", "scroll", "throwable", "tool"]), "base_type": ("ref", ["itemtype"]),
                    "label": ("str", 40), "depth": ("int", 0, 5), "effect": ("str", 60, 6)},
                    "sub": {"class": {"potion": 20, "food": 12, "scroll": 10, "throwable": 10, "tool": 8}}},
    "spell_parts": {"min": 37, "fields": {"id": ID, "pick": ("enum", ["verb", "shape", "element", "modifier"]), "name": ("str", 30)},
                    "sub": {"pick": {"verb": 12, "shape": 7, "element": 10, "modifier": 8}}},
    "wands": {"min": 12, "fields": {"id": ID, "tier": ("int", 1, 4), "depth": ("int", 0, 5),
              "spells": ("list", ("obj", {"verb": ("ref", ["spell_parts"]), "shape": ("ref", ["spell_parts"]), "element": ("ref", ["spell_parts"]),
                                          "modifier": ("ref", ["spell_parts"])}), 1, 4)}},
    "creatures": {"min": 128, "fields": {"id": ID, "kind": ("ref", ["monster"]), "tier": ("enum", TIERS), "faction": ("ref", ["factions"]),
                  "theme": ("enum", THEMES), "equipment": ("list", ("ref", ["itemtype"]), 0, 3), "name": ("str", 40)},
                  "sub": {"tier": {"fodder": 32, "regular": 48, "elite": 32, "champion": 16}}},
    "bosses": {"min": 3, "fields": {"id": ID, "kind": ("ref", ["monster"]), "tier": ("enum", ["boss"]), "faction": ("ref", ["factions"]),
               "name": ("str", 40), "phase_note": ("str", 80)}},
    "npcs": {"min": 14, "fields": {"id": ID, "role": ("enum", NPC_ROLES), "kind": ("ref", ["monster"]), "faction": ("ref", ["factions"]),
             "talk_rows": ("list", ("ref", ["history", "quests"]), 2, 8), "name": ("str", 40)}},
    "factions": {"min": 4, "fields": {"id": ID, "name": ("str", 40), "seat_room_kind": ("str", 40), "stance": ("map", ("ref", ["factions"]), ("int", -2, 2))}},
    "quests": {"min": 8, "fields": {"id": ID, "kind": ("enum", ["cure", "key", "rescue", "delivery", "flood", "wall", "beast", "chronicle"]),
               "giver": ("ref", ["npcs"]), "steps": ("list", ("obj", {"pred": ("enum", QUEST_PREDICATES), "args": ("list", ("str", 40), 0, 6)}), 1, 8),
               "reward": ("ref", ["itemtype"]), "text": ("str", 200, 20)}},
    "history": {"min": 60, "fields": {"id": ID, "year": ("int", -400, -1), "kind": ("str", 20), "template": ("ref", ["history_templates"]),
                "actors": ("list", ("ref", ["factions", "npcs"]), 1, 3), "place": ("str", 20), "text": ("str", 280, 40)}},
    "history_templates": {"min": 12, "fields": {"id": ("id",), "text": ("str", 120, 10)}},
    "secrets": {"min": 12, "fields": {"id": ID, "kind": ("enum", ["room", "recipe", "spell"]), "ref": ("str", 40), "hint": ("ref", ["history"])},
                "sub": {"kind": {"room": 6, "recipe": 3, "spell": 3}}},
    "weather": {"min": 3, "fields": {"id": ("enum", ["clear", "rain", "storm"]), "light_pct": ("int", 0, 100), "wet_per_step": ("int", 0, 10), "note": ("str", 120)}},
    "ecology": {"min": 8, "fields": {"id": ID, "species": ("enum", SPECIES + ["fungus"]), "behavior": ("str", 60), "den_theme": ("enum", THEMES)}},
    # ---- D73: haven merchants, one trade each, materials and services, not finished products
    "merchants": {"min": 5, "fields": {"id": ID, "trade": ("enum", TRADES), "npc": ("ref", ["npcs"]), "floor": ("int", 1, 4), "purse_gold": ("int", 50, 5000),
                  "buy_pct": ("int", 10, 50), "buys": ("enum", TRADES),
                  "sells": ("list", ("obj", {"kind": ("enum", SELL_KINDS), "ref": ("str", 40)}), 2, 12)}},
    # ---- set pieces (gameplay/20 sections 2.1 and 6): fixed scenes per level, apx-gen places them
    "set_pieces": {"min": 10, "fields": {"id": ID, "level": ("enum", ["surface", "floor1", "floor2", "floor3", "floor4", "caves"]),
                   "room_kind": ("str", 40), "station": ("str", 30), "parts": ("list", ("str", 30), 2, 8), "note": ("str", 160, 10)}},
}

# text of every class is scanned for these tokens (G6); no Prime Directive sweep tool exists (card
# Stop and ask 4: the token list is kept and flagged weaker)
BANNED_TOKENS_FILE = "tests/apx/banned_tokens.txt"
