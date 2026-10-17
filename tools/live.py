"""Steuert ein laufendes Spiel (LBO_MODE=script, LBO_LIVE=runs/live.txt).

live.py <rundir> [--wait SEK] [befehl ...]
Jeder Befehl wird als Zeile an runs/live.txt gehaengt (z. B. "rel 10 0 1000 0 0 0 0 0 0").
Danach wird gewartet, ein Screenshot angefordert und als PNG gespeichert; der Pfad wird ausgegeben.
"""
import os, subprocess, sys, time, glob

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
LIVE = os.path.join(ROOT, "runs", "live.txt")

def main():
    args = sys.argv[1:]
    rundir = args.pop(0)
    wait = 1.0
    if args and args[0] == "--wait":
        args.pop(0); wait = float(args.pop(0))
    with open(LIVE, "a", newline="\n") as f:
        for c in args:
            f.write(c + "\n")
    time.sleep(wait)
    before = set(glob.glob(os.path.join(rundir, "live_F*.bmp")))
    with open(LIVE, "a", newline="\n") as f:
        f.write("shot\n")
    for _ in range(100):
        time.sleep(0.2)
        new = set(glob.glob(os.path.join(rundir, "live_F*.bmp"))) - before
        if new:
            time.sleep(0.3)
            break
    else:
        print("kein Screenshot (Spiel haengt oder beendet?)"); return
    bmp = sorted(new)[-1]
    subprocess.run(["powershell", "-NoProfile", "-ExecutionPolicy", "Bypass", "-File",
                    os.path.join(ROOT, "tools", "bmp2png.ps1"), rundir], check=False)
    print(bmp[:-4] + ".png")
    with open(os.path.join(ROOT, "runs", "heartbeat.txt"), "w") as f:
        f.write(time.strftime("%Y-%m-%dT%H:%M:%S") + "\n")

main()
