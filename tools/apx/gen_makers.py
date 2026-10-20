"""Generate data/apx/makers.json from PALIMPSEST spec 11 section 5.2 (the 40 manufacturers; canon).

05b C9 (Apocky, 2026-10-19): eight makers are active in v1 -- Deepanvil Forge, Silverbough Fletchery, The
Runecourt, Vesper & Sons, Ashgrave Ironworks, Hephaesteum, The Outer Choir (cult), Dunwold Expedition
Supply (caravan). The other 32 stay in the data, inactive.

Each row keeps PALIMPSEST's text (makes, tendencies, parts, gimmick, dependency, identity) verbatim as data and
adds a "barony" port for the active eight: which Barony families it stamps (from PALIMPSEST's class codes,
ASSUMED mapping below), its mapped tendencies, and its gimmick as Barony effects. Every mapping choice is
ASSUMED and listed in "_mapping" so a reader can see what was translated and what was dropped.

Usage: python tools/apx/gen_makers.py <path to 11_ITEMS_LOOT_ECONOMY.md>
"""
import json, re, sys
from pathlib import Path

ACTIVE = {
    "mfr.mythic_fantasy.deepanvil": {"gimmick": [{"kind": "dmg_pct", "value": 15, "on": "weapon"}, {"kind": "ac_flat", "value": 1, "on": "armor"}],
        "port": "poise damage x1.5 and attack speed x0.9 -> weapons +15 % damage; armor and shields +4 poise -> +1 AC per piece"},
    "mfr.mythic_fantasy.silverbough": {"gimmick": [{"kind": "dex", "value": 1, "on": "any"}],
        "port": "attack noise -20 dB, footstep noise -6 dB (Barony has no noise model) -> +1 DEX per equipped piece"},
    "mfr.mythic_fantasy.runecourt": {"gimmick": [{"kind": "mp_on_kill", "value": 3, "on": "weapon"}],
        "port": "Spellsink: every kill restores 3 aether -> every kill with the weapon restores 3 MP"},
    "mfr.gothic_horror.vesper_sons": {"gimmick": [{"kind": "undead_pct", "value": 20, "on": "weapon"}],
        "port": "2000 Bp of each hit becomes radiant (Barony has no radiant) -> +20 % damage against undead"},
    "mfr.gothic_horror.ashgrave": {"gimmick": [{"kind": "low_hp_dmg_pct", "value": 20, "on": "weapon"}, {"kind": "low_hp_ac", "value": 2, "on": "armor"}],
        "port": "Grave Iron: at HP <= 30 % armor AR x1.3 and weapons +2000 Bp -> at HP <= 30 %: weapons +20 % damage, armor +2 AC per piece"},
    "mfr.mythic_antiquity.hephaesteum": {"gimmick": [{"kind": "no_wear", "value": 1, "on": "any"}],
        "port": "the bearer is immune to disarm and corrosion -> Hephaesteum items never lose condition from wear"},
    "mfr.cosmic_horror.outer_choir": {"gimmick": [{"kind": "dmg_pct_mp", "value": 15, "on": "weapon"}],
        "port": "2500 Bp of each hit becomes psychic, +1500 Bp, each attack costs 1 composure, none at 0 -> +15 % damage, each hit costs 1 MP, no bonus at 0 MP"},
    "mfr.cosmic_horror.dunwold": {"gimmick": [{"kind": "con", "value": 1, "on": "armor"}],
        "port": "Field-Tested: weapons reload while sprinting, armor +300 Bp acid/toxin resist (Barony has no reload or resist columns) -> +1 CON per armor piece"},
}
# PALIMPSEST class codes -> Barony families (ASSUMED; guns RG/RH and ward generators WG have no Barony family until C11)
CLASS_FAMILIES = {
    "ML": ["Daggers", "Fists"], "MM": ["Blades", "Hafts", "Polearms", "Staves"], "MH": ["Hafts", "Polearms"],
    "RD": ["Bows", "Crossbows", "Thrown"], "RG": [], "RH": [], "CA": ["Staves", "Tools"],
    "AR": ["Armour", "Helms", "Boots", "Gloves", "Cloth"], "SH": ["Shields"], "WG": [],
}
# PALIMPSEST tendency letters -> Barony (ASSUMED). Mapped: D weapon damage %, A AC (+1 per 1000 Bp), W weight %, V value %.
TEND_MAP = {"D": "dmg_pct (delta / 100)", "A": "ac_flat (delta / 1000, per armor piece)", "W": "weight_pct (delta / 100)", "V": "value_pct (delta / 100)"}
TEND_DROPPED = {"S": "attack speed", "P": "spread", "M": "magazine", "L": "reload ticks", "R": "range", "H": "handling ticks",
                "T": "stamina cost", "O": "poise damage", "X": "precision multiplier", "C": "aether cost"}


