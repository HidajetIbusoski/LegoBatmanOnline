"""Misst, ob das Spiel ohne Fensterfokus Ton ausgibt (Pegel der Windows-Audiositzung von LEGOBatman.exe).

python tools/test_audio.py <launcher.exe> [sekunden]
Startet das Spiel per --solo im Fenster (LBO_WINDOWED=1) mit einer Welt aus test_worlds.py (Profil wA),
haelt ein anderes Fenster im Vordergrund und gibt den hoechsten gemessenen Pegel aus.
"""
import os, re, subprocess, sys, time
import ctypes
from pycaw.pycaw import AudioUtilities, IAudioMeterInformation

launcher = sys.argv[1]
seconds = int(sys.argv[2]) if len(sys.argv) > 2 else 60
W = os.path.join(os.environ["LOCALAPPDATA"], "LegoBatmanOnline", "welten", "wA")
saves = [os.path.join(W, x, "spielstand") for x in os.listdir(W)][0]
env = dict(os.environ, LBO_WINDOWED="1", LBO_ALLOWSAVE="0", LBO_MAXFRAMES=str(seconds * 60 + 600))
p = subprocess.Popen([launcher, "--solo", "--savedir", saves], stdout=subprocess.PIPE, text=True, env=env)
u = ctypes.windll.user32
t0 = time.time()
peak = 0.0
samples = 0
while p.poll() is None and time.time() - t0 < seconds + 30:
    time.sleep(0.25)
    u.SetForegroundWindow(u.GetShellWindow())  # Spiel nie im Vordergrund
    for s in AudioUtilities.GetAllSessions():
        if s.Process and s.Process.name().lower() == "legobatman.exe":
            m = s._ctl.QueryInterface(IAudioMeterInformation)
            v = m.GetPeakValue()
            peak = max(peak, v)
            samples += 1
p.wait()
print(f"{os.path.basename(os.path.dirname(launcher)) or launcher}: hoechster Pegel {peak:.3f} ({samples} Messungen)")
