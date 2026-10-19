# Vergleicht zwei Speicherabzuege von lbo2.dll (LBO_MEMDUMP=von,bis: .data je Frame) und meldet die
# globalen Werte, die zuerst auseinanderlaufen. Gezaehlt werden nur 4-Byte-Werte, die im ersten Frame des
# Abzugs in beiden Laeufen gleich waren (Zeiger auf den Heap u. Ae. fallen so heraus).
# Aufruf: python tools/memdiff_b2.py runs/batman2/xa runs/batman2/xb VON [anzahl]
import os, sys
import numpy as np

BASE = 0x0101F000
SIZE = 0x38E7EC // 4 * 4


def frames(run):
    data = np.fromfile(os.path.join(run, 'memdump.bin'), dtype=np.uint32)
    per = 0x38E7EC
    n = os.path.getsize(os.path.join(run, 'memdump.bin')) // per
    raw = np.fromfile(os.path.join(run, 'memdump.bin'), dtype=np.uint8)[:n * per].reshape(n, per)
    return raw[:, :SIZE].copy().view(np.uint32)


def main():
    a, b = frames(sys.argv[1]), frames(sys.argv[2])
    first = int(sys.argv[3])
    show = int(sys.argv[4]) if len(sys.argv) > 4 else 30
    n = min(len(a), len(b))
    ok = a[0] == b[0]
    print(f'{n} Frames, {ok.sum()} von {ok.size} Werten anfangs gleich')
    shown = 0
    for i in range(1, n):
        diff = ok & (a[i] != b[i])
        idx = np.nonzero(diff)[0]
        if len(idx):
            print(f'Frame {first + i}: {len(idx)} neue Abweichungen')
            for k in idx[:show]:
                print(f'  {BASE + 4 * k:08x}: {a[i][k]:08x} / {b[i][k]:08x}  (vorher {a[i-1][k]:08x} / {b[i-1][k]:08x})')
            ok &= ~diff
            shown += 1
            if shown >= 3:
                break


if __name__ == '__main__':
    main()
