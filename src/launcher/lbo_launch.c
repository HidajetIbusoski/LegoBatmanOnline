// lbo-launch: startet LEGOBatman.exe angehalten, laedt lbo.dll per Remote-Thread
// und laesst das Spiel dann weiterlaufen. Im Spielordner wird nichts abgelegt.
//
// Ohne Argumente: Online-Menue (Hosten / Beitreten per Einladungscode).
// Entwicklermodus: lbo-launch.exe [--game <pfad\LEGOBatman.exe>] [--dll <pfad\lbo.dll>] [--wait] [-- spielargumente...]
// Die LBO_*-Umgebungsvariablen (siehe lbo.c) werden an das Spiel vererbt.
#define _WINSOCK_DEPRECATED_NO_WARNINGS
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#include <shlobj.h>
#include <shellapi.h>
#include <tlhelp32.h>
#include <restartmanager.h>
#include <stdio.h>
#include <string.h>
#include <wchar.h>
#include "online.h"
#include "../common/b2cfg.h"
#pragma comment(lib, "rstrtmgr.lib")

// --b2: LEGO Batman 2 statt LEGO Batman (eigene DLL lbo2.dll, siehe docs/batman2-phase0.md)
static int g_b2;

static int fail(const char *what)
{
    fprintf(stderr, "lbo-launch: %s fehlgeschlagen (Fehler %lu)\n", what, GetLastError());
    return 1;
}

// --- Spiel finden ---------------------------------------------------------------------
static int try_game(const char *lib, char *out)
{
    char p[MAX_PATH];
    snprintf(p, sizeof p, g_b2 ? "%s\\steamapps\\common\\LEGO Batman 2\\LEGOBatman2.exe"
                               : "%s\\steamapps\\common\\Lego Batman\\LEGOBatman.exe", lib);
    if (GetFileAttributesA(p) == INVALID_FILE_ATTRIBUTES) return 0;
    strcpy(out, p);
    return 1;
}

static int find_game(char *out)
{
    char steam[MAX_PATH], vdf[MAX_PATH], line[1024];
    DWORD len = sizeof steam;
    FILE *f;
    HKEY k;
    if (RegOpenKeyExA(HKEY_CURRENT_USER, "Software\\Valve\\Steam", 0, KEY_READ, &k) != ERROR_SUCCESS) return 0;
    if (RegQueryValueExA(k, "SteamPath", NULL, NULL, (BYTE *)steam, &len) != ERROR_SUCCESS) { RegCloseKey(k); return 0; }
    RegCloseKey(k);
    for (char *c = steam; *c; c++) if (*c == '/') *c = '\\';
    if (try_game(steam, out)) return 1;
    snprintf(vdf, sizeof vdf, "%s\\steamapps\\libraryfolders.vdf", steam);
    f = fopen(vdf, "r");
    if (!f) return 0;
    while (fgets(line, sizeof line, f)) {
        // Zeilen der Form:   "path"   "D:\\SteamLibrary"
        char *p = strstr(line, "\"path\"");
        char lib[MAX_PATH];
        int n = 0;
        if (!p) continue;
        p = strchr(p + 6, '"');
        if (!p) continue;
        for (p++; *p && *p != '"' && n < MAX_PATH - 1; p++) {
            if (p[0] == '\\' && p[1] == '\\') p++;
            lib[n++] = *p;
        }
        lib[n] = 0;
        if (try_game(lib, out)) { fclose(f); return 1; }
    }
    fclose(f);
    return 0;
}

// --- Spiel starten und DLL laden --------------------------------------------------------
// --- Steam (nur LEGO Batman 2) ------------------------------------------------------------
// Batman 2 hat einen Steam-Kopierschutz: Laeuft Steam nicht (oder ist niemand angemeldet), startet das Spiel
// Steam und beendet sich, oder es wartet auf ein Steam-Fenster. Im Playtest (20.10. abends) kam das Spiel des
// Mitspielers so zweimal nicht ueber den Start hinaus. Deshalb vorher Steam starten und auf die Anmeldung warten.
static int process_running(const char *exe)
{
    PROCESSENTRY32 pe;
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    int found = 0;
    if (snap == INVALID_HANDLE_VALUE) return 0;
    pe.dwSize = sizeof pe;
    if (Process32First(snap, &pe)) {
        do { if (!_stricmp(pe.szExeFile, exe)) { found = 1; break; } } while (Process32Next(snap, &pe));
    }
    CloseHandle(snap);
    return found;
}

