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
#define LW_MAX (1 << 20)
static struct { long f; DWORD eip, tid; } g_lw[LW_MAX];
static volatile LONG g_nlw;
static FILE *g_trace;
static int g_bgsync, g_devchange, g_rng_reset, g_no_movies;
static int g_mode, g_fixed_dt, g_fps_cap, g_max_frames, g_shot_every, g_multi;
static char g_dir[MAX_PATH], g_live[MAX_PATH];
static FILE *g_log, *g_csv, *g_liveout;
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
    if (s[0] == 'F' && sscanf(s + 1, "%d %d %n", &l.from, &l.to, &n) == 2 && parse_pad(s + 1 + n, &l.pad, &l.p)) {
        l.dom = 'F';
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

static void scripted_state(int pad, PAD *out)
{
    int i;
    memset(out, 0, sizeof *out);
    for (i = 0; i < g_nlines; i++) {
        const LINE *l = &g_lines[i];
        if (l->pad == pad && g_F >= l->from && (l->to < 0 || g_F <= l->to)) *out = l->p;
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
static DWORD WINAPI hook_getstate(DWORD idx, XSTATE *st)
{
    g_polls++;
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

static HANDLE WINAPI hook_createfile(LPCSTR path, DWORD access, DWORD share, LPSECURITY_ATTRIBUTES sa,
                                     DWORD disp, DWORD flags, HANDLE tmpl)
{
    if ((access & (GENERIC_WRITE | GENERIC_ALL)) && is_protected(path)) {
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

// --- Frame-Ende ------------------------------------------------------------------------------
static LARGE_INTEGER g_qpf, g_next;

static void cap_frame_rate(void)
{
    LARGE_INTEGER now;
    LONGLONG period;
    if (g_fps_cap <= 0) return;
    period = g_qpf.QuadPart / g_fps_cap;
    QueryPerformanceCounter(&now);
    if (!g_next.QuadPart || now.QuadPart - g_next.QuadPart > period * 4) { g_next.QuadPart = now.QuadPart + period; return; }
    while (now.QuadPart < g_next.QuadPart) {
        LONGLONG left = (g_next.QuadPart - now.QuadPart) * 1000 / g_qpf.QuadPart;
        if (left > 2) Sleep((DWORD)(left - 1));
        QueryPerformanceCounter(&now);
    }
    g_next.QuadPart += period;
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
        EnterCriticalSection(LOADLIST_CS);
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
static void wait_loads(void)
{
    DWORD t0 = GetTickCount();
    int n, st, waited = 0;
    while ((n = loads_in_flight(&st)) > 0) {
        if (GetTickCount() - t0 > 30000) {   // erstes Laden beim Start dauert mehrere Sekunden
            g_load_timeouts++;
            logf_("Laden: Zeitgrenze, %d Eintraege offen (erster Zustand %d, gewuenscht %d)", n, st / 2, st & 1);
            break;
        }
        waited = 1;
        Sleep(0);
    }
    if (waited) g_load_waits++;
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
                    if (GetThreadContext(t, &c) && c.Dr0 != 0x011A1844) {
                        c.Dr0 = 0x011A1844;
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
                        THREAD_SUSPEND_RESUME | THREAD_GET_CONTEXT | THREAD_QUERY_INFORMATION, FALSE, 0);
        CreateThread(NULL, 0, stall_watch, NULL, 0, NULL);
        g_main_tid = GetCurrentThreadId();
        if (g_watch_lcg) CreateThread(NULL, 0, lcg_watch_set, NULL, 0, NULL);
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
    if (g_loadsync) wait_loads();
    if (F % 3000 == 0 && g_loadsync) logf_("Laden: %ld Wartephasen, %ld Zeitgrenzen", g_load_waits, g_load_timeouts);
    if (F % 3000 == 0 && g_bgsync == 2) logf_("NuBgProc: %ld Wartephasen, %ld Zeitgrenzen", g_bg_waits, g_bg_timeouts);
    if (g_bgsync == 1 && G_BGPROC && !G_BGPROC[BGPROC_SYNC]) {
        G_BGPROC[BGPROC_SYNC] = 1;
        logf_("NuBgProc synchron (Warteschlange %p)", (void *)G_BGPROC);
    }
    if (g_devchange) { g_devchange = 0; notify_devchange(); }
    if (g_rng_reset && F == g_rng_reset) reset_rng();
    if (g_csv) {
        char_snapshot(&cs);
        __try {
            fprintf(g_csv, "%ld,%d,%.6f,%08x,%08x,%08x,%08x,%08x,%08x,%08x,%d,%d,%08x,%.4f,%.4f,%.4f,%.4f,%.4f,%.4f,%lu\n", F, G_FRAMECOUNT,
                    G_DT, G_RNG_PM_SEED, G_RNG_LCG16, G_RNG_PM_DEFAULT, G_RNG_A, G_RNG_B, G_RNG_C, crt_rand_state(), g_polls, cs.n, cs.hash,
                    cs.p1[0], cs.p1[1], cs.p1[2], cs.p2[0], cs.p2[1], cs.p2[2], GetTickCount() - g_t0);
        } __except (EXCEPTION_EXECUTE_HANDLER) {}
        if (F % 60 == 0) fflush(g_csv);
        if (g_trace && F == g_trace_to + 1) fflush(g_trace);
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
    int vdt_done = !g_videodt, rng_done = !g_trace_to, rrng_done = !g_render_rng;
    int mutex_done = !g_multi, frame_done = 0, xinput_done = 0, d3d_done = 0, file_done = 0, fmv_done = !g_no_movies;
    (void)arg;
    // Eng pollen: Die Importtabelle wird von der Huelle befuellt, der Mutex kurz danach angelegt
    while ((!mutex_done || !frame_done || !xinput_done || !d3d_done || !file_done || !fmv_done || !vdt_done || !rng_done || !rrng_done) && GetTickCount() - start < 120000) {
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
    if (GetEnvironmentVariableA("LBO_RENDERRNG", buf, sizeof buf)) g_render_rng = atoi(buf);
    if (GetEnvironmentVariableA("LBO_TRACERNG", buf, sizeof buf) && sscanf(buf, "%d,%d", &g_trace_from, &g_trace_to) == 2)
        g_trace = open_out("rngtrace.txt", "w");
    if (GetEnvironmentVariableA("LBO_RNGRESET", buf, sizeof buf)) g_rng_reset = atoi(buf);
    if (g_mode == MODE_SCRIPT && GetEnvironmentVariableA("LBO_SCRIPT", buf, sizeof buf)) load_script(buf);
    if (GetEnvironmentVariableA("LBO_LIVE", g_live, sizeof g_live)) {
        FILE *f = fopen(g_live, "rb");
        if (f) { fseek(f, 0, SEEK_END); g_live_pos = ftell(f); fclose(f); }
        g_liveout = open_out("live_script.txt", "w");
    }
    logf_("Spielstand-Schutz %s", g_allow_save ? "AUS (LBO_ALLOWSAVE)" : "aktiv");
    g_csv = open_out("frames.csv", "w");
    if (g_csv) fprintf(g_csv, "F,game_frame,dt,rng_pm_seed,rng_lcg16,rng_pm_default,rng_a,rng_b,rng_c,crt_rand,polls,chars,char_hash,p1x,p1y,p1z,p2x,p2y,p2z,ms\n");
    logf_("Modus %d, festes dt %d, fps %d, max Frames %d, Screenshot alle %d, Mehrfach %d", g_mode, g_fixed_dt,
          g_fps_cap, g_max_frames, g_shot_every, g_multi);
    logf_("NuBgProc synchron %d, ohne Videos %d, Zufalls-Reset bei Frame %d, Lade-Sync %d, Video-dt fest %d, virtuelle Uhr %d", g_bgsync,
          g_no_movies, g_rng_reset, g_loadsync, g_videodt, g_vclock);
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
