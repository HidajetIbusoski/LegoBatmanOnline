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
#include <stdio.h>
#include <string.h>
#include "online.h"

static int fail(const char *what)
{
    fprintf(stderr, "lbo-launch: %s fehlgeschlagen (Fehler %lu)\n", what, GetLastError());
    return 1;
}

// --- Spiel finden ---------------------------------------------------------------------
static int try_game(const char *lib, char *out)
{
    char p[MAX_PATH];
    snprintf(p, sizeof p, "%s\\steamapps\\common\\Lego Batman\\LEGOBatman.exe", lib);
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
    printf("lbo-launch: Spiel gestartet (PID %lu), lbo.dll bei 0x%08lx\n", pi.dwProcessId, mod);
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
    strcpy(slash ? slash + 1 : dll, "lbo.dll");
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

static int online_menu(void)
{
    char game[MAX_PATH], dll[MAX_PATH], line[128], code[40], appdata[MAX_PATH];
    char save_dir[MAX_PATH], guest_saves[MAX_PATH], logdir[MAX_PATH], peer[64], buf[64];
    unsigned char hash[32];
    ONLINE o;
    SYSTEMTIME st;
    int delay;

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
    if (online_measure_ping(&o) < 0) { printf("Ping-Messung fehlgeschlagen.\n"); online_close(); press_enter(); return 1; }
    // Eingabeverzoegerung: halbe Rundlaufzeit plus Puffer, in 60-Hz-Frames
    delay = (o.ping_ms / 2 + 20 + 16) / 17 + 1;
    if (delay < 2) delay = 2;
    if (delay > 10) delay = 10;
    printf("Ping: %d ms -> Eingabeverzoegerung %d Frames (%d ms)\n", o.ping_ms, delay, delay * 1000 / 60);

    SHGetFolderPathA(NULL, CSIDL_LOCAL_APPDATA, NULL, 0, appdata);
    snprintf(save_dir, sizeof save_dir, "%s\\Warner Bros. Interactive Entertainment\\LEGO Batman\\Saved Games", appdata);
    snprintf(buf, sizeof buf, "%s", "");
    snprintf(guest_saves, sizeof guest_saves, "%s\\LegoBatmanOnline", appdata);
    CreateDirectoryA(guest_saves, NULL);
    GetLocalTime(&st);
    snprintf(logdir, sizeof logdir, "%s\\LegoBatmanOnline\\logs", appdata);
    CreateDirectoryA(logdir, NULL);
    snprintf(logdir + strlen(logdir), sizeof logdir - strlen(logdir), "\\%04d%02d%02d-%02d%02d%02d-%s",
             st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute, st.wSecond, o.role ? "gast" : "host");
    CreateDirectoryA(logdir, NULL);

    if (o.role == 0) {
        if (!online_send_saves(&o, save_dir)) { online_close(); press_enter(); return 1; }
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
        if (!online_receive_saves(&o, guest_saves)) { online_close(); press_enter(); return 1; }
    }
    if (!online_ready_barrier(&o, &delay)) { printf("Mitspieler hat nicht bestaetigt.\n"); online_close(); press_enter(); return 1; }
    snprintf(peer, sizeof peer, "%s:%d", inet_ntoa(o.peer.sin_addr), ntohs(o.peer.sin_port));
    online_close();

    // Einstellungen fuer die DLL (siehe lbo.c / net.c)
    SetEnvironmentVariableA("LBO_DIR", logdir);
    SetEnvironmentVariableA("LBO_NET", o.role ? "join" : "host");
    snprintf(buf, sizeof buf, "%d", o.port); SetEnvironmentVariableA("LBO_PORT", buf);
    SetEnvironmentVariableA("LBO_PEER", peer);
    // Beide muessen dieselbe Verzoegerung nutzen; der Gast hat den Wert des Hosts uebernommen.
    snprintf(buf, sizeof buf, "%d", delay); SetEnvironmentVariableA("LBO_DELAY", buf);
    SetEnvironmentVariableA("LBO_FIXEDDT", "1");
    SetEnvironmentVariableA("LBO_FPSCAP", "60");
    SetEnvironmentVariableA("LBO_SYNCBG", "2");
    SetEnvironmentVariableA("LBO_NOMOVIES", "1");
    // Der Host speichert normal (ausser LBO_ALLOWSAVE ist vorgegeben, z. B. in Tests)
    if (o.role == 0) {
        if (!GetEnvironmentVariableA("LBO_ALLOWSAVE", buf, sizeof buf)) SetEnvironmentVariableA("LBO_ALLOWSAVE", "1");
    } else {
        SetEnvironmentVariableA("LBO_SAVEDIR", guest_saves);  // der Gast liest den Host-Stand
    }

    printf("\nStarte LEGO Batman ... (Logs: %s)\n", logdir);
    printf("%s\n", o.role ? "Warte im Spiel, bis der Host den Spielstand geladen hat. Dann Start druecken, um einzusteigen."
                          : "Du bedienst die Menues. Dein Mitspieler steigt mit Start ein.");
    launch(game, dll, "-Windowed", 1);
    press_enter();
    return 0;
}

int main(int argc, char **argv)
{
    char game[MAX_PATH] = "";
    char dll[MAX_PATH] = "";
    char args[2048] = "";
    int wait = 0, i;

    if (argc == 1) return online_menu();

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
