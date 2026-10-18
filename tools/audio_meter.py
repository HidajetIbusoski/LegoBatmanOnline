"""Misst parallel zu einem Test den Tonpegel aller LEGOBatman.exe-Audiositzungen, ohne dass ein
Spielfenster den Fokus hat. python tools/audio_meter.py <sekunden>"""
import ctypes, sys, time
from pycaw.pycaw import AudioUtilities, IAudioMeterInformation

u = ctypes.windll.user32
peaks = {}
t0 = time.time()
while time.time() - t0 < float(sys.argv[1]):
    u.SetForegroundWindow(u.GetShellWindow())
    for s in AudioUtilities.GetAllSessions():
        if s.Process and s.Process.name().lower() == "legobatman.exe":
            v = s._ctl.QueryInterface(IAudioMeterInformation).GetPeakValue()
            peaks[s.ProcessId] = max(peaks.get(s.ProcessId, 0.0), v)
    time.sleep(0.5)
print("Pegel ohne Fokus:", {pid: round(v, 3) for pid, v in peaks.items()}, flush=True)
