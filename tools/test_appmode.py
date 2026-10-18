"""Testet den App-Modus des Launchers (--stun / --connect) mit automatischem Laden eines Spielstands.

python tools/test_appmode.py [slot] [maxframes]
Startet zwei Spiele auf diesem PC. Der echte Spielstand ist geschuetzt (LBO_ALLOWSAVE=0).
"""
import os, random, re, subprocess, sys, threading

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
LAUNCHER = os.path.join(ROOT, "build", "lbo-launch.exe")
slot = int(sys.argv[1]) if len(sys.argv) > 1 else 0
maxframes = sys.argv[2] if len(sys.argv) > 2 else "7500"


def stun():
    out = subprocess.run([LAUNCHER, "--stun"], capture_output=True, text=True).stdout
    m = re.search(r"STUN port=(\d+) public=(\S+) local=(\S+) symmetric=(\d) key=(\d+)", out)
    return dict(port=m[1], public=m[2], local=m[3], key=m[5])


h, g = stun(), stun()
print("Host", h)
print("Gast", g)
env = dict(os.environ, LBO_MULTI="1", LBO_WINDOWED="1", LBO_ALLOWSAVE="0", LBO_MAXFRAMES=maxframes, LBO_SHOT_EVERY="1500")


def run(name, args):
    p = subprocess.Popen([LAUNCHER] + args, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True, env=env)
    for line in p.stdout:
        print(f"[{name}] {line.rstrip()}", flush=True)


th = threading.Thread(target=run, args=("host", ["--connect", "host", "--port", h["port"], "--key", h["key"],
      "--peer-key", g["key"], "--peer-public", g["public"], "--peer-local", g["local"], "--slot", str(slot)]))
tg = threading.Thread(target=run, args=("gast", ["--connect", "join", "--port", g["port"], "--key", g["key"],
      "--peer-key", h["key"], "--peer-public", h["public"], "--peer-local", h["local"], "--slot", str(slot)]))
th.start(); tg.start(); th.join(); tg.join()
