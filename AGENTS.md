<<<<<<< HEAD
# barony-apx -- agent boot card

Private fork of TurningWheel/Barony carrying the Apockalypsis design (D48-D51). Specs and plan live in
Apockalypsis `specs/barony-fork/00-09`; this repo holds code only. Build and gate: `tools/apx/README.md`.
Never commit Barony assets (models, images, sound, music, maps, data from the Steam install) or game
screenshots; runs read assets from the Steam install and write evidence to C:\Apocrypha\diagnostics.

## SELF (rewritten by the agent each turn; in-flight state only)

- Task: D93 combat lane, phase C1 (apx::combat) DONE and pushed on branch apx/combat (never merged to apx-main by this lane; coordinator merges via merge_train).
- State (2026-10-20 00:30): gate PASS (tools/apx/combat_gate.py): normal 40/40, -apxfloor=1 40/40 (+ lava seed 101 5/5, control 0 swings), p99 0.20-0.28 ms/tick at 158-182 creatures, 7 PNGs.
  -apxselftest=all 7/7 (moves stage through the core, f4c/d74 on applyHit); APX_COMBAT_OLD=1 control 7/7 on the old path. Evidence: C:\Apocrypha\diagnostics\combatinal.
- Next: coordinator review and merge; apx/input lands apx::in (delete src/apx/in/in_stub.cpp, take its in.hpp). C2 (block, riposte, dodge...) is the next phase for this lane.
- Traps:
  1. Source files here are CRLF: patch with the scratchpad ed.py helper or Edit; a Bash heredoc eats backslash-n in C strings and may turn tabs into spaces.
  2. apx::combat::enabled() is the impact rule's switch (impact.cpp); the core's is coreEnabled().
  3. ninja-sccache build mode fails with a shared PDB (C1041); toolspxuild.ps1 default (vs) works.
  4. -apxmapseed=101 starts the player swimming in lava: Barony's HUD weapon returns early there (no swing check, no stamina tick). Use seed 1234567 for screenshots.
  5. The QA script must `waitlevel 1` before commands (a slow machine reaches floor 1 after 8 s).
=======
# barony-apx -- agent boot card

Private fork of TurningWheel/Barony carrying the Apockalypsis design (D48-D51). Specs and plan live in
Apockalypsis `specs/barony-fork/00-09`; this repo holds code only. Build and gate: `tools/apx/README.md`.
Never commit Barony assets (models, images, sound, music, maps, data from the Steam install) or game
screenshots; runs read assets from the Steam install and write evidence to C:\Apocrypha\diagnostics.

## SELF (rewritten by the agent each turn; in-flight state only)

- Task: D93 phase C0 (apx::in) on branch apx/input, worktree ..arony-apx-input, build
inja-ccache. DONE, gate PASS, not merged.
- State: gate run C:\Apocrypha\diagnostics\input\c0-gate-2 (verdict.txt, rows.jsonl, baseline.json). selftest-all 7/7.
- Next: coordinator merges via merge_train; then MOVE lane (M1) reads take(); D92 window replaces src/apx/ui/ui.hpp stub.
- Traps:
  1. Bash heredoc turns "\n" and "\a" in Python strings; build such strings with chr(92). Working files are CRLF (autocrlf).
  2. The floor map has lava: gates use /enablecheats + /god.
  3. Gamepad path in apx::in is untested (no pad).
>>>>>>> origin/apx-main
