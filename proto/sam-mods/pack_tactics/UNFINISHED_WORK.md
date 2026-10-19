# UNFINISHED_WORK (pack_tactics; append only, newest at the bottom)

## 2026-10-18 20:53 -0700 (Claude Code, Opus 5.5, side session of local_72659dda)

In flight: S-track prototype of Apockalypsis specs/barony-fork/09 section 3 (pack brain, token
approximation, crowd verbs, density knob, telemetry). Files: mod.json, main.lua, tools/sam_mock.lua,
tools/offline_check.py (offline gate, passes: 39/0 in the mock world, live session 0 errors).
Next concrete step: with Barony closed, run
  barony.exe -samtest=pack_tactics -samtesttimeout=480   (working dir = the Barony install)
read sam_log.txt for "PACK_TACTICS RESULT" and the TRIAL / TRIALSUM lines, then write
tools/pack_telemetry.py (sam_log.txt -> JSONL rows + baseline under C:\Apocrypha\diagnostics\pack_tactics).
Nothing committed: the mods folder is not a git repo and the brief says commit nothing here.

## 2026-10-18 21:31 -0700 (same session)

Done: headless self-test passes, session 25: 41 passed, 0 failed, 0 script errors, exit code 0
(barony.exe -samtest=pack_tactics -samtestfloor=1 -samtesttimeout=480). Tokens off vs on at 3x,
floor 1, 12 pairs of 20 s (sessions 23-25, MEASURED): 389.3 vs 149.2 damage per arm, ratio 2.61,
on lower in 12 of 12. Rows + baseline: C:\Apocrypha\diagnostics\pack_tactics (122 rows).
Nothing in flight. Not checked: live play by a human (real swings, the G key, how the HUD looks;
screen captures of the fullscreen GL window came back black), co-op, 5x, tactics cost per tick
(no sub-second clock in the sandbox), morale retreats in Barony (the passive test character kills
nothing; only the offline mock exercised retreats).
Next concrete step (proposed, not started): try EFF_PACIFY as the token gate (holds monsterHitTime
at HITRATE/2, keeps the target) against target clearing in the same ABAB trial.
Before any Barony launch: no barony.exe running and no barony-load.LOCK in the shared notes folder.

## 2026-10-18 21:49 -0700 (same session): D62

Apocky dropped the 3-attacker cap (D62, relayed by the parent session). Done: `token_cap` is a
CONTROL setting, default off (new id); the "Crowd: N attacking, M waiting" HUD line is deleted;
natural limits (`limits`, default on: ROOM by body size on open floor, RHYTHM 12-tick stagger + engine
wind-up + 0.5 s recovery, LINE no swing through a body) hold swings with EFF_PACIFY; flank/retreat
steering split out of the token code (it had been switched off with the cap). Offline check passes
(mock 46/0, live mock: limits act, 0 cap grants, 0 errors). Interim fix reported to the parent 21:4x.
In flight: headless run 6 waits for barony-f0-parity.LOCK (scratchpad locked_run.ps1, task b8wiphh4n),
then measures limits off vs on (4 pairs x 20 s, floor 1). Next: parse with tools/pack_telemetry.py,
report to the parent; Apocky must restart Barony to load the change.

## 2026-10-18 22:14 -0700 (same session)

Run 6 (session 31) void: someone's key presses reached the test window (146 presses, 11 kills);
session 22 void for the same reason. The self-test now fails on any input during the trial.
Clean sessions 32-33: limits off 445.4 vs on 276.1 damage per 20 s arm, ratio 1.61, 8 of 8 pairs.
Their "leaks" were blows already wound up (engine: blow lands 9 ticks after the decision) when a hold
landed; timings fixed (SWING_WAIT 48, COMMIT_AT 18) and such blows counted as late_blows.
In flight: run 9 (session 34) verifies the fix. Next: parse sessions 32-34 into the diagnostics,
report to the parent.

## 2026-10-18 22:18 -0700 (same session)

Done: session 34 (current code) passes headless, 48 passed, 0 failed, 0 script errors, exit 0,
0 key presses, 0 leaks, 1 late blow. Limits off vs on, sessions 32-34, 12 pairs: 445.1 vs 273.2
damage per 20 s arm, ratio 1.63, 12 of 12. Diagnostics regenerated: 260 rows + baseline.json in
C:\Apocrypha\diagnostics\pack_tactics (sessions 22 and 31 flagged PLAYER_INPUT_IN_SESSION).
Nothing in flight. Apocky can restart Barony to play with the limits. Not checked: live play by a
person (feel, the pacify sparkle on held creatures, sam_log volume of about one line per hold),
co-op, 5x, ranged attackers, big-bodied kinds (only skeletons measured).
