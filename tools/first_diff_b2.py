# Erste Abweichung zwischen zwei frames.csv eines Batman-2-Netzpaars (ausgerichtet am Lockstep-Frame "rel")
import csv, sys
def byrel(p):
    m = {}
    for r in csv.DictReader(open(p)):
        if r.get('rel') not in (None, '-1') and r.get('wait_ms') is not None:   # letzte Zeile evtl. halb geschrieben
            m.setdefault(int(r['rel']), r)
    return m
H, G = byrel(sys.argv[1]), byrel(sys.argv[2])
# rng_lcg16 ist mit Zufall je Verbraucher (LBO_LCGSPLIT) der Hash aller Verbraucher-Zustaende. Darin stecken auch
# kosmetische Verbraucher (Zeichnen, Ladebildschirm), die je nach eigener Aufloesung abweichen duerfen; er wird
# deshalb getrennt gemeldet und zaehlt nicht als Abweichung der Spiellogik.
cols = ['rng_pm_seed', 'rng_pm_default', 'rng_a', 'rng_b', 'rng_c', 'crt_rand', 'chars', 'char_hash',
        'p1x', 'p1y', 'p1z', 'p2x', 'p2y', 'p2z']
first, cosmetic = {}, []
for rel in sorted(H):
    if rel in G:
        for c in cols:
            if c not in first and H[rel][c] != G[rel][c]: first[c] = (rel, H[rel][c], G[rel][c])
        if H[rel]['rng_lcg16'] != G[rel]['rng_lcg16']: cosmetic.append(rel)
for c, v in sorted(first.items(), key=lambda kv: kv[1][0]): print(c, v)
if not first: print('alles gleich bis', max(set(H) & set(G)))
# Neuere DLL: Eingaben (beide Lockstep-Pads), Frame-Zaehler und Spielzeit getrennt melden (Zaehler-Versatz ist allein
# noch kein Logikfehler, Eingaben muessen immer gleich sein)
extra = [c for c in ('pad0_btn', 'pad0_lx', 'pad0_ly', 'pad1_btn', 'pad1_lx', 'pad1_ly', 'game_frame', 'game_time')
         if c in next(iter(H.values()), {})]
for c in extra:
    d = next(((rel, H[rel][c], G[rel][c]) for rel in sorted(H) if rel in G and H[rel].get(c) != G[rel].get(c)), None)
    if d: print('(%s erst verschieden bei rel %d: %s / %s)' % (c, d[0], d[1], d[2]))
if cosmetic: print('(kosmetisch: Zufall je Verbraucher in %d Frames verschieden, erster %d)' % (len(cosmetic), cosmetic[0]))