// Bereit: angemeldeter Benutzer UND die in ActiveProcess\pid eingetragene Steam-Instanz laeuft wirklich (ActiveUser
// allein bleibt vom letzten Lauf stehen; im Playtest 22.10. galt Steam so zu frueh als bereit)
static DWORD steam_active_user(void)
{
    DWORD user = 0, pid = 0, len = sizeof user;
    HANDLE h;
    char name[MAX_PATH];
    DWORD nlen = sizeof name;
    if (RegGetValueA(HKEY_CURRENT_USER, "Software\\Valve\\Steam\\ActiveProcess", "ActiveUser", RRF_RT_REG_DWORD, NULL, &user, &len))
        return 0;
    len = sizeof pid;
    if (!user || RegGetValueA(HKEY_CURRENT_USER, "Software\\Valve\\Steam\\ActiveProcess", "pid", RRF_RT_REG_DWORD, NULL, &pid, &len) || !pid)
        return 0;
    h = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
    if (!h) return 0;
    if (!QueryFullProcessImageNameA(h, 0, name, &nlen)) { CloseHandle(h); return 0; }
    CloseHandle(h);
    {
        size_t n = strlen(name);
        if (n < 9 || _stricmp(name + n - 9, "steam.exe")) return 0;
    }
    return user;
}

static void ensure_steam(void)
{
    char exe[MAX_PATH];
    DWORD len = sizeof exe, t0;
    int asked = 0, started = 0;
    if (process_running("steam.exe") && steam_active_user()) return;
    printf("STATUS steam\n");
    if (!process_running("steam.exe")) {
        if (RegGetValueA(HKEY_CURRENT_USER, "Software\\Valve\\Steam", "SteamExe", RRF_RT_REG_SZ, NULL, exe, &len)) {
            printf("Steam laeuft nicht und wurde nicht gefunden. Bitte Steam starten.\n");
        } else {
            printf("Steam laeuft nicht, starte Steam ...\n");
            ShellExecuteA(NULL, "open", exe, "-silent", NULL, SW_SHOWMINNOACTIVE);
            started = 1;
        }
    }
    fflush(stdout);
    t0 = GetTickCount();
    while (GetTickCount() - t0 < 120000) {
        // Selbst gestartet: mindestens 20 s und bis die Steam-Oberflaeche laeuft
        if (process_running("steam.exe") && steam_active_user() &&
            (!started || (GetTickCount() - t0 > 20000 && process_running("steamwebhelper.exe")))) {
            printf("Steam ist bereit.\n");
            fflush(stdout);
            Sleep(3000);   // Steam braucht nach der Anmeldung noch einen Moment fuer seine Dienste
            return;
        }
        if (!asked && GetTickCount() - t0 > 15000) {
            printf("Warte auf Steam - bitte bei Steam anmelden, falls ein Fenster offen ist.\n");
            fflush(stdout);
            asked = 1;
        }
        Sleep(500);
    }
    printf("Steam ist nach 2 Minuten nicht bereit, starte das Spiel trotzdem.\n");
}

static int launch(const char *game, const char *dll, const char *args, int wait)
{
    char cmdline[4096], dir[MAX_PATH];
    STARTUPINFOA si = { sizeof si };
    PROCESS_INFORMATION pi;
    strcpy(dir, game);
    { char *slash = strrchr(dir, '\\'); if (slash) *slash = 0; }
    snprintf(cmdline, sizeof cmdline, "\"%s\"%s%s", game, args && *args ? " " : "", args ? args : "");
    if (!CreateProcessA(game, cmdline, NULL, NULL, FALSE, CREATE_SUSPENDED, NULL, dir, &si, &pi))
        return fail("CreateProcess");

    size_t len = strlen(dll) + 1;
    void *remote = VirtualAllocEx(pi.hProcess, NULL, len, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
    if (!remote || !WriteProcessMemory(pi.hProcess, remote, dll, len, NULL)) {
        TerminateProcess(pi.hProcess, 1);
        return fail("Schreiben des DLL-Pfads");
    }
    // kernel32 liegt in allen 32-Bit-Prozessen an derselben Adresse wie bei uns
    LPTHREAD_START_ROUTINE loadlib =
        (LPTHREAD_START_ROUTINE)GetProcAddress(GetModuleHandleA("kernel32.dll"), "LoadLibraryA");
    HANDLE th = CreateRemoteThread(pi.hProcess, NULL, 0, loadlib, remote, 0, NULL);
    if (!th) {
        TerminateProcess(pi.hProcess, 1);
        return fail("CreateRemoteThread");
    }
    WaitForSingleObject(th, 30000);
    DWORD mod = 0;
    GetExitCodeThread(th, &mod);
    CloseHandle(th);
    VirtualFreeEx(pi.hProcess, remote, 0, MEM_RELEASE);
    if (!mod) {
        TerminateProcess(pi.hProcess, 1);
        fprintf(stderr, "lbo-launch: LoadLibraryA im Spiel lieferte NULL\n");
        return 1;
    }

    ResumeThread(pi.hThread);
    printf("lbo-launch: Spiel gestartet (PID %lu), Mod-DLL bei 0x%08lx\n", pi.dwProcessId, mod);
    fflush(stdout);

    DWORD code = 0;
    if (wait) {
        WaitForSingleObject(pi.hProcess, INFINITE);
        GetExitCodeProcess(pi.hProcess, &code);
        printf("lbo-launch: Spiel beendet, Exitcode %lu\n", code);
    }
    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);
    return (int)code;
}

static void dll_next_to_exe(char *dll)
{
    char *slash;
    GetModuleFileNameA(NULL, dll, MAX_PATH);
    slash = strrchr(dll, '\\');
    strcpy(slash ? slash + 1 : dll, g_b2 ? "lbo2.dll" : "lbo.dll");
}

