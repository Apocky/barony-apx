"""gen_enum_lists.py -- tests/apx/monstertypes.txt and tests/apx/itemtypes.txt from the engine enums
(src/monster.hpp `enum Monster`, src/items.hpp `enum ItemType`), one name per line, enum order.
    python -I tools/apx/gen_enum_lists.py
"""
import re
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]


def enum_names(path, start_pat, stop_pat):
    text = Path(path).read_text(encoding="utf-8", errors="replace")
    i = re.search(start_pat, text).end()
    j = text.index(stop_pat, i)
    body = re.sub(r"//[^\n]*", "", text[i:j])
    body = re.sub(r"/\*.*?\*/", "", body, flags=re.S)
    names = []
    for tok in body.replace("{", ",").split(","):
        tok = tok.strip().split("=")[0].strip()
        if re.fullmatch(r"[A-Z][A-Z0-9_]*", tok):
            names.append(tok)
    return names


def main():
    mons = [n for n in enum_names(ROOT / "src/monster.hpp", r"enum Monster\s*:\s*int", "}") if n not in ("NOTHING", "MAX_MONSTER")]
    items = [n for n in enum_names(ROOT / "src/items.hpp", r"typedef enum ItemType", "}") if n != "ITEM_ENUM_MAX"]
    out = ROOT / "tests/apx"
    out.mkdir(parents=True, exist_ok=True)
    (out / "monstertypes.txt").write_text("\n".join(mons) + "\n", encoding="ascii", newline="\n")
    (out / "itemtypes.txt").write_text("\n".join(items) + "\n", encoding="ascii", newline="\n")
    print(f"monstertypes {len(mons)}, itemtypes {len(items)}")


if __name__ == "__main__":
    main()
