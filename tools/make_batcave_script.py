"""Erzeugt das Eingabeskript fuer den Determinismus-Test in der Bathoehle.

Ablauf (Frames gezaehlt ab Spielstart, gemessen mit LBO_FIXEDDT=1, LBO_NOMOVIES=1, LBO_FPSCAP=60):
Titel -> Hauptmenue -> Load Game -> Game 1 -> Yes -> "Autosave"-OK -> Bathoehle.
Danach laeuft Batman nach einem festen Pseudozufallsmuster herum (Stick, Springen, Angreifen).

Aufruf: python tools/make_batcave_script.py [--2p] > tools/scripts/determinismus_batcave.txt
Mit --2p steigt Spieler 2 per Pad 1 (Start) in der Bathoehle ein und bewegt sich ebenfalls.
"""
import random
import sys

TWO_PLAYERS = "--2p" in sys.argv

MENU = [  # aus runs/r12/live_script.txt (Live-Navigation)
    (3271, 3300, 0x1000),  # Titel: A
    (4101, 4120, 0x1000),  # Hauptmenue: Load Game
    (4661, 4680, 0x1000),  # Slot: Game 1
    (4801, 4820, 0x0001),  # Bestaetigung: hoch auf Yes
    (5301, 5320, 0x1000),  # Yes
    (6201, 6220, 0x1000),  # Autosave-Hinweis: OK
]
GAME_START, GAME_END = 7400, 12000
A, X = 0x1000, 0x4000

START = 0x0010


def wander(pad, seed):
    rng = random.Random(seed)
    f = GAME_START
    while f < GAME_END:
        dur = rng.randint(20, 90)
        lx = rng.choice([-30000, -15000, 0, 15000, 30000])
        ly = rng.choice([-30000, -15000, 0, 15000, 30000])
        btn = rng.choice([0, 0, 0, A, X])
        print(f"F {f} {f + dur - 1} {pad} {btn:04x} 0 0 {lx} {ly} 0 0")
        f += dur + rng.randint(0, 20)


print("# Determinismus-Test Bathoehle (erzeugt von tools/make_batcave_script.py"
      + (" --2p" if TWO_PLAYERS else "") + ")")
print("connect 0")
if TWO_PLAYERS:
    print("connect 1")
for f0, f1, btn in MENU:
    print(f"F {f0} {f1} 0 {btn:04x} 0 0 0 0 0 0")
wander(0, 20261017)
if TWO_PLAYERS:
    print(f"F 7300 7320 1 {START:04x} 0 0 0 0 0 0")  # Drop-in Spieler 2
    GAME_START = 7600
    wander(1, 4711)
