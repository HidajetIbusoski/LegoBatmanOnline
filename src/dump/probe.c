// probe.dll: Laufzeit-Sonde fuer noch unbekannte Spiele (zuerst LEGO Batman 2).
// Wartet, bis der Code entschluesselt ist (Steam-Kopierschutzhuelle), holt sich die Vtable von
// IDirect3DDevice9 ueber ein eigenes Hilfsgeraet und hakt sich in Present ein. Bei den ersten Frames
// nach LBO_PROBE_FROM (Standard 600) werden die Ruecksprungadressen in den Spielcode vom Stack
// protokolliert; daraus ergibt sich die Aufrufkette Hauptschleife -> Frame-Ende -> Present.
// Ausgabe nach LBO_PROBE_LOG (Standard %TEMP%\probe.log).
#include <windows.h>
#include <d3d9.h>
#include <math.h>
#include <stdio.h>

typedef HRESULT (STDMETHODCALLTYPE *PresentFn)(IDirect3DDevice9 *, const RECT *, const RECT *, HWND, const RGNDATA *);
typedef HRESULT (STDMETHODCALLTYPE *EndSceneFn)(IDirect3DDevice9 *);
static PresentFn real_present;
static EndSceneFn real_endscene;
static FILE *s_log;
static long s_frames, s_from = 600;
static DWORD s_text_lo, s_text_hi;

static void logf_(const char *fmt, ...)
{
    va_list ap;
    if (!s_log) return;
    va_start(ap, fmt);
    vfprintf(s_log, fmt, ap);
    va_end(ap);
    fputc('\n', s_log);
    fflush(s_log);
}

static int is_ret_addr(DWORD a)
{
    const unsigned char *p = (const unsigned char *)a;
    if (a < s_text_lo + 6 || a >= s_text_hi) return 0;
    if (p[-5] == 0xE8) return 1;
    if (p[-6] == 0xFF && (p[-5] & 0x38) == 0x10) return 1;
    if (p[-2] == 0xFF && (p[-1] & 0x38) == 0x10) return 1;
    if (p[-3] == 0xFF && (p[-2] & 0x38) == 0x10) return 1;
    return 0;
}

static void frame_mark(void *d, const char *what)
{
    long f = ++s_frames;
    if (f >= s_from && f < s_from + 5) {
        DWORD *sp = (DWORD *)&d, i;
        char line[2048];
        int len = snprintf(line, sizeof line, "%s Frame %ld, Thread %lu:", what, f, GetCurrentThreadId()), n = 0;
        __try {
            for (i = 0; i < 4096 && n < 40; i++)
                if (is_ret_addr(sp[i])) { len += snprintf(line + len, sizeof line - len, " %08lx", sp[i]); n++; }
        } __except (EXCEPTION_EXECUTE_HANDLER) {}
        logf_("%s", line);
    }
    if (f % 600 == 0) logf_("%s: %ld Frames", what, f);
}

static HRESULT STDMETHODCALLTYPE h_present(IDirect3DDevice9 *d, const RECT *a, const RECT *b, HWND w, const RGNDATA *r)
{
    frame_mark(d, "Present");
    return real_present(d, a, b, w, r);
}

static HRESULT STDMETHODCALLTYPE h_endscene(IDirect3DDevice9 *d)
{
    frame_mark(d, "EndScene");
    return real_endscene(d);
}

// --- Spielgeraet ueber den Direct3DCreate9-Aufruf des Spiels erwischen -------------------------
typedef IDirect3D9 *(WINAPI *Create9Fn)(UINT);
typedef HRESULT (STDMETHODCALLTYPE *CreateDeviceFn)(IDirect3D9 *, UINT, D3DDEVTYPE, HWND, DWORD,
                                                    D3DPRESENT_PARAMETERS *, IDirect3DDevice9 **);
static Create9Fn real_create9;
static CreateDeviceFn real_createdevice;
static DWORD s_call_site = 0x0063E59A, s_thunk = 0x00BCC518;   // LEGO Batman 2

static void hook_slot(void **slot, void *fn, void **orig)
{
    DWORD old;
    VirtualProtect(slot, 4, PAGE_EXECUTE_READWRITE, &old);
    *orig = *slot;
    *slot = fn;
    VirtualProtect(slot, 4, old, &old);
}