// --- Online-Menue -----------------------------------------------------------------------
static void read_line(char *buf, int size)
{
    if (!fgets(buf, size, stdin)) buf[0] = 0;
    buf[strcspn(buf, "\r\n")] = 0;
}

static void press_enter(void)
{
    char b[8];
    printf("\nEnter druecken zum Beenden ...");
    read_line(b, sizeof b);
}

static int run_session(ONLINE *o, const char *game, const char *dll, int autoload_slot, const char *host_saves);

// --- LEGO Batman 2: gemeinsame Einstellungen (pcconfig.txt) -------------------------------------
// Desync 23.10.2026: Die pcconfig.txt des Hosts war beim Start kurz nicht lesbar, es ging keine Datei an den Gast,
// und beide Spiele nahmen still ihre eigene. Jetzt einigen sich beide hier auf genau einen Satz Bytes:
//  Host: liest die Datei robust (b2cfg_load); sonst die zuletzt gute Kopie, sonst den eingebauten Standard. Er
//        schickt immer genau diese eine Datei und benutzt selbst dieselben Bytes.
//  Gast: verlangt genau eine Datei "pcconfig.txt".
//  Beide: schreiben die Bytes nach <logdir>\netz_basis.txt (liest sonst niemand), pruefen sie per Zuruecklesen,
//        setzen LBO_HOSTCONFIG/LBO_CFGHASH fuer lbo2.dll und vergleichen den Hash an der Bereit-Schranke.
static const char *u8(const wchar_t *w, char *out, int n)
{
    if (!WideCharToMultiByte(CP_UTF8, 0, w, -1, out, n, NULL, NULL)) strcpy(out, "?");
    return out;
}

static const char *src_name(unsigned src)
{
    return src == B2CFG_SRC_ECHT ? "pcconfig.txt des Hosts" : src == B2CFG_SRC_KOPIE ? "zuletzt gute Kopie des Hosts"
         : src == B2CFG_SRC_STANDARD ? "eingebauter Grafik-Standard" : "unbekannt";
}

// Wer haelt die Datei offen? (Restart Manager, nur fuers Log)
static void log_lock_owner(const wchar_t *path)
{
    DWORD sess, reason = 0;
    WCHAR key[CCH_RM_SESSION_KEY + 1];
    RM_PROCESS_INFO info[8];
    UINT need = 0, n = 8, i;
    LPCWSTR files[1];
    char name[400];
    files[0] = path;
    if (RmStartSession(&sess, 0, key) != ERROR_SUCCESS) return;
    if (RmRegisterResources(sess, 1, files, 0, NULL, 0, NULL) == ERROR_SUCCESS) {
        DWORD r = RmGetList(sess, &need, &n, info, &reason);
        if (r == ERROR_SUCCESS) {
            for (i = 0; i < n && i < 8; i++)
                printf("Einstellungen: Datei ist offen bei %s (PID %lu)\n", u8(info[i].strAppName, name, sizeof name), info[i].Process.dwProcessId);
            if (!n) printf("Einstellungen: kein Prozess haelt die Datei offen (laut Restart Manager)\n");
        } else if (r == ERROR_MORE_DATA) {
            printf("Einstellungen: Datei ist bei %u Prozessen offen\n", need);
        }
    }
    RmEndSession(sess);
}

static void cfg_read_error(int attempt, DWORD err, const wchar_t *path)
{
    char p[MAX_PATH * 3];
    if (attempt == 1 || attempt % 5 == 0)
        printf("Einstellungen: %s nicht lesbar (Versuch %d, Fehler %lu)\n", u8(path, p, sizeof p), attempt, err);
    if (attempt == 1 && (err == ERROR_SHARING_VIOLATION || err == ERROR_LOCK_VIOLATION || err == ERROR_ACCESS_DENIED))
        log_lock_owner(path);
}

