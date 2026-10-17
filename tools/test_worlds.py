"""Test der gemeinsamen Welten mit zwei App-Instanzen auf diesem PC (Profile wA und wB).

python tools/test_worlds.py [exe] [teile]      teile z. B. "1234" (Standard)
  1  A fuegt B hinzu und erstellt die Welt "Gotham" aus Spielstand 1; B bekommt die Welt und den Stand
  2  Zusammen spielen: A laedt B ein, Spiel startet aus dem Weltordner (7500 Frames, Vergleich)
  3  Konflikt: B aendert den Stand, waehrend A aus ist; A uebernimmt beim Start den neueren und sichert seinen
  4  B laedt hoch und beendet die App; A startet danach allein und holt den Stand vom Vermittler

Der echte Spielstand wird nur gelesen (--test setzt LBO_ALLOWSAVE=0, die Welt hat eine Kopie).
"""
import glob, hashlib, json, os, shutil, subprocess, sys, time

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
EXE = sys.argv[1] if len(sys.argv) > 1 and sys.argv[1] else os.path.join(
    ROOT, "app", "LegoBatmanOnline", "bin", "Debug", "net10.0-windows", "win-x64", "LegoBatmanOnline.exe")
PARTS = sys.argv[2] if len(sys.argv) > 2 else "1234"
BASE = os.path.join(os.environ["LOCALAPPDATA"], "LegoBatmanOnline")
REAL = os.path.join(os.environ["LOCALAPPDATA"], "Warner Bros. Interactive Entertainment", "LEGO Batman",
                    "Saved Games", "SaveGame0.LEGO Batman_SavedGame")
LOGS = os.path.join(BASE, "logs")
ok_all = True


def check(cond, text):
    global ok_all
    print(("OK    " if cond else "FEHLER") + "  " + text, flush=True)
    ok_all &= bool(cond)


def sha(path):
    return hashlib.sha256(open(path, "rb").read()).hexdigest()[:16]


def profile(p):
    path = os.path.join(BASE, "app", p, "profil.json")
    try:
        return json.load(open(path, encoding="utf-8-sig"))
    except Exception:
        return {}


def world(p):
    ws = profile(p).get("Worlds") or []
    return ws[0] if ws else None


def save_file(p, w):
    return os.path.join(BASE, "welten", p, w["Id"], "spielstand", "SaveGame0.LEGO Batman_SavedGame")


def wait_for(cond, seconds, text):
    t0 = time.time()
    while time.time() - t0 < seconds:
        if cond():
            print(f"        {text} nach {time.time() - t0:.0f} s", flush=True)
            return True
        time.sleep(0.5)
    return False


def start(p, *args):
    return subprocess.Popen([EXE, "--profil", p, "--test", "--max-frames", "7500"] + list(args))


def stop(*procs):
    # normal schliessen (WM_CLOSE), damit die App sich abmeldet; notfalls hart beenden
    for pr in procs:
        subprocess.run(["taskkill", "/PID", str(pr.pid)], capture_output=True)
    for pr in procs:
        try:
            pr.wait(15)
        except subprocess.TimeoutExpired:
            pr.kill()
            pr.wait()
    time.sleep(1)


def count_games():
    out = subprocess.run(["tasklist", "/FI", "IMAGENAME eq LEGOBatman.exe", "/NH"], capture_output=True).stdout
    return out.decode("latin-1").count("LEGOBatman.exe")


real_before = sha(REAL)
code_b = None

if "1" in PARTS:
    print("--- 1: Welt erstellen und teilen", flush=True)
    for p in ("wA", "wB"):
        shutil.rmtree(os.path.join(BASE, "app", p), ignore_errors=True)
        shutil.rmtree(os.path.join(BASE, "welten", p), ignore_errors=True)
    b = start("wB", "--name", "Kollege", "--auto-accept")
    code_file = os.path.join(BASE, "app", "wB", "code.txt")
    wait_for(lambda: os.path.exists(code_file), 30, "B bereit")
    code_b = open(code_file).read().strip()
    a = start("wA", "--name", "Hidajet", "--auto-invite", code_b, "--auto-world", "Gotham", "--slot", "0")
    got = wait_for(lambda: world("wB") and os.path.exists(save_file("wB", world("wB")))
                   and world("wB").get("VersionTs", 0) > 0, 90, "B hat Welt und Spielstand")
    check(got, "B hat die Welt bekommen")
    if got:
        wa, wb = world("wA"), world("wB")
        check(wb["Name"] == "Gotham", f"Name der Welt bei B: {wb['Name']}")
        check(sha(save_file("wB", wb)) == sha(REAL), "Spielstand bei B gleich dem Original")
        check(wa["VersionSha"] == wb["VersionSha"] and wa["VersionTs"] == wb["VersionTs"], "Version bei A und B gleich")
    stop(a, b)

