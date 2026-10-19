# F2 next step (lane protocol v2, specs/barony-fork/11-MASTER-PLAN.md section 6)

State: apx/f2-integrate 9a49387f, merge-ready on apx-main 96c0830a (gates green, goldens-6714a4df.json).
Waiting on: the parent's call on apx-gen ec69fc1 (adaptive leaf: 96 -> 50/50, 80 -> 35/50; 48/64 infeasible).

## Step A: apx-gen ec69fc1 (only on the parent's yes)
1. Offline: vendor ec69fc1; build its CLI; seeds 2001-2050 at 128 and 96 must pass in the CLI.
2. Incremental build of barony (build\vs2022-f2).
3. One batched background job (gates_f2.ps1): W2 50, control, W5 12x2, W1, mp.
4. New goldens with the reason; sheet; mark merge-ready on the board (no rebase per apx-main move).

## Step B: D72 population (DECISIONS-2.md)
Rules: large groups are mostly fodder; elites only in small packs (at most 1-2 elites, pack <= 4);
spawned packs are a small budgeted share per floor; the rest are individuals and small groups.
1. Offline: placement uses apx-gen dens and patrols (Level.dens / patrols from build_site, or the
   floor's den rooms) -> pack rows with tier; a pure function checks per floor: max pack size by
   tier, elites per pack, share of creatures in spawned packs. Unit-tested on 1000 seeds offline.
2. Control APXGEN_CONTROL=2: elite packs of 6 and an 80% pack share, which must fail the gate.
3. In-engine: the bridge logs `apxgen: D72 ...` per floor, and gate_f2.py gates on it in the same
   batched job as Step A.
Open: the parent sets the numbers for "small budgeted share" and for the tier boundaries.
