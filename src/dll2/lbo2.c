// lbo2.dll: Hook-DLL fuer LEGO Batman 2 (Steam-Build 2012-05-25, ImageBase 0x400000, kein ASLR).
// Der Code ist durch die Steam-Kopierschutzhuelle verschluesselt; alle Haken werden aus einem eigenen
// Thread gesetzt, sobald die jeweilige Stelle entschluesselt bzw. die Importtabelle befuellt ist.
// Adressen siehe docs/batman2-phase0.md.
//
// Umgebungsvariablen (wie lbo.dll fuer Batman 1):
//   LBO_DIR        Ausgabeordner (lbo.log, frames.csv, Screenshots)
//   LBO_MODE       off | script
//   LBO_SCRIPT     Eingabeskript (Format wie Batman 1: "F <von> <bis> <pad> <tasten> <lt> <rt> <lx> <ly> <rx> <ry>")
//   LBO_LIVE       Befehlsdatei (rel, shot, peek, poke, clear, quit)
//   LBO_FIXEDDT    1 = dt fest 1/60 (Ueberschreibung 0x11cf0f8)
//   LBO_FPSCAP     Bildrate begrenzen (z. B. 60)
//   LBO_MAXFRAMES  Spiel nach so vielen Frame-Enden beenden
//   LBO_SHOT_EVERY alle N Frame-Enden einen Screenshot
//   LBO_MULTI      1 = zweite Instanz erlauben (Mutex umbenennen)
//   LBO_ALLOWSAVE  1 = Spielstand und pcconfig.txt wirklich schreiben (sonst in LBO_DIR umgeleitet)
//   LBO_BGSYNC     2 = Frame-Ende wartet, bis NuBgProc leer ist (1 = eingebauter Synchronmodus, haengt
//                  im ersten Level: Auftrag 0x7d48c0 wartet auf Ladevorgaenge anderer Threads)
//   LBO_RNGRESET   Frame, an dem alle Zufallszustaende auf feste Werte gesetzt werden (Test ohne Host)
//   LBO_NOMOVIES   1 = Videos (.fmv) ueberspringen: Ihr dt laeuft nach der Videouhr, also nach Echtzeit
//   LBO_LOADSYNC   1 = Frame-Ende wartet, bis alle gewuenschten Streaming-Eintraege geladen sind
//   LBO_VIDEODT    1 = dt waehrend Videos nicht durch die Videouhr ersetzen (bleibt 1/60)
//   LBO_RENDERRNG  1 = Zufallsaufrufe beim Zeichnen (0x5e0d52, 0x5e0db8, 0x598619) und im Musiktakt
//                  (0x94cdf0) auf eigene Zustaende
//   LBO_WATCHLCG   1 = Hardware-Haltepunkt auf den LCG 0x11a1844: Schreibstellen im LBO_TRACERNG-Fenster
//   LBO_WATCHADDR  Adresse (hex): wie LBO_WATCHLCG, aber fuer einen anderen 4-Byte-Wert
//   LBO_VCLOCK     1 = virtuelle Uhr: Der Spiel-Timer liefert im Hauptthread Frame x 1/60 statt Echtzeit
//   LBO_TRACERNG   von,bis: Aufrufer von 0x51a1d0 (rng_a) je Frame nach rngtrace.txt
//   LBO_WINDOWED   1 = Fenstermodus erzwingen (Direct3D Windowed=1)
#define COBJMACROS
#include <windows.h>
#include <d3d9.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <tlhelp32.h>
#include <intrin.h>
#include "../dll/net.h"
#include "../dll/input.h"

// --- Spieladressen (LEGO Batman 2) ---------------------------------------------------
#define ADDR_FRAME_END      0x005528C0   // sub esp,40h / push ebx / push edi (5 Bytes)
#define IAT_XINPUT_GETSTATE 0x00C55334   // XINPUT1_3 Ordinal 2
#define IAT_XINPUT_GETCAPS  0x00C55330   // XINPUT1_3 Ordinal 4
#define IAT_CREATEMUTEXA    0x00C550BC
#define IAT_CREATEFILEA     0x00C5522C
#define IAT_CREATEFILEW     0x00C550C0
#define IAT_DELETEFILEA     0x00C55244
#define IAT_MOVEFILEA       0x00C55240
#define IAT_QPC             0x00C551AC   // nur der Spiel-Timer 0x521840/0x5218c0 (und CRT-Init) liest QPC
#define G_DT                (*(volatile float *)0x011CF0DC)   // gemessenes dt dieses Frames
#define G_DT_OVERRIDE       (*(volatile float *)0x011CF0F8)   // != 0: ersetzt dt
#define G_FRAMECOUNT        (*(volatile int *)0x011CF0E4)
#define G_RNG_PM_SEED       (*(volatile unsigned *)0x011A1840) // 0x1f3ad27f
#define G_RNG_LCG16         (*(volatile unsigned *)0x011A1844) // 12345
#define G_RNG_PM_DEFAULT    (*(volatile unsigned *)0x0102B358) // Park-Miller-Standardzustand (XOR 0x75bd924)
#define G_WINDOW_ACTIVE     (*(volatile int *)0x01153E18)     // WM_ACTIVATE; 0 = Spiel wartet in 0x5bc170 mit Sleep(10)
#define D3DCREATE9_CALL     0x0063E59A   // call Direct3DCreate9-Thunk
#define D3DCREATE9_THUNK    0x00BCC518
// Figuren: Array *0x1398fe8, Anzahl *0x1394b88, Abstand 0x17f0 (Schleife in 0xa4c4d0, max. 0x40)
#define G_CHAR_BASE         (*(unsigned char *volatile *)0x01398FE8)
#define G_CHAR_COUNT        (*(volatile int *)0x01394B88)
#define CHAR_STRIDE         0x17F0
#define CHAR_POS            0x5C         // float x, y, z
#define CHAR_USED           0x218        // != 0: Platz belegt
#define G_PLAYER1           (*(unsigned char *volatile *)0x013999DC)  // gesteuerte Figur Spieler 1 (0x8af4f0)
#define G_PLAYER2           (*(unsigned char *volatile *)0x013999E0)  // Spieler 2, 0 = nicht beigetreten
#define G_BGPROC            (*(unsigned char *volatile *)0x011CC114)  // NuBgProc-Warteschlange (0x556290)
#define BGPROC_SYNC         0x8058       // Byte != 0: 0x5522b0 fuehrt Auftraege sofort im Aufrufer aus
#define BGPROC_PENDING      0x44         // LONG: Auftraege in der Warteschlange
#define BGPROC_WAITFRAME    0x804C       // != 0: Worker wartet auf diesen Frame (Semaphore +0x8050 im Frame-Ende)
#define G_BGPROC_CURRENT    (*(void *volatile *)0x011CC118)   // laufender Auftrag, 0 = keiner
#define G_RNG_A             (*(volatile unsigned *)0x011BE45C) // Setter 0x51a170
#define G_RNG_B             (*(volatile unsigned *)0x01195B50) // Setter 0x77f280
#define G_RNG_C             (*(volatile unsigned *)0x01195B30) // Setter 0x77d040
#define FMV_FIND_CALL       0x0062C0EF   // in NuFMVPlayV 0x62be30: call 0x628d60 (Videodatei suchen)
#define FMV_FIND            0x00628D60
// Streaming-Eintraege (Laden/Entladen von Levelteilen), abgearbeitet in 0x7eb950 (NuBgProc-Auftrag 0x7d48c0)
#define G_LOADLIST          (*(int *volatile *)0x011BE084)    // [0] erster Eintrag, [2] Anzahl
#define LOADLIST_CS         ((LPCRITICAL_SECTION)0x0138B9F8)
#define G_LOADFILTER_ON     (*(volatile char *)0x01199108)
#define G_LOADFILTER_ID     (*(volatile int *)0x0119910C)
// Eintrag: [2] naechster, [5] Zustand (1 leer, 2 laedt, 3/4 geladen, 5 entlaedt), Byte +0x1c gewuenscht, [8] Gruppe
#define VIDEO_DT_STORE      0x00552B89   // fstp [0x11cf0dc]: dt = Videouhr (0x5bb230)
#define G_POLL_COUNTDOWN    (*(volatile int *)0x01154400)    // Frames bis zur naechsten Abfrage
#define G_POLL_DEVICE       (*(volatile short *)0x013666C8)  // naechstes Geraet (0-3 XInput, 4-13 DirectInput)
#define G_POLL_RESTART      (*(volatile char *)0x013655A7)
#define FN_RNG_A            0x0051A1D0   // float-Zufall aus 0x11be45c: push ecx / mov eax,[0x11be45c]
#define FN_GETPTD           0x00BDA791                        // CRT _getptd (rand-Zustand bei +0x14)

// --- XInput-Typen ----------------------------------------------------------------------
typedef struct { WORD wButtons; BYTE bLeftTrigger, bRightTrigger; SHORT sThumbLX, sThumbLY, sThumbRX, sThumbRY; } PAD;
typedef struct { DWORD dwPacketNumber; PAD Gamepad; } XSTATE;
typedef struct { BYTE Type, SubType; WORD Flags; PAD Gamepad; WORD vib[2]; } XCAPS;
typedef DWORD (WINAPI *GetStateFn)(DWORD, XSTATE *);
typedef DWORD (WINAPI *GetCapsFn)(DWORD, DWORD, XCAPS *);
typedef HANDLE (WINAPI *CreateMutexAFn)(LPSECURITY_ATTRIBUTES, BOOL, LPCSTR);

typedef struct { char dom; int from, to, pad; PAD p; } LINE;
enum { MODE_OFF, MODE_SCRIPT };

static long g_bg_waits, g_bg_timeouts, g_load_waits, g_load_timeouts;
static int g_loadsync, g_videodt, g_render_rng;
static int g_trace_from, g_trace_to, g_watch_lcg;
static DWORD g_watch_addr = 0x011A1844;   // LBO_WATCHADDR=Adresse: anderer Wert statt des LCG
#define LW_MAX (1 << 20)
static struct { long f; DWORD eip, tid; } g_lw[LW_MAX];
static volatile LONG g_nlw;
static FILE *g_trace;
static int g_bgsync, g_devchange, g_rng_reset, g_no_movies;
static int g_mode, g_fixed_dt, g_fps_cap, g_max_frames, g_shot_every, g_multi;
static char g_dir[MAX_PATH], g_live[MAX_PATH];
static FILE *g_log, *g_csv, *g_liveout, *g_probe;
// 1 = Hauptthread wartet bewusst in unserem Code (Bildratenbremse, Netz, Jitter); siehe gate_watch
static volatile LONG g_in_wait;
static int g_gate;   // LBO_GATE, siehe gate_window
static FILE *g_memdump;
static long g_memdump_from, g_memdump_to;
static struct { DWORD base, off; int deref; } g_probes[32];
static int g_nprobe;
static long g_live_pos;
static volatile LONG g_F;
static int g_connected[4];
static LINE *g_lines;
static int g_nlines, g_caplines;
static CRITICAL_SECTION g_cs;
static GetStateFn real_getstate;
static GetCapsFn real_getcaps;
static CreateMutexAFn real_createmutex;
static DWORD g_t0;
static int g_polls;
static HANDLE g_main_thread;
static volatile DWORD g_last_frame_tick;

static void logf_(const char *fmt, ...)
{
    va_list ap;
    if (!g_log) return;
    va_start(ap, fmt);
    fprintf(g_log, "[F%ld] ", g_F);
    vfprintf(g_log, fmt, ap);
    fputc('\n', g_log);
    fflush(g_log);
    va_end(ap);
}

static FILE *open_out(const char *name, const char *mode)
{
    char path[MAX_PATH];
    snprintf(path, sizeof path, "%s\\%s", g_dir, name);
    return fopen(path, mode);
}

// --- Skript ----------------------------------------------------------------------------
static void add_line(const LINE *l)
{
    if (g_nlines == g_caplines) {
        g_caplines = g_caplines ? g_caplines * 2 : 256;
        g_lines = (LINE *)realloc(g_lines, g_caplines * sizeof(LINE));
    }
    g_lines[g_nlines++] = *l;
}

static int parse_pad(const char *s, int *pad, PAD *p)
{
    unsigned btn; int lt, rt, lx, ly, rx, ry;
    if (sscanf(s, "%d %x %d %d %d %d %d %d", pad, &btn, &lt, &rt, &lx, &ly, &rx, &ry) != 8) return 0;
    if (*pad < 0 || *pad > 3) return 0;
    p->wButtons = (WORD)btn; p->bLeftTrigger = (BYTE)lt; p->bRightTrigger = (BYTE)rt;
    p->sThumbLX = (SHORT)lx; p->sThumbLY = (SHORT)ly; p->sThumbRX = (SHORT)rx; p->sThumbRY = (SHORT)ry;
    return 1;
}

static void parse_script_line(char *s)
{
    LINE l;
    int pad, n = 0;
    while (*s == ' ' || *s == '\t') s++;
    if (!*s || *s == '#' || *s == '\r' || *s == '\n') return;
    if (sscanf(s, "connect %d", &pad) == 1) {
        if (pad >= 0 && pad < 4 && !g_connected[pad]) { g_connected[pad] = 1; g_devchange = 1; }
        return;
    }
    // F: absolute Frames; A: Frames ab dem Levelanker (LBO_ANCHOR)
    if ((s[0] == 'F' || s[0] == 'A') && sscanf(s + 1, "%d %d %n", &l.from, &l.to, &n) == 2 &&
        parse_pad(s + 1 + n, &l.pad, &l.p)) {
        l.dom = s[0];
        add_line(&l);
        if (!g_connected[l.pad]) { g_connected[l.pad] = 1; g_devchange = 1; }
        return;
    }
    logf_("Skriptzeile nicht verstanden: %s", s);
}

static void load_script(const char *path)
{
    char buf[512];
    FILE *f = fopen(path, "r");
    if (!f) { logf_("Skript %s nicht lesbar", path); return; }
    while (fgets(buf, sizeof buf, f)) parse_script_line(buf);
    fclose(f);
    logf_("Skript %s: %d Zeilen", path, g_nlines);
}

// LBO_ANCHOR=1: Levelanker = erster Frame mit mehr als 2 Figuren. Dort werden die Zustaende abgeglichen
// (wie online beim Levelstart) und "A"-Skriptzeilen zaehlen ab hier.
static int g_anchor_mode;
static volatile long g_anchor = -1;

static void scripted_state(int pad, PAD *out)
{
    int i;
    memset(out, 0, sizeof *out);
    for (i = 0; i < g_nlines; i++) {
        const LINE *l = &g_lines[i];
        long t = l->dom == 'A' ? (g_anchor < 0 ? -1 : g_F - g_anchor) : g_F;
        if (l->pad == pad && t >= l->from && (l->to < 0 || t <= l->to)) *out = l->p;
    }
}

// --- Screenshot aus dem Direct3D-Backbuffer -------------------------------------------------
// GDI liefert beim Spielfenster nur Weiss. Deshalb: Bei einem Screenshot-Wunsch kopiert jedes EndScene,
// das in den Backbuffer zeichnet, das Bild in den Speicher; das naechste Frame-Ende schreibt das letzte
// davon als BMP. Gehakt wird der Direct3DCreate9-Aufruf des Spiels (wie src/dump/probe.c).
typedef IDirect3D9 *(WINAPI *Create9Fn)(UINT);
typedef HRESULT (STDMETHODCALLTYPE *CreateDeviceFn)(IDirect3D9 *, UINT, D3DDEVTYPE, HWND, DWORD,
                                                    D3DPRESENT_PARAMETERS *, IDirect3DDevice9 **);
typedef HRESULT (STDMETHODCALLTYPE *EndSceneFn)(IDirect3DDevice9 *);
typedef HRESULT (STDMETHODCALLTYPE *ResetFn)(IDirect3DDevice9 *, D3DPRESENT_PARAMETERS *);
static CreateDeviceFn real_createdevice;
static EndSceneFn real_endscene;
static ResetFn real_reset;
static int g_windowed;
static IDirect3DDevice9 *g_dev;
static char g_shot_name[64];
static volatile int g_shot_pending;
static unsigned char *g_shot_pix;      // 24 Bit, unten nach oben, Zeilen auf 4 Byte ausgerichtet
static int g_shot_w, g_shot_h, g_shot_have;

static void hook_slot(void **slot, void *fn, void **orig)
{
    DWORD old;
    VirtualProtect(slot, 4, PAGE_EXECUTE_READWRITE, &old);
    *orig = *slot;
    *slot = fn;
    VirtualProtect(slot, 4, old, &old);
}

