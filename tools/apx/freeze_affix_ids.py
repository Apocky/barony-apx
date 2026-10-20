"""05b invariant 3: affix ids are append-only. Writes tests/apx/affix_ids.txt (id, pool, name) from
data/apx/affixes.json in id order (pool order, then entry order). Run it ONLY to append: parts_test's
affix_ids_append_only gate fails when an existing row changes. Usage: python tools/apx/freeze_affix_ids.py"""
import json
from pathlib import Path

root = Path(__file__).resolve().parents[2]
a = json.loads((root / "data/apx/affixes.json").read_text(encoding="utf-8"))
rows = []
for p in a["pools"]:
    for e in p["affixes"]:
        rows.append(f"{len(rows)}\t{p.get('id', '?')}\t{e['name']}")
out = root / "tests/apx/affix_ids.txt"
old = out.read_text(encoding="utf-8").splitlines()[1:] if out.exists() else []
for i, r in enumerate(old):
    if i >= len(rows) or rows[i] != r:
        raise SystemExit(f"refusing: row {i} changed ({r!r} -> {rows[i] if i < len(rows) else 'gone'!r}); ids are append-only")
out.write_text("# 05b invariant 3: affix ids are append-only. id<TAB>pool<TAB>name. Never edit a row; append new ones.\n"
               + "\n".join(rows) + "\n", encoding="utf-8", newline="\n")
print(f"{out}: {len(rows)} ids ({len(rows) - len(old)} appended)")
