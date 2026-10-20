"""Analyse a APX_VARIANTS_SELFTEST run (log.txt) against the resolved loot tables (card BV1 V1-3, stop rules D76).
usage: python an_variants.py <log.txt> [--control off]
Prints per node: kills, then per loot row measured rate vs table with the 95 percent half-width; GATES at the end."""
import collections, math, re, sys
sys.path.insert(0, str(__import__('pathlib').Path(__file__).resolve().parent))
import variants_lint as vl
from pathlib import Path

log = sys.argv[1]
control = sys.argv[sys.argv.index('--control') + 1] if '--control' in sys.argv else None
nodes = vl.load(Path(__file__).resolve().parents[2] / 'data/apx/variants')
spawns = collections.Counter(); deaths = collections.Counter(); drops = collections.defaultdict(collections.Counter)
done = ''
for l in open(log, errors='ignore'):
    m = re.search(r'\[apx-variant\] spawn id=(\S+) node=(\d+) uid=(\d+) hp=(-?\d+)', l)
    if m: spawns[m[1]] += 1
    m = re.search(r'\[apx-variant\] death id=(\S+) uid=\d+ drops=(.*)$', l)
    if m:
        deaths[m[1]] += 1
        for d in m[2].split(): drops[m[1]][d] += 1
    if 'selftest_done' in l and 'apx-variant' in l: done = l.strip()
gates = []
def gate(n, ok, d=''): gates.append((n, ok, d))
if control == 'off':
    gate('control APX_VARIANTS=0: no variant applied, no variant drops', sum(spawns.values()) == 0 and sum(deaths.values()) == 0, 'spawns=%d deaths=%d' % (sum(spawns.values()), sum(deaths.values())))
else:
    nonbase = [i for i, l in nodes.items() if l[0]['level'] != 'base']
    print('%-44s %6s %6s' % ('node', 'spawn', 'kills'))
    tight = tot_entries = 0
    for nid in sorted(nonbase):
        k = deaths[nid]
        print('%-44s %6d %6d' % (nid, spawns[nid], k))
        gate('%s: spawned and killed' % nid, spawns[nid] > 0 and k > 0, '%d/%d' % (spawns[nid], k))
        table = vl.resolve_loot(nodes, nid)
        for item, p in sorted(table.items()):
            q = drops[nid][item] / k if k else 0.0
            half = 1.96 * math.sqrt(max(q * (1 - q), 1e-9) / max(k, 1))
            sig = math.sqrt(p * (1 - p) / max(k, 1))
            ok = abs(q - p) <= 4 * sig + 1e-9
            tot_entries += 1
            if half < 0.2 * p: tight += 1
            if not ok: gate('%s drop %s: measured %.3f vs table %.3f (n=%d)' % (nid, item, q, p, k), False)
    gate('every drop rate within 4 sigma of its table row (%d rows)' % tot_entries, all(g[1] for g in gates if ' drop ' in g[0]) )
    print('rows with 95%% half-width < 20%% of rate: %d of %d (stop rule D76: or 2,000 kills; kills=%d)' % (tight, tot_entries, sum(deaths.values())))
    c = dict(re.findall(r'(\w+)=(\d+)', done))
    gate('applied variants died and dropped (all but the last two batches and a few stragglers alive at the end)', int(c.get('applied', 0)) > 0 and int(c.get('applied', 0)) - int(c.get('deaths', 0)) <= 25, done)
    gate('no unknown item names', int(c.get('itemFail', 1)) == 0, 'itemFail=%s' % c.get('itemFail'))
    gate('material credits happened', int(c.get('matCredits', 0)) > 0, 'matCredits=%s matLost=%s' % (c.get('matCredits'), c.get('matLost')))
bad = 0
for n, ok, d in gates:
    if not ok or n.startswith('every') or n.startswith('control') or n.startswith('applied') or n.startswith('material') or n.startswith('no unknown'):
        print(('PASS ' if ok else 'FAIL ') + n + (' [' + d + ']' if d else ''))
    bad += 0 if ok else 1
print('GATES: %d pass, %d fail' % (len(gates) - bad, bad))
sys.exit(1 if bad else 0)
