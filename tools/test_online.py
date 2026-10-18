"""Testet den kompletten Online-Ablauf auf einem PC: zwei Launcher, Codes tauschen, zwei Spiele.

python tools/test_online.py <maxframes> [--echt] [--direkt]
Der Host bekommt die Pad-0-Eingaben aus tools/scripts/net_host.txt, der Gast die Pad-1-Eingaben
aus net_join.txt (statt echter Controller). LBO_ALLOWSAVE=0 schuetzt den echten Spielstand.
"""
import os, queue, re, subprocess, sys, threading, time

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
LAUNCHER = os.path.join(ROOT, "build", "lbo-launch.exe")
CODE_RE = re.compile(r"Dein Code:\s+([A-Z2-9-]+)")


def start(role, script, maxframes):
    env = dict(os.environ)
    env.update(LBO_MULTI="1", LBO_WINDOWED="1", LBO_ALLOWSAVE="0", LBO_MAXFRAMES=str(maxframes), LBO_SHOT_EVERY="3000")
    if script:  # ohne Skript: echte Controller/Tastatur (zum Pruefen der Geraeteerkennung)
        env["LBO_SCRIPT"] = os.path.join(ROOT, "tools", "scripts", script)
    p = subprocess.Popen([LAUNCHER], stdin=subprocess.PIPE, stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
                         env=env, text=True, bufsize=1)
    q = queue.Queue()

    def reader():
        for line in p.stdout:
            q.put(line.rstrip())
        q.put(None)
    threading.Thread(target=reader, daemon=True).start()
    p.stdin.write(role + "\n")
    p.stdin.flush()
    return p, q


def wait_for(q, pattern, name, timeout=60):
    end = time.time() + timeout
    while time.time() < end:
        try:
            line = q.get(timeout=1)
        except queue.Empty:
            continue
        if line is None:
            raise SystemExit(f"{name}: Launcher beendet")
        print(f"[{name}] {line}")
        m = pattern.search(line) if hasattr(pattern, "search") else (line if pattern in line else None)
        if m:
            return m
    raise SystemExit(f"{name}: Zeitueberschreitung beim Warten auf {pattern}")


def drain(q, name):
    while True:
        try:
            line = q.get(timeout=1)
        except queue.Empty:
            continue
        if line is None:
            return
        print(f"[{name}] {line}")


def main():
    maxframes = int(sys.argv[1]) if len(sys.argv) > 1 else 9000
    real = "--echt" in sys.argv
    if "--direkt" in sys.argv:
        # Direktmodus: Host lauscht auf Port 47100, Gast gibt 127.0.0.1 ein
        host, hq = start("3", None if real else "net_host.txt", maxframes)
        wait_for(hq, "Warte auf deinen Mitspieler", "host")
        guest, gq = start("4", None if real else "net_join.txt", maxframes)
        guest.stdin.write("127.0.0.1\n"); guest.stdin.flush()
        for p in (host, guest):
            p.stdin.write("\n\n"); p.stdin.flush()
        threads = [threading.Thread(target=drain, args=(hq, "host")), threading.Thread(target=drain, args=(gq, "gast"))]
        for t in threads: t.start()
        for t in threads: t.join()
        return
    host, hq = start("1", None if real else "net_host.txt", maxframes)
    hcode = wait_for(hq, CODE_RE, "host").group(1)
    guest, gq = start("2", None if real else "net_join.txt", maxframes)
    gcode = wait_for(gq, CODE_RE, "gast").group(1)
    host.stdin.write(gcode + "\n"); host.stdin.flush()
    guest.stdin.write(hcode + "\n"); guest.stdin.flush()
    # Am Ende fragt der Launcher nach Enter
    for p in (host, guest):
        p.stdin.write("\n\n"); p.stdin.flush()
    threads = [threading.Thread(target=drain, args=(hq, "host")), threading.Thread(target=drain, args=(gq, "gast"))]
    for t in threads: t.start()
    for t in threads: t.join()


main()
