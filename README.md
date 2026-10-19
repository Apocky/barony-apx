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

Room programs (F2): Apockalypsis/kits/rooms (read in place: crypt-hall, pillar-hall, plain-chamber) and
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

## Not done / not checked (F2)

- Not bit-identical to bl-world: the hash is splitmix64-based, not xxh3; same algorithm, different numbers.
- Secret hints in sight, legibility, budget, and the soft terms of 02 section 7 are not implemented.
- Halls 5-9 wide, population, DETAIL: later slices.
- Barony integration (maps.cpp) is the next step, not this one.

# F3 -- architecture by code at both scales (branch f3-architecture-dress)

Started 2026-10-18 21:26:36 -0700, gates measured 2026-10-18 21:57:58 -0700, README written
21:58 -0700 (32 min elapsed to the measured gates). D63: the rooms and structures are our own
programs; nothing comes from prebuilt templates or model-generated geometry (D36).

## Macro: structures, districts, special rooms

- `kits/rooms/*.toml`: 36 new room programs (tools/gen_kits.py writes them; the TOML is what runs).
  30 themed: 5 per theme (crypt, mine, sewer, temple, fortress, cave), including halls, a sky
  courtyard (temple-courtyard, fortress-keep-courtyard), caverns (noise + open/close), chasms with
  bridges (mine-chasm-bridge, cave-chasm-rift, sewer-outfall), water and lava channels
  (sewer-channel, sewer-junction, sewer-cistern, mine-lava-forge, cave-lava-tube,
  temple-lava-sanctum), fortress (gatehouse, barracks, armory, keep courtyard), temple (nave,
  rotunda), crypt (ossuary, catacomb, tomb, collapsed vault), sewer, mine; plus one small-cell
  program per theme (min 6 tiles) so small BSP cells stay themed. 6 special programs for the five
  special rooms: HAVEN (haven-shrine), BOSS (boss-arena, boss-pit for small cells), VAULT
  (vault-strongroom, on Treasure nodes), NEXUS (nexus-stairhub), STRONGHOLD (stronghold-hall).
- Feature masks: a program writes masks named `water`, `deep`, `lava`, `chasm`, `sky`, `bridge`,
  `hazard`; deep/lava/chasm leave the floor (and become HARD rock: not dug), bridges put floor back,
  water/sky/hazard tag floor. Corridors or island repairs that cross a blocker turn it to bridge.
- Districts (`kits/districts.toml`): the floor's theme is picked by seed; each lock region is a
  district of that theme (region 0 the first, the approach). LAYOUT picks a special program for a
  special node, else a program of the region's district, else of the theme, else any that fits.
- Zones per room (DRESS): entry within 2 steps of an opening, hazard on or beside a feature (or a
  trap tile), edge along walls, core the rest.

## Micro: DRESS, function programs, props

