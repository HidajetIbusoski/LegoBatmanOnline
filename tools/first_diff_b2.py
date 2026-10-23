# Erste Abweichung zwischen zwei frames.csv eines Batman-2-Netzpaars (ausgerichtet am Lockstep-Frame "rel")
import csv, sys
def byrel(p):
    m = {}
    for r in csv.DictReader(open(p)):
        if r.get('rel') not in (None, '-1') and r.get('wait_ms') is not None:   # letzte Zeile evtl. halb geschrieben
            m.setdefault(int(r['rel']), r)
    return m
H, G = byrel(sys.argv[1]), byrel(sys.argv[2])
cols = ['rng_pm_seed', 'rng_lcg16', 'rng_pm_default', 'rng_a', 'rng_b', 'rng_c', 'crt_rand', 'chars', 'char_hash',
        'p1x', 'p1y', 'p1z', 'p2x', 'p2y', 'p2z']
first = {}
for rel in sorted(H):
    if rel in G:
        for c in cols:
            if c not in first and H[rel][c] != G[rel][c]: first[c] = (rel, H[rel][c], G[rel][c])
for c, v in sorted(first.items(), key=lambda kv: kv[1][0]): print(c, v)
if not first: print('alles gleich bis', max(set(H) & set(G)))
