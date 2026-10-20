# barony-apx -- agent boot card

Private fork of TurningWheel/Barony carrying the Apockalypsis design (D48-D51). Specs and plan live in
Apockalypsis `specs/barony-fork/00-09`; this repo holds code only. Build and gate: `tools/apx/README.md`.
Never commit Barony assets (models, images, sound, music, maps, data from the Steam install) or game
screenshots; runs read assets from the Steam install and write evidence to C:\Apocrypha\diagnostics.

## SELF (rewritten by the agent each turn; in-flight state only)

- Task: D93 combat lane, phase C1 (apx::combat). Branch apx/combat, worktree ..arony-apx-combat, never merged to apx-main by this lane.
- State (2026-10-19 23:10): core built and compiling (src/apx/combat/core.cpp, data/apx/combat/moves.json, apx::in stub). -apxselftest=all
  7/7 PASS incl. the moves stage through the core (40 swings, every family, cost = table) and f4c/d74 rewritten onto applyHit. NOT yet done:
  QA-driver real-input gate (normal start and -apxfloor=1), screenshots, perf at 150 creatures, control runs (APX_COMBAT_OLD=1).
- Next: tools/apx/combat_qa.py + tools/apx/qa/combat_*.txt, run gate, JSONL + baseline to C:\Apocrypha\diagnostics\combat\, commit, push, ls-remote.
- Traps:
  1. Source files here are CRLF: patch with the scratchpad ed.py helper or Edit; the Bash heredoc eats backslash-n in C strings.
  2. apx::combat::enabled() is the impact rule's switch (impact.cpp); the core's is coreEnabled().
  3. ninja-sccache build mode fails with a shared PDB (C1041); toolspxuild.ps1 default (vs) builds in ~1-4 min.
  4. The old F4d forced-swing scripts (APX_SELFTEST_FAMILIES/ENDURANCE/NET) only run as the APX_COMBAT_OLD=1 control now.