static void grab_backbuffer(IDirect3DDevice9 *d)
{
    IDirect3DSurface9 *bb = NULL, *rt = NULL, *tmp = NULL, *sys = NULL, *src;
    D3DSURFACE_DESC desc;
    D3DLOCKED_RECT lr;
    int x, y, stride;
    if (FAILED(IDirect3DDevice9_GetRenderTarget(d, 0, &rt))) return;
    if (FAILED(IDirect3DDevice9_GetBackBuffer(d, 0, 0, D3DBACKBUFFER_TYPE_MONO, &bb))) { IDirect3DSurface9_Release(rt); return; }
    if (rt != bb) goto out;   // zeichnet gerade in ein Zwischenziel
    IDirect3DSurface9_GetDesc(bb, &desc);
    if (desc.Format != D3DFMT_X8R8G8B8 && desc.Format != D3DFMT_A8R8G8B8) goto out;
    src = bb;
    if (desc.MultiSampleType != D3DMULTISAMPLE_NONE) {
        if (FAILED(IDirect3DDevice9_CreateRenderTarget(d, desc.Width, desc.Height, desc.Format, D3DMULTISAMPLE_NONE, 0,
                                                       FALSE, &tmp, NULL))) goto out;
        if (FAILED(IDirect3DDevice9_StretchRect(d, bb, NULL, tmp, NULL, D3DTEXF_NONE))) goto out;
        src = tmp;
    }
    if (FAILED(IDirect3DDevice9_CreateOffscreenPlainSurface(d, desc.Width, desc.Height, desc.Format, D3DPOOL_SYSTEMMEM,
                                                            &sys, NULL))) goto out;
    if (FAILED(IDirect3DDevice9_GetRenderTargetData(d, src, sys))) goto out;
    if (FAILED(IDirect3DSurface9_LockRect(sys, &lr, NULL, D3DLOCK_READONLY))) goto out;
    stride = ((int)desc.Width * 3 + 3) & ~3;
    if (!g_shot_pix || g_shot_w != (int)desc.Width || g_shot_h != (int)desc.Height) {
        free(g_shot_pix);
        g_shot_pix = (unsigned char *)malloc(stride * desc.Height);
        g_shot_w = desc.Width; g_shot_h = desc.Height;
    }
    for (y = 0; y < g_shot_h; y++) {
        const unsigned char *s = (const unsigned char *)lr.pBits + (size_t)y * lr.Pitch;
        unsigned char *o = g_shot_pix + (size_t)(g_shot_h - 1 - y) * stride;
        for (x = 0; x < g_shot_w; x++) { o[3 * x] = s[4 * x]; o[3 * x + 1] = s[4 * x + 1]; o[3 * x + 2] = s[4 * x + 2]; }
    }
    IDirect3DSurface9_UnlockRect(sys);
    g_shot_have = 1;
out:
    if (sys) IDirect3DSurface9_Release(sys);
    if (tmp) IDirect3DSurface9_Release(tmp);
    IDirect3DSurface9_Release(bb);
    IDirect3DSurface9_Release(rt);
}

static HRESULT STDMETHODCALLTYPE h_endscene(IDirect3DDevice9 *d)
{
    if (g_shot_pending && d == g_dev) grab_backbuffer(d);
    return real_endscene(d);
}

// "-Windowed" liest Batman 2 zwar ein, wertet es aber nie aus (0x1365920). Im exklusiven Vollbild
// minimiert sich das Spiel bei Fokusverlust; fuer Tests mit zwei Instanzen deshalb Fenstermodus erzwingen.
static void force_windowed(D3DPRESENT_PARAMETERS *pp)
{
    if (!g_windowed || !pp) return;
    pp->Windowed = TRUE;
    pp->FullScreen_RefreshRateInHz = 0;
}

static HRESULT STDMETHODCALLTYPE h_reset(IDirect3DDevice9 *d, D3DPRESENT_PARAMETERS *pp)
{
    HRESULT hr;
    force_windowed(pp);
    hr = real_reset(d, pp);
    logf_("Reset %08lx, %ux%u, Fenster %d", hr, pp ? pp->BackBufferWidth : 0, pp ? pp->BackBufferHeight : 0,
          pp ? pp->Windowed : -1);
    return hr;
}

static HRESULT STDMETHODCALLTYPE h_createdevice(IDirect3D9 *d3d, UINT a, D3DDEVTYPE t, HWND w, DWORD f,
                                                D3DPRESENT_PARAMETERS *pp, IDirect3DDevice9 **out)
{
    HRESULT hr;
    force_windowed(pp);
    hr = real_createdevice(d3d, a, t, w, f, pp, out);
    logf_("CreateDevice %08lx, %ux%u, Fenster %d", hr, pp ? pp->BackBufferWidth : 0, pp ? pp->BackBufferHeight : 0,
          pp ? pp->Windowed : -1);
    if (SUCCEEDED(hr) && out && *out) {
        g_dev = *out;
        if (!real_endscene) hook_slot(&(*(void ***)*out)[42], (void *)h_endscene, (void **)&real_endscene);
        if (!real_reset) hook_slot(&(*(void ***)*out)[16], (void *)h_reset, (void **)&real_reset);
    }
    return hr;
}

static IDirect3D9 *WINAPI h_create9(UINT sdk)
{
    IDirect3D9 *d = ((Create9Fn)D3DCREATE9_THUNK)(sdk);
    if (d && !real_createdevice) hook_slot(&(*(void ***)d)[16], (void *)h_createdevice, (void **)&real_createdevice);
    return d;
}

// Wunsch anmelden; das Bild entsteht beim naechsten Zeichnen
static void write_shot(const char *name)
{
    snprintf(g_shot_name, sizeof g_shot_name, "%s", name);
    g_shot_have = 0;
    g_shot_pending = 1;
}

// Im Frame-Ende: Liegt ein Bild vor, als BMP schreiben
static void flush_shot(void)
{
    BITMAPINFOHEADER bi;
    BITMAPFILEHEADER bf;
    FILE *f;
    int stride;
    if (!g_shot_pending) return;
    if (!g_shot_have) {
        if (++g_shot_pending > 30) { logf_("Screenshot %s: kein Backbuffer-Bild", g_shot_name); g_shot_pending = 0; }
        return;
    }
    g_shot_pending = 0;
    stride = (g_shot_w * 3 + 3) & ~3;
    memset(&bi, 0, sizeof bi);
    bi.biSize = sizeof bi; bi.biWidth = g_shot_w; bi.biHeight = g_shot_h; bi.biPlanes = 1; bi.biBitCount = 24;
    memset(&bf, 0, sizeof bf);
    bf.bfType = 0x4D42;
    bf.bfOffBits = sizeof bf + sizeof bi;
    bf.bfSize = bf.bfOffBits + stride * g_shot_h;
    f = open_out(g_shot_name, "wb");
    if (!f) return;
    fwrite(&bf, sizeof bf, 1, f);
    fwrite(&bi, sizeof bi, 1, f);
    fwrite(g_shot_pix, stride * g_shot_h, 1, f);
    fclose(f);
    logf_("Screenshot %s (%dx%d)", g_shot_name, g_shot_w, g_shot_h);
}

// Waehrend eines Videos ersetzt 0x5bb230 das dt durch die Videouhr (Echtzeit). Dann haengt es von der
// Rechnerlast ab, an welchem Frame ein Video endet. LBO_NOMOVIES: Die Dateisuche meldet "nicht gefunden",
// NuFMVPlayV bricht ab und setzt den Videomodus 0x11cf178 zurueck.
static char __cdecl no_movie_find(char *out)
{
    (void)out;
    logf_("Video uebersprungen");
    return 0;
}

static int patch_call(DWORD at, DWORD expect_target, void *to)
{
    unsigned char *p = (unsigned char *)at;
    DWORD old;
    if (p[0] != 0xE8 || at + 5 + *(DWORD *)(p + 1) != expect_target) return 0;
    VirtualProtect(p, 5, PAGE_EXECUTE_READWRITE, &old);
    *(DWORD *)(p + 1) = (DWORD)to - (at + 5);
    VirtualProtect(p, 5, old, &old);
    FlushInstructionCache(GetCurrentProcess(), p, 5);
    return 1;
}

// --- Live-Befehle --------------------------------------------------------------------------
static void poll_live(void)
{
    char buf[512];
    FILE *f;
    if (!g_live[0]) return;
    f = fopen(g_live, "rb");
    if (!f) return;
    fseek(f, g_live_pos, SEEK_SET);
    while (fgets(buf, sizeof buf, f)) {
        size_t len = strlen(buf);
        int dur, n = 0;
        if (!len || buf[len - 1] != '\n') break;
        g_live_pos += (long)len;
        while (len && (buf[len - 1] == '\n' || buf[len - 1] == '\r')) buf[--len] = 0;
        if (!strncmp(buf, "shot", 4)) {
            char nm[64];
            snprintf(nm, sizeof nm, "live_F%ld.bmp", g_F);
            write_shot(nm);
        } else if (!strncmp(buf, "peek ", 5)) {
            unsigned addr, cnt, k;
            char hex[3 * 256 + 1];
            if (sscanf(buf + 5, "%x %u", &addr, &cnt) == 2 && cnt <= 256) {
                hex[0] = 0;
                __try { for (k = 0; k < cnt; k++) sprintf(hex + 3 * k, "%02x ", ((unsigned char *)addr)[k]); }
                __except (EXCEPTION_EXECUTE_HANDLER) { strcat(hex, "<Zugriffsfehler>"); }
                logf_("peek %08x: %s", addr, hex);
            }
        } else if (!strncmp(buf, "poke ", 5)) {
            unsigned addr, val;
            if (sscanf(buf + 5, "%x %x", &addr, &val) == 2) {
                __try { *(unsigned *)addr = val; logf_("poke %08x = %08x", addr, val); }
                __except (EXCEPTION_EXECUTE_HANDLER) { logf_("poke %08x: Zugriffsfehler", addr); }
            }
        } else if (!strncmp(buf, "clear", 5)) {
            g_nlines = 0;
        } else if (!strncmp(buf, "quit", 4)) {
            logf_("Live: quit");
            if (g_csv) fflush(g_csv);
            TerminateProcess(GetCurrentProcess(), 0);
        } else if (sscanf(buf, "rel %d %n", &dur, &n) == 1) {
            LINE l;
            if (!parse_pad(buf + n, &l.pad, &l.p)) { logf_("Live-Zeile kaputt: %s", buf); continue; }
            l.dom = 'F'; l.from = g_F + 1; l.to = g_F + dur;
            add_line(&l);
            if (!g_connected[l.pad]) { g_connected[l.pad] = 1; g_devchange = 1; }
            if (g_liveout) {
                fprintf(g_liveout, "F %d %d %d %04x %d %d %d %d %d %d\n", l.from, l.to, l.pad, l.p.wButtons,
                        l.p.bLeftTrigger, l.p.bRightTrigger, l.p.sThumbLX, l.p.sThumbLY, l.p.sThumbRX, l.p.sThumbRY);
                fflush(g_liveout);
            }
            logf_("Live: %s", buf);
        } else {
            parse_script_line(buf);
        }
    }
    fclose(f);
}

// --- XInput und Mutex -------------------------------------------------------------------------
// Netzmodus (LBO_NET=host|join, net.c): Pads 0 und 1 kommen aus dem Lockstep-Austausch des Frames.
static int g_net;
static PAD g_net_pad[2];

static DWORD WINAPI hook_getstate(DWORD idx, XSTATE *st)
{
    g_polls++;
    if (g_net) {
        if (idx > 1) return ERROR_DEVICE_NOT_CONNECTED;
        st->Gamepad = g_net_pad[idx];
        st->dwPacketNumber = (DWORD)g_F;
        return ERROR_SUCCESS;
    }
    if (g_mode == MODE_SCRIPT) {
        if (idx > 3 || !g_connected[idx]) return ERROR_DEVICE_NOT_CONNECTED;
        EnterCriticalSection(&g_cs);
        scripted_state((int)idx, &st->Gamepad);
        LeaveCriticalSection(&g_cs);
        st->dwPacketNumber = (DWORD)g_F;
        return ERROR_SUCCESS;
    }
    return real_getstate(idx, st);
}

static DWORD WINAPI hook_getcaps(DWORD idx, DWORD flags, XCAPS *caps)
{
    if (g_mode == MODE_SCRIPT) {
        if (idx > 3 || !g_connected[idx]) return ERROR_DEVICE_NOT_CONNECTED;
        memset(caps, 0, sizeof *caps);
        caps->Type = 1;
        caps->SubType = 1;
        caps->Gamepad.wButtons = 0xF3FF;
        caps->Gamepad.bLeftTrigger = caps->Gamepad.bRightTrigger = 0xFF;
        caps->Gamepad.sThumbLX = caps->Gamepad.sThumbLY = caps->Gamepad.sThumbRX = caps->Gamepad.sThumbRY = (SHORT)0xFFC0;
        return ERROR_SUCCESS;
    }
    return real_getcaps(idx, flags, caps);
}

static HANDLE WINAPI hook_createmutex(LPSECURITY_ATTRIBUTES sa, BOOL own, LPCSTR name)
{
    char buf[300];
    if (g_multi && name) {
        snprintf(buf, sizeof buf, "%s-lbo%lu", name, GetCurrentProcessId());
        logf_("Einzelinstanz-Mutex umbenannt: %s", buf);
        return real_createmutex(sa, own, buf);
    }
    return real_createmutex(sa, own, name);
}

// --- Spielstand-Schutz --------------------------------------------------------------------------
// Tests duerfen weder Spielstaende noch pcconfig.txt des Benutzers anlegen oder aendern. Ohne
// LBO_ALLOWSAVE=1 wird jeder Schreibzugriff unter dem Batman-2-Datenordner (ausser dem Shader-Cache)
// und auf Pfade mit "save" in den Ausgabeordner umgeleitet; Loeschen und Umbenennen dort entfallen.
typedef HANDLE (WINAPI *CreateFileAFn)(LPCSTR, DWORD, DWORD, LPSECURITY_ATTRIBUTES, DWORD, DWORD, HANDLE);
typedef HANDLE (WINAPI *CreateFileWFn)(LPCWSTR, DWORD, DWORD, LPSECURITY_ATTRIBUTES, DWORD, DWORD, HANDLE);
typedef BOOL (WINAPI *DeleteFileAFn)(LPCSTR);
typedef BOOL (WINAPI *MoveFileAFn)(LPCSTR, LPCSTR);
static CreateFileAFn real_createfile;
static CreateFileWFn real_createfilew;
static DeleteFileAFn real_deletefile;
static MoveFileAFn real_movefile;
static int g_allow_save;

static int is_protected(const char *path)
{
    char low[MAX_PATH];
    size_t i;
    if (!path || g_allow_save) return 0;
    for (i = 0; path[i] && i < MAX_PATH - 1; i++) low[i] = (char)tolower((unsigned char)path[i]);
    low[i] = 0;
    if (strstr(low, "\\cachedshaders\\")) return 0;
    return strstr(low, "lego batman 2\\") || strstr(low, "save") || strstr(low, "pcconfig");
}

static void redirected(const char *path, char *out)
{
    const char *base = strrchr(path, '\\');
    snprintf(out, MAX_PATH, "%s\\umgeleitet_%s", g_dir, base ? base + 1 : path);
}

static int is_save_dir(const char *path);

static HANDLE WINAPI hook_createfile(LPCSTR path, DWORD access, DWORD share, LPSECURITY_ATTRIBUTES sa,
                                     DWORD disp, DWORD flags, HANDLE tmpl)
{
    // Netzmodus: Auch Lesen der Spielstaende umleiten, damit beide Spiele dieselben (leeren) Plaetze sehen
    if (((access & (GENERIC_WRITE | GENERIC_ALL)) && is_protected(path)) || (g_net && is_save_dir(path))) {
        char red[MAX_PATH];
        redirected(path, red);
        logf_("Schreibzugriff umgeleitet: %s -> %s", path, red);
        return real_createfile(red, access, share, sa, disp, flags, tmpl);
    }
    return real_createfile(path, access, share, sa, disp, flags, tmpl);
}

static HANDLE WINAPI hook_createfilew(LPCWSTR wpath, DWORD access, DWORD share, LPSECURITY_ATTRIBUTES sa,
                                      DWORD disp, DWORD flags, HANDLE tmpl)
{
    char path[MAX_PATH];
    if ((access & (GENERIC_WRITE | GENERIC_ALL)) && wpath &&
        WideCharToMultiByte(CP_ACP, 0, wpath, -1, path, sizeof path, NULL, NULL) && is_protected(path)) {
        char red[MAX_PATH];
        redirected(path, red);
        logf_("Schreibzugriff (W) umgeleitet: %s -> %s", path, red);
        return real_createfile(red, access, share, sa, disp, flags, tmpl);
    }
    return real_createfilew(wpath, access, share, sa, disp, flags, tmpl);
}

static BOOL WINAPI hook_deletefile(LPCSTR path)
{
    if (is_protected(path)) { logf_("Loeschen verhindert: %s", path); return TRUE; }
    return real_deletefile(path);
}

static BOOL WINAPI hook_movefile(LPCSTR from, LPCSTR to)
{
    if (is_protected(from) || is_protected(to)) { logf_("Umbenennen verhindert: %s -> %s", from, to); return TRUE; }
    return real_movefile(from, to);
}

