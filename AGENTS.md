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
