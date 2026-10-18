// dump.dll: Analysehilfe fuer Spiele mit Steam-Kopierschutzhuelle (SteamStub, Abschnitt .bind).
// Der Code-Abschnitt der EXE ist verschluesselt und wird erst beim Start entschluesselt. Diese DLL wird
// wie lbo.dll per Launcher eingeklinkt, wartet, bis der Code im Speicher entschluesselt ist (Entropie von
// .text sinkt), und schreibt das Speicherabbild der EXE nach LBO_DUMP (Standard: %TEMP%\exe_dump.bin).
// Die Datei hat das Speicherlayout (Offset = Adresse - ImageBase). Spieldateien werden nicht veraendert.
#include <windows.h>
#include <math.h>
#include <stdio.h>

static double entropy(const unsigned char *p, size_t n)
{
    size_t cnt[256] = { 0 }, i;
    double h = 0;
    for (i = 0; i < n; i++) cnt[p[i]]++;
    for (i = 0; i < 256; i++)
        if (cnt[i]) { double q = (double)cnt[i] / n; h -= q * log(q) / log(2.0); }
    return h;
}

static DWORD WINAPI dumper(void *arg)
{
    unsigned char *base = (unsigned char *)GetModuleHandleA(NULL);
    IMAGE_NT_HEADERS *nt = (IMAGE_NT_HEADERS *)(base + ((IMAGE_DOS_HEADER *)base)->e_lfanew);
    IMAGE_SECTION_HEADER *sec = IMAGE_FIRST_SECTION(nt);
    DWORD size = nt->OptionalHeader.SizeOfImage, i, waited, written = 0;
    unsigned char *text = base + sec[0].VirtualAddress;
    char path[MAX_PATH], log[MAX_PATH];
    FILE *f;
    double h = 8;
    (void)arg;
    if (!GetEnvironmentVariableA("LBO_DUMP", path, sizeof path)) {
        GetTempPathA(MAX_PATH, path);
        strcat(path, "exe_dump.bin");
    }
    snprintf(log, sizeof log, "%s.log", path);
    // Bis zu 120 s warten, bis .text entschluesselt ist (verschluesselt: Entropie ~8, Code: ~6-6.5)
    for (i = 0; i < 1200; i++) {
        __try { h = entropy(text, 0x100000 < sec[0].Misc.VirtualSize ? 0x100000 : sec[0].Misc.VirtualSize); }
        __except (EXCEPTION_EXECUTE_HANDLER) { h = 8; }
        if (h < 7.2) break;
        Sleep(100);
    }
    waited = i;
    Sleep(3000);   // weitere Initialisierung abwarten
    f = fopen(path, "wb");
    if (!f) return 0;
    for (i = 0; i < size; i += 0x1000) {
        unsigned char page[0x1000];
        MEMORY_BASIC_INFORMATION mbi;
        int ok = VirtualQuery(base + i, &mbi, sizeof mbi) && mbi.State == MEM_COMMIT && !(mbi.Protect & (PAGE_NOACCESS | PAGE_GUARD));
        if (ok) {
            __try { memcpy(page, base + i, 0x1000); } __except (EXCEPTION_EXECUTE_HANDLER) { ok = 0; }
        }
        if (!ok) memset(page, 0, sizeof page);
        fwrite(page, 1, sizeof page, f);
        written += ok;
    }
    fclose(f);
    f = fopen(log, "w");
    if (f) {
        fprintf(f, "ImageBase %p, SizeOfImage %lx, Entropie .text %.2f nach %lu x 100 ms, %lu Seiten gelesen\n",
                (void *)base, size, h, waited, written);
        fclose(f);
    }
    return 0;
}

BOOL WINAPI DllMain(HINSTANCE inst, DWORD reason, LPVOID reserved)
{
    (void)reserved;
    if (reason == DLL_PROCESS_ATTACH) {
        DisableThreadLibraryCalls(inst);
        CreateThread(NULL, 0, dumper, NULL, 0, NULL);
    }
    return TRUE;
}
