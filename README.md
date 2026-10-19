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

## F3.1 -- all five special rooms on every floor (review fix)

Started 2026-10-18 22:40:10 -0700, gates measured 22:51:01 -0700 (11 min to the gates).
The review found 1-6 special rooms per floor: special nodes kept whatever cell the BFS embedding
gave them and fell back to themed programs when their special program did not fit (seed 7: boss
on mine-drift-end in a 7x6 box). Mechanism and fixes, in `src/layout.cpp`:

- Special cells: after embedding, each special node swaps (inside its lock region's band) into a
  cell its special program fits; the boss goes first and takes the largest cell of its band, at
  least `Params::boss_min` = 12 tiles a side. If no single cell fits, up to 4 cells of the band
  that share whole edges merge into one rectangle (occupants move to spare cells of the band).
- Band order: cells were cut into regions in x-then-z order, which made the last region a strip
  10-12 tiles wide along the east edge, where no boss cell can exist. Cells are now ordered along
  a Hilbert curve, so a region is a compact blob (`--band-order-x` restores the old order).
- Island repair walked only the box interior, so a corridor opening carved into a box corner had
  no way in ("island cannot join its door"); it now walks the whole box. This removed every island
  failure, including the ones F2 already had.
- Gate: a floor passes only with all five special kinds on special programs and no special node
  on a non-special program. Control `--no-special-cells`: 14 / 200 floors have all five; gate
  FAIL (ctest f3_control_no_special_cells, WILL_FAIL).

| Measure, seeds 1-1000 | F3 (527a535) | F3.1 |
|---|---|---|
| F2 hard gates | 1000 / 1000 | 1000 / 1000 |
| floors with all 5 specials on special programs | not gated (the control, special cells off, gives 14 / 200) | 1000 / 1000 |
| special nodes on a non-special program | not counted | 0 |
| boss box, smallest side | down to 6 | 12 (0 floors under 12) |
| special cell swaps / cells merged | - | 2,679 / 108 |
| floors needing a second sub-seed | 62 | 78 (16 of the first 200 seeds' dead ends: boss band too small even merged) |
| Clone Army, clearance violations, GUARANTEED misses | 0, 0, 0 | 0, 0, 0 |
| density per 10 m^2 (with trim) | mean 6.93, min 5.23 | mean 6.68, min 5.33 |
| non-hall rooms in negative space 30-45 | 78.6 percent | 79.7 percent |
| ms per floor generate / dress, mean | 9.3 / 9.6 | 10.5 / 9.7 |
| ctest | 6 / 6 | 7 / 7 |

Negative space by room type (function / hall or room), 1000 seeds: halls keep the void term by
design (boss 82.7, crypt 75.5, temple 74.6, fortress 73.9, sewer 68.8, mine 63.2, nexus 62.4,
cave 56.0 percent mean); rooms: haven 43.8 (99.5 percent in band), stronghold 44.4 (100), vault
42.1 (94.7), sewer 40.5 (89.8), crypt 36.8 (78.6), temple 35.7 (74.7), fortress 34.9 (71.8), mine
31.6 (62.2). The CLI prints these rows (`neg_space_by_type`) after the per-floor rows.

## F3.2 -- retries under 10 per 1000 (review fix)

Started 2026-10-19 00:18:12 -0700, gates measured 00:24:09, ctest 00:26:55 (9 min to the gates).
The 78 floors per 1000 that needed a second sub-seed had two causes, both fixed at the mechanism:

- A boss region with no room for a 12-tile boss cell even after merging (61 of the 78). Fix:
  band steal. The nearest fitting cell of another band, preferring one that shares a boundary
  with the boss's band, joins the boss's band; its occupant moves to a spare cell of its own band.
  Bands only steer corridors; the critic still checks every lock (1000 / 1000).
- A corridor that cannot be dug (the other 17): rooms of the same region a few tiles apart, walled
  in by corridors dug earlier for other regions (a corridor may not touch another region's
  corridor). Fix: edge reorder. A failed edge restarts the corridor pass with that edge first, at
  most `Params::edge_reorders` = 4 times.
- Special cells now prefer the nearest fitting cell to the node's BFS cell, not the largest
  (`--boss-largest` restores the F3.1 rule); the boss keeps its 12-tile minimum.

| Measure, seeds 1-1000 | F3.1 (38b819c) | F3.2 |
|---|---|---|
| floors needing a second sub-seed | 78 | 1 |
| F2 hard gates, all 5 specials, determinism | 1000, 1000, 0 mismatches | 1000, 1000, 0 mismatches |
| boss box, smallest side | 12 | 12 |
| band steals / edge reorders | - | 66 / 29 |
| Clone Army, clearance, GUARANTEED misses, NEVER hits | 0, 0, 0, 0 | 0, 0, 0, 0 |
| density with trim, mean / min | 6.68 / 5.33 | 6.80 / 5.36 |
| ctest | 7 / 7 | 9 / 9 |

Gate: `--max-retried 10` (the w2_gate ctest now carries it). Controls, each must fail it:
`--no-edge-reorder` gives 17 retries, `--no-band-steal` gives 67 (ctest f3_control_no_edge_reorder,
f3_control_no_band_steal). Timing in this run: mean 16.0 ms, max 145 ms per floor while other
sessions were building; not re-measured on an idle machine.

# H1a -- the Hollow's site (gameplay/20 sections 2-3)

Started 2026-10-19 01:30:13 -0700, site gates measured 01:49:49-01:53:17, ctest 11/11 by 01:56
(about 26 min to the gates). `src/site.cpp`, `grammar/hollow.grammar`, `apxgen --site N`.

- Levels: a 64 x 64 surface pocket (open sky; a stream of shallow water, a broken ruin of
  diggable stone holding the way down, a copse, a camp), then floor 1 fortress (faction garrison),
  floor 2 temple (cult), floor 3 crypt or sewer by seed (undead and vermin), floor 4 mine (goblins),
  then the cave layer (beasts) whose first fitting plain room takes `cave-lava-vent` (a lava pool,
  a crust ring).
- Floors run `grammar/hollow.grammar`: the default grammar plus a second boss (`boss:2`, hung in
  the stronghold region): 3 keys, 3 locks, 2 secrets, 2 bosses, 2 cycles, 24-40 rooms, 2 loops.
- Links, in order down the site: stairs (surface to floor 1), shaft (1 to 2), flooded sump (2 to 3:
  shallow water round both ends), collapsed level (3 to 4: a dead-end pocket of 2-4 diggable tiles
  out of a room of the deepest lock region, walled HARD so nothing else reaches it; the way down
  sits at its far end; the floor's critic runs again on the changed tiles), stairs (4 to caves).
- Population per floor: dens (the stronghold, rooms dressed with a den, then spread farthest:
  3 + rooms/12), 3 patrol routes from dens to the farthest room door of the same lock region
  (never across a lock), then more patrols or dens on the farthest dead room until 70 percent of
  rooms are alive (cap 8 patrols, 3 extra dens). Writings (dress rows of content writing and the
  map plaque) each get a text kind: history, map_region, quest_hint or form.
- Lit: a room is lit by a light-content item, lava beside its floor, or open sky. Function
  programs gained `light` and `lit_permille` (900): a room with no emissive gets one; the rest are
  dark on purpose and the site dump marks them.

Mechanisms fixed on the way (all floors): corridors pay `Params::corridor_feat_cost` = 24 per
lava/chasm/deep tile under their whole footprint (a 3-wide corridor used to pave a vent with its
side lanes); island repair first looks for a path that keeps off those features.

Gates per site (`site_critic`), MEASURED on sites 1-1000:

| Gate | Result |
|---|---|
| sites passing every gate | 1000 / 1000 (determinism: sites 1-10 built twice, 0 mismatches) |
| surface: ruin, stream, copse, camp, one walkable piece | 1000 / 1000 |
| floors 1-4: F2 hard gates, 24-40 rooms, 3 keys, 3 locks, 2 secrets, 2 bosses, 2 cycles, 2 loops, all 5 specials | 4000 / 4000 floors |
| 4 distinct factions on the 4 floors | 1000 / 1000 |
| links stairs, shaft, sump, collapse, in order; collapse reached only by digging from its room | 1000 / 1000 |
| caves: vent room with lava | 1000 / 1000 |
| rooms per floor | 27-40, mean 32.3 |
| dens / patrols per floor (gate: 2 or more each) | min 5 / 3, mean 5.5 / 3.4 |
| Alive, rooms within 16 steps of a den or patrol (gate 60 percent) | min 70.0, mean 77.4 percent |
| Lit (gate 80 percent) | min 82.1, mean 99.8 percent |
| Readable, writings per floor (gate 12) | min 91, mean 230 |
| ms per site (5 levels + surface, generate + dress + populate) | mean 140, max 464 |

Control `--thin` (1 den, no patrols, no wall dressing): 0 / 200 sites pass (Alive min 2.5
percent, writings min 3, patrols 0); ctest h1a_control_thin (WILL_FAIL). Floor gates after the
H1a changes (1000 seeds): pass 1000, retried 1, all 5 specials 1000, Clone Army 0, clearance 0,
GUARANTEED misses 0, presence out of band 0, density mean 6.77 / min 5.36. ctest 11 / 11.

Dumps: `out/site/site-1.txt` .. `site-3.txt` (links, every level's map with dens D and patrol
tiles -, rooms, dark rooms, writings); per-site rows `out/h1a-1000.jsonl`.

## H1a.1 -- Readable as a band (review fix)

Started 2026-10-19 02:12:47 -0700, measured 02:14:30-02:19:16. The review found 229.8 writings per
floor (min 102) against "at least 12" in gameplay/20 section 3: every wall plaque, graffiti, mosaic
and chalk circle counted. Now:

- Readable kinds (the spec's "books, plaques, graffiti"): book, scroll, note, map_scroll, plaque,
  map_plaque, graffiti. Mosaics, chalk circles and other inscribed trim are decoration.
- A text pass gives a text (history, map_region, quest_hint or form) to 12-16 of the readable
  candidates per floor (seeded), the map plaque first, one per room before any room gets a second;
  the other candidates stay as worn, illegible trim (content "worn").
- Gate: 10-20 legible writings per floor (`SiteParams::gate_writings_lo/hi`). Control
  `--all-legible` (every candidate gets a text, the old count): 0 / 50 sites pass, 186 per floor;
  ctest h1a_control_all_legible (WILL_FAIL).

Sites 1-1000: 1000 / 1000 pass; writings per floor min 12, max 16, mean 13.99; readable candidates
184.9 per floor (worn trim). Floor gates 1000, retried 1; ctest 12 / 12.
Caveat: the per-room content-kind measure of DRESS (rooms with 3 kinds, 99.9 percent) was taken
before the text pass and counts a worn plaque as writing.

Not done / not checked (H1a):
- "16 m" is 16 walking steps at 1 tile = 1 m (ASSUMED, as the density rule).
- A writing's text is a seeded id with a kind, not written text (see H1a.1 for the count).
- The link endpoints are not aligned between levels (an x, z on each side, no shared geometry).
- Not in Barony yet; the vendored copy in barony-apx (F2-integration) has not taken these commits.
- Spec rows not built here: Vertical (30 percent with a second level, pit, shaft or balcony),
  interactive materials, the director's three-candidate pick, decay stages, crafting depth.

## H1b -- encounter tables (D75), haven merchants (D73), set pieces

Measured 2026-10-19 15:09-15:14 -0700. `kits/hollow.toml` holds per-level encounter tables (Barony
monster kinds WEAKEST FIRST with spawn weights), the haven trades and the set pieces; content rows
(names, text, the merchant stock) live in barony-apx `data/apx/hollow` (branch apx/h1b), ids match.
- D75: a kind's tier is the band its cumulative share starts in (under 60 percent fodder, to 90
  regular, to 98 elite, then champion). Floor 1: RAT and SKELETON are the first 60 percent, so they
  are fodder. Each creature (D72 tiers as before) takes a kind of its tier, weighted. Emergent draws
  steer the level to 60 percent fodder.
- D73: each floor's HAVEN room holds a merchant with one trade from the level's list (floor 1 smith,
  2 alchemist, 3 scribe or fence, 4 provisioner); Barony's general store on about half the floors
  from floor 2.
- Set pieces: the room running the named program, else the largest plain room (counted).

| measure, sites 1-1000 | value |
|---|---|
| sites passing every gate | 1000 / 1000 |
| fodder share per level, mean / min (gate 50-70) | 59.4 / 58.6 percent |
| floors 1-4 with a haven merchant of the level's trade | 4000 / 4000 |
| floors with a general store (floors 2-4) | 1469 / 3000 |
| set pieces placed / in their named room program | 10000 / 8269 (82.7 percent) |

Gates (`site_critic`): kinds in strength order have non-decreasing tiers; a fodder kind is never
placed above fodder; floor 1 RAT and SKELETON are fodder; fodder 50-70 percent per level; a haven
merchant of the level's trade in the haven room; every set piece placed. Control `--flat-tiers`
(kinds drawn ignoring tiers): 0 / 100 sites pass; ctest h1b_control_flat_tiers (WILL_FAIL).

## D72 -- group composition (spec 04 section 2.1)

Measured 2026-10-19 06:25-06:28 -0700. Every level of a site gets 2.0 creatures per plain room
(bosses apart). SPAWNED packs go to dens, then patrol starts, while the floor's spawned budget lasts
(20 percent of its creatures). A pack is an elite-led pack (2-4 members, 1-2 elites or champions,
the rest fodder or regular; 15 percent of draws), a fodder swarm (4-6, fodder, at most one
regular; 35 percent) or a small mixed pack (2-3, fodder and regular). The rest are EMERGENT: groups
of 1-3 in plain rooms, tiers drawn from spec 04 section 2's shares (fodder 60, regular 30, elite 8,
champion 2 percent), at most 2 elites in a group. Every count here is ASSUMED.

Gates per level (`site_critic`): a group with an elite has at most 4 members and 1-2 elites; a
group of 5 or more has no elite and at least 75 percent fodder; emergent groups at most 3; the
spawned share at most 20 percent; every creature is counted once.

| measure, sites 1-1000 (5000 levels) | value |
|---|---|
| sites passing every gate | 1000 / 1000 |
| spawned packs / with an elite | 18,894 / 2,853 |
| largest pack / largest pack with an elite | 6 / 4 |
| most elites in a pack | 2 |
| fodder share in large packs (5 or more), minimum | 80 percent |
| largest emergent group | 3 |
| spawned share per level, max / mean | 20.0 / 18.8 percent |

Control `--break-packs` (packs of 2-8 of any tiers, half the floor spawned): 0 / 100 sites pass
(largest elite pack 8, 8 elites in a pack, large-pack fodder minimum 0 percent, spawned share 50
percent); ctest d72_control_break_packs (WILL_FAIL). Not done: creatures are counts per group and
room, not placed entities; the emergent convergence itself is AI work (spec 03).

## Small floor sizes (F2-integration report)

MEASURED 2026-10-19, seeds 3001-3050, `--size N`: every failure below 128 was "N BSP cells for N
nodes", fewer BSP cells than mission rooms. Fix: when cells < nodes + 2 the BSP splits again with a
smaller largest leaf (24, 22, ... down to 2 * leaf_min - 1), `Params::adaptive_leaf`,
`--no-adaptive-leaf` is the control.

| size | before (adaptive off) | after |
|---|---|---|
| 48 | 0 / 50 | 0 / 50 |
| 64 | 0 / 50 | 0 / 50 |
| 80 | 8 / 50 | 35 / 50 |
| 96 | 48 / 50 | 50 / 50 |
| 128 | 1000 / 1000 | 1000 / 1000 (retried 1; sites 200 / 200) |

48 and 64 cannot hold the mission: a 62-tile interior with 10-tile minimum cells holds at most 36
cells, the mission has 24-40 rooms, and two bosses need cells of 16 or more. That needs a design
decision (floor size at least 96, a mission scaled to the floor, or smaller rooms), not a patch.

Decision 2026-10-19 (relayed from the parent session): 80 is the minimum floor size; targets are
96-128; 48 and 64 are out of scope. `Params::min_size` = 80 clamps every floor to 80-160. To make
80 pass:
- room cap: when a floor has fewer cells than rooms + 6, the mission's grow row stops at cells - 6
  (`Params::cap_rooms_to_cells`, `cap_spare` = 6; never under the grammar's 24); cells stay large
  so special rooms still fit, and the 6 spares feed the special swaps and band steals. The
  adaptive leaf shrink runs only when that cap would go under 24.
- a GUARANTEED wall fixture may stand free when door bands leave no wall tile (counted relaxed).
- tried and rejected: rotating the band order so the boss band ends on the largest cell made 96 and
  128 worse (retries 19 and 8 against 1 and 1); it stays off (`boss_cell_last`).

MEASURED, seeds 1-1000 per size:

| size | pass | retried | rooms mean | GUARANTEED misses | clearance |
|---|---|---|---|---|---|
| 80 | 1000 | 242 | 27.3 | 0 | 0 |
| 96 | 1000 | 1 | 28.8 | 0 | 0 |
| 128 | 1000 | 1 | 32.5 | 0 | 0 |

Control at 80 with `--no-room-cap`: 620 / 1000 pass. ctest: size80_gate (300 seeds, retries at
most 90) and size80_control_no_room_cap (WILL_FAIL); 14 / 14 pass; sites 200 / 200.
Open: at 80, 24 percent of floors need a second sub-seed (mostly "no cell in band fits special
boss"), and one presence row (fortress card-game vignette, LIKELY) measures 56 percent, under 60.

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
