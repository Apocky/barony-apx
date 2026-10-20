
## BF1-1 (2026-10-18 session f1-instruments)
In flight: BF1-1 writer + floor rows on branch apx/f1-instruments (worktree ..\barony-apx-f1). Code written (src/apx/telemetry.*, hooks in game.cpp/maps.cpp); first build running. Resume: pwsh tools\apx\build.ps1; python tools\apx\apx_check.py floors --exe build\vs2022\Release\barony.exe. Next: pass gate, commit, BF1-2 floor hash.

## 2026-10-18 23:05 F2 integrate (session local f2-integrate)
In flight: apx-gen vendored at src/apx/gen (527a535), bridge src/apx/apx_gen_bridge.cpp, hook maps.cpp generateDungeon, -apxgen flag. Build: tools\apx\build.ps1 -BuildDir build\vs2022-f2. Next: run floor-1 with -apxgen, then W1/W2/W5 gates, push apx/f2-integrate, D46 sheet.


## 2026-10-19 03:31:07 -07:00 F2 integrate: gates done on 36d9cf06 (W2 50/50, control 4/4 fail, W5 0/12 differ, host=client hashes at 128/96, W1 measured). Branch apx/f2-integrate pushed, NOT merged. Next: apx-gen small floors (48-80), dress fixtures beyond table/bench, boss per band.


## 2026-10-19 06:33 -07:00 F2: rebased (801cf254), apx-gen 7057f3d9, gates PASS on db9ca6c8; merge-ready pending Apocky/parent OK.

<<<<<<< HEAD

## 2026-10-19 23:10 -07:00 COMBAT lane C1 (branch apx/combat, worktree ..arony-apx-combat)
In flight: D93 combat core built, selftest-all 7/7 PASS. Resume: build toolspxuild.ps1; next write toolspx\combat_qa.py (QA-driver real-input gate, APX_COMBAT_QA=<family>[:crowd]), run on normal start and -apxfloor=1, commit, push, ls-remote.

## 2026-10-20 00:30 -07:00 COMBAT lane C1 done
Nothing in flight. apx/combat pushed (gate PASS, evidence C:\Apocrypha\diagnostics\combatinal). Resume: C2 = block, timed block, riposte, chamber, dodge, feint, kick, bash on apx::combat (04b section 8); when apx/input merges, delete src/apx/in/in_stub.cpp and take its in.hpp.
=======
## 2026-10-19 22:32 -07:00 D93 Phase C0 (apx::in input lane)
In flight: Building Phase C0 (`src/apx/in/`, `data/apx/input/actions.json`, `data/apx/input/bindings.json`, `tests/apx/test_in.cpp`, engine glue in `src/game.cpp`, `src/apx/qadriver.*`) on branch `apx/input` in `C:\Users\Apocky\source\repos\barony-apx-input` (from `951f805e`). Resume: run offline `test_in.cpp`, then `pwsh -NoProfile -File tools\apx\build.ps1 -Cache ccache` and C0 gate to `C:\Apocrypha\diagnostics\input\`. Next: write `src/apx/in/in.hpp`, `src/apx/ui/ui.hpp` stub, `data/apx/input/*.json`, and `tests/apx/test_in.cpp`.



## 2026-10-20 D93 C0 DONE: apx::in on apx/input, gate PASS (c0-gate-2). Nothing in flight; next lane step M1 after merge.
## 2026-10-19 22:34 -07:00 D93 Phase M1 (`apx::move`)
In flight: Implementing Phase M1 (`src/apx/move/`, `src/apx/in/in.hpp` stub, `data/apx/move/move.json`, `tests/apx/test_move.cpp`, `actplayer.cpp` & creature impulse glue, `-apxmovegate` gate) on branch `apx/move` in `C:\Users\Apocky\source\repos\barony-apx-move`.
Resume: build and run `tests/apx/test_move.cpp`, then `pwsh -NoProfile -File tools\apx\build.ps1 -Cache ccache`, then run `-apxmovegate` under a slot lock.
Next: read remaining lines of `handlePlayerMovement` and `handlePlayerCameraUpdate` in `src/actplayer.cpp`, write `src/apx/in/in.hpp`, `src/apx/move/move.hpp`, `data/apx/move/move.json`, and `tests/apx/test_move.cpp`.



## 2026-10-19 hostility lane (apx/hostility)
In flight: nothing after this commit. apx::hostility (src/apx/hostility.cpp, data/apx/combat/hostility.json), gate tools/apx/hostility_gate.py. Resume: python tools/apx/hostility_gate.py --exe build/vs2022/Release/barony.exe. Next: COMBAT lane routes applyHit through apx::hostility::onHit; decide post-calm hits on a standing player.

## 2026-10-19 23:46 -07:00 D93 M1 (`apx::move`) done on apx/move
Nothing in flight. Gate: `python tools/apx/move_gate.py --exe build/vs2022/Release/barony.exe` (normal + floor1, new + control APX_MOVE_OLD=1) PASS; results C:\Apocrypha\diagnostics\move\ (summary-20261019-234124.json, baseline.json). Offline units `pwsh tools/apx/move_test.ps1` (44 checks). selftest-all 7/7, golden parity 3/3.
Open: src/apx/in/in.hpp+in.cpp are a STUB of apx::in (same Button names as branch apx/input); on merge take the INPUT lane's in.hpp, delete src/apx/in/in.cpp and its CMake line. Not covered by the gate: swim, levitate, crawl states, slippery/grease tiles, co-op (remote players keep dead reckoning), creature impulses (code path in frame(), no gate row).
>>>>>>> origin/apx-main
