# Pack Tactics (S.A.M mod) -- PROTOTYPE, never ships

S-track of Apockalypsis `specs/barony-fork/09` section 3: the fork's tactics layer (03 sections 4-5)
and crowd combat (04 sections 3 and 8), tried in Lua on the installed S.A.M 3.4.0 so the C++ of
slices F3 and F4 is written from measurements. Needs S.A.M >= 3.4.0. Enable it in the Mods menu.

## What it does

| Part | Behaviour | Spec |
|---|---|---|
| Packs | Spawned packs and vanilla monsters (grouped by room) get a home room, morale and a target player. On contact one member of a pack of 3+ flanks: it detours through a side waypoint and arrives behind the target. Each death costs 90 / pack size morale (+15 for the leader); below 45 the pack falls back home, heals 1 HP/s, regroups at 70. | 03 s4 |
| Natural limits (D62) | No attack cap. ROOM: only as many creatures attack one player as fit round it, on a ring of body-sized slots (body plus 0.25 tile of swing room) counted only on open floor -- 5 skeletons in the open, fewer in a corridor or for big bodies. RHYTHM: on one player a new attacker is let in at most every 12 ticks (0.24 s); it swings after the engine's own wind-up (0.46 s), then steps back for 0.5 s. LINE: nobody swings while another body stands in its swing line. | D62 |
| Token cap (CONTROL) | Off by default; only for comparisons. The old 3-attacker cap of spec 03 section 5, which D62 dropped. | 03 s5 |
| Verbs | A melee hit sweeps typed damage to the other hostiles in a 100 degree arc within reach (falloff 60/45/30 percent, cap 3 sword/axe, 2 mace/polearm, 1 unarmed). Shove (key G): a kick (15 stamina, force 2.0), or a shield bash with a shield (25 stamina, force 3.5), 0.6 s stagger. Sweeps cost 10 stamina. Stamina 100, regenerates 15/s after 0.8 s. HUD bar with its number. | 04 s3 |
| Density | When a level starts, extra packs (3-5) are spawned room by room, never within 10 tiles of a player, until the floor holds `population` times its vanilla hostile count. Special levels (town, temple, boss...) keep their vanilla population. | 09 s2-3 |
| Telemetry | One `pack_tactics: ENCOUNTER ...` line per encounter in sam_log.txt: pack size, flank arrivals, retreats, damage taken per player per minute, time to kill, deaths, outcome. Counters every 30 s (releases, holds, line blocks, bursts, leaks...). | 04 s8 |

Settings > General > MOD SETTINGS: Population (x vanilla, 1-5, default 3; 1 = off), Pack tactics,
Natural attack limits (on), CONTROL: old 3-attacker cap (off), Crowd verbs. Restart Barony after
changing the mod's files.

## Substitutes (each is logged once per level; the fork must not copy them)

- **Natural limits.** The fork gates the swing decision in handleMonsterAttack. From S.A.M a swing
  is held with the engine's PACIFY effect: it pins the monster's attack timer at 22 of 45 ticks (no
  swing) and makes it back away while it keeps its target; the engine shows its pacify sparkle on
  held creatures, and every application writes one line to sam_log.txt. Swings are seen through
  on_damage_multiplier; a released creature that never hits is taken as a miss after 32 ticks in reach.
- **Engine fact:** vanilla already skips a monster's swing when its thin attack ray reaches an
  unrelated monster first (LINETRACE_ATK_CHECK_FRIENDLYFIRE); the LINE rule widens that ray by
  0.15 tile beyond the blocking body.
- **Engine fact, wind-up:** a humanoid's blow lands 9 ticks after the swing decision
  (ANIMATE_DURATION_WINDUP), so a hold applied in that window does not stop it. The limits therefore
  never re-hold for the line after 18 ticks in reach, wait 48 ticks for a missed swing, and count a
  blow within 12 ticks of a hold as a late blow, not a leak.
- **Token cap (CONTROL).** Target clearing every tick; lossy (creatures re-acquire).
- **Flank route.** No penalised second A* in S.A.M: a side-waypoint detour, cleared every tick.
- **Retreat.** Target clearing while walking home.
- **Sweep damage** has no attacker: no XP, no aggro.
- **Not modelled:** elites, ranged attackers, directional swings, poise, timed blocks.

## Tests

- Offline (seconds, no game): `python tools/offline_check.py` -- real Lua 5.4 (lupa) against a
  stubbed S.A.M world (`tools/sam_mock.lua`, which mirrors the engine's 45-tick attack timer, the
  PACIFY pin and back-off, and the thin ally-blocking ray): compile, the whole self-test path, a
  two-level live session, and a planted-error control. The mock is not Barony; its numbers are
  never reported.
- Headless: only when no other *.LOCK is in the shared notes folder and no barony / barony_vanilla
  runs (a second instance exits with code 1); `barony.exe -samtest=pack_tactics -samtestfloor=1
  -samtesttimeout=600` from the Barony folder (a new game opens on the Start Map, which has no
  monsters; floor 1 is The Mines). Runs only on S.A.M's test character (human barbarian, INT -2,
  under 500 ticks played, AND the runner's fixed seed 1514995713 -- a new human barbarian in a real
  game also has INT -2). It checks the API and the pure rules (ring slots, swing line, arc,
  falloff, morale), scans the real floor, spawns and removes a 3x population, moves the character
  to the most open quiet room tile to sweep and shove rats (and puts it back), then runs the trial:
  12 sword-armed skeletons in 3 packs around the passive test character, natural limits off / on,
  4 pairs, 20 s measured each. Incoming damage is counted and negated, so the character never
  dies; everything spawned is removed; the settings file is never written. CONTROL: with the limits
  off the character must take more damage.
- Natural limits, measured 2026-10-18 (MEASURED, S.A.M sessions 32-34, floor 1, 0 key presses,
  12 off/on pairs of 20 s): damage per arm limits off 445.1 (sd 35.6), on 273.2 (sd 34.3), ratio
  1.63, lower in 12 of 12 pairs (pair ratios 1.30-2.02). Hits 65.2 vs 40.6 per arm; share of hits
  within 5 ticks of another hit 0.142-0.165 vs 0.021-0.030 (sessions 33-34); at most 3 creatures
  released at once against a ring of 5; creatures within 6 tiles about 7.4 both ways (nobody pushed
  out of the fight). Session 34 (current timings): self-test 48 passed, 0 failed; 0 leaks, 1 late blow.
- Token cap, measured 2026-10-18 before D62 (MEASURED, S.A.M sessions 23-25, floor 1, 12 off/on
  pairs of 20 s): damage per arm cap off 389.3 (sd 50.8), cap on 149.2 (sd 30.4), ratio 2.61.
  Sessions 22 and 31 are void: someone's key presses reached the test window.
  Rows: C:\Apocrypha\diagnostics\pack_tactics.
- Telemetry: `python tools/pack_telemetry.py [LOG ...]` turns ENCOUNTER / TRIAL / TRIALSUM lines
  into `C:\Apocrypha\diagnostics\pack_tactics\rows.jsonl` (one row each, then one baseline row
  per run) and `baseline.json`. Exit 1 if the control failed, the self-test failed or the session
  had script errors.