def main(src):
    lines = Path(src).read_text(encoding="utf-8").splitlines()
    rows, ident = [], {}
    for ln in lines:
        m = re.match(r"^\| `(mfr\.[a-z_]+\.[a-z_]+)` \| ([^|]+) \| ([^|]+) \| ([^|]+) \| ([^|]+) \| (.+) \| ([a-z]+) \|$", ln)
        if m:
            rows.append(m.groups())
            continue
        m = re.match(r"^\| `(mfr\.[a-z_]+\.[a-z_]+)` \| ([^|]+) \|$", ln)
        if m:
            ident[m.group(1)] = m.group(2).strip()
    seen, makers = set(), []
    for mid, name, makes, tend, parts, gim, dep in rows:
        if mid in seen:
            continue
        seen.add(mid)
        tendencies = {}
        for t in re.findall(r"([A-Z])([+\u2212-])(\d+)", tend):
            tendencies[t[0]] = int(t[2]) * (-1 if t[1] in "-\u2212" else 1)
        classes = makes.split() if "only" not in makes and "all" not in makes else []
        row = {"id": mid, "name": name.strip(), "makes": makes.strip(), "classes": classes, "tendencies": tendencies,
               "parts": parts.split() if parts.strip() not in ("none", "all") else [], "gimmick_text": gim.strip(),
               "dependency": dep, "identity": ident.get(mid, ""), "active": mid in ACTIVE}
        if mid in ACTIVE:
            fams = sorted({f for c in classes for f in CLASS_FAMILIES.get(c, [])})
            row["barony"] = {"families": fams, "gimmick": ACTIVE[mid]["gimmick"], "port": ACTIVE[mid]["port"],
                             "tendencies": {k: v for k, v in tendencies.items() if k in TEND_MAP}}
        makers.append(row)
    missing = set(ACTIVE) - seen
    if missing:
        sys.exit(f"active makers not found in the table: {sorted(missing)}")
    out = {
        "_note": "Generated by tools/apx/gen_makers.py from PALIMPSEST spec 11 section 5.2 (40 manufacturers, canon). "
                 "Maker id in a part = index in this list + 1 (0 = none). Only 'active' makers roll (05b C9, Apocky 2026-10-19).",
        "_mapping": {"class_families": CLASS_FAMILIES, "tendencies_mapped": TEND_MAP, "tendencies_dropped": TEND_DROPPED,
                     "status": "ASSUMED (Claude, 2026-10-19); Barony lacks noise, poise, stamina, reload and resist columns"},
        "foreign_pct": 75,
        "foreign_allowance_by_rarity": [0, 1, 2, 3, 3, 3],
        "makers": makers,
    }
    dst = Path(__file__).resolve().parents[2] / "data" / "apx" / "makers.json"
    dst.write_text(json.dumps(out, indent=1, ensure_ascii=True) + "\n", encoding="utf-8", newline="\n")
    print(f"{dst}: {len(makers)} makers, {sum(m['active'] for m in makers)} active")


if __name__ == "__main__":
    main(sys.argv[1] if len(sys.argv) > 1 else r"C:\Users\Apocky\source\repos\bench-wt-1\docs\palimpsest\spec\11_ITEMS_LOOT_ECONOMY.md")
