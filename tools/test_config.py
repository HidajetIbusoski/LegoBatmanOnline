"""Desync-Suche: zwei Instanzen (Launcher --connect, Autoload) mit unterschiedlicher pcconfig.txt.

python tools/test_config.py <name> [schluessel=wert ...] [--frames N] [--both] [--gast-env K=V]
Der Host bekommt eine Kopie der echten pcconfig.txt, der Gast dieselbe mit den angegebenen
Aenderungen (z. B. ScreenWidth=1280 ScreenRefreshRate=60). Mit --both bekommen beide die geaenderte.
Ergebnis: Anzahl DESYNC-Meldungen im Host-Log und erster betroffener Frame.
"""
import os, re, subprocess, sys, threading

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
LAUNCHER = os.environ.get("LBO_TEST_LAUNCHER") or os.path.join(ROOT, "build", "lbo-launch.exe")
REAL_CFG = os.path.join(os.environ["LOCALAPPDATA"], "Warner Bros. Interactive Entertainment", "LEGO Batman", "pcconfig.txt")
TMP = os.path.join(ROOT, "runs", "cfgtest")

args = sys.argv[1:]
name = args.pop(0)
frames = "4500"
both = False
changes = {}
gast_env = {}
scripts = False  # --skript: Menues und Spieler 2 per net_host.txt/net_join.txt statt Autoload
focus = None  # (sekunde, art): art = msg (WM_ACTIVATEAPP), min (minimieren), fg (Fokus wegnehmen)
while args:
    a = args.pop(0)
    if a == "--frames":
        frames = args.pop(0)
    elif a == "--skript":
        scripts = True
    elif a == "--both":
        both = True
    elif a == "--gast-focus":
        sec, kind = args.pop(0).split(":")
        focus = (float(sec), kind)
    elif a == "--gast-env":
        k, v = args.pop(0).split("=", 1)
        gast_env[k] = v
    else:
        k, v = a.split("=", 1)
        changes[k] = v


def write_cfg(path, mods):
    lines = []
    for line in open(REAL_CFG, encoding="latin-1").read().splitlines():
        parts = line.split()
        if parts and parts[0] in mods:
            line = f"{parts[0]:<28}{mods[parts[0]]}"
        lines.append(line)
    os.makedirs(os.path.dirname(path), exist_ok=True)
    open(path, "w", encoding="latin-1", newline="\r\n").write("\n".join(lines) + "\n")


host_cfg = os.path.join(TMP, name, "host_pcconfig.txt")
gast_cfg = os.path.join(TMP, name, "gast_pcconfig.txt")
write_cfg(host_cfg, changes if both else {})
write_cfg(gast_cfg, changes)


def stun():
    out = subprocess.run([LAUNCHER, "--stun"], capture_output=True, text=True).stdout
    m = re.search(r"STUN port=(\d+) public=(\S+) local=(\S+) symmetric=(\d) key=(\d+)", out)
    return dict(port=m[1], public=m[2], local=m[3], key=m[5])


h, g = stun(), stun()
logdirs = {}
pids = {}


def gast_focus():
    import ctypes, time
    from ctypes import wintypes
    u = ctypes.windll.user32
    sec, kind = focus
    t0 = time.time()
    while "join" not in pids and time.time() - t0 < 60:
        time.sleep(0.5)
    time.sleep(sec)
    found = []
    proto = ctypes.WINFUNCTYPE(wintypes.BOOL, wintypes.HWND, wintypes.LPARAM)

    def cb(h, _):
        pid = wintypes.DWORD()
        u.GetWindowThreadProcessId(h, ctypes.byref(pid))
        if pid.value == pids["join"] and u.IsWindowVisible(h):
            found.append(h)
        return True
    u.EnumWindows(proto(cb), 0)
    if not found:
        print("Fenster des Gasts nicht gefunden"); return
    h = found[0]
    print(f"Fokus-Test ({kind}) am Gastfenster nach {sec} s", flush=True)
    if kind == "msg":
        u.PostMessageW(h, 0x001C, 0, 0)       # WM_ACTIVATEAPP verliert
        u.PostMessageW(h, 0x0006, 0, 0)       # WM_ACTIVATE WA_INACTIVE
        u.PostMessageW(h, 0x0008, 0, 0)       # WM_KILLFOCUS
        time.sleep(3)
        u.PostMessageW(h, 0x001C, 1, 0)
        u.PostMessageW(h, 0x0006, 1, 0)
        u.PostMessageW(h, 0x0007, 0, 0)       # WM_SETFOCUS
    elif kind == "min":
        u.ShowWindow(h, 6)                    # SW_MINIMIZE
        time.sleep(3)
        u.ShowWindow(h, 9)                    # SW_RESTORE
    elif kind == "keys":
        # Gastfenster nach vorn, dann Enter (START), Escape und F2 (Beitritt Spieler 2 per Tastatur)
        u.keybd_event(0x12, 0, 0, 0); u.SetForegroundWindow(h); u.keybd_event(0x12, 0, 2, 0)
        time.sleep(1)
        for vk in (0x0D, 0x1B, 0x71, 0x0D):
            u.keybd_event(vk, 0, 0, 0); time.sleep(0.3); u.keybd_event(vk, 0, 2, 0); time.sleep(4)
        print("Vordergrund war Gast:", u.GetForegroundWindow() == h, flush=True)
        u.SetForegroundWindow(u.GetShellWindow())
    elif kind == "fg":
        u.SetForegroundWindow(h)
        time.sleep(3)
        u.SetForegroundWindow(u.GetShellWindow())


def run(role, me, peer, cfg):
    env = dict(os.environ, LBO_MULTI="1", LBO_WINDOWED="1", LBO_ALLOWSAVE="0", LBO_MAXFRAMES=frames, LBO_PCCONFIG=cfg)
    if role == "join":
        env.update(gast_env)
    cmd = [LAUNCHER, "--connect", role, "--port", me["port"], "--key", me["key"],
           "--peer-key", peer["key"], "--peer-public", peer["public"], "--peer-local", peer["local"]]
    if scripts:
        env["LBO_SCRIPT"] = os.path.join(ROOT, "tools", "scripts", "net_host.txt" if role == "host" else "net_join.txt")
    else:
        cmd += ["--slot", "0"]
    p = subprocess.Popen(cmd, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True, env=env)
    for line in p.stdout:
        m = re.search(r"PID (\d+)", line)
        if m:
            pids[role] = int(m[1])
        m = re.search(r"Logs: (.+)\)", line)
        if m:
            logdirs[role] = m[1].strip()


th = threading.Thread(target=run, args=("host", h, g, host_cfg))
tg = threading.Thread(target=run, args=("join", g, h, gast_cfg))
th.start(); tg.start()
if focus:
    threading.Thread(target=gast_focus, daemon=True).start()
th.join(); tg.join()

log = open(os.path.join(logdirs["host"], "lbo.log"), encoding="latin-1").read()
des = re.findall(r"DESYNC bei Frame (\d+)", log)
thaw = [len(re.findall(r"Wachhund: Haupt", open(os.path.join(d, "lbo.log"), encoding="latin-1").read())) for d in logdirs.values()]
print(f"{name}: {changes} {gast_env} {'(beide)' if both else ''} -> {len(des)} Desyncs, Auftauungen {thaw}" + (f", erster bei Frame {des[0]}" if des else ""))
print("  Host:", logdirs.get("host"), " Gast:", logdirs.get("join"))
