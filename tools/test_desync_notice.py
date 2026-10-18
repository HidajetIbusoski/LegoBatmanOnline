"""Test des Desync-Hinweises: zwei App-Instanzen (Profile wA/wB aus test_worlds.py, Teil 1 vorher
ausfuehren), beim Gast meldet die Mod ab Frame 4000 absichtlich eine falsche Pruefsumme (LBO_FAKEDESYNC).

Erwartet: Beide Spiele melden "NICHT SYNCHRON" (Meldungsfenster), nach dem Spiel tauschen die Apps
die Fehlerdaten aus (logs\\*-partner mit partner_frames.csv auf beiden Seiten).
python tools/test_desync_notice.py [exe] [--shot datei.png]
"""
import glob, os, subprocess, sys, time

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
args = sys.argv[1:]
shot = None
if "--shot" in args:
    i = args.index("--shot")
    shot = args[i + 1]
    del args[i:i + 2]
EXE = args[0] if args else os.path.join(
    ROOT, "app", "LegoBatmanOnline", "bin", "Debug", "net10.0-windows", "win-x64", "LegoBatmanOnline.exe")
BASE = os.path.join(os.environ["LOCALAPPDATA"], "LegoBatmanOnline")
LOGS = os.path.join(BASE, "logs")


def games():
    out = subprocess.run(["tasklist", "/FI", "IMAGENAME eq LEGOBatman.exe", "/NH"], capture_output=True).stdout
    return out.decode("latin-1").count("LEGOBatman.exe")


before = set(glob.glob(os.path.join(LOGS, "*")))
code_b = open(os.path.join(BASE, "app", "wB", "code.txt")).read().strip()
common = ["--test", "--max-frames", "6500"]
b = subprocess.Popen([EXE, "--profil", "wB", "--auto-accept"] + common, env=dict(os.environ, LBO_FAKEDESYNC="4000"))
a = subprocess.Popen([EXE, "--profil", "wA", "--auto-invite", code_b, "--auto-world", "Gotham", "--auto-play"] + common)
t0 = time.time()
while games() < 2 and time.time() - t0 < 120:
    time.sleep(1)
print(f"Spiele laufen nach {time.time() - t0:.0f} s", flush=True)
shot_taken = False
while games() > 0 and time.time() - t0 < 500:
    time.sleep(2)
    if shot and not shot_taken:
        new = [d for d in set(glob.glob(os.path.join(LOGS, "*"))) - before if d.endswith("gast")]
        if new and "NICHT SYNCHRON" in open(os.path.join(new[0], "lbo.log"), encoding="latin-1").read():
            time.sleep(3)
            subprocess.run(["powershell", "-ExecutionPolicy", "Bypass", "-File",
                            os.path.join(ROOT, "tools", "desktop.ps1"), shot])
            shot_taken = True
            print("Bildschirmfoto der Meldung:", shot, flush=True)
print(f"Spiele beendet nach {time.time() - t0:.0f} s", flush=True)
time.sleep(20)  # Apps schicken die Fehlerdaten
new = sorted(set(glob.glob(os.path.join(LOGS, "*"))) - before)
for d in new:
    log = os.path.join(d, "lbo.log")
    if os.path.exists(log):
        txt = open(log, encoding="latin-1").read()
        print(os.path.basename(d), "Desync gemeldet:", "NICHT SYNCHRON" in txt)
partner = [d for d in new if d.endswith("partner")]
print("Fehlerdaten empfangen:", len(partner), [os.path.basename(d) for d in partner])
for d in partner:
    rows = open(os.path.join(d, "partner_frames.csv")).read().count("\n")
    print("  ", os.path.basename(d), open(os.path.join(d, "info.txt")).read().replace("\n", " | "), f"{rows} Zeilen")
for p in (a, b):
    subprocess.run(["taskkill", "/PID", str(p.pid)], capture_output=True)