// Leere Ordner SavedGames\SlotN zeigt das Spiel als "beschaedigt" an. Im Schutzmodus werden sie deshalb
// gar nicht erst angelegt (Erfolg melden, nichts tun).
typedef BOOL (WINAPI *CreateDirectoryAFn)(LPCSTR, LPSECURITY_ATTRIBUTES);
typedef int (WINAPI *SHCreateDirectoryExAFn)(HWND, LPCSTR, const SECURITY_ATTRIBUTES *);
static CreateDirectoryAFn real_createdir;
static SHCreateDirectoryExAFn real_shcreatedir;

static int is_save_dir(const char *path)
{
    char low[MAX_PATH];
    size_t i;
    if (!path || g_allow_save) return 0;
    for (i = 0; path[i] && i < MAX_PATH - 1; i++) low[i] = (char)tolower((unsigned char)path[i]);
    low[i] = 0;
    return strstr(low, "lego batman 2\\savedgames") != NULL;
}

static BOOL WINAPI hook_createdir(LPCSTR path, LPSECURITY_ATTRIBUTES sa)
{
    if (is_save_dir(path)) { logf_("Ordner nicht angelegt: %s", path); return TRUE; }
    return real_createdir(path, sa);
}

static int WINAPI hook_shcreatedir(HWND w, LPCSTR path, const SECURITY_ATTRIBUTES *sa)
{
    if (is_save_dir(path)) { logf_("Ordner nicht angelegt: %s", path); return ERROR_SUCCESS; }
    return real_shcreatedir(w, path, sa);
}

// --- Virtuelle Uhr --------------------------------------------------------------------------------
// Effekte beim Zeichnen (z. B. 0x59861e, 0x5e0470) ziehen je nach gemessener Zeit unterschiedlich oft
// Zufallszahlen. Mit LBO_VCLOCK liefert QPC im Hauptthread: Start + Frame x Frequenz/60 + 1 Tick je Aufruf
// im Frame (damit Schleifen, die auf Zeit warten, weiterkommen). Andere Threads sehen die Echtzeit.
typedef BOOL (WINAPI *QpcFn)(LARGE_INTEGER *);
static QpcFn real_qpc;
static int g_vclock;
static volatile DWORD g_main_tid;
static LONGLONG g_vbase, g_vfreq;
static long g_vframe = -1, g_vcalls;

static BOOL WINAPI hook_qpc(LARGE_INTEGER *out)
{
    if (!g_vclock || !g_main_tid || GetCurrentThreadId() != g_main_tid) return real_qpc(out);
    if (!g_vbase) {
        LARGE_INTEGER f, n;
        QueryPerformanceFrequency(&f);
        real_qpc(&n);
        g_vfreq = f.QuadPart;
        g_vbase = n.QuadPart - (LONGLONG)g_F * (g_vfreq / 60);
    }
    if (g_vframe != g_F) { g_vframe = g_F; g_vcalls = 0; }
    out->QuadPart = g_vbase + (LONGLONG)g_F * (g_vfreq / 60) + ++g_vcalls;
    return TRUE;
}

static void patch_iat(DWORD slot, void *fn, void **orig)
{
    DWORD old;
    VirtualProtect((void *)slot, 4, PAGE_READWRITE, &old);
    *orig = *(void **)slot;
    *(void **)slot = fn;
    VirtualProtect((void *)slot, 4, old, &old);
}

// --- XAudio2 ----------------------------------------------------------------------------------------
// Das Spiel erzeugt XAudio2 in 0x612930 per CoCreateInstance(IID_IXAudio2) (Vtable wie 2.7:
// 5 Initialize, 8 CreateSourceVoice). Stimmen-Vtable: 18 DestroyVoice, 19 Start, 20 Stop,
// 21 SubmitSourceBuffer, 22 FlushSourceBuffers, 25 GetState, 26 SetFrequencyRatio.
// LBO_AUDIOLOG=1 zaehlt zunaechst nur mit, welche Methoden und Rueckrufe das Spiel nutzt.
typedef struct { UINT32 Flags, AudioBytes; const BYTE *pAudioData; UINT32 PlayBegin, PlayLength, LoopBegin, LoopLength, LoopCount; void *pContext; } XA_BUFFER;
typedef struct { void *pCurrentBufferContext; UINT32 BuffersQueued; UINT64 SamplesPlayed; } XA_STATE;
typedef HRESULT (WINAPI *CoCreateInstanceFn)(REFCLSID, LPUNKNOWN, DWORD, REFIID, LPVOID *);
typedef HRESULT (STDMETHODCALLTYPE *CreateSourceVoiceFn)(void *, void **, const WAVEFORMATEX *, UINT32, float, void *, const void *, const void *);
typedef HRESULT (STDMETHODCALLTYPE *VStartFn)(void *, UINT32, UINT32);
typedef HRESULT (STDMETHODCALLTYPE *VSubmitFn)(void *, const XA_BUFFER *, const void *);
typedef HRESULT (STDMETHODCALLTYPE *VFlushFn)(void *);
typedef void (STDMETHODCALLTYPE *VGetStateFn)(void *, XA_STATE *);
typedef void (STDMETHODCALLTYPE *VDestroyFn)(void *);
typedef void (STDMETHODCALLTYPE *CbCtxFn)(void *, void *);
typedef void (STDMETHODCALLTYPE *CbVoidFn)(void *);
static const GUID IID_XAudio2_ = { 0x8bcf1f58, 0x9fe7, 0x4583, { 0x8a, 0xc6, 0xe2, 0xad, 0xc4, 0x65, 0xc8, 0xbb } };
static CoCreateInstanceFn real_cocreate;
static CreateSourceVoiceFn real_create_src;
static VStartFn real_vstart, real_vstop;
static VSubmitFn real_vsubmit;
static VFlushFn real_vflush;
static VGetStateFn real_vgetstate;
static VDestroyFn real_vdestroy;
static int g_audiolog;
static volatile LONG g_a_voices, g_a_cbvoices, g_a_start, g_a_stop, g_a_submit, g_a_flush, g_a_getstate, g_a_destroy;
static volatile LONG g_a_cb[7], g_a_cb_main[7];
static void *g_a_cbvt[8][7];
static int g_a_ncbvt;
static struct { DWORD ret; LONG n; } g_a_gs_callers[16];

// Virtuelle Stimmen: Warteschlange der eingereichten Puffer mit ihrer Laenge in Samples. va_tick spielt
// am Frame-Ende Abtastrate x Frequenzverhaeltnis / 60 Samples ab und liefert die faelligen Rueckrufe.
// Das Ergebnis haengt nur noch von den Aufrufen des Spiels ab, nicht mehr von der Echtzeit.
typedef struct { UINT32 samples; void *ctx; UINT32 flags; int infinite, started; } VA_BUF;
typedef struct {
    void *voice, *cb;
    UINT32 rate, align, channels, tag, spb;   // spb: Samples je Block (ADPCM)
    float ratio;
    int running, n, head;
    double pos;
    UINT64 played;
    VA_BUF q[64];
} VA_VOICE;
static VA_VOICE g_va[64];
static int g_vaudio;                     // LBO_VAUDIO=1
static volatile DWORD g_va_delivering;   // Thread-ID, die gerade virtuelle Rueckrufe ausliefert
static CRITICAL_SECTION g_va_cs;
static volatile LONG g_va_cs_init;
typedef HRESULT (STDMETHODCALLTYPE *VExitLoopFn)(void *, UINT32);
typedef HRESULT (STDMETHODCALLTYPE *VSetRatioFn)(void *, float, UINT32);
static VExitLoopFn real_vexitloop;
static VSetRatioFn real_vsetratio;
static long g_va_unknown_fmt, g_va_overflow;

static VA_VOICE *va_find(void *v)
{
    int i;
    for (i = 0; i < 64; i++) if (g_va[i].voice == v) return &g_va[i];
    return NULL;
}

static UINT32 va_samples(VA_VOICE *a, const XA_BUFFER *b, const void *wma)
{
    UINT32 n;
    if (b->PlayLength) n = b->PlayLength;
    else if (a->tag == 1 || a->tag == 3 || a->tag == 0xFFFE) n = a->align ? b->AudioBytes / a->align : 0;
    else if (a->tag == 2 && a->align && a->spb) n = (b->AudioBytes / a->align) * a->spb;
    else if ((a->tag == 0x161 || a->tag == 0x162) && wma && ((const UINT32 *)wma)[1])
        n = ((const UINT32 * const *)wma)[0][((const UINT32 *)wma)[1] - 1] / (a->channels ? a->channels * 2 : 2);
    else { g_va_unknown_fmt++; n = a->align ? b->AudioBytes / a->align : b->AudioBytes; }
    if (b->LoopCount && b->LoopCount != 255) n += (b->LoopLength ? b->LoopLength : n) * b->LoopCount;
    return n;
}

static HRESULT STDMETHODCALLTYPE a_start(void *v, UINT32 f, UINT32 o)
{
    VA_VOICE *a;
    InterlockedIncrement(&g_a_start);
    if (g_vaudio) { EnterCriticalSection(&g_va_cs); if ((a = va_find(v)) != NULL) a->running = 1; LeaveCriticalSection(&g_va_cs); }
    return real_vstart(v, f, o);
}

static HRESULT STDMETHODCALLTYPE a_stop(void *v, UINT32 f, UINT32 o)
{
    VA_VOICE *a;
    InterlockedIncrement(&g_a_stop);
    if (g_vaudio) { EnterCriticalSection(&g_va_cs); if ((a = va_find(v)) != NULL) a->running = 0; LeaveCriticalSection(&g_va_cs); }
    return real_vstop(v, f, o);
}

static HRESULT STDMETHODCALLTYPE a_submit(void *v, const XA_BUFFER *b, const void *w)
{
    VA_VOICE *a;
    InterlockedIncrement(&g_a_submit);
    if (g_vaudio && b) {
        EnterCriticalSection(&g_va_cs);
        if ((a = va_find(v)) != NULL) {
            if (a->n < 64) {
                VA_BUF *e = &a->q[(a->head + a->n) % 64];
                e->samples = va_samples(a, b, w);
                e->ctx = b->pContext;
                e->flags = b->Flags;
                e->infinite = b->LoopCount == 255;
                e->started = 0;
                a->n++;
            } else g_va_overflow++;
        }
        LeaveCriticalSection(&g_va_cs);
    }
    return real_vsubmit(v, b, w);
}

static HRESULT STDMETHODCALLTYPE a_flush(void *v)
{
    VA_VOICE *a;
    InterlockedIncrement(&g_a_flush);
    if (g_vaudio) {
        // Wartende Puffer verwerfen, der laufende bleibt (wie bei XAudio2). Sie bekommen noch ihr BufferEnd:
        // dafuer werden sie mit Laenge 0 an den Anfang gestellt.
        EnterCriticalSection(&g_va_cs);
        if ((a = va_find(v)) != NULL) {
            int i;
            for (i = a->q[a->head].started ? 1 : 0; i < a->n; i++) a->q[(a->head + i) % 64].samples = 0;
        }
        LeaveCriticalSection(&g_va_cs);
    }
    return real_vflush(v);
}

static HRESULT STDMETHODCALLTYPE a_exitloop(void *v, UINT32 o)
{
    VA_VOICE *a;
    if (g_vaudio) {
        EnterCriticalSection(&g_va_cs);
        if ((a = va_find(v)) != NULL && a->n) {
            VA_BUF *e = &a->q[a->head];
            if (e->infinite) { e->infinite = 0; e->samples = (UINT32)a->pos; }
        }
        LeaveCriticalSection(&g_va_cs);
    }
    return real_vexitloop(v, o);
}

static HRESULT STDMETHODCALLTYPE a_setratio(void *v, float r, UINT32 o)
{
    VA_VOICE *a;
    if (g_vaudio) { EnterCriticalSection(&g_va_cs); if ((a = va_find(v)) != NULL) a->ratio = r; LeaveCriticalSection(&g_va_cs); }
    return real_vsetratio(v, r, o);
}

static void STDMETHODCALLTYPE a_destroy(void *v)
{
    VA_VOICE *a;
    InterlockedIncrement(&g_a_destroy);
    if (g_vaudio) { EnterCriticalSection(&g_va_cs); if ((a = va_find(v)) != NULL) memset(a, 0, sizeof *a); LeaveCriticalSection(&g_va_cs); }
    real_vdestroy(v);
}

static void STDMETHODCALLTYPE a_getstate(void *v, XA_STATE *s)
{
    DWORD ret = (DWORD)_ReturnAddress();
    VA_VOICE *a;
    int i;
    InterlockedIncrement(&g_a_getstate);
    for (i = 0; i < 16 && g_a_gs_callers[i].ret && g_a_gs_callers[i].ret != ret; i++) {}
    if (i < 16) { g_a_gs_callers[i].ret = ret; g_a_gs_callers[i].n++; }
    if (g_vaudio && s) {
        EnterCriticalSection(&g_va_cs);
        if ((a = va_find(v)) != NULL) {
            s->pCurrentBufferContext = a->n ? a->q[a->head].ctx : NULL;
            s->BuffersQueued = a->n;
            s->SamplesPlayed = a->played;
            LeaveCriticalSection(&g_va_cs);
            return;
        }
        LeaveCriticalSection(&g_va_cs);
    }
    real_vgetstate(v, s);
}

// Rueckrufe des Spiels zaehlen (pro Rueckruf-Vtable gemerkte Originale)
#define CB_STUB(name, idx, sig, call)                                                                  \
    static void STDMETHODCALLTYPE name sig                                                             \
    {                                                                                                  \
        void **vt = *(void ***)self;                                                                   \
        int k;                                                                                         \
        InterlockedIncrement(&g_a_cb[idx]);                                                            \
        if (GetCurrentThreadId() == g_main_tid) InterlockedIncrement(&g_a_cb_main[idx]);               \
        if (g_vaudio && g_va_delivering != GetCurrentThreadId()) return; /* echte zurueckhalten */    \
        for (k = 0; k < g_a_ncbvt; k++)                                                                \
            if ((void **)g_a_cbvt[k][0] == vt) { call; return; }                                      \
    }
// g_a_cbvt[k][0] = Vtable-Adresse, [1..] = Originale der Methoden 2..5 (OnStreamEnd, OnBufferStart, OnBufferEnd, OnLoopEnd)
CB_STUB(cb_stream_end, 2, (void *self), ((CbVoidFn)g_a_cbvt[k][1])(self))
CB_STUB(cb_buf_start, 3, (void *self, void *ctx), ((CbCtxFn)g_a_cbvt[k][2])(self, ctx))
CB_STUB(cb_buf_end, 4, (void *self, void *ctx), ((CbCtxFn)g_a_cbvt[k][3])(self, ctx))
CB_STUB(cb_loop_end, 5, (void *self, void *ctx), ((CbCtxFn)g_a_cbvt[k][4])(self, ctx))

static void hook_callback_vtable(void *cb)
{
    void **vt = *(void ***)cb, *dummy;
    int k;
    for (k = 0; k < g_a_ncbvt; k++) if ((void **)g_a_cbvt[k][0] == vt) return;
    if (g_a_ncbvt >= 8) return;
    k = g_a_ncbvt;
    g_a_cbvt[k][0] = vt;
    g_a_cbvt[k][1] = vt[2];
    g_a_cbvt[k][2] = vt[3];
    g_a_cbvt[k][3] = vt[4];
    g_a_cbvt[k][4] = vt[5];
    g_a_ncbvt++;
    hook_slot(&vt[2], (void *)cb_stream_end, &dummy);
    hook_slot(&vt[3], (void *)cb_buf_start, &dummy);
    hook_slot(&vt[4], (void *)cb_buf_end, &dummy);
    hook_slot(&vt[5], (void *)cb_loop_end, &dummy);
    logf_("Ton: Rueckruf-Vtable %p (%p %p %p %p)", (void *)vt, vt[2], vt[3], vt[4], vt[5]);
}

