"""Erzeugt das Eingabeskript fuer den Determinismus-Test in der Bathoehle.

Ablauf (Frames gezaehlt ab Spielstart, gemessen mit LBO_FIXEDDT=1, LBO_NOMOVIES=1, LBO_FPSCAP=60):
Titel -> Hauptmenue -> Load Game -> Game 1 -> Yes -> "Autosave"-OK -> Bathoehle.
Danach laeuft Batman nach einem festen Pseudozufallsmuster herum (Stick, Springen, Angreifen).

Aufruf: python tools/make_batcave_script.py > tools/scripts/determinismus_batcave.txt
"""
import random

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

rng = random.Random(20261017)
print("# Determinismus-Test Bathoehle (erzeugt von tools/make_batcave_script.py)")
print("connect 0")
for f0, f1, btn in MENU:
    print(f"F {f0} {f1} 0 {btn:04x} 0 0 0 0 0 0")
f = GAME_START
while f < GAME_END:
    dur = rng.randint(20, 90)
    lx = rng.choice([-30000, -15000, 0, 15000, 30000])
    ly = rng.choice([-30000, -15000, 0, 15000, 30000])
    btn = rng.choice([0, 0, 0, A, X])
    print(f"F {f} {f + dur - 1} 0 {btn:04x} 0 0 {lx} {ly} 0 0")
    f += dur + rng.randint(0, 20)
