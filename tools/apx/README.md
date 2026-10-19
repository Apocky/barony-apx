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
