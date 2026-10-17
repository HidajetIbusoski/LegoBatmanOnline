"""Ende-zu-Ende-Test der App: zwei Instanzen auf diesem PC finden sich ueber den Vermittler,
werden Freunde, laden ein und starten das Spiel mit automatisch geladenem Spielstand.

python tools/test_app.py [exe] [slot] [maxframes]
Der echte Spielstand ist geschuetzt (--test setzt LBO_ALLOWSAVE=0). Die Testprofile testA/testB
werden jedes Mal neu angelegt.
"""
import glob, os, shutil, subprocess, sys, time

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
EXE = sys.argv[1] if len(sys.argv) > 1 else os.path.join(
    ROOT, "app", "LegoBatmanOnline", "bin", "Debug", "net10.0-windows", "win-x64", "LegoBatmanOnline.exe")
slot = sys.argv[2] if len(sys.argv) > 2 else "0"
maxframes = sys.argv[3] if len(sys.argv) > 3 else "7500"
BASE = os.path.join(os.environ["LOCALAPPDATA"], "LegoBatmanOnline")
LOGS = os.path.join(BASE, "logs")


def tasklist(name):
    out = subprocess.run(["tasklist", "/FI", f"IMAGENAME eq {name}", "/NH"], capture_output=True).stdout
    return out.decode("latin-1").count(name)


for p in ("testA", "testB"):
    shutil.rmtree(os.path.join(BASE, "app", p), ignore_errors=True)
before = set(glob.glob(os.path.join(LOGS, "*")))

common = ["--test", "--max-frames", maxframes]
b = subprocess.Popen([EXE, "--profil", "testB", "--name", "Kollege", "--auto-accept"] + common)
code_file = os.path.join(BASE, "app", "testB", "code.txt")
for _ in range(60):
    if os.path.exists(code_file):
        break
    time.sleep(0.5)
code = open(code_file).read().strip()
print("Code von B:", code, flush=True)
a = subprocess.Popen([EXE, "--profil", "testA", "--name", "Hidajet", "--auto-invite", code, "--slot", slot] + common)

t0 = time.time()
started = False
while time.time() - t0 < 600:
    n = tasklist("LEGOBatman.exe")
    if n >= 2 and not started:
        started = True
        print(f"Beide Spiele laufen nach {time.time() - t0:.0f} s", flush=True)
    if started and n == 0:
        print(f"Beide Spiele beendet nach {time.time() - t0:.0f} s", flush=True)
        break
    time.sleep(2)
else:
    print("ZEITUEBERSCHREITUNG", flush=True)

time.sleep(3)
new = sorted(set(glob.glob(os.path.join(LOGS, "*"))) - before)
print("Neue Protokolle:", new, flush=True)
host = [d for d in new if d.endswith("host")]
gast = [d for d in new if d.endswith("gast")]
if host and gast:
    subprocess.run([sys.executable, os.path.join(ROOT, "tools", "compare_runs.py"), host[-1], gast[-1]])
a.terminate()
b.terminate()
