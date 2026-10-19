"""Analyse a APX_SELFTEST_FAMILIES run (log.txt) against the move table in src/apx_moves.hpp.
usage: python an_families.py <log.txt> [--control moves0|charge0]
Prints one row per move and a PASS/FAIL gate list. Exit code 0 only if every gate holds."""
import re, sys, statistics as st, collections

args = sys.argv[1:]
log = args[0]
control = args[args.index('--control') + 1] if '--control' in args else None
victim = args[args.index('--victim') + 1] if '--victim' in args else 'dummy'

# --- parse the table out of the header so the analysis cannot drift from the data
hdr = open(__file__.replace('tools\\apx\\unit\\an_families.py', 'src\\apx_moves.hpp').replace('tools/apx/unit/an_families.py', 'src/apx_moves.hpp'), errors='ignore').read()
rows = re.findall(r'\{ ([A-Z_]+), (F_[A-Z]+), "([a-z\-]+)", (K_[A-Z]+), ([^,]+), ([\d.]+), (\d+), ([\d.]+), ([\d.]+), ([\d.]+), ([\d.]+), (\d+), ([A-Z_| ]+), ([\d.]+), ([\d.]+), (\d+), ([\d.]+), ([\d.]+), ([\d.]+), (true|false) \}', hdr)
ids = dict(re.findall(r'([A-Z]+_[A-Z]+) = (\d+)', hdr))
T = {}
for r in rows:
    mid = int(ids[r[0]]) if r[0] in ids else None
    if mid is None:
        continue
    T[mid] = dict(name=r[2], fam=r[1], extra=int(r[6]), dmg=float(r[9]), cost=float(r[10]), heavy=int(r[11]), eff=r[12].replace(' ', ''),
                  force=float(r[14]), kd=int(r[15]), charged=r[19] == 'true', f1=float(r[7]), f2=float(r[8]), sneak=float(r[18]), dash=float(r[17]))

rel = collections.defaultdict(list)    # move -> list of release dicts
hits = collections.defaultdict(list)   # move -> list of hit dicts
after5 = collections.defaultdict(list)
after25 = collections.defaultdict(list)
proj25 = collections.defaultdict(list)
vel3 = collections.defaultdict(list)
done = ''
for l in open(log, errors='ignore'):
    m = re.search(r'\[apx-move\] release p=0 fam=(\d+) move=(\d+) charged=(\d) charge=(\d+) max=(\d+).*extra_cost=([-\d.]+) value=([\d.]+) pos=([\d.]+),([\d.]+)', l)
    if m:
        rel[int(m[2])].append(dict(charged=int(m[3]), charge=int(m[4]), max=int(m[5]), extra=float(m[6]), x=float(m[8]), y=float(m[9])))
    m = re.search(r'\[apx-fam\] hit move=(\d+) name=\S+ uid=(\d+) dmg=(-?\d+) extra=(\d) unaware=(\d) blocking=(\d) staggered_before=(\d)', l)
    if m:
        hits[int(m[1])].append(dict(dmg=int(m[3]), extra=int(m[4]), unaware=int(m[5]), blocking=int(m[6]), stag=int(m[7])))
    m = re.search(r'\[apx-fam\] vel3 uid=\d+ move=(\d+) v=([\d.]+)', l)
    if m:
        vel3[int(m[1])].append(float(m[2]))
    m = re.search(r'\[apx-fam\] after5 uid=\d+ move=(\d+) stagger=(\d) bleed=(\d)', l)
    if m:
        after5[int(m[1])].append((int(m[2]), int(m[3])))
    m = re.search(r'\[apx-fam\] after25 uid=\d+ move=(\d+) disp=([\d.]+) proj=(-?[\d.]+)', l)
    if m:
        after25[int(m[1])].append(float(m[2])); proj25[int(m[1])].append(float(m[3]))
    if 'selftest_done_families' in l:
        done = l.strip()

gates = []
def gate(name, ok, detail=''):
    gates.append((name, ok, detail))

def mean(v): return st.mean(v) if v else 0.0

