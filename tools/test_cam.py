"""Kamera-Versuche in einem einzelnen Spiel (Skriptmodus, Bathoehle mit zwei Spielern).

python tools/test_cam.py <name> [--frames N] [--shots N] [K=V ...]
Startet das Spiel im Fenster mit tools/scripts/determinismus_batcave_2p.txt (Spieler 2 steigt ein,
beide laufen herum), Kamera-Log alle 60 Frames und Positionswaechter. Weitere LBO_*-Variablen als K=V,
z. B. LBO_OWNCAM=1 LBO_OWNCAM_PLAYER=1. Ausgabe nach runs/cam/<name>, Bilder als PNG.
"""
import os, subprocess, sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
args = sys.argv[1:]
name = args.pop(0)
frames, shots, extra = "11000", "1000", {}
while args:
    a = args.pop(0)
    if a == "--frames":
        frames = args.pop(0)
    elif a == "--shots":
        shots = args.pop(0)
    else:
        k, v = a.split("=", 1)
        extra[k] = v
out = os.path.join(ROOT, "runs", "cam", name)
os.makedirs(out, exist_ok=True)
env = dict(os.environ, LBO_DIR=out, LBO_MODE="script",
           LBO_SCRIPT=os.path.join(ROOT, "tools", "scripts", "determinismus_batcave_2p.txt"),
           LBO_FIXEDDT="1", LBO_FPSCAP="60", LBO_NOMOVIES="1", LBO_SYNCBG="2", LBO_MULTI="1",
           LBO_ALLOWSAVE="0", LBO_MAXFRAMES=frames, LBO_SHOT_EVERY=shots, LBO_CAMLOG="60", LBO_WATCHPOS="1")
env.update(extra)
subprocess.run([os.path.join(ROOT, "build", "lbo-launch.exe"), "--wait", "--", "-Windowed"], env=env)
subprocess.run(["powershell", "-NoProfile", "-ExecutionPolicy", "Bypass", "-File",
                os.path.join(ROOT, "tools", "bmp2png.ps1"), out, "640"], capture_output=True)
print(out)
