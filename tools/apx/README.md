# tools/apx -- building barony-apx on Windows and the F0 parity gate

barony-apx is a private fork of TurningWheel/Barony (code BSD 2-Clause). It carries code only: the
game's assets belong to Turning Wheel and are never committed; every run reads them from a Steam
install (players must own the game). Design and plan: Apockalypsis `specs/barony-fork/00-09`.

## Branches

| branch | what |
|---|---|
| `apx/base-v5.0.2-steam` | pinned base: upstream master 962a5ce3, the source of Steam v5.0.2 (see below) |
| `apx-main` | our work on top of the base |
| `master` | mirror of upstream master |

`upstream` (TurningWheel/Barony) is fetch-only; its push URL is disabled on purpose.

Why 962a5ce3 and not the `v5.0.2` tag (be0e3aa2): the Steam `barony_vanilla.exe` (PE link time
2026-04-03) contains the debug string added after the tag ("Map: %s Chest: Locked: %d Mimic: %d")
and the post-tag `kItemsJsonHash` 2516917045 (4 occurrences; the tag's 1443175221 occurs 0 times);
every non-source file the repository shares with the install (32 files: lang/, licences,
gamecontrollerdb.txt, EDITING.txt) is byte-identical after line-ending normalisation.

## Build

```
pwsh -NoProfile -File tools\apx\fetch-deps.ps1     # once: pinned, SHA256-checked libraries
pwsh -NoProfile -File tools\apx\build.ps1 -Editor  # -> build\vs2022\Release\barony.exe + editor.exe
```

- Toolchain: CMake 4.x, Visual Studio 2022 (v143, the toolset of Barony.vcxproj), Windows SDK 10.
- Libraries (in `..\barony-apx-deps\prefix`): SDL2 2.0.22, SDL2_image 2.0.5, SDL2_net 2.0.1,
  SDL2_ttf 2.0.15 and GLEW 2.1.0 (the versions Steam v5.0.2 ships), zlib 1.3.2, libpng 1.6.59,
  PhysFS 3.0.1, RapidJSON 24b5e7a8, dirent 1.26.
- Sound: FMOD Core API from `C:\Program Files (x86)\FMOD SoundSystem\FMOD Studio API Windows`
  (2.03.15 here; Steam ships 2.02.14); `-NoFmod` builds without sound. Steamworks and EOS are off.
- Never build the INSTALL target, and never configure with `BARONY_DATADIR` set: CMake then adds
  install rules that copy barony.exe into that folder (for us the Steam install, whose barony.exe
  is Apocky's S.A.M build). build.ps1 removes the variable from its own environment.

### Shared compiler cache (ccache, all worktrees)

```
winget install Ccache.Ccache Ninja-build.Ninja                       # once per machine
pwsh -NoProfile -File tools\apx\build.ps1 -Cache ccache              # -> build\ninja-ccache\barony.exe
```

`-Cache ccache` builds with Ninja in the VS 2022 Build Tools x64 environment (vcvars64, the same v143
compiler as the VS build) and runs every cl.exe through ccache. The cache is
`C:\Apocrypha\cache\ccache` (20 GB cap), shared by every barony-apx worktree: paths are rewritten
relative to each worktree's root (CCACHE_BASEDIR), so a second worktree reuses what the first compiled.
Release builds have no /Zi, so there is no shared PDB to defeat the cache; /MP is passed only to the
Visual Studio generator (CMakeLists.txt). The exe lands in `build\ninja-ccache\` (Ninja is single-config),
not `build\vs2022\Release\`. `-Cache vs` (default) is the original Visual Studio build, `-Cache none` is
Ninja without a cache, `-Cache sccache` also works but measured slower.

Measured 2026-10-19 (tools\apx\build_cache_bench.ps1; rows in C:\Apocrypha\diagnostics\barony-build),
seconds; free commit memory varied 3.4-10.4 GB between steps (other lanes were building):

| mode | cold, worktree A | clean build, worktree B, warm cache | one file changed |
|---|---|---|---|
| vs (original) | 184.8 | -- | 9.3 |
| none (Ninja, control) | 184.0 | 248.5 | 10.0 |
| sccache 0.18 | 547.3 (1 compiler process at 3.4 GB free) | 56.2 (158/158 hits) | 4.0 |
| ccache 4.14.1 | 271.3 | 8.6 (158/158 hits) | 3.7 |

F0 parity gate on the exe built entirely from the ccache in a second worktree (barony-apx-cachetest, 158/158 hits): PASS (2026-10-19 05:3x; output C:\Apocrypha\diagnostics\barony-build\parity-ccache).

**Lanes: build with `-Cache ccache`** (exe: `build\ninja-ccache\barony.exe`, DLLs beside it). The default stays `-Cache vs` (exe: `build\vs2022\Release\barony.exe`) until every lane script has moved to the new path.

## Run against the Steam assets

```
$env:BARONY_DATADIR = 'C:\Program Files (x86)\Steam\steamapps\common\Barony'
cd <an empty folder>; & <repo>\build\vs2022\Release\barony.exe
```

On Windows the game writes log.txt, config, saves and compiled books to its working directory, so
run it from its own folder. `-datadir=<path>` does the same and wins over the variable. Upstream
half-honoured `-datadir=` (about 40 loaders used the compile-time `BASE_DATA_DIR`); apx-main fixes
that, so a stock Barony run this way silently misses its base language file while ours does not.

## F0 parity gate

```
python tools\apx\parity_f0.py --ours build\vs2022\Release\barony.exe [--lock-name barony-<lane>.LOCK]
python tools\apx\f0_sheet.py --commit <sha>
```

Barony runs one instance per machine, and several sessions launch it. Before every run the driver
waits until no Barony process is running and no other `*.LOCK` file exists in the shared lock folder
`C:\Apocrypha\locks\barony` (the machine convention; `--lock-dir` is repeatable for more folders),
then holds its own lock file there. Give each lane its own `--lock-name`.

Same seed, same dungeon: each run starts a quickstart game, types `/enablecheats`,
`/setmapseed <seed>`, `/nextlevel` into the window (posted messages; no focus needed, each step
confirmed in the log), and records the floor-1 line "successfully generated a dungeon with N rooms,
M monsters, G gold, I items, D decorations" plus an F6 screenshot. Ours runs with BARONY_DATADIR;
`barony_vanilla.exe` runs in a mirror folder (junctions to the install's images/music/sound/themes,
a private copy of the rest) so the install is only read. Rows go to
`C:\Apocrypha\diagnostics\barony-f0\parity.jsonl`; `summary.json` holds the gate.

### F0 result (2026-10-18, MEASURED; driver and build at ccce8244, exe sha256 5c423b5c...)

| seed | our build (BARONY_DATADIR) | barony_vanilla.exe (Steam v5.0.2) |
|---|---|---|
| 1234567 | 13 rooms, 12 monsters, 21 gold, 18 items, 4 decorations | same |
| 7654321 | 14 rooms, 13 monsters, 20 gold, 19 items, 2 decorations | same |

Seven runs per matrix (both seeds, a repeat of the first, our build also in the mirror folder);
three matrices. The third, with this driver, passed every check: parity per seed, the control
(the two seeds differ), repeats agree, no data-load error that vanilla lacks, 0 of 13,871 install
data files changed. The game's /pos put the camera at the same place in both builds per seed
([25, 24, -4] and [26, 15, -4]); the floor-1 picture pairs measure PSNR 28.8 and 30.6 dB (vanilla
against itself across repeats: 28.4 dB). The second matrix failed one run (a dropped key press:
the key-hold fix in ccce8244). One picture of the third matrix is excluded from the sheet because
keys pressed on the real keyboard reached that game window (logged as "You say: wwwwwwwwwa/pos").
Sheet: `C:\Apocrypha\diagnostics\barony-f0\sheet\sheet.png` (game art, so outside the repository).

From-scratch check (2026-10-18 22:50, MEASURED): `build.ps1 -BuildDir build\vs2022-clean` at 2a76428b
(clean tree) built barony.exe in 227 s with 2 compiler processes; with that exe a 4-run matrix
(ours and vanilla, both seeds) gave the same two lines, gate PASS, 0 install files changed
(`C:\Apocrypha\diagnostics\barony-f0\clean-build`).

## Merge train: test by risk (D77)

Every merge candidate runs two steps; the second adds launches only where the change carries that risk.

```
python tools\apx\test_select.py --base origin/apx-main                       # which checks this branch needs, and why
pwsh -NoProfile -File tools\apx\selftest_all.ps1 -Exe <exe> -Headless        # always for game code: one launch, every self-test
```

`test_select.py` maps `git diff --name-only origin/apx-main...HEAD` to checks: game code or build ->
`selftest_all`; generation or RNG (src/maps.cpp, src/prng.*, src/apx/gen, apx_gen_bridge) -> `golden_parity` +
`floor_hash` (headless_gate.py); net (net.cpp, lobbies, MainMenu.cpp, apx_stamina.hpp's packets) -> `host_client`
(gate_f2.py mp); save format (scores.cpp, apx_inventory, items.hpp, stat.hpp) -> `save_load` (u1_selftest.ps1);
build files -> `golden_parity`; docs and tools only -> no launch. Offline test: `python tools\apx\test_select_test.py`
(17 cases, under a second; `APX_TEST_SELECT_BREAK=1` drops the generation rule and must FAIL).

`-apxselftest=all` (with `-quickstart=barbarian -apxfloor=1 -apxmapseed=<n>`) runs, in one process on floor 1,
the stages features -> impact -> tierdmg -> inventory -> moves -> f4c (src/apx/selftest_all.cpp), writes
`<APX_DIAG_DIR>/selftest-all.json` (+ `.jsonl`), logs `[apx-selftest] summary pass=N fail=M`, and quits; a stage limit (`APX_SELFTEST_STAGE_S`, default 120) and a budget (`APX_SELFTEST_BUDGET_S`, default 240) fail a stuck stage as "timeout"; if player 0 dies, the running stage fails and the rest are marked "not run". Each check keeps its control:

| stage | passes when | control (must FAIL) |
|---|---|---|
| features | the new `[apx] features ...` line shows every default-on feature on | any knob below |
| impact (F4b) | over a quarter of the monster x class multipliers change, magic none | -- (toggles itself) |
| tierdmg (D74) | a hostile rat tagged fodder hits player 0 for less than vanilla 20; rule off gives 20 | `APX_TIER_DAMAGE=0` |
| inventory (U1) | mass 100/100, wrong rows caught, no slot clash, carry rule, save/load identical; windowed also the UI phases | `APX_GRID=1` |
| moves (F4a) | far dummy: plain swings 0 hits, thrusts and lunges hit; cluster hits | `APX_MOVES=0` |
| f4c | all 19 poise/zone/downed checks pass (runs last: it downs player 0) | `APX_POISE=0`, `APX_ZONES=0`, `APX_DOWNED=0` |

Headless skips the inventory UI phases (they need a visible window); windowed runs them.
### FPS gate (lag regressions)

```
pwsh -NoProfile -File tools\apx\fps_gate.ps1 -Candidate <exe> -Base <apx-main exe> [-Repeat 2]
```

Runs both builds back to back through `tools\apx\fps_probe.ps1` (exclusive, windowed 1280x720, never touches the
mouse): `-apxfpsprobe` records uncapped frame times for 10 s with the inventory closed, then 10 s open, on the start
map and on floor 1 (seed 1234567). FAIL when the candidate's median fps p50 falls below 85 percent of the base's, or
its p99 below 80 percent, in any (floor, phase). Same-session comparison: the machine's load varies a lot between
runs (0.1.2's own inventory-open p50 measured 84-131 on one day), so absolute numbers are recorded, not gated.
## Test farm: -apxheadless and farm.py

`-apxheadless` (or `APX_HEADLESS=1`) runs the game with a hidden window (never focused, so no key
press reaches it), no audio device, no vsync, no frame limiter, no drawing, and a fixed batch of
logic ticks per main-loop pass (`APX_HEADLESS_BATCH`, default 50). Logic that lives inside draw paths
(the crosshair pick) does not run headless; scenario bots that need it stay windowed.

`farm.py` is the one runner for Barony jobs on this PC. A job is a JSON file (exe, args, mode
headless | windowed | exclusive, timeout_s, lane, optional until regex, env, datadir, cwd); the
runner starts jobs first in, first out per mode under capacity (headless 6, windowed 3, exclusive 1)
and a commit-memory budget (headless 2.3 GB, windowed 3.2 GB per game, MEASURED peaks 2.23 and 3.13),
and follows the run protocol (11-MASTER-PLAN section 5 rule 1): nothing new starts while Apocky
plays, while any `.WANT` exists or another lane holds `barony\*.LOCK`; windowed jobs hold a D70 slot
file; an exclusive job writes `barony-want\farm-<id>.WANT` when it is first in line and runs alone.
Each job gets `C:\Apocrypha\barony-jobs\runs\<id>` as working folder; `done\<id>.json` holds exit
code, wall time, how it ended (exit, until, timeout, launch_error, orphaned), peak memory, log paths.

```
python tools\apx\farm.py run --exe build\vs2022\Release\barony.exe --mode headless --timeout 120 ^
    --lane mylane --until "successfully generated a dungeon with[^.]*\." -- ^
    -quickstart=barbarian -apxfloor=1 -apxmapseed=1234567 -apxheadless
python tools\apx\farm.py submit job.json      # returns at once; done\<id>.json appears later
python tools\apx\farm.py status
python tools\apx\test_farm.py                 # offline: 13 tests, about 18 s, no Barony
python tools\apx\headless_gate.py --exe <exe with -apxheadless and F1's floor hash>
python tools\apx\farm_bench.py --exe <exe> --n 24
```

`farm.py run` is the one-line submit-and-wait: put it in a lane's single background job; it starts
the runner (hidden) when none runs and exits with the job's exit code.

Headless gate (2026-10-19, MEASURED, local merge of apx/farm and apx/f1-instruments 5fa55c3a, 22
jobs in 222 s): 10/10 seeds give the same F1 floor-1 hash and F0 line windowed and headless; seed + 1
and APX_HASH_POKE=1 change the hash; the headless window was never visible; audio devices opened:
headless 0, windowed at least 5. Rows: `C:\Apocrypha\diagnostics\barony-farm\gate.jsonl`.

Throughput (2026-10-19, MEASURED, farm_bench.py, 24 floor-1 jobs, same dungeon lines both ways): before,
one windowed launch at a time: 313.7 s = 275 jobs per hour; after, all 24 submitted to the farm headless
(code 76491ea6, 13.8 GB commit free, at most 5 games at once by the memory budget): 35.0 s = 2,468 jobs
per hour, 8.96x. Rows: `C:\Apocrypha\diagnostics\barony-farm\bench.jsonl`.
## F2: apx-gen floors (`-apxgen`)

`-apxgen` (or `APXGEN=1`) makes `generateDungeon` (src/maps.cpp) build each generated floor with the
apx-gen core vendored at `src/apx/gen` (apx-gen 7057f3d9, see VENDOR.txt). Without the switch the vanilla
generator runs unchanged: it is the control. Knobs: `-apxgen-size=N` (tiles per side, default 128),
`-apxgen-pop=PCT` (population, 100 = 1x), `-apxgen-root=DIR` (kits); `APXGEN_CONTROL=1` moves every key
behind its own lock (the W2 control, must fail).

What the bridge (src/apx/apx_gen_bridge.cpp) does per floor:
1. reads the band's floor/wall/ceiling/water/lava tiles from the level set's own template map;
2. `apx::generate` + `apx::dress_floor` on (map seed, size);
3. writes the tiles as a tile-only `.lmp` in the run folder and reloads it with `loadMap`, so every
   size-dependent buffer is Barony's own (vismaps, lightmaps, shoparea);
4. entity rows: player starts, ladder, doors (a Lock room's every opening gets a locked door; an
   opening is walled down to one tile only where the floor stays one piece), skeleton keys, torches
   (one per 6 wall faces, one per 12 corridor tiles), free light sources where a room interior is
   over 5 tiles from any light, monsters (monstercurve via sprite 10), items, gold, chests, boss,
   shopkeeper, table/chair fixtures;
5. in-engine W2 on Barony's tiles and door entities (connected, keys open their locks in order, each
   lock seals its room, exit reachable) and a floor hash over tiles + entity rows (W5).

Log lines: `apxgen: floor|entities|W2|hash ...`; a failure logs `apxgen: FAILED` then
`apxgen: FALLBACK` (vanilla floor), never silently. Instruments: `/apxfps_begin [uncap]`,
`/apxfps_end`, `/apxhall`, `/apxnet`. Gate runs: `-apxhost` / `-apxjoin=ADDR` open, join and ready a
LAN lobby through the lobby's own calls. Driver: `tools/apx/gate_f2.py` (sweep, w1, mp),
`tools/apx/gates_f2.ps1`, sheet `tools/apx/f2_sheet.py`, top-down `tools/apx/f2_topdown.py`.
Results: D46 sheet `C:\Apocrypha\diagnostics\barony-f2\sheet` (game art, outside the repository).

Clients regenerate the floor from the map seed like vanilla clients do; both sides must run with the
same `-apxgen*` switches (not yet carried in the lobby's svFlags).

## F6: parts-based gear (`APX_PARTS`, default on)

Card: Apockalypsis handovers/tasks/BF6-parts-gear-card.md. Plan and stop rules: tools/apx/F6-PLAN.md.
- Pure core (no Barony header): src/apx/parts_model.hpp, parts_data.cpp (hand JSON reader), parts_roll.cpp
  (FNV-1a key over realm, depth, source, index; splitmix64 stream; Item Power cap 250 + 90 x depth with
  rerolls and a counted clamp; save string v1 with a checksum; packet blob up to 205 bytes; salvage).
- Data (T0, numbers ASSUMED): data/apx/materials.json, forms.json, affixes.json, salvage.json, and
  form_map.json + tests/apx/itemtypes.txt + src/apx/parts_typenames.inc generated by
  `python tools/apx/gen_forms.py` (`--check` fails when stale).
- Offline gates (T1): `cmake -S tests/apx -B build-tests -G "Visual Studio 17 2022" -A x64`,
  `cmake --build build-tests --config Release`, `build-tests\Release\parts_test.exe data\apx tests\apx\itemtypes.txt`.
- Engine (src/apx/parts_engine.cpp): loot binds parts without drawing Barony RNG (floors unchanged);
  items with parts never stack; floor entities carry parts in a server map; saves stay byte-identical,
  parts go to `<save>.apxparts`; ITEM/ITEQ carry the blob after byte 29; the tinkering station
  salvages by parts. Controls: APX_PARTS=0, APX_IP_CAP=0, APX_SAVE_STRIP=1, APX_NET_STRIP=1,
  APX_SALVAGE_SYMMETRIC=1. Counters in the probe rows.
- Batched launch (T3): `python tools/apx/gate_f6.py --exe <frozen barony.exe>` (save, net, controls, vanilla).
- Slice 2: DROP, USEI (use/equip), EQUI/EQUS/EQUM and all chest packets (CITM, RCIT) carry the blob;
  the Formbook (break teaches a Form, mastered at 5; saved in the sidecar; FBOK to the server); keep-one-part
  salvage for the BU1 bench (src/apx/parts_bench.cpp registers with src/apx_bench_api.hpp when present); palette
  tint on the held weapon (materials.json "tint", Entity::lightBonus; APX_PARTS_VIS=0 off). Not yet: crafting
  (next slice), shop sync (shops sell materials per D73), part models (BF6-5, D79 voxel tool).
