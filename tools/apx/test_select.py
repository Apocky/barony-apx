#!/usr/bin/env python3
"""tools/apx/test_select.py -- pick the checks a change needs (D77: test by risk).

    python tools/apx/test_select.py [--base origin/apx-main] [--head HEAD] [--json]

Lists the files changed between the merge base of --base and --head (git diff --name-only base...head)
and maps them to checks:

  selftest_all   any game code or build change: tools/apx/selftest_all.ps1 (one launch, every in-game self-test)
  golden_parity  generation, RNG or build changes: tools/apx/golden_parity.ps1 (floor-1 lines vs F0 vanilla)
  floor_hash     generation or RNG changes: tools/apx/headless_gate.py (floor hash, 10 seeds, windowed = headless)
  host_client    net code changes: tools/apx/gate_f2.py mp (host and client in a LAN lobby agree)
  save_load      save format changes: tools/apx/u1_selftest.ps1 (save/load round trip, windowed UI)
  input_gate     play input (src/apx/in, qadriver, data/apx/input): tools/apx/tests/run_in_units.ps1 + tools/apx/input_gate.py all

Docs-only and tools-only changes need no game launch. The rules are data (RULES below); the offline test
tools/apx/test_select_test.py checks them. APX_TEST_SELECT_BREAK=1 drops the generation rule (the control:
that test must then fail).
"""
import argparse
import fnmatch
import json
import os
import subprocess
import sys

# (check, reason, patterns). Patterns are fnmatch globs on repository paths with forward slashes.
RULES = [
    ("floor_hash", "generation or RNG", [
        "src/maps.cpp", "src/maps.hpp", "src/prng.cpp", "src/prng.hpp",
        "src/apx/gen/*", "src/apx/apx_gen_bridge.*", "src/mapgen*", "src/editor.cpp",
    ]),
    ("golden_parity", "generation, RNG or build", [
        "src/maps.cpp", "src/maps.hpp", "src/prng.cpp", "src/prng.hpp",
        "src/apx/gen/*", "src/apx/apx_gen_bridge.*", "src/mapgen*",
        "CMakeLists.txt", "src/CMakeLists.txt", "cmake/*", "tools/apx/build.ps1", "tools/apx/fetch-deps.ps1",
    ]),
    ("host_client", "net code", [
        "src/net.cpp", "src/net.hpp", "src/lobbies.cpp", "src/lobbies.hpp", "src/interface/*net*",
        "src/ui/MainMenu.cpp", "src/apx_stamina.hpp", "src/apx/apx_gen_bridge.*", "src/steam*", "src/eos*",
    ]),
    ("save_load", "save format", [
        "src/scores.cpp", "src/scores.hpp", "src/apx_inventory.*", "src/items.hpp", "src/stat.hpp",
    ]),
    ("input_gate", "play input (apx::in, D93)", [
        "src/apx/in/*", "src/apx/qadriver.*", "data/apx/input/*", "tools/apx/input_gate.py", "src/apx/ui/*",
    ]),
]

GAME_CODE = ["src/*", "CMakeLists.txt", "cmake/*", "lang/*", "tools/apx/build.ps1", "tools/apx/fetch-deps.ps1"]
NO_LAUNCH = ["*.md", "docs/*", "tools/*", ".github/*", "*.txt"]

COMMANDS = {
    "selftest_all": r"pwsh -NoProfile -File tools\apx\selftest_all.ps1 -Exe <exe> -Headless",
    "golden_parity": r"pwsh -NoProfile -File tools\apx\golden_parity.ps1 -Exe <exe>",
    "floor_hash": r"python tools\apx\headless_gate.py --exe <exe>",
    "host_client": r"python tools\apx\gate_f2.py mp --exe <exe>",
    "save_load": r"pwsh -NoProfile -File tools\apx\u1_selftest.ps1 -Exe <exe>",
    "input_gate": r"pwsh -NoProfile -File tools\apx\tests\run_in_units.ps1 ; python tools\apx\input_gate.py --exe <exe> all",
}
ORDER = ["selftest_all", "golden_parity", "floor_hash", "host_client", "save_load", "input_gate"]


def _match(path, patterns):
    return any(fnmatch.fnmatch(path, p) for p in patterns)


def rules():
    if os.environ.get("APX_TEST_SELECT_BREAK") == "1":
        return [r for r in RULES if r[0] != "floor_hash"]
    return RULES


def select(files):
    """Pure: changed paths -> {"checks": [...], "reasons": {check: [paths]}}."""
    reasons = {}
    for raw in files:
        path = raw.replace("\\", "/").strip()
        if not path:
            continue
        game = _match(path, GAME_CODE) and not (path.endswith(".md") or path.endswith(".txt") and path != "CMakeLists.txt")
        if game:
            reasons.setdefault("selftest_all", []).append(path)
        for check, _why, patterns in rules():
            if _match(path, patterns):
                reasons.setdefault(check, []).append(path)
    checks = [c for c in ORDER if c in reasons]
    return {"checks": checks, "reasons": reasons}


def changed_files(base, head):
    out = subprocess.run(["git", "diff", "--name-only", f"{base}...{head}"], capture_output=True, text=True, check=True)
    return [line for line in out.stdout.splitlines() if line.strip()]


def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    ap.add_argument("--base", default="origin/apx-main")
    ap.add_argument("--head", default="HEAD")
    ap.add_argument("--json", action="store_true", help="print the selection as JSON")
    ap.add_argument("files", nargs="*", help="paths instead of git diff (for trying a rule)")
    a = ap.parse_args(argv)
    files = a.files or changed_files(a.base, a.head)
    sel = select(files)
    sel["files"] = len(files)
    sel["commands"] = {c: COMMANDS[c] for c in sel["checks"]}
    if a.json:
        print(json.dumps(sel, indent=1))
        return 0
    print(f"{len(files)} changed file(s) vs {a.base}")
    if not sel["checks"]:
        print("no game launch needed (docs or tools only)")
    for c in sel["checks"]:
        shown = ", ".join(sel["reasons"][c][:4]) + (" ..." if len(sel["reasons"][c]) > 4 else "")
        print(f"  {c:<14} {COMMANDS[c]}\n                 because: {shown}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
