"""gen_magic_json.py -- data/apx/magic/magic.json from the default tables in src/apx/magic/magic_core.hpp (one source of truth for
the numbers; edit the header, regenerate, then tune the JSON by hand if a balance pass wants to diverge).
    python tools/apx/gen_magic_json.py
"""
import json
import re
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
h = (ROOT / "src/apx/magic/magic_core.hpp").read_text()
V = ["bolt", "wall", "pit", "burst", "beam", "ward", "lift", "pull", "bind", "mend", "sense", "veil"]
S = ["point", "line", "cone", "sphere", "ring", "plane", "trail"]
E = ["fire", "ice", "stone", "iron", "acid", "lightning", "light", "shadow", "void", "life"]
M = ["none", "trigger", "timer", "chain", "echo", "silent", "anchored", "inverted"]


def arr(name):
    m = re.search(name + r"\[[A-Z_]+\] = \{([^}]*)\}", h)
    return [float(x.strip().rstrip("f")) for x in m.group(1).split(",")]


def conv(tok):
    return "*" if tok == "-1" else tok.split("_", 1)[1].lower()


rules = []
for m in re.finditer(r'\{ (V_\w+|-1),\s*(S_\w+|-1),\s*(E_\w+|-1),\s*(M_\w+|-1),\s*(G_\w+),\s*"([^"]*)" \}', h):
    v, s, e, mo, g, w = m.groups()
    rules.append({"verb": conv(v), "shape": conv(s), "element": conv(e), "modifier": conv(mo), "grade": "hard" if g == "G_HARD" else "soft", "why": w})
d = {
    "version": 1,
    "cost": {"base": dict(zip(V, arr("base"))), "area": dict(zip(S, arr("area"))), "affinity": dict(zip(E, arr("affinity"))),
             "factor": dict(zip(M, arr("factor"))), "nearby": 0.5, "nearby_tiles": 4},
    "concentration": {"spend": dict(zip(V, arr("spend"))), "spend_modifier": dict(zip(M, arr("spendMod"))), "regen_per_second": 8,
                      "fail_below": 20, "time_stretch": 0.5},
    "cast_time_ms": {"verb": dict(zip(V, arr("ms"))), "modifier_mul": dict(zip(M, arr("modMul")))},
    "contradiction": {"backfire_chance": 0.25, "soft_strength": 0.5, "rules": rules},
    "projectile": {"speed_units_per_tick": 4.0, "ricochet_deg": 20, "max_bounces": 3, "bounce_keep": 0.8, "life_ticks": 150, "radius_units": 2.0},
}
out = ROOT / "data/apx/magic/magic.json"
out.parent.mkdir(parents=True, exist_ok=True)
out.write_text(json.dumps(d, indent=1) + "\n")
print("wrote", out, len(rules), "contradiction rules")
