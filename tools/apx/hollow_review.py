"""hollow_review.py -- turn data/apx/hollow/*.json into plain-English review pages for Apocky.

    python tools/apx/hollow_review.py <out_dir>
Writes HOLLOW-REVIEW-1-WORLD.md (factions, people, trade, quests, places) and
HOLLOW-REVIEW-2-LORE-AND-THINGS.md (history, creatures, consumables, magic). ASCII only.
Each entry shows its id in brackets so a note can point back at the exact row.
"""
import json
import sys
from collections import defaultdict
from pathlib import Path

DATA = Path(__file__).resolve().parents[2] / "data" / "apx" / "hollow"


def load(name):
    return json.loads((DATA / f"{name}.json").read_text(encoding="utf-8"))


def ascii_(s):
    return str(s).encode("ascii", "replace").decode("ascii")


def words(s):
    return ascii_(str(s).replace("_", " "))


def world():
    out = ["# The Hollow, content review 1 of 2: world, people, trade, quests, places", "",
           "Placeholder-grade text written by an agent. Mark anything off-tone, off-canon or against the",
           "Prime Directive; quote the [id] so it can be fixed. STUB means waiting on parts gear or alchemy.", ""]
    factions = load("factions")
    names = {f["id"]: f["name"] for f in factions}
    out += ["## Factions", ""]
    for f in factions:
        st = ", ".join(f"{names.get(k, k)} {v:+d}" for k, v in f["stance"].items() if k != f["id"])
        out.append(f"- **{ascii_(f['name'])}** [{f['id']}] -- seat: {words(f['seat_room_kind'])}. Feelings toward others: {st}.")
    talk = {h["id"]: h["text"] for h in load("history")}  # NPC talk rows recount history entries
    out += ["", "## People (NPCs)", ""]
    for n in load("npcs"):
        out.append(f"- **{ascii_(n['name'])}** [{n['id']}] -- {words(n['role'])}, {words(n['kind'])}, of {names.get(n['faction'], words(n['faction']))}.")
        for t in n.get("talk_rows", []):
            out.append(f"  - says [{t}]: \"{ascii_(talk.get(t, '(missing line ' + t + ')'))}\"")
    out += ["", "## Bosses", ""]
    for b in load("bosses"):
        out.append(f"- **{ascii_(b['name'])}** [{b['id']}] -- {words(b['kind'])}, tier {b['tier']}, of {names.get(b['faction'], words(b['faction']))}. {ascii_(b['phase_note'])}")
    out += ["", "## Merchants (D73: one trade each, buy only their trade, no finished goods)", ""]
    for m in load("merchants"):
        sells = "; ".join(f"{words(s['kind'])}: {words(s['ref'])}" for s in m["sells"])
        out.append(f"- **{words(m['trade'])}** [{m['id']}] on floor {m['floor']}, kept by {words(m['npc'])}. Sells {sells}. "
                   f"Buys {words(m['buys'])} at {m['buy_pct']}% of value, purse {m['purse_gold']} gold.")
    out += ["", "## Quests", ""]
    for q in load("quests"):
        steps = "; then ".join(f"{words(s['pred'])} {' '.join(words(a) for a in s['args'])}".strip() for s in q["steps"])
        out.append(f"- [{q['id']}] from {words(q['giver'])} ({words(q['kind'])}): \"{ascii_(q['text'])}\" Steps: {steps}. Reward: {words(q['reward'])}.")
    out += ["", "## Secrets", ""]
    for s in load("secrets"):
        out.append(f"- [{s['id']}] {words(s['kind'])} ({words(s['ref'])}): hint \"{ascii_(s['hint'])}\"")
    out += ["", "## Set pieces", ""]
    for p in load("set_pieces"):
        st = f", station {words(p['station'])}" if p.get("station") else ""
        out.append(f"- [{p['id']}] {words(p['room_kind'])} on {words(p['level'])}{st}: {ascii_(p['note'])} (parts: {', '.join(words(x) for x in p['parts'])})")
    out += ["", "## Weather", ""]
    for w in load("weather"):
        out.append(f"- [{w['id']}] light {w['light_pct']}%, wetness {w['wet_per_step']} per step: {ascii_(w['note'])}")
    out += ["", "## Ecology (who lives where)", ""]
    for e in load("ecology"):
        out.append(f"- [{e['id']}] {words(e['species'])} in {words(e['den_theme'])} dens: {words(e['behavior'])}")
    return out


def lore():
    out = ["# The Hollow, content review 2 of 2: history, creatures, consumables, magic", ""]
    out += ["## History (in year order)", ""]
    for h in sorted(load("history"), key=lambda x: x["year"]):
        out.append(f"- Year {h['year']}, {words(h['place'])} [{h['id']}]: {ascii_(h['text'])}")
    out += ["", "## Creatures (grouped by kind; tiers fodder / regular / elite / champion)", ""]
    by = defaultdict(list)
    for c in load("creatures"):
        by[c["kind"]].append(c)
    for kind in sorted(by):
        rows = ", ".join(f"{ascii_(c['name'])} ({c['tier']}, {words(c['theme'])})" for c in by[kind])
        out.append(f"- **{words(kind)}**: {rows}")
    out += ["", "## Consumables", ""]
    for c in sorted(load("consumables"), key=lambda x: (x["class"], x["depth"])):
        out.append(f"- [{c['id']}] **{ascii_(c['label'])}** ({words(c['class'])}, from depth {c['depth']}): {ascii_(c['effect'])}")
    out += ["", "## Spell parts", ""]
    for s in load("spell_parts"):
        out.append(f"- [{s['id']}] {ascii_(s['name'])} ({words(s['pick'])})")
    out += ["", "## Wands", ""]
    for w in load("wands"):
        sp = "; ".join(f"{words(s['verb'])} {words(s['element'])} as {words(s['shape'])} ({words(s['modifier'])})" for s in w["spells"])
        out.append(f"- [{w['id']}] tier {w['tier']}, depth {w['depth']}: {sp}")
    stubs = [c["file"] for c in load("manifest")["classes"] if c.get("stub")]
    out += ["", "## Waiting (STUB until parts gear or alchemy merge)", "", "- " + ", ".join(stubs)]
    return out


def main():
    od = Path(sys.argv[1]); od.mkdir(parents=True, exist_ok=True)
    for name, lines in (("HOLLOW-REVIEW-1-WORLD.md", world()), ("HOLLOW-REVIEW-2-LORE-AND-THINGS.md", lore())):
        text = "\n".join(lines) + "\n"
        (od / name).write_text(text, encoding="ascii")
        print(name, len(text.encode("ascii")), "bytes")


if __name__ == "__main__":
    main()