static HRESULT STDMETHODCALLTYPE h_create_src(void *xa, void **out, const WAVEFORMATEX *fmt, UINT32 flags, float maxr,
                                              void *cb, const void *sends, const void *fx)
{
    HRESULT hr = real_create_src(xa, out, fmt, flags, maxr, cb, sends, fx);
    if (SUCCEEDED(hr) && out && *out) {
        void **vt = *(void ***)*out;
        InterlockedIncrement(&g_a_voices);
        if (!real_vgetstate) {
            hook_slot(&vt[18], (void *)a_destroy, (void **)&real_vdestroy);
            hook_slot(&vt[19], (void *)a_start, (void **)&real_vstart);
            hook_slot(&vt[20], (void *)a_stop, (void **)&real_vstop);
            hook_slot(&vt[21], (void *)a_submit, (void **)&real_vsubmit);
            hook_slot(&vt[22], (void *)a_flush, (void **)&real_vflush);
            hook_slot(&vt[25], (void *)a_getstate, (void **)&real_vgetstate);
            hook_slot(&vt[24], (void *)a_exitloop, (void **)&real_vexitloop);
            hook_slot(&vt[26], (void *)a_setratio, (void **)&real_vsetratio);
            logf_("Ton: Stimmen-Vtable %p gehakt", (void *)vt);
        }
        if (g_vaudio && fmt) {
            int i;
            EnterCriticalSection(&g_va_cs);
            for (i = 0; i < 64 && g_va[i].voice; i++) {}
            if (i < 64) {
                VA_VOICE *a = &g_va[i];
                memset(a, 0, sizeof *a);
                a->voice = *out;
                a->cb = cb;
                a->rate = fmt->nSamplesPerSec;
                a->align = fmt->nBlockAlign;
                a->channels = fmt->nChannels;
                a->tag = fmt->wFormatTag;
                a->spb = (fmt->wFormatTag == 2 && fmt->cbSize >= 2) ? *(const WORD *)((const BYTE *)fmt + 20) : 0;
                a->ratio = 1.0f;
                if (g_a_voices <= 20)
                    logf_("Ton: Stimme %p Format %u, %u Hz, %u Kanaele, Block %u, Rueckruf %p", *out, a->tag, a->rate,
                          a->channels, a->align, cb);
            }
            LeaveCriticalSection(&g_va_cs);
        }
        if (cb) { InterlockedIncrement(&g_a_cbvoices); hook_callback_vtable(cb); }
    }
    return hr;
}

static HRESULT WINAPI hook_cocreate(REFCLSID clsid, LPUNKNOWN outer, DWORD ctx, REFIID riid, LPVOID *out)
{
    HRESULT hr = real_cocreate(clsid, outer, ctx, riid, out);
    if ((g_audiolog || g_vaudio) && SUCCEEDED(hr) && out && *out && IsEqualGUID(riid, &IID_XAudio2_) && !real_create_src) {
        hook_slot(&(*(void ***)*out)[8], (void *)h_create_src, (void **)&real_create_src);
        logf_("Ton: IXAudio2 %p, CreateSourceVoice gehakt", *out);
    }
    return hr;
}

// Am Frame-Ende: virtuelle Wiedergabe um einen Frame weiterschieben, faellige Rueckrufe im Hauptthread
// ausliefern und warten, bis die Ton-Threads (Tabelle 0x102dbe0: 16 NuSoundDecodeThread,
// 17 NuSoundStreamerWorkerThread) darauf reagiert haben.
typedef struct { void *cb; int kind; void *ctx; } VA_EVENT;   // kind 3 BufferStart, 4 BufferEnd, 2 StreamEnd
static long g_va_events;

static ULONG64 sound_cycles(void)
{
    ULONG64 sum = 0, c;
    int i;
    for (i = 16; i <= 17; i++) {
        HANDLE h = *(HANDLE *)(0x0102DBE0 + i * 0xB4 + 0xA8);
        if (h && QueryThreadCycleTime(h, &c)) sum += c;
    }
    return sum;
}

static void va_tick(void)
{
    static VA_EVENT ev[1024];
    int nev = 0, i, k;
    EnterCriticalSection(&g_va_cs);
    for (i = 0; i < 64; i++) {
        VA_VOICE *a = &g_va[i];
        double budget;
        if (!a->voice || !a->cb || !a->running) continue;
        budget = (double)a->rate * a->ratio / 60.0;
        while (a->n && nev < 1020) {
            VA_BUF *e = &a->q[a->head];
            double rem;
            if (!e->started) { e->started = 1; ev[nev].cb = a->cb; ev[nev].kind = 3; ev[nev++].ctx = e->ctx; }
            if (e->infinite) { a->pos += budget; a->played += (UINT64)budget; break; }
            rem = (double)e->samples - a->pos;
            if (rem > budget) { a->pos += budget; a->played += (UINT64)budget; break; }
            if (rem > 0) { budget -= rem; a->played += (UINT64)rem; }
            ev[nev].cb = a->cb; ev[nev].kind = 4; ev[nev++].ctx = e->ctx;
            if (e->flags & 0x40) { ev[nev].cb = a->cb; ev[nev].kind = 2; ev[nev++].ctx = NULL; }   // END_OF_STREAM
            a->head = (a->head + 1) % 64;
            a->n--;
            a->pos = 0;
        }
    }
    LeaveCriticalSection(&g_va_cs);
    if (!nev) return;
    g_va_events += nev;
    g_va_delivering = GetCurrentThreadId();
    for (i = 0; i < nev; i++) {
        void **vt = *(void ***)ev[i].cb;
        for (k = 0; k < g_a_ncbvt; k++)
            if ((void **)g_a_cbvt[k][0] == vt) {
                if (ev[i].kind == 2) ((CbVoidFn)g_a_cbvt[k][1])(ev[i].cb);
                else ((CbCtxFn)g_a_cbvt[k][ev[i].kind == 3 ? 2 : 3])(ev[i].cb, ev[i].ctx);
                break;
            }
    }
    g_va_delivering = 0;
    {   // Ton-Threads reagieren lassen: warten, bis sie 2 ms lang nicht gerechnet haben (hoechstens 50 ms)
        DWORD t0 = GetTickCount();
        ULONG64 prev = sound_cycles(), now;
        for (;;) {
            Sleep(2);
            now = sound_cycles();
            if (now == prev || GetTickCount() - t0 > 50) break;
            prev = now;
        }
    }
}

static void audio_log(void)
{
    int i;
    char line[600];
    int len;
    logf_("Ton: %ld Stimmen (%ld mit Rueckruf), Start %ld, Stop %ld, Submit %ld, Flush %ld, GetState %ld, Destroy %ld",
          g_a_voices, g_a_cbvoices, g_a_start, g_a_stop, g_a_submit, g_a_flush, g_a_getstate, g_a_destroy);
    logf_("Ton: Rueckrufe StreamEnd %ld (Haupt %ld), BufferStart %ld (%ld), BufferEnd %ld (%ld), LoopEnd %ld (%ld)",
          g_a_cb[2], g_a_cb_main[2], g_a_cb[3], g_a_cb_main[3], g_a_cb[4], g_a_cb_main[4], g_a_cb[5], g_a_cb_main[5]);
    if (g_vaudio)
        logf_("Ton virtuell: %ld Rueckrufe ausgeliefert, %ld unbekannte Formate, %ld Ueberlaeufe", g_va_events,
              g_va_unknown_fmt, g_va_overflow);
    len = snprintf(line, sizeof line, "Ton: GetState-Aufrufer");
    for (i = 0; i < 16 && g_a_gs_callers[i].ret; i++)
        len += snprintf(line + len, sizeof line - len, " %08lx:%ld", g_a_gs_callers[i].ret, g_a_gs_callers[i].n);
    logf_("%s", line);
}

// --- Frame-Ende ------------------------------------------------------------------------------
static LARGE_INTEGER g_qpf, g_next;

// LBO_SLOWSTART=Frames: In der Startphase (Logos, Intro, Titel) jeden Frame zusaetzlich 50 ms warten. Dort
// starten Szenen und Animationen, sobald ihre Daten geladen sind; so sind Ladevorgaenge praktisch immer
// innerhalb eines Frames fertig (Netztest: sonst gelegentlich 1 Frame Versatz um Frame 280).
static int g_slow_start;

static void cap_frame_rate(void)
{
    LARGE_INTEGER now;
    LONGLONG period;
    if (g_F < g_slow_start) { g_in_wait = 1; Sleep(50); g_in_wait = 0; }
    if (g_fps_cap <= 0) return;
    g_in_wait = 1;
    period = g_qpf.QuadPart / g_fps_cap;
    QueryPerformanceCounter(&now);
    if (!g_next.QuadPart || now.QuadPart - g_next.QuadPart > period * 4) {
        g_next.QuadPart = now.QuadPart + period;
        g_in_wait = 0;
        return;
    }
    while (now.QuadPart < g_next.QuadPart) {
        LONGLONG left = (g_next.QuadPart - now.QuadPart) * 1000 / g_qpf.QuadPart;
        if (left > 2) Sleep((DWORD)(left - 1));
        QueryPerformanceCounter(&now);
    }
    g_next.QuadPart += period;
    g_in_wait = 0;
}

static unsigned *crt_rand_ptr(void)
{
    unsigned char *ptd = NULL;
    __try { ptd = ((unsigned char *(__cdecl *)(void))FN_GETPTD)(); } __except (EXCEPTION_EXECUTE_HANDLER) { return NULL; }
    return ptd ? (unsigned *)(ptd + 0x14) : NULL;
}

// Alle bekannten Zufallszustaende auf feste Werte setzen. Online uebernimmt der Gast sie spaeter vom Host;
// lokal ersetzt das den Abgleich, weil Intro und Menues die Zustaende unterschiedlich weit vorruecken.
static void reset_rng(void)
{
    unsigned *crt = crt_rand_ptr();
    G_RNG_PM_SEED = 0x1F3AD27F;
    G_RNG_LCG16 = 12345;
    G_RNG_PM_DEFAULT = 1;
    G_RNG_A = 0x1234;
    G_RNG_B = 0x5678;
    G_RNG_C = 0x9ABC;
    if (crt) *crt = 1;
    // Eingabe-Takt (0x63bcd0): pro Frame wird nur ein Geraet gelesen, reihum. Die Phase stammt aus der
    // Startphase und muss wie die Zufallszustaende auf beiden Seiten gleich sein.
    G_POLL_COUNTDOWN = 1;
    G_POLL_DEVICE = 0;
    G_POLL_RESTART = 0;
    logf_("Zufallszustaende und Eingabe-Takt zurueckgesetzt");
}

// --- Netz: Abgleich und Lockstep ----------------------------------------------------------------------
// Abgleichpunkte: Frame 30 (nach dem ersten Laden, das die Zustaende je nach Ladezeit verschiebt) und der
// Levelstart (erster Frame mit mehr als 2 Figuren). Der Host schickt seine Zustaende, der Gast uebernimmt sie
// im selben Frame an derselben Stelle des Frame-Endes.
#define SYNC_EARLY_FRAME 30
typedef struct { unsigned rng[7]; int poll_countdown; short poll_device; char poll_restart, pad; } SYNCSTATE;

static void sync_capture(SYNCSTATE *s)
{
    unsigned *crt = crt_rand_ptr();
    memset(s, 0, sizeof *s);
    s->rng[0] = G_RNG_PM_SEED; s->rng[1] = G_RNG_LCG16; s->rng[2] = G_RNG_PM_DEFAULT;
    s->rng[3] = G_RNG_A; s->rng[4] = G_RNG_B; s->rng[5] = G_RNG_C; s->rng[6] = crt ? *crt : 0;
    s->poll_countdown = G_POLL_COUNTDOWN; s->poll_device = G_POLL_DEVICE; s->poll_restart = G_POLL_RESTART;
}

static void sync_apply(const SYNCSTATE *s)
{
    unsigned *crt = crt_rand_ptr();
    G_RNG_PM_SEED = s->rng[0]; G_RNG_LCG16 = s->rng[1]; G_RNG_PM_DEFAULT = s->rng[2];
    G_RNG_A = s->rng[3]; G_RNG_B = s->rng[4]; G_RNG_C = s->rng[5];
    if (crt) *crt = s->rng[6];
    G_POLL_COUNTDOWN = s->poll_countdown; G_POLL_DEVICE = s->poll_device; G_POLL_RESTART = s->poll_restart;
}

static void net_sync_point(long F, const char *what)
{
    SYNCSTATE s;
    if (net_role() == 0) {
        sync_capture(&s);
        net_send_sync((int)F, &s, sizeof s);
    } else {
        int got;
        g_in_wait = 1;
        got = net_wait_sync((int)F, &s, sizeof s, 60000);
        g_in_wait = 0;
        if (got) sync_apply(&s);
        if (got != F) logf_("Netz: Abgleich %s bei Frame %ld passt nicht (Host-Frame %d)", what, F, got);
    }
    logf_("Netz: Abgleich %s bei Frame %ld", what, F);
}

// Im Netzmodus darf das Spiel nur die beiden Lockstep-Pads sehen. 0x63b6f0 (this, Geraet, Ausgabe) liest
// Geraet 0-3 per XInput (unser Haken), ab 4 DirectInput-Geraete inkl. Tastatur. Die Aufrufe 0x63bd59,
// 0x63d608 und 0x63d8ee gehen ueber diesen Stummel: Geraete ab 4 melden "keine Eingabe".
// LBO_NODINPUT=1: dasselbe im lokalen Test (sonst Standard nur im Netz). Die Tastatur-/DirectInput-Abfrage
// aenderte im Paartest bei Frame 269 Eingabe-Zustaende (0x13655a4) und verschob den Spielstart um 1 Frame.
static int g_nodinput;

__declspec(naked) static void stub_poll_device(void)
{
    __asm {
        cmp dword ptr [esp + 4], 4
        jae no_input
        push 0063B6F0h
        ret
    no_input:
        xor eax, eax
        ret 8
    }
}

// Lokale Eingabe abtasten (Skript zum Testen, sonst echter Controller) und mit dem Partner tauschen
static void net_exchange(long F)
{
    NETPAD local, both[2];
    PAD p;
    int i;
    memset(&local, 0, sizeof local);
    if (g_nlines > 0) {
        EnterCriticalSection(&g_cs);
        scripted_state(net_role(), &p);
        LeaveCriticalSection(&g_cs);
        local.buttons = p.wButtons; local.lt = p.bLeftTrigger; local.rt = p.bRightTrigger;
        local.lx = p.sThumbLX; local.ly = p.sThumbLY; local.rx = p.sThumbRX; local.ry = p.sThumbRY;
        local.connected = 1;
    } else {
        input_read(&local);
    }
    g_in_wait = 1;
    net_frame((int)F, &local, both);
    g_in_wait = 0;
    for (i = 0; i < 2; i++) {
        g_net_pad[i].wButtons = both[i].buttons;
        g_net_pad[i].bLeftTrigger = both[i].lt; g_net_pad[i].bRightTrigger = both[i].rt;
        g_net_pad[i].sThumbLX = both[i].lx; g_net_pad[i].sThumbLY = both[i].ly;
        g_net_pad[i].sThumbRX = both[i].rx; g_net_pad[i].sThumbRY = both[i].ry;
    }
    if (F % 3000 == 0) {
        long stalls, desyncs;
        net_stats(&stalls, &desyncs);
        logf_("Netz: %ld Wartephasen, %ld Desyncs bis Frame %ld", stalls, desyncs, F);
    }
}

static unsigned crt_rand_state(void)
{
    unsigned char *ptd = NULL;
    __try { ptd = ((unsigned char *(__cdecl *)(void))FN_GETPTD)(); } __except (EXCEPTION_EXECUTE_HANDLER) { return 0; }
    return ptd ? *(unsigned *)(ptd + 0x14) : 0;
}

// --- Stillstands-Waechter: Steht das Frame-Ende laenger, Aufrufstapel des Hauptthreads protokollieren ---
static void log_main_stack(const char *why)
{
    CONTEXT c;
    DWORD i, n = 0, words[512];
    char line[800];
    int len;
    if (!g_main_thread || SuspendThread(g_main_thread) == (DWORD)-1) return;
    c.ContextFlags = CONTEXT_CONTROL;
    if (GetThreadContext(g_main_thread, &c)) {
        len = snprintf(line, sizeof line, "%s: Hauptthread EIP=%08lx Stack:", why, c.Eip);
        if (ReadProcessMemory(GetCurrentProcess(), (void *)c.Esp, words, sizeof words, NULL) ||
            ReadProcessMemory(GetCurrentProcess(), (void *)c.Esp, words, 1024, NULL)) {
            for (i = 0; i < 512 && n < 20; i++)
                if (words[i] >= 0x00401000 && words[i] < 0x00C55000) {
                    len += snprintf(line + len, sizeof line - len, " %08lx", words[i]);
                    n++;
                }
        }
        ResumeThread(g_main_thread);
        logf_("%s", line);
        return;
    }
    ResumeThread(g_main_thread);
}

static DWORD WINAPI stall_watch(void *arg)
{
    long logged_for = -1;
    (void)arg;
    for (;;) {
        Sleep(250);
        if (g_last_frame_tick && GetTickCount() - g_last_frame_tick > 2000 && logged_for != g_F) {
            logged_for = g_F;
            log_main_stack("Stillstand");
        }
    }
}

// Neue Pads erkennt das Spiel nur beim Start und nach WM_DEVICECHANGE (Fensterprozedur 0x640a20,
// DBT_DEVNODES_CHANGED setzt 0x1153e9c). Nach "connect" im laufenden Spiel deshalb die Nachricht schicken.
static BOOL CALLBACK find_own_window(HWND h, LPARAM out)
{
    DWORD pid = 0;
    char cls[32];
    GetWindowThreadProcessId(h, &pid);
    if (pid == GetCurrentProcessId() && GetClassNameA(h, cls, sizeof cls) && !strcmp(cls, "TTalesWindow")) {
        *(HWND *)out = h;
        return FALSE;
    }
    return TRUE;
}