static int write_file_w(const wchar_t *path, const void *data, DWORD len)
{
    HANDLE h = CreateFileW(path, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    DWORD w = 0;
    BOOL ok;
    if (h == INVALID_HANDLE_VALUE) return 0;
    ok = WriteFile(h, data, len, &w, NULL) && w == len;
    if (!CloseHandle(h)) ok = FALSE;
    return ok;
}

// Rueckgabe 1: hash (SHA-256 der benutzten Bytes) und *src gesetzt, LBO_HOSTCONFIG/LBO_CFGHASH gesetzt
static int b2_settings(ONLINE *o, const char *logdir, unsigned char hash[32], unsigned *src)
{
    static char buf[B2CFG_MAX], back[B2CFG_MAX];
    wchar_t real[MAX_PATH], cache[MAX_PATH], tmp[MAX_PATH], dir[MAX_PATH], basis[MAX_PATH];
    DWORD len = 0, err = 0, err_real = 0, blen = 0;
    unsigned char h2[32];
    char hex[65], p[MAX_PATH * 3];
    *src = B2CFG_SRC_NONE;
    if (o->role == 0) {
        if (SHGetFolderPathW(NULL, CSIDL_APPDATA, NULL, 0, real) != S_OK) real[0] = 0;
        else wcscat_s(real, MAX_PATH, L"\\Warner Bros. Interactive Entertainment\\LEGO Batman 2\\pcconfig.txt");
        if (SHGetFolderPathW(NULL, CSIDL_LOCAL_APPDATA, NULL, 0, cache) != S_OK) cache[0] = 0;
        else {
            wcscat_s(cache, MAX_PATH, L"\\LegoBatmanOnline");
            CreateDirectoryW(cache, NULL);
            wcscat_s(cache, MAX_PATH, L"\\host-einstellungen");
            CreateDirectoryW(cache, NULL);
            wcscat_s(cache, MAX_PATH, L"\\letzte_pcconfig.txt");
        }
        if (b2cfg_load(real, buf, sizeof buf, &len, &err_real, 15, cfg_read_error)) {
            *src = B2CFG_SRC_ECHT;
            // zuletzt gute Kopie fuer das naechste Mal (erst schreiben, dann ersetzen)
            if (cache[0]) {
                swprintf(tmp, MAX_PATH, L"%ls.neu", cache);
                if (!write_file_w(tmp, buf, len) || !MoveFileExW(tmp, cache, MOVEFILE_REPLACE_EXISTING))
                    printf("Einstellungen: Kopie %s nicht gespeichert (Fehler %lu)\n", u8(cache, p, sizeof p), GetLastError());
            }
        } else if (cache[0] && b2cfg_load(cache, buf, sizeof buf, &len, &err, 3, NULL)) {
            *src = B2CFG_SRC_KOPIE;
            printf("Einstellungen: pcconfig.txt nicht lesbar (Fehler %lu), nehme die zuletzt gute Kopie\n", err_real);
        } else {
            *src = B2CFG_SRC_STANDARD;
            len = (DWORD)strlen(k_b2cfg_default);
            memcpy(buf, k_b2cfg_default, len + 1);
            printf("Einstellungen: pcconfig.txt nicht lesbar (Fehler %lu), keine Kopie - Grafik-Standard verwendet\n", err_real);
        }
        if (!online_send_blob(o, "pcconfig.txt", buf, len)) { printf("ERROR Einstellungen konnten nicht gesendet werden.\n"); return 0; }
    } else {
        unsigned ulen = 0;
        if (!online_receive_blob(o, "pcconfig.txt", buf, sizeof buf - 1, &ulen)) {
            printf("ERROR Einstellungen des Hosts nicht empfangen.\n");
            return 0;
        }
        len = ulen;
        buf[len] = 0;
    }
    b2_sha256(buf, len, hash);
    b2_hex(hash, hex);
    // Dieselben Bytes fuer die eigene DLL: Datei im Log-Ordner, die sonst niemand oeffnet; zuruecklesen und pruefen
    if (!MultiByteToWideChar(CP_ACP, 0, logdir, -1, dir, MAX_PATH)) { printf("ERROR Log-Ordner ungueltig.\n"); return 0; }
    swprintf(basis, MAX_PATH, L"%ls\\netz_basis.txt", dir);
    if (!write_file_w(basis, buf, len)) {
        printf("ERROR Einstellungen nicht speicherbar (%s, Fehler %lu).\n", u8(basis, p, sizeof p), GetLastError());
        return 0;
    }
    if (!b2cfg_load(basis, back, sizeof back, &blen, &err, 5, NULL) || (b2_sha256(back, blen, h2), memcmp(h2, hash, 32))) {
        printf("ERROR Einstellungen fehlerhaft (%s, Fehler %lu).\n", u8(basis, p, sizeof p), err);
        return 0;
    }
    SetEnvironmentVariableW(L"LBO_HOSTCONFIG", basis);
    SetEnvironmentVariableA("LBO_CFGHASH", hex);
    printf("Einstellungen: %s, %lu Bytes, SHA-256 %.16s\n", o->role == 0 ? src_name(*src) : "vom Host empfangen", len, hex);
    return 1;
}

// Immer Vollbild (unabhaengig von der Einstellung im Spiel). Nur Tests mit zwei Spielen auf
// einem PC laufen im Fenster (LBO_WINDOWED=1).
static const char *window_mode(void)
{
    char b[8];
    return GetEnvironmentVariableA("LBO_WINDOWED", b, sizeof b) && b[0] == '1' ? "-Windowed" : "-Fullscreen";
}

static int online_menu(void)
{
    char game[MAX_PATH], dll[MAX_PATH], line[128], code[40];
    unsigned char hash[32];
    ONLINE o;

    setvbuf(stdout, NULL, _IONBF, 0);  // sofort anzeigen, auch wenn die Ausgabe umgeleitet ist
    SetConsoleOutputCP(CP_UTF8);
    printf("==============================================\n");
    printf("  LEGO Batman Online  (Koop fuer 2 Spieler)\n");
    printf("==============================================\n\n");

    if (!find_game(game)) {
        printf("LEGO Batman wurde in keiner Steam-Bibliothek gefunden.\n"
               "Bitte den vollen Pfad zu LEGOBatman.exe eingeben:\n> ");
        read_line(game, sizeof game);
        if (GetFileAttributesA(game) == INVALID_FILE_ATTRIBUTES) { printf("Datei nicht gefunden.\n"); press_enter(); return 1; }
    }
    printf("Spiel: %s\n", game);
    dll_next_to_exe(dll);
    if (GetFileAttributesA(dll) == INVALID_FILE_ATTRIBUTES) { printf("lbo.dll fehlt neben lbo-launch.exe.\n"); press_enter(); return 1; }
    online_sha256_file(game, hash);

    memset(&o, 0, sizeof o);
    printf("\n  1 = Hosten (du bist Spieler 1 und bedienst die Menues)\n"
           "  2 = Beitreten (du bist Spieler 2)\n"
           "\n  Falls die Verbindung per Code nicht klappt (VPN wie Radmin/Tailscale):\n"
           "  3 = Hosten direkt per IP\n"
           "  4 = Beitreten direkt per IP\n> ");
    read_line(line, sizeof line);
    if (line[0] < '1' || line[0] > '4') { printf("Abgebrochen.\n"); press_enter(); return 1; }
    o.role = line[0] == '2' || line[0] == '4';

    if (line[0] <= '2') {
        if (!online_prepare(&o, code)) { press_enter(); return 1; }
        printf("\nDein Code:   %s\n\n", code);
        printf("Schick diesen Code an deinen Mitspieler (Discord, WhatsApp, ...).\n");
        for (;;) {
            printf("Code deines Mitspielers eingeben:\n> ");
            read_line(line, sizeof line);
            if (online_set_peer_code(&o, line)) break;
            printf("Der Code ist ungueltig, bitte nochmal pruefen.\n");
        }
    } else {
        // Direktverbindung (VPN, LAN oder Portweiterleitung)
        o.port = o.role ? 0 : DIRECT_PORT;
        if (!online_prepare_direct(&o)) { press_enter(); return 1; }
        if (o.role == 0) {
            printf("\nWarte auf deinen Mitspieler (UDP-Port %d).\n"
                   "Er gibt deine IP-Adresse ein (bei Radmin VPN/Tailscale die dort angezeigte IP).\n", DIRECT_PORT);
        } else {
            for (;;) {
                printf("IP-Adresse des Hosts eingeben (z. B. 26.12.34.56):\n> ");
                read_line(line, sizeof line);
                if (online_set_peer_ip(&o, line)) break;
                printf("Ungueltige Adresse.\n");
            }
        }
    }
    if (!online_connect(&o, hash, o.role == 0 && o.port == DIRECT_PORT ? 600 : 90)) {
        printf("\nTipps: Beide muessen den Code des anderen eingegeben haben. Klappt es gar nicht,\n"
               "nutzt beide ein VPN wie Radmin VPN oder Tailscale (siehe ANLEITUNG.txt).\n");
        online_close();
        press_enter();
        return 1;
    }
    {
        int rc = run_session(&o, game, dll, -1, NULL);
        press_enter();
        return rc;
    }
}

// Nach erfolgreichem Verbindungsaufbau: Ping, Spielstand, Bereit-Abgleich, Spiel starten.
// autoload_slot >= 0: der Host laedt diesen Spielstand automatisch (Menue per Skript).
// host_saves: Spielstand-Ordner des Hosts (z. B. eine gemeinsame Welt); NULL = normaler Ordner des Spiels.
static int run_session(ONLINE *o_, const char *game, const char *dll, int autoload_slot, const char *host_saves)
{
    char save_dir[MAX_PATH], guest_saves[MAX_PATH], logdir[MAX_PATH], peer[64], buf[64], appdata[MAX_PATH];
    SYSTEMTIME st;
    int delay, rb;
    unsigned char cfg[32], peer_cfg[32];
    unsigned src = B2CFG_SRC_NONE, peer_src = B2CFG_SRC_NONE;
    ONLINE o = *o_;
    memset(cfg, 0, sizeof cfg);
    memset(peer_cfg, 0, sizeof peer_cfg);
    printf("STATUS ping\n");
    if (online_measure_ping(&o) < 0) { printf("ERROR Ping-Messung fehlgeschlagen.\n"); online_close(); return 1; }
    // Eingabeverzoegerung: halbe Rundlaufzeit plus Puffer, in 60-Hz-Frames
    delay = (o.ping_ms / 2 + 20 + 16) / 17 + 1;
    if (delay < 2) delay = 2;
    if (delay > 10) delay = 10;
    printf("Ping: %d ms -> Eingabeverzoegerung %d Frames (%d ms)\n", o.ping_ms, delay, delay * 1000 / 60);

    SHGetFolderPathA(NULL, CSIDL_LOCAL_APPDATA, NULL, 0, appdata);
    if (host_saves) snprintf(save_dir, sizeof save_dir, "%s", host_saves);
    else snprintf(save_dir, sizeof save_dir, "%s\\Warner Bros. Interactive Entertainment\\LEGO Batman\\Saved Games", appdata);
    snprintf(guest_saves, sizeof guest_saves, "%s\\LegoBatmanOnline", appdata);
    CreateDirectoryA(guest_saves, NULL);
    GetLocalTime(&st);
    snprintf(logdir, sizeof logdir, "%s\\LegoBatmanOnline\\logs", appdata);
    CreateDirectoryA(logdir, NULL);
    snprintf(logdir + strlen(logdir), sizeof logdir - strlen(logdir), "\\%04d%02d%02d-%02d%02d%02d-%s",
             st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute, st.wSecond, o.role ? "gast" : "host");
    CreateDirectoryA(logdir, NULL);

    printf("STATUS spielstand\n");
    // LEGO Batman 2: noch kein Spielstand-Austausch. Beide starten ohne Spielstaende (die DLL leitet
    // Lesen und Schreiben im Netzmodus um), der Host waehlt "Neues Spiel". Uebertragen wird nur die pcconfig.txt
    // des Hosts: Andere Grafikeinstellungen lassen Figuren auseinanderlaufen (Test CA, 23.10.: Gast mit niedrigen
    // Einstellungen, ab Frame 912 nach Levelstart andere Positionen). Die DLL baut daraus die Einstellungen
    // beider Spiele (eigene nur Aufloesung, Fenster, Lautstaerke und Steuerung), siehe b2_settings.
    if (g_b2) {
        if (!b2_settings(&o, logdir, cfg, &src)) { online_close(); return 1; }
    } else if (o.role == 0) {
        if (!online_send_saves(&o, save_dir, "SaveGame*")) { printf("ERROR Spielstand konnte nicht gesendet werden.\n"); online_close(); return 1; }
    } else {
        strcat(guest_saves, "\\gast-spielstand");
        // alte Dateien entfernen, damit nur der Stand des Hosts sichtbar ist
        {
            char pat[MAX_PATH], p[MAX_PATH];
            WIN32_FIND_DATAA fd;
            HANDLE fh;
            snprintf(pat, sizeof pat, "%s\\*", guest_saves);
            fh = FindFirstFileA(pat, &fd);
            if (fh != INVALID_HANDLE_VALUE) {
                do {
                    if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) continue;
                    snprintf(p, sizeof p, "%s\\%s", guest_saves, fd.cFileName);
                    DeleteFileA(p);
                } while (FindNextFileA(fh, &fd));
                FindClose(fh);
            }
        }
        if (!online_receive_saves(&o, guest_saves)) { printf("ERROR Spielstand nicht empfangen.\n"); online_close(); return 1; }
    }
    rb = online_ready_barrier(&o, &delay, cfg, src, peer_cfg, &peer_src);
    if (rb == -2) { printf("ERROR Dein Mitspieler hat eine andere Version von LEGO Online - bitte beide aktualisieren.\n"); online_close(); return 1; }
    if (rb == -1) {
        char mine[65], theirs[65];
        b2_hex(cfg, mine);
        b2_hex(peer_cfg, theirs);
        printf("ERROR Einstellungen verschieden (Host %.8s / Gast %.8s) - bitte neu einladen.\n",
               o.role == 0 ? mine : theirs, o.role == 0 ? theirs : mine);
        online_close();
        return 1;
    }
    if (rb != 1) { printf("ERROR Mitspieler hat nicht bestaetigt.\n"); online_close(); return 1; }
    if (g_b2 && o.role == 1) printf("Einstellungen des Hosts: %s (gleich, SHA-256 geprueft)\n", src_name(peer_src));
    snprintf(peer, sizeof peer, "%s:%d", inet_ntoa(o.peer.sin_addr), ntohs(o.peer.sin_port));
    online_close();

    // Einstellungen fuer die DLL (siehe lbo.c / net.c)
    SetEnvironmentVariableA("LBO_DIR", logdir);
    SetEnvironmentVariableA("LBO_NET", o.role ? "join" : "host");
    snprintf(buf, sizeof buf, "%d", o.port); SetEnvironmentVariableA("LBO_PORT", buf);
    SetEnvironmentVariableA("LBO_PEER", peer);
    // Beide muessen dieselbe Verzoegerung nutzen; der Gast hat den Wert des Hosts uebernommen.
    snprintf(buf, sizeof buf, "%d", delay); SetEnvironmentVariableA("LBO_DELAY", buf);
    if (g_b2) {
        // Alle Determinismus-Schalter setzt lbo2.dll im Netzmodus selbst
        SetEnvironmentVariableA("SteamAppId", "213330");
        SetEnvironmentVariableA("LBO_FPSCAP", "60");
        SetEnvironmentVariableA("LBO_ALLOWSAVE", "0");
        printf("\nStarte LEGO Batman 2 ... (Logs: %s)\n", logdir);
        printf("%s\n", o.role ? "Der Host bedient die Menues (Neues Spiel). Im Level mit Start einsteigen."
                              : "Du bedienst die Menues: Neues Spiel, ohne zu speichern fortfahren. Dein Mitspieler steigt mit Start ein.");
        printf("STATUS spiel\n");
        {
            // Beendet sich das Spiel in den ersten 20 s von selbst, hat meist der Steam-Kopierschutz es ueber Steam neu
            // gestartet (Steam war noch nicht bereit). Dann einmal erneut starten; der Partner wartet so lange.
            DWORD t0 = GetTickCount();
            launch(game, dll, "", 1);
            if (GetTickCount() - t0 < 20000) {
                printf("Spiel hat sich sofort beendet - starte es in 10 s noch einmal.\n");
                fflush(stdout);
                Sleep(10000);
                if (process_running("LEGOBatman2.exe")) {
                    // Steam hat es selbst neu gestartet, dann aber ohne Mod: nicht noch ein zweites daneben starten
                    printf("ERROR Steam hat LEGO Batman 2 ohne Mod gestartet. Bitte das Spiel schliessen und neu einladen.\n");
                } else {
                    ensure_steam();
                    launch(game, dll, "", 1);
                }
            }
        }
        printf("STATUS beendet\n");
        return 0;
    }
    SetEnvironmentVariableA("LBO_FIXEDDT", "1");
    SetEnvironmentVariableA("LBO_FPSCAP", "60");
    SetEnvironmentVariableA("LBO_SYNCBG", "2");
    SetEnvironmentVariableA("LBO_NOMOVIES", "1");
    // Der Host speichert normal (ausser LBO_ALLOWSAVE ist vorgegeben, z. B. in Tests)
    if (o.role == 0) {
        if (!GetEnvironmentVariableA("LBO_ALLOWSAVE", buf, sizeof buf)) SetEnvironmentVariableA("LBO_ALLOWSAVE", "1");
        if (host_saves) SetEnvironmentVariableA("LBO_SAVEDIR", host_saves);  // Welt-Ordner statt echter Spielstaende
    } else {
        SetEnvironmentVariableA("LBO_SAVEDIR", guest_saves);  // der Gast liest den Host-Stand
    }

    if (autoload_slot >= 0) {  // beide Seiten muessen es wissen
        snprintf(buf, sizeof buf, "%d", autoload_slot);
        SetEnvironmentVariableA("LBO_AUTOLOAD", buf);
    }

    printf("\nStarte LEGO Batman ... (Logs: %s)\n", logdir);
    printf("%s\n", o.role ? "Warte im Spiel, bis der Host den Spielstand geladen hat. Dann Start druecken, um einzusteigen."
                          : "Du bedienst die Menues. Dein Mitspieler steigt mit Start ein.");
    printf("STATUS spiel\n");
    // Immer Vollbild; nur Tests mit zwei Spielen auf einem PC laufen im Fenster (LBO_WINDOWED=1)
    launch(game, dll, window_mode(), 1);
    printf("STATUS beendet\n");
    return 0;
}