static HRESULT STDMETHODCALLTYPE h_createdevice(IDirect3D9 *d3d, UINT a, D3DDEVTYPE t, HWND w, DWORD f,
                                                D3DPRESENT_PARAMETERS *pp, IDirect3DDevice9 **out)
{
    HRESULT hr = real_createdevice(d3d, a, t, w, f, pp, out);
    logf_("CreateDevice %08lx, Flags %08lx, %ux%u, Fenster %d", hr, f, pp ? pp->BackBufferWidth : 0,
          pp ? pp->BackBufferHeight : 0, pp ? pp->Windowed : -1);
    if (SUCCEEDED(hr) && out && *out && !real_present) {
        hook_slot(&(*(void ***)*out)[42], (void *)h_endscene, (void **)&real_endscene);
        real_present = (PresentFn)1;   // nur als "schon gehakt"-Merker
        logf_("Present-Haken am Spielgeraet gesetzt, Aufrufketten ab Frame %ld", s_from);
    }
    return hr;
}

static IDirect3D9 *WINAPI h_create9(UINT sdk)
{
    IDirect3D9 *d = ((Create9Fn)s_thunk)(sdk);
    logf_("Direct3DCreate9(%u) -> %p", sdk, (void *)d);
    if (d && !real_createdevice) hook_slot(&(*(void ***)d)[16], (void *)h_createdevice, (void **)&real_createdevice);
    return d;
}

static void patch_d3d_call(void)
{
    unsigned char *p = (unsigned char *)s_call_site;
    DWORD old, target;
    int i;
    for (i = 0; i < 120000; i++) {   // bis 2 min warten, bis die Stelle entschluesselt ist
        __try { target = p[0] == 0xE8 ? s_call_site + 5 + *(DWORD *)(p + 1) : 0; } __except (EXCEPTION_EXECUTE_HANDLER) { target = 0; }
        if (target == s_thunk) break;
        Sleep(1);
    }
    if (target != s_thunk) { logf_("Direct3DCreate9-Aufruf bei %08lx nicht gefunden", s_call_site); return; }
    VirtualProtect(p, 5, PAGE_EXECUTE_READWRITE, &old);
    *(DWORD *)(p + 1) = (DWORD)h_create9 - (s_call_site + 5);
    VirtualProtect(p, 5, old, &old);
    FlushInstructionCache(GetCurrentProcess(), p, 5);
    logf_("Direct3DCreate9-Aufruf bei %08lx umgebogen (nach %d ms)", s_call_site, i);
}

static double entropy(const unsigned char *p, size_t n)
{
    size_t cnt[256] = { 0 }, i;
    double h = 0;
    for (i = 0; i < n; i++) cnt[p[i]]++;
    for (i = 0; i < 256; i++)
        if (cnt[i]) { double q = (double)cnt[i] / n; h -= q * log(q) / log(2.0); }
    return h;
}

static DWORD WINAPI probe(void *arg)
{
    unsigned char *base = (unsigned char *)GetModuleHandleA(NULL);
    IMAGE_NT_HEADERS *nt = (IMAGE_NT_HEADERS *)(base + ((IMAGE_DOS_HEADER *)base)->e_lfanew);
    IMAGE_SECTION_HEADER *sec = IMAGE_FIRST_SECTION(nt);
    char path[MAX_PATH], buf[32];
    IDirect3D9 *d3d;
    IDirect3DDevice9 *dev = NULL;
    D3DPRESENT_PARAMETERS pp;
    HWND wnd;
    void **vtbl;
    DWORD old;
    int i;
    (void)arg;
    if (!GetEnvironmentVariableA("LBO_PROBE_LOG", path, sizeof path)) {
        GetTempPathA(MAX_PATH, path);
        strcat(path, "probe.log");
    }
    if (GetEnvironmentVariableA("LBO_PROBE_FROM", buf, sizeof buf)) s_from = atol(buf);
    s_log = fopen(path, "w");
    s_text_lo = (DWORD)(base + sec[0].VirtualAddress);
    s_text_hi = s_text_lo + sec[0].Misc.VirtualSize;
    for (i = 0; i < 120000; i++) {
        double h = 8;
        __try { h = entropy((unsigned char *)s_text_lo, 0x10000); } __except (EXCEPTION_EXECUTE_HANDLER) {}
        if (h < 7.2) break;
        Sleep(1);
    }
    logf_("Code entschluesselt nach %d ms", i);
    (void)d3d; (void)dev; (void)pp; (void)wnd; (void)vtbl; (void)old;
    // Den Direct3DCreate9-Aufruf des Spiels umbiegen, sobald der Code an dieser Stelle entschluesselt ist
    patch_d3d_call();
    return 0;
}

BOOL WINAPI DllMain(HINSTANCE inst, DWORD reason, LPVOID reserved)
{
    (void)reserved;
    if (reason == DLL_PROCESS_ATTACH) {
        DisableThreadLibraryCalls(inst);
        CreateThread(NULL, 0, probe, NULL, 0, NULL);
    }
    return TRUE;
}
