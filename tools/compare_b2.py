# Vergleicht zwei frames.csv von lbo2.dll (LEGO Batman 2) Frame fuer Frame.
# Aufruf: python tools/compare_b2.py runs/batman2/d1a runs/batman2/d1b [--ab FRAME]
# Meldet je Spalte den ersten abweichenden Frame. Laufzeit (ms) wird ignoriert.
import csv, os, sys

IGNORE = {'ms'}


def load(run, key='F'):
    with open(os.path.join(run, 'frames.csv'), newline='') as f:
        rows = csv.DictReader(f)
        return {int(r[key]): r for r in rows if r.get(key) not in (None, '') and int(r[key]) >= 0}


def main():
    # --anker: Zeilen ueber die Spalte "rel" (Frames ab Levelanker, LBO_ANCHOR) statt ueber F zuordnen
    key = 'rel' if '--anker' in sys.argv else 'F'
    args = [a for a in sys.argv[1:] if not a.startswith('--') and not a.lstrip('-').isdigit()]
    start = 0 if key == 'rel' else 1
    if '--ab' in sys.argv:
        start = int(sys.argv[sys.argv.index('--ab') + 1])
    a, b = load(args[0], key), load(args[1], key)
    IGNORE.update({'F', 'rel', 'game_frame'} if key == 'rel' else set())
    frames = sorted(f for f in set(a) & set(b) if f >= start)
    if not frames:
        print('keine gemeinsamen Frames')
        return 1
    cols = [c for c in a[frames[0]] if c not in IGNORE and c != 'F']
    first = {}
    for f in frames:
        for c in cols:
            if c not in first and a[f][c] != b[f][c]:
                first[c] = f
    print(f'{len(frames)} gemeinsame Frames ({frames[0]}..{frames[-1]})')
    for c in cols:
        if c in first:
            f = first[c]
            print(f'  {c:15s} weicht ab ab Frame {f}: {a[f][c]} / {b[f][c]}')
        else:
            print(f'  {c:15s} gleich')
    return 1 if first else 0


if __name__ == '__main__':
    sys.exit(main())
