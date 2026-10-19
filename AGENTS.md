# barony-apx -- agent boot card

Private fork of TurningWheel/Barony carrying the Apockalypsis design (D48-D51). Specs and plan live in
Apockalypsis `specs/barony-fork/00-09`; this repo holds code only. Build and gate: `tools/apx/README.md`.
Never commit Barony assets (models, images, sound, music, maps, data from the Steam install) or game
screenshots; runs read assets from the Steam install and write evidence to C:\Apocrypha\diagnostics.

## SELF (rewritten by the agent each turn; in-flight state only)

- Task: slice F2 integrate (apx-gen inside barony-apx behind -apxgen). Side session, 2026-10-18/19.
- Branch apx/f2-integrate (worktree ..\barony-apx-f2, build dir build\vs2022-f2), NOT merged to apx-main.
- State: MERGE-READY 76477d6d on apx-main 73d03aeb, apx-gen fc30a151, D72 packs. Gates (build a90e21c3): W2 150/150 at 80/96/128, controls fail, W5 0 differ, host=client, D72 162/162, fallbacks 0. Goldens C:\Apocrypha\diagnostics\barony-f2\goldens-a90e21c3.json.
  in-engine, control 4/4 fail, W5 12 seeds x 2 launches 0 differ, W1 measured (noisy machine).
  Host+client on f50ee3b4: same hash at 128 and 96. Sheet: C:\Apocrypha\diagnostics\barony-f2\sheet.
- Next: host+client on 36d9cf06 with a switchless client (adopts the host's switches via STRT bytes
  17..24); apx-gen small floors (0/50 seeds at 48-64 in apx-gen 527a535); dress fixtures beyond
  table/bench; boss per band.
- Traps:
  1. apx-gen door_x/z is the room centre; Lock rooms are sealed on their box ring by the bridge.
  2. Dungeon level 5 is the fixed minetoswamp map: sweeps run 4 floors per process.
  3. Other lanes launch Barony without honouring C:\Apocrypha\locks; gate_f2.py backs off and retries.
  4. The Bash tool heredoc turns "\\n" in Python patch strings into newlines; patch C strings with Edit.
  5. Frame cap 60 fps: measure with /apxfps_begin uncap.
