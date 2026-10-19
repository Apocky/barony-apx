# barony-apx -- agent boot card

Private fork of TurningWheel/Barony carrying the Apockalypsis design (D48-D51). Specs and plan live in
Apockalypsis `specs/barony-fork/00-09`; this repo holds code only. Build and gate: `tools/apx/README.md`.
Never commit Barony assets (models, images, sound, music, maps, data from the Steam install) or game
screenshots; runs read assets from the Steam install and write evidence to C:\Apocrypha\diagnostics.

## SELF (rewritten by the agent each turn; in-flight state only)

- Task: slice F0 (build upstream on this PC, prove parity with Steam v5.0.2). Side session of the
  barony-fork design session, 2026-10-18.
- State: F0 done. apx-main builds (CMake 4.3 + VS 2022 v143 + FMOD 2.03.15); parity matrices in
  C:\Apocrypha\diagnostics\barony-f0 (parity.jsonl, summary.json, sheet/). Seeds 1234567 and 7654321
  give the same floor-1 line in our build and barony_vanilla.exe. Evidence exe came from incremental
  builds of the committed sources; a from-scratch rebuild (separate build folder) is NOT done yet:
  at 22:15 drive C: was full (0.25 GB free; C1085 "No space left on device").
- Next: when C: has space, `build.ps1 -BuildDir build\vs2022-clean` + one parity run; then F1
  instruments (floor and encounter JSONL, floor hash, headless sim, screenshot hook).
- Traps:
  1. Never configure CMake with BARONY_DATADIR set and never build INSTALL: CMakeLists.txt then installs
     barony.exe and editor.exe INTO that folder (the Steam install; its barony.exe is Apocky's S.A.M build).
  2. Never run any Barony with the Steam install as working directory for tests: on Windows the game
     writes log.txt, config, saves to its cwd, and starting a new game deletes the current slot's save.
  3. barony_vanilla.exe outside Steam needs steam_appid.txt in its cwd (or SteamAppId), else
     SteamAPI_RestartAppIfNecessary asks Steam to launch barony.exe (S.A.M) in the install.
  4. Stock Barony reads base files through compile-time BASE_DATA_DIR "./"; only our build honours
     -datadir=/BARONY_DATADIR fully. Vanilla runs in a mirror folder (parity_f0.py builds it).
  5. Barony samples keys once per frame: posted key presses must be held (350 ms); keep the window
     uncovered (HWND_TOPMOST, no activation) or slow frames drop presses.
  6. Close Barony with WM_CLOSE; TerminateProcess left it stuck in GPU teardown for up to 2 minutes.
  7. Other sessions launch Barony too: honour *.LOCK files in the shared notes folder
     (parity_f0.py --lock-dir) and check for running Barony before every launch.
  8. Upstream .gitignore ignores *.ps1; tools/apx/*.ps1 is re-included explicitly.
  9. FMOD here is 2.03.15 (Steam ships 2.02.14); sound.hpp defines F_CALLBACK when FMOD lacks it.
