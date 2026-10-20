
## BF1-1 (2026-10-18 session f1-instruments)
In flight: BF1-1 writer + floor rows on branch apx/f1-instruments (worktree ..\barony-apx-f1). Code written (src/apx/telemetry.*, hooks in game.cpp/maps.cpp); first build running. Resume: pwsh tools\apx\build.ps1; python tools\apx\apx_check.py floors --exe build\vs2022\Release\barony.exe. Next: pass gate, commit, BF1-2 floor hash.

## 2026-10-18 23:05 F2 integrate (session local f2-integrate)
In flight: apx-gen vendored at src/apx/gen (527a535), bridge src/apx/apx_gen_bridge.cpp, hook maps.cpp generateDungeon, -apxgen flag. Build: tools\apx\build.ps1 -BuildDir build\vs2022-f2. Next: run floor-1 with -apxgen, then W1/W2/W5 gates, push apx/f2-integrate, D46 sheet.


## 2026-10-19 03:31:07 -07:00 F2 integrate: gates done on 36d9cf06 (W2 50/50, control 4/4 fail, W5 0/12 differ, host=client hashes at 128/96, W1 measured). Branch apx/f2-integrate pushed, NOT merged. Next: apx-gen small floors (48-80), dress fixtures beyond table/bench, boss per band.


## 2026-10-19 06:33 -07:00 F2: rebased (801cf254), apx-gen 7057f3d9, gates PASS on db9ca6c8; merge-ready pending Apocky/parent OK.

## 2026-10-19 22:34 -07:00 D93 Phase M1 (`apx::move`)
In flight: Implementing Phase M1 (`src/apx/move/`, `src/apx/in/in.hpp` stub, `data/apx/move/move.json`, `tests/apx/test_move.cpp`, `actplayer.cpp` & creature impulse glue, `-apxmovegate` gate) on branch `apx/move` in `C:\Users\Apocky\source\repos\barony-apx-move`.
Resume: build and run `tests/apx/test_move.cpp`, then `pwsh -NoProfile -File tools\apx\build.ps1 -Cache ccache`, then run `-apxmovegate` under a slot lock.
Next: read remaining lines of `handlePlayerMovement` and `handlePlayerCameraUpdate` in `src/actplayer.cpp`, write `src/apx/in/in.hpp`, `src/apx/move/move.hpp`, `data/apx/move/move.json`, and `tests/apx/test_move.cpp`.


