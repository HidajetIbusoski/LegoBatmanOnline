// eip.exe <pid> [samples]: zeigt fuer jeden Thread mehrfach den EIP und die oberen Stackwerte.
// Hilft, Endlosschleifen im Spiel zu finden. Kompilieren: tools\vs.bat cl /nologo tools\eip.c
#include <windows.h>
#include <tlhelp32.h>
#include <stdio.h>
#include <stdlib.h>

int main(int argc, char **argv)
{
    DWORD pid = (DWORD)atoi(argv[1]);
    int samples = argc > 2 ? atoi(argv[2]) : 3, s;
    HANDLE proc = OpenProcess(PROCESS_VM_READ, FALSE, pid);
    if (argc > 2 && !strcmp(argv[2], "mods")) {
        // Modulliste (32-Bit) mit Basisadressen ausgeben
        HANDLE ms = CreateToolhelp32Snapshot(TH32CS_SNAPMODULE | TH32CS_SNAPMODULE32, pid);
        MODULEENTRY32 me = { sizeof me };
        for (BOOL ok = Module32First(ms, &me); ok; ok = Module32Next(ms, &me))
            printf("%08lx %8lx %s\n", (DWORD)me.modBaseAddr, me.modBaseSize, me.szModule);
        return 0;
    }
    for (s = 0; s < samples; s++) {
        HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0);
        THREADENTRY32 te = { sizeof te };
        printf("--- Probe %d\n", s);
        for (BOOL ok = Thread32First(snap, &te); ok; ok = Thread32Next(snap, &te)) {
            if (te.th32OwnerProcessID != pid) continue;
            HANDLE th = OpenThread(THREAD_SUSPEND_RESUME | THREAD_GET_CONTEXT | THREAD_QUERY_INFORMATION, FALSE, te.th32ThreadID);
            if (!th) continue;
            FILETIME a, b, k, u;
            GetThreadTimes(th, &a, &b, &k, &u);
            double cpu = (((ULONGLONG)u.dwHighDateTime << 32) | u.dwLowDateTime) / 1e7;
            if (cpu > 1.0) {
                CONTEXT c = { 0 };
                c.ContextFlags = CONTEXT_CONTROL | CONTEXT_INTEGER;
                SuspendThread(th);
                if (GetThreadContext(th, &c)) {
                    DWORD stack[512] = { 0 };
                    SIZE_T n = 0;
                    ReadProcessMemory(proc, (void *)c.Esp, stack, sizeof stack, &n);
                    printf("TID %5lu cpu %7.2fs EIP %08lx ESP %08lx EBP %08lx | ", te.th32ThreadID, cpu, c.Eip, c.Esp, c.Ebp);
                    for (int i = 0; i < 512; i++)
                        if (stack[i] >= 0x401000 && stack[i] < 0x850000) printf("%08lx ", stack[i]);
                    printf("\n");
                }
                ResumeThread(th);
            }
            CloseHandle(th);
        }
        CloseHandle(snap);
        Sleep(200);
    }
    return 0;
}