if "2" in PARTS:
    print("--- 2: zusammen spielen", flush=True)
    code_b = code_b or open(os.path.join(BASE, "app", "wB", "code.txt")).read().strip()
    before = set(glob.glob(os.path.join(LOGS, "*")))
    b = start("wB", "--auto-accept")
    a = start("wA", "--auto-invite", code_b, "--auto-world", "Gotham", "--auto-play")
    started = wait_for(lambda: count_games() >= 2, 120, "beide Spiele laufen")
    check(started, "Spiele gestartet")
    if started:
        wait_for(lambda: count_games() == 0, 400, "beide Spiele beendet")
        time.sleep(3)
        new = sorted(set(glob.glob(os.path.join(LOGS, "*"))) - before)
        host = [d for d in new if d.endswith("host")]
        gast = [d for d in new if d.endswith("gast")]
        if host and gast:
            r = subprocess.run([sys.executable, os.path.join(ROOT, "tools", "compare_runs.py"), host[-1], gast[-1]],
                               capture_output=True, text=True).stdout
            print("        " + r.strip().replace("\n", "\n        "), flush=True)
            check("IDENTISCH" in r, "Host und Gast identisch")
            log = open(os.path.join(host[-1], "lbo.log"), encoding="latin-1").read()
            check("welten" in log.lower(), "Host spielte aus dem Weltordner")
        else:
            check(False, "Protokolle gefunden")
    stop(a, b)

if "3" in PARTS:
    print("--- 3: Konflikt, neuester gewinnt", flush=True)
    wb = world("wB")
    fb = save_file("wB", wb)
    data = bytearray(open(fb, "rb").read())
    data[-1] ^= 0xFF  # B hat "weitergespielt" (Datei geaendert, nur fuer den Test)
    open(fb, "wb").write(data)
    newer = sha(fb)
    b = start("wB")
    wait_for(lambda: world("wB")["VersionSha"] != wb["VersionSha"], 30, "B erkennt neuen Stand")
    time.sleep(12)  # B laedt nach dem Verbinden hoch
    a = start("wA")
    wa = world("wA")
    got = wait_for(lambda: sha(save_file("wA", wa)) == newer, 60, "A hat den neueren Stand")
    check(got, "A hat den neueren Stand von B übernommen")
    backups = glob.glob(os.path.join(BASE, "welten", "wA", wa["Id"], "sicherungen", "*", "SaveGame0*"))
    check(any(sha(x) == sha(REAL) for x in backups), f"alter Stand von A gesichert ({len(backups)} Sicherung(en))")
    check(world("wA")["VersionBy"] == "Kollege", "Stand stammt von Kollege")
    stop(a, b)

if "4" in PARTS:
    print("--- 4: offline weitergeben (B laedt hoch und geht, danach startet A allein)", flush=True)
    wb = world("wB")
    fb = save_file("wB", wb)
    data = bytearray(open(fb, "rb").read())
    data[-2] ^= 0xFF
    open(fb, "wb").write(data)
    newer = sha(fb)
    b = start("wB")
    wait_for(lambda: world("wB")["VersionSha"] != wb["VersionSha"], 30, "B erkennt neuen Stand")
    time.sleep(15)
    stop(b)
    a = start("wA")
    wa = world("wA")
    got = wait_for(lambda: sha(save_file("wA", wa)) == newer, 60, "A hat den Stand vom Vermittler")
    check(got, "A bekommt den Stand, obwohl B offline ist")
    stop(a)

check(sha(REAL) == real_before, "echter Spielstand unverändert")
print("ALLES OK" if ok_all else "ES GAB FEHLER", flush=True)