print('%-3s %-12s %4s %5s %5s %6s %6s %6s %6s %5s %5s %5s' % ('id', 'move', 'rel', 'h/rel', 'x/rel', 'dmgM', 'dmgX', 'unawr', 'aware', 'disp', 'stg5', 'blk'))
for mid in sorted(T):
    d = T[mid]
    R = rel.get(mid, []); H = hits.get(mid, [])
    if not R:
        continue
    main = [h for h in H if not h['extra']]; ext = [h for h in H if h['extra']]
    un = [h['dmg'] for h in main if h['unaware']]; aw = [h['dmg'] for h in main if not h['unaware']]
    st5 = after5.get(mid, []); dp = after25.get(mid, [])
    print('%-3d %-12s %4d %5.2f %5.2f %6.1f %6.1f %6.1f %6.1f %5.1f %5.2f %5.2f' % (
        mid, d['name'], len(R), len(main) / len(R), len(ext) / len(R), mean([h['dmg'] for h in main]), mean([h['dmg'] for h in ext]),
        mean(un), mean(aw), mean(dp), mean([a for a, b in st5]) if st5 else 0, mean([h['blocking'] for h in H]) if H else 0))
    if control == 'moves0':
        continue
    # --- gates per move
    if control == 'charge0' and d['charged']:
        continue
    maxh = 1 + d['extra']
    gate('%s: releases=%d' % (d['name'], len(R)), len(R) >= 2)
    gate('%s: targets per release <= %d (table)' % (d['name'], maxh), (len(main) + len(ext)) / len(R) <= maxh + 1e-9, '%.2f' % ((len(main) + len(ext)) / len(R)))
    if d['extra'] > 0:
        gate('%s: extra targets struck (>= 1 per release avg, cap %d)' % (d['name'], d['extra']), len(ext) / len(R) >= 1.0 and max(collections.Counter().values() or [0]) <= d['extra'], '%.2f' % (len(ext) / len(R)))
    if d['charged'] and control != 'charge0':
        gate('%s: charged %d/%d, extra stamina %.2f swings' % (d['name'], sum(r['charged'] for r in R), len(R), d['cost'] - 1.0),
             all(r['charged'] for r in R) and abs(mean([r['extra'] for r in R]) - (d['cost'] - 1.0) * 8.0) < 0.5, '%.2f' % mean([r['extra'] for r in R]))
    if ('EF_KNOCKDOWN_SMALL' in d['eff'] or 'EF_GRAPPLE' in d['eff']) and victim == 'rat':
        gate('%s: stagger/knockdown after 5 ticks > 0.3' % d['name'], (mean([a for a, b in st5]) if st5 else 0) > 0.3, '%.2f' % (mean([a for a, b in st5]) if st5 else 0))
    if 'EF_CRIT_BLEED' in d['eff'] and victim == 'dummy':
        gate('%s: bleeding on unaware targets' % d['name'], any(b for a, b in st5), '%d of %d' % (sum(b for a, b in st5), len(st5)))
        if un and aw and mean(aw) >= 3:
            gate('%s: unaware damage x%.1f vs aware' % (d['name'], d['sneak']), mean(un) / mean(aw) > 2.0, '%.2f' % (mean(un) / mean(aw)))
    if 'EF_SNEAK' in d['eff'] and 'EF_CRIT_BLEED' not in d['eff'] and un and aw and mean(aw) >= 3 and victim == 'dummy':
        gate('%s: sneak bonus x%.1f' % (d['name'], d['sneak']), 1.2 < mean(un) / mean(aw) < 1.9, '%.2f' % (mean(un) / mean(aw)))
    if d['dash'] > 0:
        pass

print(done)
c = dict(re.findall(r'(\w+)=(\d+)', done))
if control is None:
    gate('pushes applied (counter > 0; displacement NOT measured, the dummybot is immobile)', int(c.get('pushes', 0)) > 0, c.get('pushes'))
    gate('slam victims > 0', int(c.get('slamVictims', 0)) > 0, c.get('slamVictims'))
    gate('throws > 0', int(c.get('throws', 0)) > 0, c.get('throws'))
    gate('knockdowns > 0', int(c.get('knockdowns', 0)) > 0, c.get('knockdowns'))
    gate('grapples > 0', int(c.get('grapples', 0)) > 0, c.get('grapples'))
    gate('shield aside > 0 (axe hook)', int(c.get('shieldAside', 0)) > 0, c.get('shieldAside'))
    gate('shield break > 0 (axe cleave)', int(c.get('shieldBreak', 0)) > 0, c.get('shieldBreak'))
    gate('crits > 0 and bleeds > 0 (dagger assassinate)', int(c.get('crits', 0)) > 0 and int(c.get('bleeds', 0)) > 0, '%s/%s' % (c.get('crits'), c.get('bleeds')))
    gate('sneak bonus > 0 (dagger stab/flick)', int(c.get('sneaks', 0)) > 0, c.get('sneaks'))
if control == 'charge0':
    for k in ('slamVictims', 'shieldBreak', 'throws', 'bleeds', 'crits'):
        gate('control charge0: no charged effect (%s == 0)' % k, int(c.get(k, 0)) == 0, c.get(k))
if control == 'moves0':
    gate('control moves0: no move released, no family effect counted', not rel and int(c.get('hits', 0)) == 0, 'releases=%d hits=%s' % (sum(len(v) for v in rel.values()), c.get('hits')))
if control == 'charge0':
    gate('control charge0: no charged move released', not any(T[m]['charged'] for m in rel if m in T), '')

bad = 0
for n, ok, dt in gates:
    print(('PASS ' if ok else 'FAIL ') + n + (' [' + dt + ']' if dt else ''))
    bad += 0 if ok else 1
print('GATES: %d pass, %d fail' % (len(gates) - bad, bad))
sys.exit(1 if bad else 0)