static void notify_devchange(void)
{
    HWND hwnd = NULL;
    EnumWindows(find_own_window, (LPARAM)&hwnd);
    if (hwnd) PostMessageA(hwnd, WM_DEVICECHANGE, 7 /* DBT_DEVNODES_CHANGED */, 0);
    logf_("Pad-Aenderung gemeldet (WM_DEVICECHANGE)");
}

// Pruefsumme ueber alle belegten Figurenplaetze (Positionen) und die Positionen von Spieler 1 und 2
typedef struct { unsigned hash; int n; float p1[3], p2[3]; } CHARSNAP;

static void char_snapshot(CHARSNAP *cs)
{
    unsigned char *base, *c;
    int i, n, k;
    memset(cs, 0, sizeof *cs);
    cs->hash = 2166136261u;
    __try {
        base = G_CHAR_BASE;
        n = G_CHAR_COUNT;
        if (base && n > 0 && n <= 0x40)
            for (i = 0; i < n; i++) {
                c = base + i * CHAR_STRIDE;
                if (!*(int *)(c + CHAR_USED)) continue;
                cs->n++;
                for (k = 0; k < 12; k++) cs->hash = (cs->hash ^ c[CHAR_POS + k]) * 16777619u;
            }
        if (G_PLAYER1) memcpy(cs->p1, G_PLAYER1 + CHAR_POS, 12);
        if (G_PLAYER2) memcpy(cs->p2, G_PLAYER2 + CHAR_POS, 12);
    } __except (EXCEPTION_EXECUTE_HANDLER) {}
}

// LBO_BGSYNC=2: Am Frame-Ende warten, bis NuBgProc keinen Auftrag mehr hat oder nur noch auf einen
// spaeteren Frame wartet. Dann enden Ladeauftraege an festen Frames statt je nach Ladezeit.
static void bg_wait_idle(void)
{
    unsigned char *q = G_BGPROC;
    DWORD t0;
    int waited = 0;
    if (!q) return;
    t0 = GetTickCount();
    for (;;) {
        LONG pending = *(volatile LONG *)(q + BGPROC_PENDING);
        int frame_wait = *(volatile int *)(q + BGPROC_WAITFRAME);
        if ((pending <= 0 && !G_BGPROC_CURRENT) || frame_wait) break;
        if (GetTickCount() - t0 > 3000) {
            g_bg_timeouts++;
            logf_("NuBgProc: Zeitgrenze (offen %ld, laufend %p)", pending, G_BGPROC_CURRENT);
            break;
        }
        waited = 1;
        Sleep(0);
    }
    if (waited) g_bg_waits++;
}

// Zaehlt Streaming-Eintraege, die gewuenscht, aber noch nicht geladen sind. Eintraege einer anderen Gruppe ueberspringt auch 0x7eb950.
static int loads_in_flight(int *first_state)
{
    int *list, *it, n = 0, guard = 0;
    *first_state = -1;
    __try {
        list = G_LOADLIST;
        if (!list) return 0;
        if (g_gate) { if (!TryEnterCriticalSection(LOADLIST_CS)) return 0; }   // nie blockieren: Halter evtl. angehalten
        else EnterCriticalSection(LOADLIST_CS);
        for (it = (int *)list[0]; it && guard < 4096; it = (int *)it[2], guard++) {
            int st = it[5];
            char want = *((char *)it + 0x1c);
            if (G_LOADFILTER_ON && G_LOADFILTER_ID != -1 && it[8] != G_LOADFILTER_ID) continue;
            // Nur Laden zaehlt: Entladen (5) wartet auf das Zeichnen im Hauptthread und aendert keine Spiellogik
            if (!want || st == 3 || st == 4 || st == 5 || st == 0) continue;
            if (!n) *first_state = st * 2 + (want != 0);
            n++;
        }
        LeaveCriticalSection(LOADLIST_CS);
    } __except (EXCEPTION_EXECUTE_HANDLER) { return 0; }
    return n;
}

// LBO_LOADSYNC: Laden und Entladen werden innerhalb des Frames fertig, in dem die Spiellogik sie anfordert.
// So erscheinen Figuren und Levelteile an festen Frames, unabhaengig von der Ladezeit.
// LBO_IOWAIT=1: Arbeits-Threads des Spiels (Thread-Tabelle 0x102dbe0, Abstand 0xb4, Handle +0xa8):
// 2 m_readThread, 3 m_decodeThread, 4 TextureDecode, 5-12 NuUbershaderBytecodeCache::Work,
// 13 NuShaderManagerGen::WorkerThread. Das Frame-Ende wartet, bis keiner von ihnen 3 ms lang gerechnet hat
// (QueryThreadCycleTime), hoechstens 500 ms. Eine neue Szene startet erst, wenn ihre Shader bereit sind
// (Netztest: Szenen-Flag *0x11bdf70+0xdd beim Host einen Frame frueher).
static int g_iowait;
static long g_io_waits, g_io_timeouts;

static ULONG64 io_cycles(void)
{
    static const int idx[] = { 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13 };
    ULONG64 sum = 0, c;
    int i;
    for (i = 0; i < (int)(sizeof idx / sizeof idx[0]); i++) {
        HANDLE h = *(HANDLE *)(0x0102DBE0 + idx[i] * 0xB4 + 0xA8);
        if (h && QueryThreadCycleTime(h, &c)) sum += c;
    }
    return sum;
}

// Lese-Pipeline der Datenstroeme (Objekt = Argument von m_readThread, Thread-Tabelle Eintrag 2, +4):
// m_readThread (0x567460) liest Bloecke von der Platte, m_decodeThread (0x563980) entpackt sie. Wartet der
// Leser auf die Platte, rechnet er nicht - die CPU-Ruhe allein sagt dann nichts. Leerlauf heisst:
// Leser wartet auf sein Ereignis (+0x16792c = 1) und keine Anfrage ist offen (wie die Pruefung in 0x567460).
static int stream_busy(void)
{
    __try {
        unsigned char *o = *(unsigned char **)(0x0102DBE0 + 2 * 0xB4 + 4);
        unsigned char *q;
        if (!o) return 0;
        q = *(unsigned char **)(o + 0xC);
        if (*(volatile LONG *)(o + 0x16792C) != 1) return 1;
        if (q && *(int *)(q + 0x18) + *(int *)(q + 8) - *(int *)(o + 0x18) != 0) return 1;
        if (*(int *)(o + 0x4124) != *(int *)(o + 0x4120)) return 1;
    } __except (EXCEPTION_EXECUTE_HANDLER) {}
    return 0;
}

static void wait_io(void)
{
    DWORD t0 = GetTickCount();
    ULONG64 prev = io_cycles(), now;
    int waited = 0;
    for (;;) {
        Sleep(3);
        now = io_cycles();
        if (now == prev && !stream_busy()) break;
        prev = now;
        waited = 1;
        if (GetTickCount() - t0 > 500) { g_io_timeouts++; break; }
    }
    if (waited) g_io_waits++;
}

// Speicher-Thread SaveCore (0x540f70): Auftragsring bei 0x102ca58 (Kopf 0x102cab8, Ende 0x102cabc), waehrend
// der Bearbeitung ist 0x11cc19a gesetzt. Er schaut nur alle 100 ms nach (Sleep bei 0x5410c1) und haelt
// Meldungen wie "Speichern..." eine Mindestzeit in Echtzeit (Schleife 0x540db0, Uhr per QPC). Im Paartest
// erschien der Spielstand-Bildschirm deshalb mal bei Frame 4842, mal bei 4879.
// Mit LBO_IOWAIT: Pause auf 1 ms, Mindestzeit uebersprungen (0x540e2b jnp -> jmp, nur im Speicher), und das
// Frame-Ende wartet, bis der Ring leer ist und nichts mehr bearbeitet wird.
static long g_save_waits, g_save_timeouts;

static void wait_savecore(void)
{
    DWORD t0 = GetTickCount();
    int waited = 0;
    for (;;) {
        LONG head = *(volatile LONG *)0x0102CAB8, tail = *(volatile LONG *)0x0102CABC;
        if (head == tail && !*(volatile BYTE *)0x011CC19A) break;
        if (GetTickCount() - t0 > 3000) {
            g_save_timeouts++;
            logf_("SaveCore: Zeitgrenze (Ring %ld/%ld)", head, tail);
            break;
        }
        waited = 1;
        Sleep(0);
    }
    if (waited) g_save_waits++;
}

static void patch_byte(DWORD at, BYTE expect, BYTE val)
{
    DWORD old;
    if (*(BYTE *)at != expect) return;   // noch verschluesselt oder schon gepatcht
    VirtualProtect((void *)at, 1, PAGE_EXECUTE_READWRITE, &old);
    *(BYTE *)at = val;
    VirtualProtect((void *)at, 1, old, &old);
    FlushInstructionCache(GetCurrentProcess(), (void *)at, 1);
}

static void patch_savecore(void)
{
    if (*(WORD *)0x005410C1 == 0x646A) patch_byte(0x005410C2, 0x64, 1);      // push 100 -> push 1
    if (*(WORD *)0x00540E2B == 0x117B) patch_byte(0x00540E2B, 0x7B, 0xEB);   // jnp -> jmp: keine Mindestzeit
}

// Alle Streaming-Eintraege im Uebergang, auch Entladen: laedt (2), entlaedt (5), gewuenscht aber leer (1),
// geladen aber nicht mehr gewuenscht (3/4). Siehe post_frame_end.
static int g_unsettled_first;   // Zustand*2+gewuenscht des ersten offenen Eintrags (Diagnose)

static int list_unsettled(void)
{
    int *list, *it, n = 0, guard = 0;
    __try {
        list = G_LOADLIST;
        if (!list) return 0;
        EnterCriticalSection(LOADLIST_CS);
        for (it = (int *)list[0]; it && guard < 4096; it = (int *)it[2], guard++) {
            int st = it[5];
            char want = *((char *)it + 0x1c);
            if (G_LOADFILTER_ON && G_LOADFILTER_ID != -1 && it[8] != G_LOADFILTER_ID) continue;
            if (st == 0 || (st == 1 && !want) || ((st == 3 || st == 4) && want)) continue;
            if (!n) g_unsettled_first = st * 2 + (want != 0);
            n++;
        }
        LeaveCriticalSection(LOADLIST_CS);
    } __except (EXCEPTION_EXECUTE_HANDLER) { return 0; }
    return n;
}

// Nach dem Frame-Ende (also nach dem Zeichnen) und vor der Spiellogik des Schritts: Solange Eintraege im
// Uebergang sind, zwei volle Durchlaeufe der Lade-Schleife 0x7eb950 abwarten. Ob ein Eintrag fertig
// laedt oder entlaedt, haengt am Stand des Hauptthreads (Entladen braucht mehrere gezeichnete Frames);
// nach zwei Durchlaeufen ist das Ergebnis dieses Frames festgelegt, statt mal vor, mal nach der Logik
// einzutreten (Netztest: Szene startete beim Host 1 Frame spaeter).
static long g_post_waits, g_post_timeouts;
static volatile LONG g_bgloop;   // Durchlaeufe der Lade-Schleife (Aufruf 0x557850 bei 0x7ebb90)

__declspec(naked) static void stub_bgloop(void)
{
    __asm {
        lock inc dword ptr [g_bgloop]
        mov eax, 00557850h
        jmp eax
    }
}

// Die Lade-Schleife laeuft gerade (eingereiht reicht nicht: NuBgProc hat nur einen Thread, ein anderer
// Auftrag wie StreamingLevel::Unload kann davor laufen)
static int bg_loader_active(void)
{
    int *cur = (int *)G_BGPROC_CURRENT;
    return G_BGPROC && cur && cur[2] == 0x007D48C0;
}

// LBO_LOADSLOW=ms: Solange geladen wird (anderer NuBgProc-Auftrag, wartende Auftraege oder Eintraege im
// Uebergang), nach dem Zeichnen zusaetzlich warten. Auftraege wie StreamingLevel::Unload (0x7ddc60, raeumt
// ueber 0x7ebef0 die Liste ab) brauchen den Hauptthread ueber mehrere Frames und lassen sich nicht hart
// abwarten. Mit der Pause enden sie praktisch immer im selben Frame.
static int g_load_slow;
static long g_slow_frames;
// LBO_JITTER=ms (Test): zufaellige Pause 0..ms nach jedem Frame-Ende. Ahmt die unterschiedlichen
// Wartezeiten im Netz nach, damit sich Lade-Wettlaeufe lokal mit zwei Instanzen nachstellen lassen.
static int g_jitter;
static unsigned g_jitter_state;

// --- Frame-gebundene Lade-Threads (LBO_GATE=1) ------------------------------------------------------
// Die Lade-Threads (Tabelle 0x102dbe0: 2 m_readThread, 15 NuBgProc)
// arbeiten nur im Fenster nach dem Frame-Ende: Dort laufen sie, bis sie 4 ms lang nicht gerechnet haben;
// waehrend Spiellogik und Zeichnen sind sie angehalten. Wie viel sie pro Frame schaffen, haengt dann nicht
// mehr von der Rechnergeschwindigkeit ab (Test: schnelle und verzoegerte Instanz liefen sonst verschieden).
// Wartet der Hauptthread im Spielcode auf einen dieser Threads (z. B. beim Laden), gibt der Waechter sie
// frei, bis der Hauptthread weiterrechnet.
static volatile LONG g_gate_suspended;
static long g_gate_windows, g_gate_timeouts, g_gate_assists;
// LBO_GATESET=Indizes (Komma), Standard 2,15
static int g_gate_idx[19] = { 2, 15 };
static int g_gate_n = 2;
#define GATE_N g_gate_n

static HANDLE gate_handle(int i) { return *(HANDLE *)(0x0102DBE0 + g_gate_idx[i] * 0xB4 + 0xA8); }

static ULONG64 gate_cycles(void)
{
    ULONG64 sum = 0, c;
    int i;
    for (i = 0; i < GATE_N; i++) { HANDLE h = gate_handle(i); if (h && QueryThreadCycleTime(h, &c)) sum += c; }
    return sum;
}

static void gate_resume(void)
{
    int i;
    if (!InterlockedExchange(&g_gate_suspended, 0)) return;
    for (i = 0; i < GATE_N; i++) { HANDLE h = gate_handle(i); if (h) ResumeThread(h); }
}

static volatile DWORD g_gate_close_tick;

static void gate_suspend(void)
{
    int i;
    g_gate_close_tick = GetTickCount();
    if (InterlockedExchange(&g_gate_suspended, 1)) return;
    for (i = 0; i < GATE_N; i++) { HANDLE h = gate_handle(i); if (h) SuspendThread(h); }
}

// Fenster: Threads laufen lassen, bis sie 4 ms am Stueck ruhen (hoechstens 300 ms)
static void gate_window(void)
{
    DWORD t0 = GetTickCount();
    ULONG64 prev, now;
    int idle = 0;
    gate_resume();
    g_gate_windows++;
    prev = gate_cycles();
    for (;;) {
        Sleep(1);
        now = gate_cycles();
        if (now == prev) { if (++idle >= 4) break; }
        else { idle = 0; prev = now; }
        if (GetTickCount() - t0 > 300) { g_gate_timeouts++; break; }
    }
}

static DWORD WINAPI gate_watch(void *arg)
{
    ULONG64 last = 0, c;
    int idle = 0;
    (void)arg;
    for (;;) {
        Sleep(2);
        if (!g_gate || !g_main_thread || g_in_wait) { idle = 0; continue; }
        if (!QueryThreadCycleTime(g_main_thread, &c)) continue;
        // Freigeben, wenn der Hauptthread im Spielcode wartet: unter 300.000 Zyklen in 2 ms (auch
        // Sleep-Schleifen wachen kurz auf), oder er kommt 100 ms lang nicht in unseren Haken an (aktives Warten
        // auf eine Sperre, die ein angehaltener Thread haelt, z. B. StreamingLevel::Unload beim Zeichnen).
        // Angehalten wird erst wieder am Ende des naechsten Fensters.
        if (g_gate_suspended) {
            if (c - last < 300000) idle++;
            else idle = 0;
            if (idle >= 5 || GetTickCount() - g_gate_close_tick > 100) {
                gate_resume();
                g_gate_assists++;
                idle = 0;
            }
        }
        last = c;
    }
}

static void __cdecl post_frame_end_inner(void);

static void __cdecl post_frame_end(void)
{
    if (g_gate && g_F > 30) gate_window();
    post_frame_end_inner();
    if (g_gate && g_F > 30) gate_suspend();
}

// --- Getaktete Lade-Schleife (LBO_LOOPGATE=1) -------------------------------------------------------
// Die Streaming-Schleife 0x7eb950 dreht im Hintergrund ununterbrochen, solange es Eintraege gibt, und
// aendert deren Zustand zu beliebigen Zeitpunkten (Speichervergleich: bei Frame 281 steckte sie in einem
// Lauf mitten im Durchlauf, im anderen nicht). Am Schleifenanfang (0x7eb979, keine Sperre gehalten) wartet
// sie deshalb auf eine Freigabe. Nach dem Frame-Ende gibt loop_window einzelne Durchlaeufe frei, bis sich
// die Eintraege zwei Durchlaeufe lang nicht aendern und keiner mehr laedt. Waehrend Logik und Zeichnen
// steht sie. Laeuft die Schleife im Hauptthread (erstes Laden), wird nicht gebremst.
static int g_loopgate;
static HANDLE g_loop_sem;
static volatile LONG g_loop_at_gate, g_loop_arrivals;
static long g_loop_iters, g_loop_timeouts, g_loop_windows;

