"""bf3_plot.py -- top-down positions of the BF3 trial creatures, drawn from the game log (measured, not rendered).

The trial (-apx-bf3trial with APX_BF3_DUMP=1) logs the arena's wall map ("[apx] bf3 arena row") and, at 1/4, 1/2
and 3/4 of each measured window, every trial creature's distance and bearing from the pinned character
("[apx] bf3 dump arm"). This draws one panel per chosen arm at one dump time: walls, the reach radius
(STRIKERANGE + the character's body), the keep-out radius, the ring radius, and each creature coloured by its
place (reach slot, ring slot, other). First-person screenshots cannot show a ring round the character.

Usage: python tools/apx/bf3_plot.py <run log.txt> <out.png> [--t 500] [--arms 0,4,1,7]
"""
import argparse
import math
import re

import matplotlib
matplotlib.use("Agg")
import matplotlib.pyplot as plt
from matplotlib.patches import Circle, Rectangle

TILE = 16.0
REACH_R = 24 + 4      # STRIKERANGE + the character's body radius (trial: in reach)
KEEP_R = 4 + 24 + 6   # tactics keepOut(rTarget)
RING_R = 44.0

ROW = re.compile(r"\[apx\] bf3 arena row ([+-]\d+) (\S+)")
ARM = re.compile(r"\[apx\] bf3 arm (\d+) (\S+): dmg (\d+) .*attackers mean ([\d.]+)")
DUMP = re.compile(r"\[apx\] bf3 dump arm (\d+) t (\d+) uid (\d+) hp -?\d+ d ([\d.]+) ang (-?[\d.]+) st (-?\d+) tgt (-?\d+) "
                  r"elig (-?\d+) reach (-?\d+) ring (-?\d+) stance (-?\d+) flank (-?\d+) atk (-?\d+)")


def parse(path):
    rows, arms, dumps = {}, {}, {}
    with open(path, encoding="utf-8", errors="replace") as fh:
        for line in fh:
            m = ROW.search(line)
            if m:
                rows[int(m.group(1))] = m.group(2)
                continue
            m = ARM.search(line)
            if m:
                arms[int(m.group(1))] = (m.group(2), int(m.group(3)), float(m.group(4)))
                continue
            m = DUMP.search(line)
            if m:
                a, t = int(m.group(1)), int(m.group(2))
                dumps.setdefault((a, t), []).append({
                    "d": float(m.group(4)), "ang": float(m.group(5)), "st": int(m.group(6)), "tgt": int(m.group(7)),
                    "reach": int(m.group(9)), "ring": int(m.group(10)), "stance": int(m.group(11)), "flank": int(m.group(12)),
                    "atk": int(m.group(13))})
    return rows, arms, dumps


def panel(ax, rows, creatures, title):
    for dy, s in rows.items():
        half = len(s) // 2
        for i, ch in enumerate(s):
            if ch == "#":
                dx = i - half
                ax.add_patch(Rectangle((dx * TILE - 8, dy * TILE - 8), TILE, TILE, color="#555555"))
    for r, style, col in ((REACH_R, "-", "#c0392b"), (KEEP_R, "--", "#7f8c8d"), (RING_R, ":", "#2471a3")):
        ax.add_patch(Circle((0, 0), r, fill=False, linestyle=style, color=col, linewidth=1))
    ax.add_patch(Circle((0, 0), 4, color="black"))
    n_reach = n_ring = n_other = 0
    for c in creatures:
        x, y = c["d"] * math.cos(c["ang"]), c["d"] * math.sin(c["ang"])
        if c["reach"] >= 0:
            col, n_reach = "#27ae60", n_reach + 1
        elif c["ring"] >= 0:
            col, n_ring = "#2e86c1", n_ring + 1
        else:
            col, n_other = "#e67e22", n_other + 1
        ax.add_patch(Circle((x, y), 4, color=col, alpha=0.75))
    lim = 8.5 * TILE
    ax.set_xlim(-lim, lim)
    ax.set_ylim(lim, -lim)   # map y grows downward, as the logged arena rows
    ax.set_aspect("equal")
    ax.set_xticks([])
    ax.set_yticks([])
    ax.set_title(f"{title}\nreach slot {n_reach}, ring {n_ring}, other {n_other}", fontsize=9)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("log")
    ap.add_argument("out")
    ap.add_argument("--t", type=int, default=500, help="dump time in ticks into the measured window")
    ap.add_argument("--arms", default=None, help="arm indices; default: the first arm of each kind")
    args = ap.parse_args()
    rows, arms, dumps = parse(args.log)
    if args.arms:
        pick = [int(x) for x in args.arms.split(",")]
    else:
        seen, pick = set(), []
        for a in sorted(arms):
            if arms[a][0] not in seen:
                seen.add(arms[a][0])
                pick.append(a)
    pick = [a for a in pick if (a, args.t) in dumps]
    if not pick:
        raise SystemExit("no dump lines for the chosen arms at t=%d (run with APX_BF3_DUMP=1)" % args.t)
    fig, axes = plt.subplots(1, len(pick), figsize=(4 * len(pick), 4.4))
    if len(pick) == 1:
        axes = [axes]
    for ax, a in zip(axes, pick):
        kind, dmg, att = arms.get(a, ("?", 0, 0.0))
        panel(ax, rows, dumps[(a, args.t)], f"arm {a} {kind}: dmg {dmg}/20 s, attackers {att:.2f}")
    fig.suptitle(f"BF3 trial, positions at t={args.t} ticks (MEASURED, game log); red: in reach, grey dashed: keep-out, "
                 f"blue dotted: ring", fontsize=9)
    fig.tight_layout()
    fig.savefig(args.out, dpi=110)
    print(f"wrote {args.out}: arms {pick}")


if __name__ == "__main__":
    main()
