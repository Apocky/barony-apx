# F6 parts-based gear: plan (lane protocol v2; card Apockalypsis handovers/tasks/BF6-parts-gear-card.md)

Branch apx/f6 from origin/apx-main fa246a44 (worktree ..\barony-apx-f6, build dir build\vs2022-f6).
Scope set by the parent session 2026-10-19 15:00: the pure core (BF6-1 + salvage numbers), then the
engine hooks for items, save, net and the tinkering salvage station (BF6-2, -3, -4, -6 station path),
then ONE batched launch. NOT in this pass: BF6-5 part visuals, the Formbook (BF6-6 second half).

## Test plan (D76 stop rules, D77 test by risk)
| What | Tier | Critical numbers | Stop rule |
|---|---|---|---|
| roll determinism S1 | T1 offline | blob hash equal twice over 10^6 rolls; ctrl_index differs | deterministic: run once |
| Item Power cap S2 | T1 offline | over_cap 0 over 10^6; APX_IP_CAP=0 control over_cap > 0; ip_rerolls share | deterministic: run once |
| coverage | T1 offline | forms_unmapped 0 over every ItemType | deterministic |
| io / pack round trip, corrupt and short inputs | T1 offline | 10^5 equal; corrupt never accepted as original; truncations never crash | deterministic |
| salvage S4 | T1 offline | looted 60 % +- 1, crafted 100 %, discharge 20 % +- 1, aura 25 % +- 1 per affix, low-integrity halving 100 %; SYMMETRIC control fails | measured: stop when the 95 % half-width < 0.5 percentage point or 10^5 items |
| data JSON | T0 | parses; ids unique | -- |
| engine hooks (bind, copy, stack, save fill, net blob, station) | T2 | ride the one launch below | -- |
| save format, host = client, vanilla (APX_PARTS=0) | T3 | 40 rolled items: save file round trip equal 40/40, save_blob_dropped 0; host->client ITEM blob equal 40/40, net_blob_dropped 0; APX_PARTS=0: no apx field written, ITEM len 29 | gate: stop at the first decisive failure |

One batched launch at the end (D70 slots for the save probe; exclusive lock for host+client):
mapseed 101, `-apxfloor=1`, `APX_BF6_PROBE=save,net`, `APX_BF6_OUT=C:\Apocrypha\diagnostics\bf6\<step>-<time>.jsonl`;
controls in the same job: APX_SAVE_STRIP=1, APX_NET_STRIP=1, APX_PARTS=0.
