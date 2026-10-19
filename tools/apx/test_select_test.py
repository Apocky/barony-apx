#!/usr/bin/env python3
"""Offline test for tools/apx/test_select.py (no game, under a second).

    python tools/apx/test_select_test.py                      # must print PASS
    APX_TEST_SELECT_BREAK=1 python tools/apx/test_select_test.py   # control: must print FAIL
"""
import os
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import test_select  # noqa: E402

CASES = [
    # (changed files, expected checks in order)
    (["README.md"], []),
    (["tools/apx/farm.py", "tools/apx/README.md"], []),
    (["src/apx_combat.hpp"], ["selftest_all"]),
    (["src/entity.cpp", "src/apx/tiers.cpp"], ["selftest_all"]),
    (["src/maps.cpp"], ["selftest_all", "golden_parity", "floor_hash"]),
    (["src/prng.hpp"], ["selftest_all", "golden_parity", "floor_hash"]),
    (["src/apx/gen/src/generate.cpp"], ["selftest_all", "golden_parity", "floor_hash"]),
    (["src/apx/apx_gen_bridge.cpp"], ["selftest_all", "golden_parity", "floor_hash", "host_client"]),
    (["src/net.cpp"], ["selftest_all", "host_client"]),
    (["src/apx_stamina.hpp"], ["selftest_all", "host_client"]),
    (["src/scores.cpp"], ["selftest_all", "save_load"]),
    (["src/apx_inventory.cpp"], ["selftest_all", "save_load"]),
    (["CMakeLists.txt"], ["selftest_all", "golden_parity"]),
    (["tools/apx/build.ps1"], ["selftest_all", "golden_parity"]),
    (["src\\maps.cpp"], ["selftest_all", "golden_parity", "floor_hash"]),  # Windows separators
    (["src/net.cpp", "src/scores.cpp", "src/maps.cpp", "docs/x.md"],
     ["selftest_all", "golden_parity", "floor_hash", "host_client", "save_load"]),
]


def main():
    fails = 0
    for files, want in CASES:
        got = test_select.select(files)["checks"]
        if got != want:
            fails += 1
            print(f"FAIL {files}: got {got}, want {want}")
    # reasons name the files that triggered each check
    r = test_select.select(["src/net.cpp", "src/maps.cpp"])["reasons"]
    if r.get("host_client") != ["src/net.cpp"] or r.get("floor_hash") != ["src/maps.cpp"]:
        fails += 1
        print(f"FAIL reasons: {r}")
    print(f"{'PASS' if fails == 0 else 'FAIL'}: {len(CASES) + 1 - fails}/{len(CASES) + 1} cases")
    return 1 if fails else 0


if __name__ == "__main__":
    sys.exit(main())