// --- Modi fuer die App ------------------------------------------------------------------
// lbo-launch --stun [--port P]
//   Belegt einen UDP-Port, fragt per STUN die oeffentliche Adresse ab und gibt aus:
//   STUN port=P public=IP:PORT local=IP:PORT symmetric=0|1 key=K
// lbo-launch --connect host|join --port P --key K --peer-key K2 --peer-public IP:PORT
//            --peer-local IP:PORT [--slot N]
//   Verbindet (Hole Punching), uebertraegt den Spielstand und startet das Spiel.
//   Fortschritt als Zeilen "STATUS ...", Fehler als "ERROR ...".
//   --savedir DIR: der Host spielt mit den Spielstaenden aus DIR (gemeinsame Welt).
// lbo-launch --solo --savedir DIR
//   Startet das Spiel ohne Netz; Spielstaende werden aus DIR gelesen und dorthin gespeichert.
static int solo_mode(const char *savedir)
{
    char game[MAX_PATH], dll[MAX_PATH], logdir[MAX_PATH], appdata[MAX_PATH];
    SYSTEMTIME st;
    setvbuf(stdout, NULL, _IONBF, 0);
    if (!savedir) { printf("ERROR --savedir fehlt\n"); return 1; }
    if (!find_game(game)) { printf("ERROR LEGOBatman.exe nicht gefunden\n"); return 1; }
    dll_next_to_exe(dll);
    CreateDirectoryA(savedir, NULL);
    SHGetFolderPathA(NULL, CSIDL_LOCAL_APPDATA, NULL, 0, appdata);
    GetLocalTime(&st);
    snprintf(logdir, sizeof logdir, "%s\\LegoBatmanOnline\\logs", appdata);
    CreateDirectoryA(logdir, NULL);
    snprintf(logdir + strlen(logdir), sizeof logdir - strlen(logdir), "\\%04d%02d%02d-%02d%02d%02d-solo",
             st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute, st.wSecond);
    CreateDirectoryA(logdir, NULL);
    SetEnvironmentVariableA("LBO_DIR", logdir);
    SetEnvironmentVariableA("LBO_SAVEDIR", savedir);
    // Spielstaende landen ohnehin in savedir; so bleiben auch Einstellungen (pcconfig.txt) erhalten
    { char b[8]; if (!GetEnvironmentVariableA("LBO_ALLOWSAVE", b, sizeof b)) SetEnvironmentVariableA("LBO_ALLOWSAVE", "1"); }
    printf("STATUS spiel\n");
    launch(game, dll, window_mode(), 1);
    printf("STATUS beendet\n");
    return 0;
}

