"""Vergleicht zwei frames.csv-Logs Frame fuer Frame.

compare_runs.py <runA> <runB>   (Ordner unter runs/ oder Pfade)
Gibt fuer jede Spalte den ersten abweichenden Frame aus und eine Zusammenfassung.
"""
import csv, os, sys

COLS = ["F", "game_frame", "nchars", "chars_hash", "rng_hash", "p1x", "p1y", "p1z", "p2x", "p2y", "p2z"]


def load(run):
    path = run if run.endswith(".csv") else os.path.join(run, "frames.csv")
    if not os.path.exists(path):
        path = os.path.join("runs", run, "frames.csv")
    with open(path, newline="") as f:
        return {int(r["F"]): r for r in csv.DictReader(f) if r.get("F", "").isdigit()}


def by_w(rows):
    """Letzte Zeile je Weltupdate-Nummer W (Frames ohne Weltupdate zaehlen nicht)."""
    out = {}
    for fr in sorted(rows):
        w = int(rows[fr]["W"])
        if w > 0:
            out[w] = rows[fr]
    return out


def main():
    a, b = load(sys.argv[1]), load(sys.argv[2])
    if len(sys.argv) > 3 and sys.argv[3] == "--by-w":
        a, b = by_w(a), by_w(b)
        print("Vergleich nach Weltupdate W (Spalte 'W' unten = Schluessel)")
    frames = sorted(set(a) & set(b))
    print(f"gemeinsame Frames: {len(frames)} (A {len(a)}, B {len(b)})")
    first = {}
    diffs = 0
    for fr in frames:
        ra, rb = a[fr], b[fr]
        bad = [c for c in COLS if ra.get(c) != rb.get(c)]
        if bad:
            diffs += 1
            for c in bad:
                first.setdefault(c, fr)
    if not frames:
        print("FEHLER: keine gemeinsamen Frames (ein Lauf ist leer oder abgebrochen)")
        sys.exit(2)
    if not diffs:
        print("IDENTISCH: alle Spalten in allen gemeinsamen Frames gleich")
        return
    print(f"abweichende Frames: {diffs}")
    for c in COLS:
        if c in first:
            fr = first[c]
            print(f"  {c:11s} erstmals bei F={fr}: A={a[fr][c]} B={b[fr][c]} (F A={a[fr]["F"]} B={b[fr]["F"]})")


main()
