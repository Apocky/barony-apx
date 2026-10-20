
## BF1-1 (2026-10-18 session f1-instruments)
In flight: BF1-1 writer + floor rows on branch apx/f1-instruments (worktree ..\barony-apx-f1). Code written (src/apx/telemetry.*, hooks in game.cpp/maps.cpp); first build running. Resume: pwsh tools\apx\build.ps1; python tools\apx\apx_check.py floors --exe build\vs2022\Release\barony.exe. Next: pass gate, commit, BF1-2 floor hash.

## 2026-10-18 23:05 F2 integrate (session local f2-integrate)
In flight: apx-gen vendored at src/apx/gen (527a535), bridge src/apx/apx_gen_bridge.cpp, hook maps.cpp generateDungeon, -apxgen flag. Build: tools\apx\build.ps1 -BuildDir build\vs2022-f2. Next: run floor-1 with -apxgen, then W1/W2/W5 gates, push apx/f2-integrate, D46 sheet.


## 2026-10-19 03:31:07 -07:00 F2 integrate: gates done on 36d9cf06 (W2 50/50, control 4/4 fail, W5 0/12 differ, host=client hashes at 128/96, W1 measured). Branch apx/f2-integrate pushed, NOT merged. Next: apx-gen small floors (48-80), dress fixtures beyond table/bench, boss per band.


## 2026-10-19 06:33 -07:00 F2: rebased (801cf254), apx-gen 7057f3d9, gates PASS on db9ca6c8; merge-ready pending Apocky/parent OK.

## 2026-10-19 22:32 -07:00 D93 Phase C0 (apx::in input lane)
In flight: Building Phase C0 (`src/apx/in/`, `data/apx/input/actions.json`, `data/apx/input/bindings.json`, `tests/apx/test_in.cpp`, engine glue in `src/game.cpp`, `src/apx/qadriver.*`) on branch `apx/input` in `C:\Users\Apocky\source\repos\barony-apx-input` (from `951f805e`). Resume: run offline `test_in.cpp`, then `pwsh -NoProfile -File tools\apx\build.ps1 -Cache ccache` and C0 gate to `C:\Apocrypha\diagnostics\input\`. Next: write `src/apx/in/in.hpp`, `src/apx/ui/ui.hpp` stub, `data/apx/input/*.json`, and `tests/apx/test_in.cpp`.



## 2026-10-20 D93 C0 DONE: apx::in on apx/input, gate PASS (c0-gate-2). Nothing in flight; next lane step M1 after merge.
