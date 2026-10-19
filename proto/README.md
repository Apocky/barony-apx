# proto/ -- prototypes, never shipped

Nothing under proto/ is built, packaged or loaded by Apocky's Barony. It is reference material
for the native ports (S-track and F-track cards in Apockalypsis specs/barony-fork).

## sam-mods/

Snapshot (2026-10-19) of the S.A.M Lua prototype mods from
`C:\Program Files (x86)\Steam\steamapps\common\Barony\mods\`:
pack_tactics, smart_followers, study_assist, study_assist_repair, SprintMod.
They run only under the S.A.M build of Barony. They are PROTOTYPES THAT NEVER SHIP: the fork
re-implements their behaviour natively. The live copies in the Steam folder stay the working
versions; this snapshot is not kept in sync with them.

## Source commits this branch was built against

| Repo | Commit |
|---|---|
| apx-gen (C:\Users\Apocky\source\repos\apx-gen, local) | 38b819ce313edceb60c92803c82f9ef54be97f0a (F3.1: all five special rooms on every floor) |
| apockys-barony-art (local) | ad3302b45903df3a6fbecad188b5f4157bd6d797 (icon with 16 px tiny variant) |

The icon files at the repo root, src/game.ico, src/editor.ico and the xcode AppIcon sets come
from apockys-barony-art/icon at the commit above.