- `src/dress.cpp`, `kits/functions/*.toml` (12 function programs: 6 themes, generic, haven, boss,
  vault, nexus, stronghold; tools/gen_dress_kits.py writes them). Each has activities (an anchor
  fixture placed by salience, members in its eight neighbours, props on top), items, wall trim with
  shares, story vignettes (fallen adventurer, grave robbers, ritual interrupted, cave-in, miners'
  break, smugglers' cache, last stand, card game, hermit camp, ...), a clutter list and a NEVER list.
  Buckets: GUARANTEED 1000, LIKELY 800, RARE 80, NEVER 0 per mille.
- Clearance (hard): door bands and mission-entity tiles stay free; a blocking fixture needs a free
  access tile on its use side and is accepted only if the room's walkable tiles stay one
  4-connected piece. `dress_check` re-checks every room from the emitted rows alone.
- Clone Army cap: wall trim and clutter never place a 4th identical (zone, id, variant); the
  next variant or kind is taken. `--no-clone-cap` is the control.
- Fixtures are entity rows; tiles are never changed, so the F2 hard gates cannot be moved by DRESS.
- Prop grammar `src/vox.cpp`, `kits/props/<id>.toml` for the 12 fixtures (table, bench, shelf,
  barrel, crate, brazier, altar, bed, rack, pillar, statue, rubble; 33 files with variants): ops
  box, clear, paint, cyl, sphere, chunks, repeat. Writes Barony's slab .vox: int32 x, y, z; x*y*z
  palette bytes (255 empty) at index z + y*sz + x*sy*sz; 768 palette bytes 0..63. Layout MEASURED
  from barony-apx/src/files.cpp (loadVoxel and the mesher's index formula).

## F3 gates, MEASURED 2026-10-18 21:57 -0700, seeds 1-1000, 128x128, Release, MSVC 2022

| Gate / measure | Result |
|---|---|
| F2 hard gates (connectivity, reach, locks, dig, mission) | 1000 / 1000 each |
| determinism, seeds 1-50 twice (floor hash and dress hash) | 0 mismatches |
| floors needing a second sub-seed | 62 / 1000 (was 51 before F3) |
| program fallbacks to a plain box | 0 |
| rooms on a themed program / special program / district program | 28,814 / 3,649 / 10,469 of 32,463 rooms |
| clearance violations (independent re-check) | 0 rooms |
| GUARANTEED misses / NEVER hits | 0 / 0 |
| presence buckets in band (100 / over 60 / under 15 / 0 percent) | 85 / 85 rows (out/presence-1000.json) |
| Clone Army (over 3 identical per zone): fixtures and props / wall trim | 0 / 0 groups, all floors |
| density, fixtures + props + wall trim per 10 m^2, per floor | min 5.23, mean 6.93, max 9.05; 1000 / 1000 floors at 3 or more |
| density without wall trim | min 1.26, mean 2.55, max 3.90 (below 3 on most floors) |
| rooms filled (1 or more of the 9 content kinds) | 100.0 percent; 1000 / 1000 floors |
| rooms with 3 or more kinds | 99.9 percent (32,434 / 32,463); 1000 / 1000 floors at 70 percent or more |
| negative space, non-hall rooms | mean 36.8 percent; 78.6 percent of rooms inside 30-45 |
| negative space, halls (keep the void term) | mean 64.5 percent |
| wall faces dressed | 80.3 percent (gameplay/20 asks 60) |
| .vox write + read back (C++ reader) and the Python reader in tools/sheet.py | 33 / 33 files same dims, same voxel counts, same bytes |
| ms per floor: generate / dress | mean 9.3 / 9.6, p99 24.2 / 25.1 |

Controls: `--no-clone-cap` brings Clone Army back (200 seeds: 1,164 item groups, 14,646 trim
groups) and fails `--strict-soft` (ctest f3_control_no_clone_cap); `--no-hard-walls` and the
corrupt grammar still fail. `ctest --test-dir build -C Release`: 6 / 6 pass.

.vox props (variant 0), dims x by y by z and voxel count, read back from disk:
table 24x12x8 690, bench 12x5x5 138, shelf 14x5x20 643, barrel 10x10x12 960, crate 10x10x10 1000,
brazier 8x8x10 198, altar 24x12x10 2336, bed 14x28x7 1448, rack 14x4x18 396, pillar 12x12x28 2008,
statue 12x12x28 976, rubble 14x14x5 185.

Sheet: `out/sheet/sheet.png` (1192 x 1308 px) and `out/sheet/sheet.txt`: six dressed floors, the
first seed of each theme (temple 1, cave 2, fortress 3, sewer 4, mine 6, crypt 14), and the 12
props (front and top views). Per-floor rows: `out/f3-1000.jsonl`; dumps: `out/dressed/`.

```
build/Release/apxgen.exe --seeds 1000 --presence out/presence-1000.json > out/f3-1000.jsonl
build/Release/apxgen.exe --vox out/vox
build/Release/apxgen.exe --dump 14 out/dressed/crypt-14.txt
python tools/sheet.py out/dressed/*.txt --vox out/vox --out out/sheet
```

## Not done / not checked (F3)

- ASSUMED: 1 tile = 1 m^2 for the density rule (`DressLib::m2_per_tile`); no spec gives Barony's
  tile size. At 4 m^2 per tile the measured density divides by 4.
- ASSUMED: negative space counts fixture footprints, access tiles, floor props and door bands as
  used. 21 percent of non-hall rooms sit outside 30-45: small cells under 30 (door bands use most
  of the floor) and large rooms over 45 where the Clone Army cap stops the clutter.
- ASSUMED: grammar z is up and is written flipped into Barony's z index (Barony's z grows down).
  The props were not loaded in Barony; check one in the editor before shipping.
- Small props (candle, bones, sconce, ...) are entity rows with a content kind, not .vox models yet.
- Variants of floor props are a parameter only (2 per prop) until they have models.
- Not checked in a running Barony; Barony integration (maps.cpp) is still the next step.
- MSBuild sometimes skipped the rebuild after an edit: build with `--clean-first` when measuring.