static long g_loop_selfrelease;

static void __cdecl loop_gate(void)
{
    LONG f0;
    DWORD t0;
    if (!g_loopgate || !g_loop_sem || !g_main_tid || GetCurrentThreadId() == g_main_tid) return;
    InterlockedIncrement(&g_loop_at_gate);
    InterlockedIncrement(&g_loop_arrivals);
    // Sicherheitsnetz: Wartet der Hauptthread im Spielcode auf die Schleife, kaeme kein Fenster mehr.
    // Freigabe, wenn 1 s lang kein Frame endet und der Hauptthread nicht in einer gewollten Pause steckt.
    f0 = g_F;
    t0 = GetTickCount();
    for (;;) {
        if (WaitForSingleObject(g_loop_sem, 50) == WAIT_OBJECT_0) break;
        if (g_F != f0 || g_in_wait) { f0 = g_F; t0 = GetTickCount(); }
        else if (GetTickCount() - t0 > 1000) {
            int *cur = (int *)G_BGPROC_CURRENT;
            InterlockedIncrement(&g_loop_selfrelease);
            logf_("Lade-Schleife: Selbstfreigabe (Auftrag %08x)", cur ? cur[2] : 0);
            break;
        }
    }
    InterlockedDecrement(&g_loop_at_gate);
}

__declspec(naked) static void stub_loop_top(void)
{
    __asm {
        pushad
        pushfd
        call loop_gate
        popfd
        popad
        push 0138BA90h      // ueberschriebener Befehl bei 0x7eb979
        push 007EB97Eh
        ret
    }
}

// Aufraeum-Schleife 0x7ebef0 (StreamingLevel::Unload 0x7ddc60 ruft sie im NuBgProc-Thread): dreht, bis
// alle nicht mehr gewuenschten Eintraege entladen sind, und wartet dabei auf das Zeichnen im Hauptthread.
// Ohne Takt endete Unload je nach Rechnertempo einen Frame frueher oder spaeter (Paar F2, Frame 7851).
// Gleiche Freigabe wie die Lade-Schleife, Haken am Schleifenanfang 0x7ebf19 (push 0x138ba90).
// Startphase: Dort wartet der Hauptthread selbst auf StreamingLevel::Unload (sonst 2x 5 s Stillstand)
static void __cdecl flush_gate(void)
{
    if (g_F >= 300) loop_gate();
}

__declspec(naked) static void stub_flush_top(void)
{
    __asm {
        pushad
        pushfd
        call flush_gate
        popfd
        popad
        push 0138BA90h      // ueberschriebener Befehl bei 0x7ebf19
        push 007EBF1Eh
        ret
    }
}

// Signatur der Streaming-Liste (Eintrag, Zustand, gewuenscht) und Zahl der ladenden Eintraege
static unsigned list_signature(int *loading)
{
    int *list = G_LOADLIST, *it, guard = 0;
    unsigned h = 2166136261u;
    *loading = 0;
    if (!list) return 0;
    __try {
        EnterCriticalSection(LOADLIST_CS);
        for (it = (int *)list[0]; it && guard < 4096; it = (int *)it[2], guard++) {
            h = (h ^ (unsigned)(DWORD)it) * 16777619u;
            h = (h ^ (unsigned)it[5]) * 16777619u;
            h = (h ^ (unsigned)*((char *)it + 0x1c)) * 16777619u;
            if (it[5] == 2) (*loading)++;
        }
        h ^= (unsigned)list[2];
        LeaveCriticalSection(LOADLIST_CS);
    } __except (EXCEPTION_EXECUTE_HANDLER) {}
    return h;
}

static ULONG64 bgproc_cycles(void)
{
    ULONG64 c = 0;
    HANDLE h = *(HANDLE *)(0x0102DBE0 + 15 * 0xB4 + 0xA8);   // NuBgProc
    if (h) QueryThreadCycleTime(h, &c);
    return c;
}

static void loop_window(void)
{
    DWORD t0 = GetTickCount();
    int stable = 0, loading;
    unsigned sig, prev;
    if (!g_loop_at_gate) return;
    g_loop_windows++;
    prev = list_signature(&loading);
    for (;;) {
        LONG a0 = g_loop_arrivals;
        ULONG64 c, last = bgproc_cycles();
        int idle = 0;
        ReleaseSemaphore(g_loop_sem, 1, NULL);
        g_loop_iters++;
        // Warten, bis die Schleife wieder am Anfang steht. Endet sie (Liste leer), kommt sie nicht wieder:
        // dann ruht NuBgProc oder arbeitet am naechsten Auftrag ausserhalb der Schleife.
        while (g_loop_arrivals == a0) {
            Sleep(0);
            c = bgproc_cycles();
            if (c == last) { if (++idle > 2000) break; } else { idle = 0; last = c; }
            if (GetTickCount() - t0 > 2000) break;
        }
        if (g_loop_arrivals == a0) break;   // Schleife verlassen
        sig = list_signature(&loading);
        if (sig == prev && !loading) { if (++stable >= 2) break; }
        else stable = 0;
        prev = sig;
        if (GetTickCount() - t0 > 2000) {
            g_loop_timeouts++;
            logf_("Lade-Schleife: Zeitgrenze (%d Eintraege laden)", loading);
            break;
        }
    }
}

// Nach dem Frame-Ende NuBgProc zur Ruhe kommen lassen: Andere Auftraege (z. B. SampleLoader 0x7359b0 beim
// Levelstart) liefen sonst je nach Rechnertempo noch in den naechsten Spielschritt hinein (Paar F4: bei
// Anker +2 lief er in der verzoegerten Instanz noch, ab +4 kippte der LCG). Ruhe heisst: kein Auftrag,
// Auftrag wartet auf einen Frame, Schleife steht an ihrer Freigabe, oder der Thread rechnet 5 ms lang
// nicht und liest nichts (wartet auf den Hauptthread). Rueckgabe 1, wenn gewartet wurde.
static long g_settle_waits, g_settle_timeouts;

static int bg_settle(void)
{
    DWORD t0 = GetTickCount();
    ULONG64 last = bgproc_cycles(), c;
    DWORD still = GetTickCount();
    int waited = 0;
    if (!G_BGPROC) return 0;
    for (;;) {
        int *cur = (int *)G_BGPROC_CURRENT;
        LONG pend = *(volatile LONG *)(G_BGPROC + BGPROC_PENDING);
        if (*(volatile int *)(G_BGPROC + BGPROC_WAITFRAME)) break;
        if (!cur && pend <= 0) break;
        if (g_loop_at_gate) break;
        c = bgproc_cycles();
        if (c != last || stream_busy()) { last = c; still = GetTickCount(); }
        else if (GetTickCount() - still >= 5) break;
        if (GetTickCount() - t0 > 2000) { g_settle_timeouts++; break; }
        waited = 1;
        Sleep(0);
    }
    if (waited) g_settle_waits++;
    return waited;
}

static void __cdecl post_frame_end_inner(void)
{
    DWORD t0;
    LONG c0;
    int waited = 0;
    if (g_jitter) {
        g_jitter_state = g_jitter_state * 1103515245u + 12345u;
        g_in_wait = 1;
        Sleep((g_jitter_state >> 16) % (unsigned)(g_jitter + 1));
        g_in_wait = 0;
    }
    if (g_iowait) { wait_io(); wait_savecore(); }   // nach dem Zeichnen: Shader-, Textur- und Speicherarbeit
    if (g_loopgate) {
        int k;
        loop_window();
        // Ein fertig gewordener Auftrag kann die Schleife neu starten: dann noch ein Fenster
        // Ab Frame 30 (davor laufen die Start-Auftraege, auf die der Hauptthread selbst wartet). Beim ersten
        // Laden (Frame 280-284) endete der Lade-Auftrag sonst manchmal erst im naechsten Spielschritt.
        for (k = 0; k < 4 && g_F >= 30 && bg_settle(); k++) loop_window();
        // Ist die Liste leer, beendet sich der Auftrag gleich (siehe unten): auch hier abwarten
        t0 = GetTickCount();
        while (G_LOADLIST && G_LOADLIST[2] == 0 && bg_loader_active() && !g_loop_at_gate) {
            if (GetTickCount() - t0 > 1000) { g_post_timeouts++; break; }
            Sleep(0);
        }
        return;
    }
    if (!g_loadsync || !G_BGPROC) return;
    if (g_load_slow) {
        int *cur = (int *)G_BGPROC_CURRENT;
        if ((cur && cur[2] != 0x007D48C0) || *(volatile LONG *)(G_BGPROC + BGPROC_PENDING) > 0 || list_unsettled()) {
            Sleep(g_load_slow);
            g_slow_frames++;
        }
    }
    t0 = GetTickCount();
    c0 = g_bgloop;
    while (list_unsettled() && bg_loader_active() && g_bgloop < c0 + 2) {
        if (GetTickCount() - t0 > 1000) {
            g_post_timeouts++;
            logf_("Nach Frame-Ende: Zeitgrenze (%d offen, Zustand %d, gewuenscht %d)", list_unsettled(),
                  g_unsettled_first / 2, g_unsettled_first & 1);
            break;
        }
        waited = 1;
        Sleep(0);
    }
    // Ist die Liste leer, beendet sich der Auftrag gleich. Der naechste Spielschritt prueft, ob ein Auftrag
    // laeuft (*0x11cc118, 0x7e6cd0) - dieses Ende darf nicht mal vor, mal nach der Pruefung liegen.
    t0 = GetTickCount();
    while (G_LOADLIST && G_LOADLIST[2] == 0 && bg_loader_active()) {
        if (GetTickCount() - t0 > 1000) { g_post_timeouts++; logf_("Nach Frame-Ende: Auftrag endet nicht"); break; }
        waited = 1;
        Sleep(0);
    }
    if (waited) g_post_waits++;
}

// Ersatz fuer "call 0x5528c0" im Spielschritt (0x7e6e03, 0x7e6f98): Rueckgabewert dt in st(0) erhalten
__declspec(naked) static void stub_after_frame_end(void)
{
    __asm {
        mov eax, 005528C0h
        call eax
        sub esp, 8
        fstp qword ptr [esp]
        pushad
        pushfd
        call post_frame_end
        popfd
        popad
        fld qword ptr [esp]
        add esp, 8
        ret
    }
}

// Startphase: Hier laufen weitere NuBgProc-Auftraege (Ladebildschirm, Samples), deren Dauer sonst
// bestimmt, an welchem Frame das Intro beginnt. Bis Frame 300 auf jeden Auftrag ausser der
// Streaming-Schleife warten (der Hauptthread laeuft weiter, sobald die Zeitgrenze erreicht ist).
static void boot_bg_wait(void)
{
    DWORD t0;
    if (g_F >= 300 || !G_BGPROC) return;
    t0 = GetTickCount();
    for (;;) {
        int *cur = (int *)G_BGPROC_CURRENT;
        LONG pend = *(volatile LONG *)(G_BGPROC + BGPROC_PENDING);
        if (*(volatile int *)(G_BGPROC + BGPROC_WAITFRAME)) break;
        if (cur && cur[2] == 0x007D48C0 && pend <= 0) break;
        if (!cur && pend <= 0) break;
        if (GetTickCount() - t0 > 5000) { logf_("Start: NuBgProc-Zeitgrenze (Auftrag %08x, offen %ld)", cur ? cur[2] : 0, pend); break; }
        Sleep(0);
    }
}

static void wait_loads(void)
{
    DWORD t0 = GetTickCount();
    int n, st, waited = 0;
    // Nur warten, solange der Hintergrund-Auftrag (0x7d48c0 -> 0x7eb950) laeuft oder eingereiht ist. Beim
    // ersten Laden nach dem Start bearbeitet der Hauptthread die Liste selbst; Warten wuerde ihn nur blockieren.
    while ((n = loads_in_flight(&st)) > 0 && G_BGPROC &&
           (G_BGPROC_CURRENT || *(volatile LONG *)(G_BGPROC + BGPROC_PENDING) > 0)) {
        if (GetTickCount() - t0 > 30000) {
            g_load_timeouts++;
            logf_("Laden: Zeitgrenze, %d Eintraege offen (erster Zustand %d, gewuenscht %d)", n, st / 2, st & 1);
            break;
        }
        waited = 1;
        Sleep(0);
    }
    if (waited) g_load_waits++;
    boot_bg_wait();
}

// Aufrufer der Zufallsgeneratoren mitschreiben (Diagnose: wer zieht unterschiedlich oft?)
// a = 0x51a1d0 (rng_a), c = CRT rand 0xbd1c9d
static void __stdcall rng_trace(DWORD caller, DWORD tag)
{
    if (g_trace && g_F >= g_trace_from && g_F <= g_trace_to)
        fprintf(g_trace, "%ld %c %08lx %s\n", g_F, (char)tag, caller,
                GetCurrentThreadId() == GetThreadId(g_main_thread) ? "main" : "thread");
}

__declspec(naked) static void stub_rng_a(void)
{
    __asm {
        pushad
        pushfd
        push 'a'
        push dword ptr [esp + 40]   // Ruecksprungadresse
        call rng_trace
        popfd
        popad
        push ecx                    // ueberschriebener Anfang von 0x51a1d0
        mov eax, dword ptr ds:[011BE45Ch]
        push 0051A1D6h
        ret
    }
}

// LBO_RENDERRNG: Diese Aufrufer beim Zeichnen ziehen je nach Effekt-Takt unterschiedlich oft Zufallszahlen
// (zur Laufzeit per LBO_TRACERNG ermittelt). Mit eigenen Zustaenden verschieben sie die Folgen der Spiellogik
// (CRT rand, rng_a 0x11be45c) nicht mehr. ecx/edx bleiben wie beim Original erhalten.
static unsigned g_rr_crt = 1, g_rr_a = 0x1234;
// Der Musik-/Zustandswechsel 0x94cdf0 (Objekt 0x11ad524, ausgeloest ueber 0x9edb10) laeuft im Takt der
// Musik, also in Echtzeit, und zieht dabei aus dem Logik-LCG 0x11a1844. Er und seine Schwesterfunktionen
// 0x94cbc0 und 0x94d110 (gleiches Muster) bekommen je einen eigenen Zustand.
static unsigned g_rr_music_lcg = 12345, g_rr_music_lcg2 = 23456, g_rr_music_lcg3 = 34567;

static int patch_abs32(DWORD at, DWORD expect, void *to)
{
    DWORD old;
    if (*(DWORD *)at != expect) return 0;
    VirtualProtect((void *)at, 4, PAGE_EXECUTE_READWRITE, &old);
    *(DWORD *)at = (DWORD)to;
    VirtualProtect((void *)at, 4, old, &old);
    FlushInstructionCache(GetCurrentProcess(), (void *)at, 4);
    return 1;
}

__declspec(naked) static void render_rand(void)
{
    __asm {
        mov eax, g_rr_crt
        imul eax, eax, 343FDh
        add eax, 269EC3h
        mov g_rr_crt, eax
        shr eax, 16
        and eax, 7FFFh
        ret
    }
}

__declspec(naked) static void render_rng_a(void)
{
    __asm {
        push ecx
        mov eax, g_rr_a
        imul eax, eax, 19660Dh
        add eax, 3C6EF35Fh
        mov g_rr_a, eax
        and eax, 7FFFFFh
        or eax, 3F800000h
        mov dword ptr [esp], eax
        fld dword ptr [esp]
        fld1
        fsubp st(1), st
        fstp dword ptr [esp]
        fld dword ptr [esp]
        pop ecx
        ret
    }
}

// LBO_WATCHLCG: DR0 auf 0x11a1844 (Schreiben, 4 Byte) im Hauptthread. Der Handler merkt sich nur Frame
// und Codeadresse; geschrieben wird am Ende des Fensters.
// LBO_BREAKAT=Adresse (Diagnose): Ausfuehrungs-Haltepunkt (DR1) im Hauptthread. Beim ersten Treffer werden
// Frame und Aufrufstapel protokolliert, danach ist der Haltepunkt aus.
static DWORD g_breakat;
static int g_break_write;   // LBO_BREAKW: Schreib- statt Ausfuehrungs-Haltepunkt
static long g_break_from;   // LBO_BREAKFROM: erst ab diesem Frame scharf
static volatile LONG g_break_hit;

