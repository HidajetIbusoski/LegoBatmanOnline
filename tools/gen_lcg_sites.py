# Erzeugt src/dll2/lcg_sites.h: alle Zugriffe des Spiels auf den Logik-LCG 0x11a1844 (LEGO Batman 2), damit
# lbo2.dll jedem Verbraucher einen eigenen Zustand geben kann. Eingabe ist das entschluesselte Abbild
# runs/batman2/dump.bin (Basis 0x400000, nicht im Repo). Aufruf: python tools/gen_lcg_sites.py
#
# Gruppen: eine pro Funktion (Grenze = CC-Fuellbytes), weil Lesen und Schreiben eines Schritts in derselben
# Funktion liegen. Die Hilfsfunktion 0x88cdd0 (ein Schritt, 55 Aufrufer) wird ganz ersetzt; dort waehlt die DLL
# den Zustand nach der Ruecksprungadresse. Das Zuruecksetzen auf 12345 in 0x9d576f behandelt die DLL selbst.
import os, struct, sys
from capstone import Cs, CS_ARCH_X86, CS_MODE_32, x86

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
B, T = 0x400000, 0x11A1844
RAND16, RESET = 0x88CDD0, 0x9D576F
d = open(os.path.join(ROOT, 'runs', 'batman2', 'dump.bin'), 'rb').read()
md = Cs(CS_ARCH_X86, CS_MODE_32)
md.detail = True


def fstart(a):
    x = a - B
    while x > 1 and not (d[x - 1] == 0xCC and d[x - 2] == 0xCC):
        x -= 1
    return x + B


sites = []   # (Adresse des 4-Byte-Operanden, Befehlsadresse, lesend?)
o = 0
pat = T.to_bytes(4, 'little')
while True:
    o = d.find(pat, o)
    if o < 0:
        break
    a = o + B
    for back in (1, 2, 3):
        ins = list(md.disasm(d[a - back - B:a - back - B + 16], a - back, 1))
        if ins and any(op.type == x86.X86_OP_MEM and op.mem.base == 0 and op.mem.index == 0 and
                       (op.mem.disp & 0xffffffff) == T for op in ins[0].operands):
            i = ins[0]
            sites.append((a, i.address, not (i.mnemonic == 'mov' and i.op_str.startswith('dword ptr [0x11a1844]'))))
            break
    o += 1

funcs = {}
for a, ia, rd in sites:
    f = fstart(ia)
    if f == RAND16 or ia == RESET:
        continue
    funcs.setdefault(f, []).append((a, ia, rd))
# Jede Gruppe muss mit einem Lesen beginnen und nach dem letzten Lesen noch schreiben (sonst stimmt die Grenze nicht)
for f, l in funcs.items():
    k = ''.join('R' if rd else 'W' for _, _, rd in sorted(l, key=lambda t: t[1]))
    if not k.startswith('R') or 'W' not in k:
        sys.exit('Gruppe %x sieht falsch aus: %s' % (f, k))

rets = []
for o in range(0x1000, len(d) - 5):
    if d[o] == 0xE8 and (struct.unpack_from('<i', d, o + 1)[0] + o + B + 5) & 0xffffffff == RAND16:
        rets.append(o + B + 5)

out = os.path.join(ROOT, 'src', 'dll2', 'lcg_sites.h')
with open(out, 'w', newline='\r\n') as h:
    h.write('// Erzeugt von tools/gen_lcg_sites.py - nicht von Hand aendern.\n')
    h.write('// Zugriffe auf den LCG16 0x11a1844: Operandenadresse und Zustand (eine Gruppe je Funktion).\n')
    h.write('#define LCG_NFUNC %d\n#define LCG_NRAND16 %d\n' % (len(funcs), len(rets)))
    h.write('static const struct { DWORD at; unsigned short s; } k_lcg_sites[] = {\n')
    n = 0
    for s, f in enumerate(sorted(funcs)):
        for a, _, _ in sorted(funcs[f]):
            h.write('    { 0x%08X, %d },\n' % (a, s))
            n += 1
    h.write('};\n')
    h.write('// Ruecksprungadressen der Aufrufe von 0x88cdd0; Zustand LCG_NFUNC + Index\n')
    h.write('static const DWORD k_lcg_rand16_rets[] = {\n')
    for i in range(0, len(rets), 8):
        h.write('    ' + ', '.join('0x%08X' % r for r in rets[i:i + 8]) + ',\n')
    h.write('};\n')
print('%d Zugriffe in %d Funktionen, %d Aufrufe von 0x88cdd0 -> %s' % (n, len(funcs), len(rets), out))
