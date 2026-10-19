# apx-gen -- the F2 generator core (C++17, no Barony dependency)

Slice F2 of specs/barony-fork/09 (Apockalypsis repo), against specs/barony-fork/02 sections 3, 7, 8.
A port of Apockalypsis `crates/bl-world` MISSION and LAYOUT onto a 2D tile grid, plus a 2D
interpreter for the TOML room programs and the critic's hard checks. Integration into Barony's
maps.cpp is a later step.

Started 2026-10-18 20:43:11 -0700, gates measured 2026-10-18 21:14:23 -0700 (31 min elapsed).

## What is in it

| Pass | File | What |
|---|---|---|
| MISSION | src/mission.cpp | grammar rows (grammar/default.grammar) build a cyclic graph: entrance, nexus, haven, stronghold, 3 keys, 3 locks, boss, 2 treasures, 2 secrets, exit, 2 cycles, grown to 24-40 nodes; check_mission, lock regions, behind-lock masks |
| LAYOUT | src/layout.cpp | BSP of a 48-160 tile floor (default 128x128) into 10-24 tile cells; lock-region bands; BFS embedding; program per node by its fits (boss takes the largest); A* corridors 3 wide on the critical path, 1 to leaves, 2 elsewhere (narrower only if the wide route has no path, counted as `narrowed`); loop quota 2; BFS connectivity repair; program islands joined |
| CARVE | src/program.cpp | TOML subset parser + 2D interpreter on the plan slice y = 1: stamp (box, cylinder, disc), union, subtract, intersect, xor, dilate, erode, open, close, edge, noise threshold, components/ccl, repeat, assign. `[detail]` (FINE) is DETAIL, not CARVE: not run |
| critic | src/critic.cpp | overlap, connectivity (one 4-connected floor component), reach, locks after keys (solvable in key order; with lock k closed key k reached, no behind-k tile reached, 8-connected), dig-BFS gate (same, every ROCK tile passable), mission checks |
| section 8 | src/layout.cpp | ROCK next to any floor behind a lock becomes HARD (undiggable); map border HARD |

Output per floor: tile grid (ROCK, FLOOR, HARD), ROOM rows (node, program, box, door), entity rows
(stairs_up, stairs_down, key, lock_door, boss, chest, secret, merchant, map_plaque, faction_seat),
FNV-1a 64 floor hash over the grid + sorted entity rows + room rows. A floor failing the critic is
regenerated from the next sub-seed (up to 8) and the failure is logged (02 section 7).

Room programs: Apockalypsis/kits/rooms (read in place: crypt-hall, pillar-hall, plain-chamber) and
kits/rooms/cavern-grotto.toml (new: noise, close, open, components, dilate, erode).

## Build and run

```
cmake -S . -B build -G "Visual Studio 17 2022" -A x64
cmake --build build --config Release
build/Release/apxgen.exe --seeds 1000            # JSONL per floor + summary, exit 0 = gate PASS
build/Release/apxgen.exe --dump 1 out/seed1.txt  # ASCII map + rows of one floor
ctest --test-dir build -C Release                # gate + both controls
```

## Gates (W2), MEASURED 2026-10-18, seeds 1-1000, 128x128, Release, MSVC 2022, this machine

| Gate | Result |
|---|---|
| connectivity 100 percent | 1000 / 1000 |
| reach | 1000 / 1000 |
| locks after keys | 1000 / 1000 |
| dig-BFS gate | 1000 / 1000 |
| mission checks | 1000 / 1000 |
| determinism (seeds 1-50 generated twice) | 0 hash mismatches |
| floors needing a second sub-seed | 51 / 1000 (layout dead end or critic fail, logged) |
| rooms per floor | 32.5 mean (24-40 by seed) |
| open floor | 20.6 percent mean |
| ms per floor (including retries) | mean 15.7, p50 14.0, p99 38.9, max 62.9 |

Timing caveat: the CPU was at 99.7 percent from other sessions' builds during the runs; a second
run measured mean 28.3 / p99 69.1 ms, an earlier grammar 12.1 / 24.1 ms. Re-measure on an idle box.

Controls (both must fail, both do):

| Control | Result |
|---|---|
| grammar/corrupt-key-behind-lock.grammar (one row changed: key 1 hung behind lock 1) | 0 / 200 pass; 189 rejected "mission: key 1 behind its own lock", tile check `locks:false` too; the rest by layout dead ends; exit 1 |
| `--no-hard-walls` (section 8 rule off) | 0 / 1000 pass; 960 rejected "digging bypasses lock 1"; exit 1 |

## Not done / not checked

- Not bit-identical to bl-world: the hash is splitmix64-based, not xxh3; same algorithm, different numbers.
- Secret hints in sight, legibility, budget, and the soft terms of 02 section 7 are not implemented.
- Districts for 160x160 floors, halls 5-9 wide, population, DRESS, DETAIL: later slices.
- Barony integration (maps.cpp) is the next step, not this one.