static LONG CALLBACK break_handler(EXCEPTION_POINTERS *e)
{
    CONTEXT *c = e->ContextRecord;
    DWORD *sp = (DWORD *)c->Esp, i;
    char line[600];
    int len, n = 0;
    if (e->ExceptionRecord->ExceptionCode != EXCEPTION_SINGLE_STEP || !(c->Dr6 & 2)) return EXCEPTION_CONTINUE_SEARCH;
    len = snprintf(line, sizeof line, "Haltepunkt %08lx bei Frame %ld, Stack:", g_breakat, g_F);
    __try {
        for (i = 0; i < 512 && n < 24; i++)
            if (sp[i] >= 0x00401000 && sp[i] < 0x00C55000) { len += snprintf(line + len, sizeof line - len, " %08lx", sp[i]); n++; }
    } __except (EXCEPTION_EXECUTE_HANDLER) {}
    logf_("%s", line);
    g_break_hit = 1;
    c->Dr7 &= ~0x0000000Cu;   // DR1 aus
    c->Dr6 = 0;
    c->EFlags |= 0x10000;     // RF: Befehl nicht erneut anhalten
    return EXCEPTION_CONTINUE_EXECUTION;
}

// Das Spiel setzt Debug-Register gelegentlich zurueck: alle 100 ms neu setzen, bis der Haltepunkt traf
static DWORD WINAPI break_set(void *arg)
{
    CONTEXT c;
    (void)arg;
    AddVectoredExceptionHandler(1, break_handler);
    logf_("Haltepunkt auf %08lx", g_breakat);
    while (!g_break_hit) {
        if (SuspendThread(g_main_thread) != (DWORD)-1) {
            c.ContextFlags = CONTEXT_DEBUG_REGISTERS;
            if (!g_break_hit && g_F >= g_break_from && GetThreadContext(g_main_thread, &c) && (c.Dr1 != g_breakat || !(c.Dr7 & 4))) {
                c.Dr1 = g_breakat;
                // L1; Ausfuehren (Laenge 1) oder mit LBO_BREAKW Schreiben (4 Byte)
                c.Dr7 = (c.Dr7 & ~0x00F0000Cu) | 4u | (g_break_write ? (1u << 20) | (3u << 22) : 0u);
                SetThreadContext(g_main_thread, &c);
            }
            ResumeThread(g_main_thread);
        }
        Sleep(100);
    }
    return 0;
}

static LONG CALLBACK lcg_watch_handler(EXCEPTION_POINTERS *e)
{
    LONG i;
    if (e->ExceptionRecord->ExceptionCode != EXCEPTION_SINGLE_STEP || !(e->ContextRecord->Dr6 & 1))
        return EXCEPTION_CONTINUE_SEARCH;
    if (g_F >= g_trace_from && g_F <= g_trace_to && (i = InterlockedIncrement(&g_nlw) - 1) < LW_MAX) {
        g_lw[i].f = g_F;
        g_lw[i].eip = e->ContextRecord->Eip;
        g_lw[i].tid = GetCurrentThreadId();
    }
    e->ContextRecord->Dr6 = 0;
    return EXCEPTION_CONTINUE_EXECUTION;
}

// Setzt DR0 in allen Threads des Spiels (ausser dem eigenen) und wiederholt das alle 2 s fuer neue Threads
static DWORD WINAPI lcg_watch_set(void *arg)
{
    (void)arg;
    AddVectoredExceptionHandler(1, lcg_watch_handler);
    for (;;) {
        HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0);
        THREADENTRY32 te;
        int n = 0;
        te.dwSize = sizeof te;
        if (snap != INVALID_HANDLE_VALUE && Thread32First(snap, &te)) {
            do {
                HANDLE t;
                CONTEXT c;
                if (te.th32OwnerProcessID != GetCurrentProcessId() || te.th32ThreadID == GetCurrentThreadId()) continue;
                t = OpenThread(THREAD_SUSPEND_RESUME | THREAD_GET_CONTEXT | THREAD_SET_CONTEXT, FALSE, te.th32ThreadID);
                if (!t) continue;
                if (SuspendThread(t) != (DWORD)-1) {
                    c.ContextFlags = CONTEXT_DEBUG_REGISTERS;
                    if (GetThreadContext(t, &c) && c.Dr0 != g_watch_addr) {
                        c.Dr0 = g_watch_addr;
                        c.Dr7 = (c.Dr7 & ~0x000F0003u) | 1u | (1u << 16) | (3u << 18);
                        if (SetThreadContext(t, &c)) n++;
                    }
                    ResumeThread(t);
                }
                CloseHandle(t);
            } while (Thread32Next(snap, &te));
        }
        if (snap != INVALID_HANDLE_VALUE) CloseHandle(snap);
        if (n) logf_("LCG-Waechter in %d Threads gesetzt", n);
        Sleep(2000);
    }
}

// Diagnose (LBO_WATCHLCG): Wer ruft den 723-Frame-Ausloeser 0x94cdf0 auf? Stack einmal protokollieren.
static void __stdcall trig_trace(DWORD *sp, DWORD obj)
{
    static int logged;
    char line[600];
    int i, n = 0, len;
    if (logged++ > 2) return;
    len = snprintf(line, sizeof line, "0x94cdf0 Objekt %08lx Stack:", obj);
    for (i = 0; i < 256 && n < 16; i++)
        if (sp[i] >= 0x00401000 && sp[i] < 0x00C55000) { len += snprintf(line + len, sizeof line - len, " %08lx", sp[i]); n++; }
    logf_("%s", line);
}

__declspec(naked) static void stub_trig(void)
{
    __asm {
        pushad
        pushfd
        push ecx
        lea eax, [esp + 40]
        push eax
        call trig_trace
        popfd
        popad
        push esi                    // ueberschriebener Anfang von 0x94cdf0 (7 Bytes)
        mov esi, ecx
        push edi
        mov edi, dword ptr [esi + 10h]
        push 0094CDF7h
        ret
    }
}

// CRT rand 0xbd1c9d beginnt mit call _getptd (0xbda791)
__declspec(naked) static void stub_crt_rand(void)
{
    __asm {
        pushad
        pushfd
        push 'c'
        push dword ptr [esp + 40]
        call rng_trace
        popfd
        popad
        mov eax, 00BDA791h
        call eax
        push 00BD1CA2h
        ret
    }
}