static int app_mode(int argc, char **argv)
{
    char game[MAX_PATH], dll[MAX_PATH], code[40];
    unsigned char hash[32];
    ONLINE o;
    int i, stun = 0, slot = -1, solo = 0;
    const char *savedir = NULL;
    unsigned key = 0, peer_key = 0;
    const char *peer_pub = NULL, *peer_loc = NULL;
    setvbuf(stdout, NULL, _IONBF, 0);
    memset(&o, 0, sizeof o);
    for (i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--stun")) stun = 1;
        else if (!strcmp(argv[i], "--connect") && i + 1 < argc) o.role = !strcmp(argv[++i], "join");
        else if (!strcmp(argv[i], "--port") && i + 1 < argc) o.port = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--key") && i + 1 < argc) key = (unsigned)atoi(argv[++i]);
        else if (!strcmp(argv[i], "--peer-key") && i + 1 < argc) peer_key = (unsigned)atoi(argv[++i]);
        else if (!strcmp(argv[i], "--peer-public") && i + 1 < argc) peer_pub = argv[++i];
        else if (!strcmp(argv[i], "--peer-local") && i + 1 < argc) peer_loc = argv[++i];
        else if (!strcmp(argv[i], "--slot") && i + 1 < argc) slot = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--savedir") && i + 1 < argc) savedir = argv[++i];
        else if (!strcmp(argv[i], "--solo")) solo = 1;
        else if (!strcmp(argv[i], "--b2")) g_b2 = 1;
    }
    if (solo) return solo_mode(savedir);
    if (!online_prepare(&o, code)) { printf("ERROR UDP-Port nicht verfuegbar\n"); return 1; }
    if (stun) {
        char pub[32], loc[32];
        snprintf(pub, sizeof pub, "%s:%d", inet_ntoa(o.pub.sin_addr), ntohs(o.pub.sin_port));
        snprintf(loc, sizeof loc, "%s:%d", inet_ntoa(o.local.sin_addr), ntohs(o.local.sin_port));
        printf("STUN port=%d public=%s local=%s symmetric=%d key=%u\n", o.port, pub, loc, o.symmetric_nat, online_key_me());
        online_close();
        return 0;
    }
    if (!find_game(game)) { printf("ERROR %s nicht gefunden\n", g_b2 ? "LEGOBatman2.exe" : "LEGOBatman.exe"); online_close(); return 1; }
    dll_next_to_exe(dll);
    online_sha256_file(game, hash);
    online_set_keys(key, peer_key);
    if (!peer_pub || !online_parse_endpoint(peer_pub, &o.peer_pub)) { printf("ERROR Adresse des Partners fehlt\n"); online_close(); return 1; }
    if (peer_loc) online_parse_endpoint(peer_loc, &o.peer_local);
    // Vor dem Verbinden (60 s Zeitgrenze fuer beide): Steam muss fuer Batman 2 schon laufen (die App prueft das
    // normalerweise vorher, das hier ist der Rueckfall)
    if (g_b2) ensure_steam();
    printf("STATUS verbinde\n");
    if (!online_connect(&o, hash, 60)) {
        printf("ERROR %s\n", online_error[0] ? online_error : "Keine Verbindung zum Partner");
        online_close();
        return 1;
    }
    printf("STATUS verbunden\n");
    return run_session(&o, game, dll, slot, savedir);
}

