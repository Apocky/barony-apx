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

## Results 2026-10-19 15:46 (build 1224df99; rows C:\Apocrypha\diagnostics\bf6\matrix-20261019-152209.jsonl)
| Gate | Samples | Result |
|---|---|---|
| T1 S1 determinism + control | 10^6 rolls x2, 2.9 s | equal; changed index differs |
| T1 S2 cap + control | 10^6 | over_cap 0 (cap off: 10,110 over); ip_rerolls 6.8 % (target under 5 %, ASSUMED), ip_clamped 7,909 |
| T1 coverage | 524 types | forms_unmapped 0 |
| T1 codecs | 10^5 each | string and blob round trips equal; 1,000 corruptions and every truncation rejected |
| T1 S4 salvage + control | 92,363 items (stopped at 10^5 cap; looted half-width 0.0014 points) | looted 59.91 %, crafted 100 %, discharge 19.94 %, aura 25.04 %, halving 63,333/63,333; symmetric fails |
| T3 save round trip | 1 launch | 49/49 items equal, parts 40/40, save_blob_dropped 0, sidecar written |
| T3 save control APX_SAVE_STRIP=1 | 1 launch | parts kept 0/40 |
| T3 vanilla APX_PARTS=0 | 1 launch | no parts, no sidecar, floor line equal to parts-on (11 rooms, 12 monsters, 21 gold, 18 items, 2 decorations) |
| T3 host = client | 2 processes | 40 sent, 40 received, 40 blobs equal (ITEM 74-106 bytes) |
| T3 net controls | 2 + 2 processes | APX_NET_STRIP: 40/40 without blob; APX_PARTS=0 host: ITEM 29 bytes, 0 blobs |
Not covered: a 6-part item in-engine (floor-1 quality allows 2-3 parts; offline round trips cover 0-12 parts), the
tinkering station in a live game (T2 hook, no launch yet), DROP/EQUI/CITM/shop blobs, BF6-5 visuals, Formbook.

## Slice 2 results 2026-10-19 16:36 (build 8fc5dafa; rows C:\Apocrypha\diagnostics\bf6\matrix-20261019-162916.jsonl): 14/14 gates pass
T1: rerolls 4.04 % of rolls (was 6.8 %; 0.71 % of rolls need any), over_cap 0. T3/T2 one batch: forced 6-part item (max 8); Formbook 5 breaks -> mastered, equal after save/load; salvage live 10/10 station + 10/10 bench keep-part (metal 41, aura 1 = counted); save 51/51, parts 42/42; strip 0/42; vanilla floor equal; tint blade column 6.1 % vs 0.52 % off; host->client 40/40; client->host DROP x5 + USEI x1 = 6/6; FBOK hash equal; net strip 40/40 none; vanilla ITEM 29 bytes. 0 stray key presses.