static void __cdecl on_frame_end(void)
{
    CHARSNAP cs;
    long F = InterlockedIncrement(&g_F);
    if (!g_main_thread) {
        DuplicateHandle(GetCurrentProcess(), GetCurrentThread(), GetCurrentProcess(), &g_main_thread,
                        THREAD_SUSPEND_RESUME | THREAD_GET_CONTEXT | THREAD_SET_CONTEXT | THREAD_QUERY_INFORMATION, FALSE, 0);
        CreateThread(NULL, 0, stall_watch, NULL, 0, NULL);
        g_main_tid = GetCurrentThreadId();
        if (g_watch_lcg) CreateThread(NULL, 0, lcg_watch_set, NULL, 0, NULL);
        if (g_breakat) CreateThread(NULL, 0, break_set, NULL, 0, NULL);
        if (g_gate) CreateThread(NULL, 0, gate_watch, NULL, 0, NULL);
    }
    if (g_loopgate && F % 3000 == 0)
        logf_("Lade-Schleife: %ld Fenster, %ld Durchlaeufe, %ld Zeitgrenzen, %ld Selbstfreigaben; NuBgProc-Ruhe: %ld Wartephasen, %ld Zeitgrenzen",
              g_loop_windows, g_loop_iters, g_loop_timeouts, g_loop_selfrelease, g_settle_waits, g_settle_timeouts);
    if (g_gate && F % 3000 == 0)
        logf_("Lade-Threads: %ld Fenster, %ld Zeitgrenzen, %ld Freigaben fuer den Hauptthread", g_gate_windows,
              g_gate_timeouts, g_gate_assists);
    if (g_memdump && F >= g_memdump_from && F <= g_memdump_to) {
        // LBO_MEMDUMP=von,bis (Diagnose): .data des Spiels (0x101f000, 0x38e7ec Byte) je Frame anhaengen;
        // tools/memdiff_b2.py findet den ersten globalen Wert, der zwischen zwei Laeufen auseinanderlaeuft
        // Erst im Speicher sammeln: Schreiben je Frame bremst genau das Fenster, in dem der Wettlauf passiert
        static unsigned char *buf;
        if (!buf) buf = (unsigned char *)VirtualAlloc(NULL, (SIZE_T)(g_memdump_to - g_memdump_from + 1) * 0x38E7EC,
                                                      MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
        __try { if (buf) memcpy(buf + (SIZE_T)(F - g_memdump_from) * 0x38E7EC, (void *)0x0101F000, 0x38E7EC); }
        __except (EXCEPTION_EXECUTE_HANDLER) {}
        if (F == g_memdump_to) {
            if (buf) fwrite(buf, 0x38E7EC, (size_t)(g_memdump_to - g_memdump_from + 1), g_memdump);
            fclose(g_memdump);
            g_memdump = NULL;
            logf_("Speicherabzug fertig");
        }
    }
    if (g_watch_lcg && g_trace && F == g_trace_to + 2) {
        LONG i, n = g_nlw < LW_MAX ? g_nlw : LW_MAX;
        for (i = 0; i < n; i++)
            fprintf(g_trace, "%ld l %08lx %s\n", g_lw[i].f, g_lw[i].eip, g_lw[i].tid == g_main_tid ? "main" : "thread");
        fflush(g_trace);
    }
    g_last_frame_tick = GetTickCount();
    flush_shot();
    cap_frame_rate();
    if (g_fixed_dt) G_DT_OVERRIDE = 1.0f / 60.0f;
    // Ohne Fensterfokus haelt das Spiel an (0x5bc170). Im Skriptmodus soll es trotzdem weiterlaufen.
    if (g_mode == MODE_SCRIPT) G_WINDOW_ACTIVE = 1;
    if (g_bgsync == 2) bg_wait_idle();
    // Mit LBO_GATE sind die Lade-Threads hier angehalten; gewartet wird im Fenster nach dem Frame-Ende
    if (g_loadsync && !(g_gate && F > 30) && !g_loopgate) wait_loads();
    else if (g_loopgate) boot_bg_wait();
    if (g_iowait) wait_io();
    // Maus (0x633860) und Tastatur (0x6338f0) per DirectInput: Ob Poll/Acquire gelingt, haengt am
    // Fensterfokus, und jeder Erfolg schaltet Puffer um (0x13658bc, 0x13655a4). Das Spiel hat dafuer
    // Abschalt-Bits (0x136555c: 1 Maus, 2 Tastatur); im Netz zaehlen ohnehin nur die Lockstep-Pads.
    if (g_net || g_nodinput) *(volatile BYTE *)0x0136555C |= 3;
    if (g_vaudio) va_tick();
    if (g_audiolog && F % 600 == 0) audio_log();
    if (g_iowait && F % 3000 == 0) logf_("Datei-Threads: %ld Wartephasen, %ld Zeitgrenzen; SaveCore: %ld Wartephasen, %ld Zeitgrenzen",
                                         g_io_waits, g_io_timeouts, g_save_waits, g_save_timeouts);
    if (F % 3000 == 0 && g_loadsync) logf_("Laden: %ld Wartephasen, %ld Zeitgrenzen; nach Frame-Ende %ld Wartephasen, %ld Zeitgrenzen, %ld verlangsamte Frames", g_load_waits, g_load_timeouts, g_post_waits, g_post_timeouts, g_slow_frames);
    if (F % 3000 == 0 && g_bgsync == 2) logf_("NuBgProc: %ld Wartephasen, %ld Zeitgrenzen", g_bg_waits, g_bg_timeouts);
    if (g_bgsync == 1 && G_BGPROC && !G_BGPROC[BGPROC_SYNC]) {
        G_BGPROC[BGPROC_SYNC] = 1;
        logf_("NuBgProc synchron (Warteschlange %p)", (void *)G_BGPROC);
    }
    if (g_devchange) { g_devchange = 0; notify_devchange(); }
    if (g_rng_reset && F == g_rng_reset) reset_rng();
    if (g_net && F == SYNC_EARLY_FRAME) net_sync_point(F, "nach dem Start");
    if (g_anchor_mode && g_anchor < 0 && G_CHAR_COUNT > 2) {
        g_anchor = F;
        if (g_net) {
            // Schranke am Levelstart: Vorher duerfen die Frames durch Ladezeiten leicht auseinanderliegen.
            // Der Gast uebernimmt den Zustand des Hosts, danach zaehlt der Lockstep ab dem Anker neu.
            SYNCSTATE s;
            if (net_role() == 0) sync_capture(&s);
            int ok;
            g_in_wait = 1;
            ok = net_barrier((int)F, &s, sizeof s, 120000);
            g_in_wait = 0;
            if (ok && net_role() == 1) sync_apply(&s);
        } else reset_rng();
        logf_("Levelanker bei Frame %ld", F);
    }
    if (g_net) {
        // Lockstep-Frame: vor dem Anker absolut, danach ab dem Anker (Epoche 1 in net.c)
        long NF = g_anchor < 0 ? F : F - g_anchor + 1;
        net_exchange(NF);
        // Pruefsummen erst ab dem Anker: vorher sind kleine Frame-Versaetze erlaubt
        if (g_anchor >= 0 && NF % 60 == 0) {
            CHARSNAP k;
            unsigned long long ck;
            char_snapshot(&k);
            ck = ((unsigned long long)k.hash << 32) ^ (unsigned)k.n ^ G_RNG_LCG16 ^ ((unsigned long long)G_RNG_A << 16) ^
                 ((unsigned long long)G_RNG_PM_DEFAULT << 40) ^ crt_rand_state();
            net_checksum((int)NF, ck);
        }
    }
    if (g_csv) {
        char_snapshot(&cs);
        __try {
            int st, inflight = loads_in_flight(&st), *ll = G_LOADLIST;
            unsigned char *q = G_BGPROC;
            int *cur = (int *)G_BGPROC_CURRENT;
            fprintf(g_csv, "%ld,%d,%.6f,%08x,%08x,%08x,%08x,%08x,%08x,%08x,%d,%d,%08x,%.4f,%.4f,%.4f,%.4f,%.4f,%.4f,"
                    "%d,%d,%d,%d,%d,%d,%ld,%08x,%d,%d,%ld,%lu\n", F, G_FRAMECOUNT,
                    G_DT, G_RNG_PM_SEED, G_RNG_LCG16, G_RNG_PM_DEFAULT, G_RNG_A, G_RNG_B, G_RNG_C, crt_rand_state(), g_polls, cs.n, cs.hash,
                    cs.p1[0], cs.p1[1], cs.p1[2], cs.p2[0], cs.p2[1], cs.p2[2],
                    *(int *)0x011CF178, *(int *)0x0131356C, *(int *)0x01313574, *(int *)0x0131359C, *(int *)0x013135B0,
                    *(int *)0x01313568, q ? *(LONG *)(q + BGPROC_PENDING) : -1, cur ? cur[2] : 0, ll ? ll[2] : -1, inflight,
                    g_anchor < 0 ? -1L : F - g_anchor, GetTickCount() - g_t0);
        } __except (EXCEPTION_EXECUTE_HANDLER) {}
        if (F % 60 == 0) fflush(g_csv);
        if (g_trace && F == g_trace_to + 1) fflush(g_trace);
        if (g_probe) {
            // LBO_PROBE: frei waehlbare Adressen je Frame nach probes.csv (Diagnose)
            int i;
            fprintf(g_probe, "%ld", F);
            for (i = 0; i < g_nprobe; i++) {
                int v = -1;
                __try {
                    DWORD a = g_probes[i].base;
                    if (g_probes[i].deref) a = *(DWORD *)a;
                    v = a ? *(int *)(a + g_probes[i].off) : -1;
                } __except (EXCEPTION_EXECUTE_HANDLER) {}
                fprintf(g_probe, ",%08x", v);
            }
            fputc('\n', g_probe);
            if (F % 60 == 0) fflush(g_probe);
        }
    }
    g_polls = 0;
    if (F % 10 == 0) {
        EnterCriticalSection(&g_cs);
        poll_live();
        LeaveCriticalSection(&g_cs);
    }
    if (g_shot_every > 0 && F % g_shot_every == 0) {
        char nm[64];
        snprintf(nm, sizeof nm, "shot_F%06ld.bmp", F);
        write_shot(nm);
    }
    if (g_max_frames > 0 && F >= g_max_frames) {
        logf_("LBO_MAXFRAMES erreicht, beende Spiel");
        if (g_csv) fclose(g_csv);
        g_csv = NULL;
        TerminateProcess(GetCurrentProcess(), 0);
    }
}

__declspec(naked) static void stub_frame_end(void)
{
    __asm {
        pushad
        pushfd
        call on_frame_end
        popfd
        popad
        sub esp, 40h        // ueberschriebener Anfang von 0x5528c0
        push ebx
        push edi
        push 005528C5h
        ret
    }
}

static int patch_jmp(DWORD at, void *to, const unsigned char *expect)
{
    DWORD old;
    unsigned char *p = (unsigned char *)at;
    if (memcmp(p, expect, 5)) return 0;
    VirtualProtect(p, 5, PAGE_EXECUTE_READWRITE, &old);
    p[0] = 0xE9;
    *(DWORD *)(p + 1) = (DWORD)to - (at + 5);
    VirtualProtect(p, 5, old, &old);
    FlushInstructionCache(GetCurrentProcess(), p, 5);
    return 1;
}

// --- Haken setzen, sobald die Huelle entschluesselt hat -----------------------------------------
static DWORD WINAPI installer(void *arg)
{
    static const unsigned char pro_frame[5] = { 0x83, 0xEC, 0x40, 0x53, 0x57 };
    void *mutex_fn = (void *)GetProcAddress(GetModuleHandleA("kernel32.dll"), "CreateMutexA");
    DWORD start = GetTickCount();
    void *file_fn = (void *)GetProcAddress(GetModuleHandleA("kernel32.dll"), "CreateFileA");
    static const unsigned char vdt_orig[6] = { 0xD9, 0x1D, 0xDC, 0xF0, 0x1C, 0x01 };
    int vdt_done = !g_videodt, rng_done = !g_trace_to, rrng_done = !g_render_rng, poll_done = !g_net && !g_nodinput, post_done = !g_loadsync;
    int mutex_done = !g_multi, frame_done = 0, xinput_done = 0, d3d_done = 0, file_done = 0, fmv_done = !g_no_movies;
    (void)arg;
    // Eng pollen: Die Importtabelle wird von der Huelle befuellt, der Mutex kurz danach angelegt
    while ((!mutex_done || !frame_done || !xinput_done || !d3d_done || !file_done || !fmv_done || !vdt_done || !rng_done || !rrng_done || !poll_done || !post_done) && GetTickCount() - start < 120000) {
        __try {
            if (!mutex_done && *(void **)IAT_CREATEMUTEXA == mutex_fn) {
                patch_iat(IAT_CREATEMUTEXA, (void *)hook_createmutex, (void **)&real_createmutex);
                mutex_done = 1;
            }
            if (!file_done && *(void **)IAT_CREATEFILEA == file_fn) {
                patch_iat(IAT_CREATEFILEA, (void *)hook_createfile, (void **)&real_createfile);
                patch_iat(IAT_CREATEFILEW, (void *)hook_createfilew, (void **)&real_createfilew);
                patch_iat(IAT_DELETEFILEA, (void *)hook_deletefile, (void **)&real_deletefile);
                patch_iat(IAT_MOVEFILEA, (void *)hook_movefile, (void **)&real_movefile);
                patch_iat(IAT_QPC, (void *)hook_qpc, (void **)&real_qpc);
                patch_iat(0x00C55388, (void *)hook_cocreate, (void **)&real_cocreate);          // CoCreateInstance
                patch_iat(0x00C551C0, (void *)hook_createdir, (void **)&real_createdir);        // CreateDirectoryA
                patch_iat(0x00C5526C, (void *)hook_shcreatedir, (void **)&real_shcreatedir);    // SHCreateDirectoryExA
                file_done = 1;
            }
            if (!xinput_done && (DWORD)*(void **)IAT_XINPUT_GETSTATE > 0x10000000) {
                patch_iat(IAT_XINPUT_GETSTATE, (void *)hook_getstate, (void **)&real_getstate);
                patch_iat(IAT_XINPUT_GETCAPS, (void *)hook_getcaps, (void **)&real_getcaps);
                xinput_done = 1;
            }
            if (!frame_done && !memcmp((void *)ADDR_FRAME_END, pro_frame, 5))
                frame_done = patch_jmp(ADDR_FRAME_END, (void *)stub_frame_end, pro_frame);
            if (!d3d_done) d3d_done = patch_call(D3DCREATE9_CALL, D3DCREATE9_THUNK, (void *)h_create9);
            if (!rng_done) {
                static const unsigned char pro_rng[6] = { 0x51, 0xA1, 0x5C, 0xE4, 0x1B, 0x01 };
                static const unsigned char pro_rand[5] = { 0xE8, 0xEF, 0x8A, 0x00, 0x00 };
                static const unsigned char pro_trig[5] = { 0x56, 0x8B, 0xF1, 0x57, 0x8B };
                if (!memcmp((void *)FN_RNG_A, pro_rng, 6) && !memcmp((void *)0x00BD1C9D, pro_rand, 5))
                    rng_done = (!g_watch_lcg || patch_jmp(0x0094CDF0, (void *)stub_trig, pro_trig)) &&
                               patch_jmp(FN_RNG_A, (void *)stub_rng_a, pro_rng) &&
                               patch_jmp(0x00BD1C9D, (void *)stub_crt_rand, pro_rand);
            }
            if (!post_done && g_iowait) patch_savecore();
            if (!post_done && g_loopgate) {
                static const unsigned char pro_loop[5] = { 0x68, 0x90, 0xBA, 0x38, 0x01 };   // push 0x138ba90
                if (!memcmp((void *)0x007EB979, pro_loop, 5) && !patch_jmp(0x007EB979, (void *)stub_loop_top, pro_loop))
                    logf_("Lade-Schleife: Haken bei 0x7eb979 fehlgeschlagen");
                if (!memcmp((void *)0x007EBF19, pro_loop, 5) && !patch_jmp(0x007EBF19, (void *)stub_flush_top, pro_loop))
                    logf_("Aufraeum-Schleife: Haken bei 0x7ebf19 fehlgeschlagen");
            }
            if (!post_done)
                post_done = patch_call(0x007EBB90, 0x00557850, (void *)stub_bgloop) &&
                            patch_call(0x007E6E03, 0x005528C0, (void *)stub_after_frame_end) &&
                            patch_call(0x007E6F98, 0x005528C0, (void *)stub_after_frame_end);
            if (!poll_done)
                poll_done = patch_call(0x0063BD59, 0x0063B6F0, (void *)stub_poll_device) &&
                            patch_call(0x0063D608, 0x0063B6F0, (void *)stub_poll_device) &&
                            patch_call(0x0063D8EE, 0x0063B6F0, (void *)stub_poll_device);
            if (!rrng_done)
                rrng_done = patch_call(0x005E0D52, 0x00BD1C9D, (void *)render_rand) &&
                            patch_call(0x005E0DB8, 0x00BD1C9D, (void *)render_rand) &&
                            patch_call(0x00598619, 0x0051A1D0, (void *)render_rng_a) &&
                            patch_abs32(0x0094CEC4, 0x011A1844, &g_rr_music_lcg) &&
                            patch_abs32(0x0094CED5, 0x011A1844, &g_rr_music_lcg) &&
                            patch_abs32(0x0094CC92, 0x011A1844, &g_rr_music_lcg2) &&
                            patch_abs32(0x0094CCA3, 0x011A1844, &g_rr_music_lcg2) &&
                            patch_abs32(0x0094D201, 0x011A1844, &g_rr_music_lcg3) &&
                            patch_abs32(0x0094D212, 0x011A1844, &g_rr_music_lcg3);
            if (!fmv_done) fmv_done = patch_call(FMV_FIND_CALL, FMV_FIND, (void *)no_movie_find);
            if (!vdt_done && !memcmp((void *)VIDEO_DT_STORE, vdt_orig, 6)) {
                // fstp [0x11cf0dc] -> fstp st(0) + 4 x nop
                static const unsigned char vdt_new[6] = { 0xDD, 0xD8, 0x90, 0x90, 0x90, 0x90 };
                DWORD old;
                VirtualProtect((void *)VIDEO_DT_STORE, 6, PAGE_EXECUTE_READWRITE, &old);
                memcpy((void *)VIDEO_DT_STORE, vdt_new, 6);
                VirtualProtect((void *)VIDEO_DT_STORE, 6, old, &old);
                FlushInstructionCache(GetCurrentProcess(), (void *)VIDEO_DT_STORE, 6);
                vdt_done = 1;
            }
        } __except (EXCEPTION_EXECUTE_HANDLER) {}
        if (mutex_done && file_done) Sleep(1); else SwitchToThread();
    }
    logf_("Haken: Frame-Ende %d, XInput %d, Mutex %d, D3D %d, Dateien %d (nach %lu ms)", frame_done, xinput_done, mutex_done, d3d_done, file_done,
          GetTickCount() - start);
    return 0;
}

static void init(void)
{
    char buf[MAX_PATH];
    if (!GetEnvironmentVariableA("LBO_DIR", g_dir, MAX_PATH)) GetTempPathA(MAX_PATH, g_dir);
    CreateDirectoryA(g_dir, NULL);
    g_log = open_out("lbo.log", "w");
    InitializeCriticalSection(&g_cs);
    InitializeCriticalSection(&g_va_cs);
    QueryPerformanceFrequency(&g_qpf);
    g_t0 = GetTickCount();
    logf_("lbo2.dll (LEGO Batman 2) geladen, Ausgabe nach %s", g_dir);
    if (GetEnvironmentVariableA("LBO_MODE", buf, sizeof buf) && !_stricmp(buf, "script")) g_mode = MODE_SCRIPT;
    if (GetEnvironmentVariableA("LBO_FIXEDDT", buf, sizeof buf)) g_fixed_dt = atoi(buf);
    if (GetEnvironmentVariableA("LBO_FPSCAP", buf, sizeof buf)) g_fps_cap = atoi(buf);
    if (GetEnvironmentVariableA("LBO_MAXFRAMES", buf, sizeof buf)) g_max_frames = atoi(buf);
    if (GetEnvironmentVariableA("LBO_SHOT_EVERY", buf, sizeof buf)) g_shot_every = atoi(buf);
    if (GetEnvironmentVariableA("LBO_MULTI", buf, sizeof buf)) g_multi = atoi(buf);
    if (GetEnvironmentVariableA("LBO_WINDOWED", buf, sizeof buf)) g_windowed = atoi(buf);
    if (GetEnvironmentVariableA("LBO_ALLOWSAVE", buf, sizeof buf)) g_allow_save = atoi(buf);
    if (GetEnvironmentVariableA("LBO_BGSYNC", buf, sizeof buf)) g_bgsync = atoi(buf);
    if (GetEnvironmentVariableA("LBO_NOMOVIES", buf, sizeof buf)) g_no_movies = atoi(buf);
    if (GetEnvironmentVariableA("LBO_LOADSYNC", buf, sizeof buf)) g_loadsync = atoi(buf);
    if (GetEnvironmentVariableA("LBO_VIDEODT", buf, sizeof buf)) g_videodt = atoi(buf);
    if (GetEnvironmentVariableA("LBO_VCLOCK", buf, sizeof buf)) g_vclock = atoi(buf);
    if (GetEnvironmentVariableA("LBO_WATCHLCG", buf, sizeof buf)) g_watch_lcg = atoi(buf);
    if (GetEnvironmentVariableA("LBO_WATCHADDR", buf, sizeof buf)) { g_watch_addr = strtoul(buf, NULL, 16); g_watch_lcg = 1; }
    if (GetEnvironmentVariableA("LBO_BREAKAT", buf, sizeof buf)) g_breakat = strtoul(buf, NULL, 16);
    if (GetEnvironmentVariableA("LBO_BREAKW", buf, sizeof buf)) { g_breakat = strtoul(buf, NULL, 16); g_break_write = 1; }
    if (GetEnvironmentVariableA("LBO_BREAKFROM", buf, sizeof buf)) g_break_from = atol(buf);
    if (GetEnvironmentVariableA("LBO_RENDERRNG", buf, sizeof buf)) g_render_rng = atoi(buf);
    if (GetEnvironmentVariableA("LBO_ANCHOR", buf, sizeof buf)) g_anchor_mode = atoi(buf);
    if (GetEnvironmentVariableA("LBO_IOWAIT", buf, sizeof buf)) g_iowait = atoi(buf);
    if (GetEnvironmentVariableA("LBO_SLOWSTART", buf, sizeof buf)) g_slow_start = atoi(buf);
    if (GetEnvironmentVariableA("LBO_LOADSLOW", buf, sizeof buf)) g_load_slow = atoi(buf);
    if (GetEnvironmentVariableA("LBO_GATE", buf, sizeof buf)) g_gate = atoi(buf);
    if (GetEnvironmentVariableA("LBO_LOOPGATE", buf, sizeof buf)) g_loopgate = atoi(buf);
    if (GetEnvironmentVariableA("LBO_NODINPUT", buf, sizeof buf)) g_nodinput = atoi(buf);
    if (GetEnvironmentVariableA("LBO_MEMDUMP", buf, sizeof buf) && sscanf(buf, "%ld,%ld", &g_memdump_from, &g_memdump_to) == 2)
        g_memdump = open_out("memdump.bin", "wb");
    if (GetEnvironmentVariableA("LBO_GATESET", buf, sizeof buf)) {
        char *t = strtok(buf, ",");
        g_gate_n = 0;
        while (t && g_gate_n < 19) { g_gate_idx[g_gate_n++] = atoi(t); t = strtok(NULL, ","); }
    }
    if (GetEnvironmentVariableA("LBO_JITTER", buf, sizeof buf)) { g_jitter = atoi(buf); g_jitter_state = GetCurrentProcessId(); }
    if (GetEnvironmentVariableA("LBO_AUDIOLOG", buf, sizeof buf)) g_audiolog = atoi(buf);
    if (GetEnvironmentVariableA("LBO_VAUDIO", buf, sizeof buf)) g_vaudio = atoi(buf);
    if (GetEnvironmentVariableA("LBO_PROBE", buf, sizeof buf)) {
        // Format: Adresse[+Versatz] oder *Zeiger[+Versatz], durch Kommas getrennt, hexadezimal
        char *t = strtok(buf, ",");
        while (t && g_nprobe < 32) {
            g_probes[g_nprobe].deref = *t == '*';
            if (*t == '*') t++;
            g_probes[g_nprobe].base = strtoul(t, &t, 16);
            g_probes[g_nprobe].off = *t == '+' ? strtoul(t + 1, NULL, 16) : 0;
            g_nprobe++;
            t = strtok(NULL, ",");
        }
        g_probe = open_out("probes.csv", "w");
    }
    if (GetEnvironmentVariableA("LBO_TRACERNG", buf, sizeof buf) && sscanf(buf, "%d,%d", &g_trace_from, &g_trace_to) == 2)
        g_trace = open_out("rngtrace.txt", "w");
    if (GetEnvironmentVariableA("LBO_RNGRESET", buf, sizeof buf)) g_rng_reset = atoi(buf);
    // Netzmodus: alle Schalter, die den Ablauf deterministisch machen, sind Pflicht (docs/batman2-phase0.md)
    g_net = net_init(logf_);
    if (g_net) {
        g_mode = MODE_SCRIPT;
        g_connected[0] = g_connected[1] = 1;
        g_fixed_dt = g_loadsync = g_videodt = g_vclock = g_render_rng = g_vaudio = g_anchor_mode = 1;
        g_rng_reset = 0;
        if (g_fps_cap <= 0) g_fps_cap = 60;
        if (!g_slow_start) g_slow_start = 900;
        if (!g_load_slow) g_load_slow = 40;
        input_init(GetProcAddress(LoadLibraryA("xinput1_3.dll"), (LPCSTR)2), logf_);   // Ordinal 2 = XInputGetState
    }
    if (g_mode == MODE_SCRIPT && GetEnvironmentVariableA("LBO_SCRIPT", buf, sizeof buf)) load_script(buf);
    if (GetEnvironmentVariableA("LBO_LIVE", g_live, sizeof g_live)) {
        FILE *f = fopen(g_live, "rb");
        if (f) { fseek(f, 0, SEEK_END); g_live_pos = ftell(f); fclose(f); }
        g_liveout = open_out("live_script.txt", "w");
    }
    logf_("Spielstand-Schutz %s", g_allow_save ? "AUS (LBO_ALLOWSAVE)" : "aktiv");
    g_csv = open_out("frames.csv", "w");
    if (g_csv) fprintf(g_csv, "F,game_frame,dt,rng_pm_seed,rng_lcg16,rng_pm_default,rng_a,rng_b,rng_c,crt_rand,polls,chars,char_hash,p1x,p1y,p1z,p2x,p2y,p2z,vid,fmv_a,fmv_b,fmv_c,fmv_d,fmv_frame,bg_pend,bg_job,ll_n,ll_loading,rel,ms\n");
    logf_("Modus %d, festes dt %d, fps %d, max Frames %d, Screenshot alle %d, Mehrfach %d", g_mode, g_fixed_dt,
          g_fps_cap, g_max_frames, g_shot_every, g_multi);
    logf_("NuBgProc synchron %d, ohne Videos %d, Zufalls-Reset bei Frame %d, Lade-Sync %d, Video-dt fest %d, virtuelle Uhr %d", g_bgsync,
          g_no_movies, g_rng_reset, g_loadsync, g_videodt, g_vclock);
    if (g_loopgate) g_loop_sem = CreateSemaphoreA(NULL, 0, 100000, NULL);
    CreateThread(NULL, 0, installer, NULL, 0, NULL);
}

BOOL WINAPI DllMain(HINSTANCE inst, DWORD reason, LPVOID reserved)
{
    (void)reserved;
    if (reason == DLL_PROCESS_ATTACH) {
        DisableThreadLibraryCalls(inst);
        init();
    }
    return TRUE;
}
