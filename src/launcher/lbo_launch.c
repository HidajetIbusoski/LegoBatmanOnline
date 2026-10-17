// lbo-launch: startet LEGOBatman.exe angehalten, laedt lbo.dll per Remote-Thread
// und laesst das Spiel dann weiterlaufen. Im Spielordner wird nichts abgelegt.
//
// Aufruf: lbo-launch.exe [--game <pfad\LEGOBatman.exe>] [--dll <pfad\lbo.dll>] [--wait] [-- spielargumente...]
// Die LBO_*-Umgebungsvariablen (siehe lbo.c) werden an das Spiel vererbt.
#include <windows.h>
#include <stdio.h>
#include <string.h>

#define DEFAULT_GAME "D:\\SteamLibrary\\steamapps\\common\\Lego Batman\\LEGOBatman.exe"

static int fail(const char *what)
{
    fprintf(stderr, "lbo-launch: %s fehlgeschlagen (Fehler %lu)\n", what, GetLastError());
    return 1;
}

int main(int argc, char **argv)
{
    char game[MAX_PATH] = DEFAULT_GAME;
    char dll[MAX_PATH] = "";
    char cmdline[4096];
    char dir[MAX_PATH];
    int wait = 0, i;

    for (i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--game") && i + 1 < argc) strncpy(game, argv[++i], MAX_PATH - 1);
        else if (!strcmp(argv[i], "--dll") && i + 1 < argc) strncpy(dll, argv[++i], MAX_PATH - 1);
        else if (!strcmp(argv[i], "--wait")) wait = 1;
        else if (!strcmp(argv[i], "--")) { i++; break; }
        else { fprintf(stderr, "lbo-launch: unbekanntes Argument %s\n", argv[i]); return 2; }
    }

    if (!dll[0]) {
        // Standard: lbo.dll neben dem Launcher
        char *slash;
        GetModuleFileNameA(NULL, dll, MAX_PATH);
        slash = strrchr(dll, '\\');
        strcpy(slash ? slash + 1 : dll, "lbo.dll");
    }
    if (GetFileAttributesA(dll) == INVALID_FILE_ATTRIBUTES) {
        fprintf(stderr, "lbo-launch: DLL nicht gefunden: %s\n", dll);
        return 1;
    }

    strcpy(dir, game);
    { char *slash = strrchr(dir, '\\'); if (slash) *slash = 0; }

    snprintf(cmdline, sizeof cmdline, "\"%s\"", game);
    for (; i < argc; i++) {
        strncat(cmdline, " ", sizeof cmdline - strlen(cmdline) - 1);
        strncat(cmdline, argv[i], sizeof cmdline - strlen(cmdline) - 1);
    }

    STARTUPINFOA si = { sizeof si };
    PROCESS_INFORMATION pi;
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