int main(int argc, char **argv)
{
    char game[MAX_PATH] = "";
    char dll[MAX_PATH] = "";
    char args[2048] = "";
    int wait = 0, i;

    if (argc == 1) return online_menu();
    for (i = 1; i < argc; i++)
        if (!strcmp(argv[i], "--stun") || !strcmp(argv[i], "--connect") || !strcmp(argv[i], "--solo"))
            return app_mode(argc, argv);

    for (i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--game") && i + 1 < argc) strncpy(game, argv[++i], MAX_PATH - 1);
        else if (!strcmp(argv[i], "--dll") && i + 1 < argc) strncpy(dll, argv[++i], MAX_PATH - 1);
        else if (!strcmp(argv[i], "--wait")) wait = 1;
        else if (!strcmp(argv[i], "--")) { i++; break; }
        else { fprintf(stderr, "lbo-launch: unbekanntes Argument %s\n", argv[i]); return 2; }
    }
    for (; i < argc; i++) {
        if (args[0]) strncat(args, " ", sizeof args - strlen(args) - 1);
        strncat(args, argv[i], sizeof args - strlen(args) - 1);
    }
    if (!game[0] && !find_game(game)) { fprintf(stderr, "lbo-launch: LEGOBatman.exe nicht gefunden, bitte --game angeben\n"); return 1; }
    if (!dll[0]) dll_next_to_exe(dll);
    if (GetFileAttributesA(dll) == INVALID_FILE_ATTRIBUTES) {
        fprintf(stderr, "lbo-launch: DLL nicht gefunden: %s\n", dll);
        return 1;
    }
    return launch(game, dll, args, wait);
}
