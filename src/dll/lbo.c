// lbo.dll: Hook-DLL fuer LEGO Batman (Steam-Build 2025-07-04, ImageBase 0x400000, kein ASLR).
// Wird von lbo-launch.exe in das angehaltene Spiel geladen. Adressen siehe docs/phase0-befunde.md.
//
// Steuerung ueber Umgebungsvariablen:
//   LBO_DIR        Ausgabeordner (Pflicht), dort landen lbo.log, frames.csv, Screenshots, Aufnahmen
//   LBO_MODE       off | record | script        (Standard: off = nur loggen, echte Eingaben)
//   LBO_SCRIPT     Eingabeskript fuer LBO_MODE=script
//   LBO_LIVE       Befehlsdatei, die waehrend des Laufs angehaengt werden darf (nur LBO_MODE=script)
//   LBO_FIXEDDT    1 = dt fest auf 1/60 (Override 0xadf690)
//   LBO_MAXFRAMES  Spiel nach so vielen Frame-Enden beenden
//   LBO_SHOT_EVERY alle N Frame-Enden einen Screenshot (BMP) schreiben
//
// Skriptformat (eine Zeile pro Eintrag, # = Kommentar, spaetere Zeilen ueberschreiben fruehere):
//   connect <pad>
//   <F|W> <von> <bis|-1> <pad> <tasten-hex> <lt> <rt> <lx> <ly> <rx> <ry>
// F zaehlt Frame-Enden (0x6e0240), W zaehlt Weltupdates (0x4aee90).
// Live-Datei: "rel <dauer> <pad> <tasten-hex> <lt> <rt> <lx> <ly> <rx> <ry>" gilt ab dem naechsten Frame,
// "shot" schreibt sofort einen Screenshot, "quit" beendet das Spiel.
#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <mmsystem.h>

// --- Spieladressen ---------------------------------------------------------
#define ADDR_FRAME_END      0x006E0240  // sub esp,14h / fldz  (5 Bytes)
#define ADDR_WORLD_UPDATE   0x004AEE90  // sub esp,38h / push ebx / push ebp (5 Bytes)
#define ADDR_DEVICE_READ    0x005206C0  // sub esp,13Ch (6 Bytes)
#define IAT_XINPUT_GETSTATE 0x02A17C38
#define IAT_XINPUT_GETCAPS  0x02A17C3C
#define G_DT_OVERRIDE       (*(volatile float *)0x00ADF690)
#define G_DT_MEASURED       (*(volatile float *)0x00ADF688)
#define G_DT_GAME           (*(volatile float *)0x00A95FE0)
#define G_FRAMECOUNT        (*(volatile int *)0x00ADF68C)
#define G_CHAR_ARRAY        (*(volatile unsigned char **)0x00AB364C)
#define G_CHAR_COUNT        (*(volatile int *)0x00AB3648)
#define CHAR_STRIDE         0x1648
#define G_PLAYER_SLOTS      ((volatile unsigned char **)0x00AB3960)
#define G_P2_ACTIVE         (*(volatile int *)0x00AB3984)

static const DWORD rng_addrs[] = { 0xAD3B6C, 0x95FD58, 0x95FD5C, 0x969730, 0x95DEDC, 0x95E15C };

// --- XInput-Typen (ohne xinput.h) -------------------------------------------
typedef struct {
    WORD wButtons;
    BYTE bLeftTrigger, bRightTrigger;
    SHORT sThumbLX, sThumbLY, sThumbRX, sThumbRY;
} PAD;
typedef struct { DWORD dwPacketNumber; PAD Gamepad; } XSTATE;
typedef struct { BYTE Type, SubType; WORD Flags; PAD Gamepad; WORD vib[2]; } XCAPS;
typedef DWORD (WINAPI *GetStateFn)(DWORD, XSTATE *);
typedef DWORD (WINAPI *GetCapsFn)(DWORD, DWORD, XCAPS *);

// --- Zustand ------------------------------------------------------------------
enum { MODE_OFF, MODE_RECORD, MODE_SCRIPT };
typedef struct { char dom; int from, to, pad; PAD p; } LINE;

static int g_mode;
static int g_fixed_dt;
static int g_max_frames;
static int g_shot_every;
static char g_dir[MAX_PATH];
static FILE *g_log, *g_csv, *g_rec, *g_liveout;
static char g_live[MAX_PATH];
static long g_live_pos;

static volatile LONG g_F;  // Frame-Enden
static volatile LONG g_W;  // Weltupdates
static int g_connected[4];
static LINE *g_lines;
static int g_nlines, g_caplines;
static PAD g_last_rec[4];
static int g_last_rec_conn[4];
static int g_polls_this_frame;
static DWORD g_t0;  // Startzeit fuer die ms-Spalte im CSV

static GetStateFn real_getstate;
static GetCapsFn real_getcaps;
static CRITICAL_SECTION g_cs;

static void logf_(const char *fmt, ...)
{
    if (!g_log) return;
    va_list ap;
    va_start(ap, fmt);
    fprintf(g_log, "[F%ld W%ld] ", g_F, g_W);
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

// --- Skript -------------------------------------------------------------------
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
        if (pad >= 0 && pad < 4) g_connected[pad] = 1;
        return;
    }
    if ((s[0] == 'F' || s[0] == 'W') && sscanf(s + 1, "%d %d %n", &l.from, &l.to, &n) == 2 &&
        parse_pad(s + 1 + n, &l.pad, &l.p)) {
        l.dom = s[0];
        add_line(&l);
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
    logf_("Skript %s geladen: %d Zeilen, verbunden %d%d%d%d", path, g_nlines,
          g_connected[0], g_connected[1], g_connected[2], g_connected[3]);
}

static void scripted_state(int pad, PAD *out)
{
    int i;
    memset(out, 0, sizeof *out);
    for (i = 0; i < g_nlines; i++) {
        const LINE *l = &g_lines[i];
        long cur = l->dom == 'F' ? g_F : g_W;
        if (l->pad != pad) continue;
        if (l->dom == 'W' && g_W == 0) continue;
        if (cur >= l->from && (l->to < 0 || cur <= l->to)) *out = l->p;
    }
}

static void write_shot(const char *name);

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
        if (!len || buf[len - 1] != '\n') break;  // halbe Zeile, beim naechsten Mal
        g_live_pos += (long)len;
        while (len && (buf[len - 1] == '\n' || buf[len - 1] == '\r')) buf[--len] = 0;
        if (!strncmp(buf, "shot", 4)) {
            char nm[64];
            snprintf(nm, sizeof nm, "live_F%ld.bmp", g_F);
            write_shot(nm);
        } else if (!strncmp(buf, "peek ", 5)) {
            unsigned addr, n, k;
            char hex[3 * 256 + 1];
            if (sscanf(buf + 5, "%x %u", &addr, &n) == 2 && n <= 256) {
                hex[0] = 0;
                __try {
                    for (k = 0; k < n; k++) sprintf(hex + 3 * k, "%02x ", ((unsigned char *)addr)[k]);
                } __except (EXCEPTION_EXECUTE_HANDLER) { strcat(hex, "<Zugriffsfehler>"); }
                logf_("peek %08x: %s", addr, hex);
            }
        } else if (!strncmp(buf, "poke ", 5)) {
            unsigned addr, val;
            if (sscanf(buf + 5, "%x %x", &addr, &val) == 2) {
                __try { *(unsigned *)addr = val; logf_("poke %08x = %08x", addr, val); }
                __except (EXCEPTION_EXECUTE_HANDLER) { logf_("poke %08x: Zugriffsfehler", addr); }
            }
        } else if (!strncmp(buf, "clear", 5)) {
            logf_("Live: clear (%d Skriptzeilen verworfen)", g_nlines);
            g_nlines = 0;
        } else if (!strncmp(buf, "quit", 4)) {
            logf_("Live: quit");
            if (g_csv) fflush(g_csv);
            TerminateProcess(GetCurrentProcess(), 0);
        } else if (sscanf(buf, "rel %d %n", &dur, &n) == 1) {
            LINE l;
            if (!parse_pad(buf + 4 + (n - 4), &l.pad, &l.p)) { logf_("Live-Zeile kaputt: %s", buf); continue; }
            l.dom = 'F'; l.from = g_F + 1; l.to = g_F + dur;
            add_line(&l);
            g_connected[l.pad] = 1;
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

// --- XInput-Hooks -----------------------------------------------------------------
static DWORD WINAPI hook_getstate(DWORD idx, XSTATE *st)
{
    DWORD r;
    g_polls_this_frame++;
    if (g_mode == MODE_SCRIPT) {
        if (idx > 3 || !g_connected[idx]) return ERROR_DEVICE_NOT_CONNECTED;
        EnterCriticalSection(&g_cs);
        scripted_state((int)idx, &st->Gamepad);
        LeaveCriticalSection(&g_cs);
        st->dwPacketNumber = (DWORD)g_F;
        return ERROR_SUCCESS;
    }
    r = real_getstate(idx, st);
    if (g_mode == MODE_RECORD && idx < 4 && g_rec) {
        int conn = r == ERROR_SUCCESS;
        if (conn && !g_last_rec_conn[idx]) fprintf(g_rec, "connect %lu\n", idx);
        if (conn && (!g_last_rec_conn[idx] || memcmp(&g_last_rec[idx], &st->Gamepad, sizeof(PAD)))) {
            const PAD *p = &st->Gamepad;
            fprintf(g_rec, "F %ld -1 %lu %04x %d %d %d %d %d %d\n", g_F, idx, p->wButtons, p->bLeftTrigger,
                    p->bRightTrigger, p->sThumbLX, p->sThumbLY, p->sThumbRX, p->sThumbRY);
            g_last_rec[idx] = *p;
        }
        if (!conn && g_last_rec_conn[idx]) fprintf(g_rec, "F %ld -1 %lu 0000 0 0 0 0 0 0\n", g_F, idx);
        g_last_rec_conn[idx] = conn;
    }
    return r;
}

static DWORD WINAPI hook_getcaps(DWORD idx, DWORD flags, XCAPS *caps)
{
    if (g_mode == MODE_SCRIPT) {
        if (idx > 3 || !g_connected[idx]) return ERROR_DEVICE_NOT_CONNECTED;
        memset(caps, 0, sizeof *caps);
        caps->Type = 1;     // XINPUT_DEVTYPE_GAMEPAD
        caps->SubType = 1;  // XINPUT_DEVSUBTYPE_GAMEPAD
        caps->Gamepad.wButtons = 0xF3FF;
        caps->Gamepad.bLeftTrigger = caps->Gamepad.bRightTrigger = 0xFF;
        caps->Gamepad.sThumbLX = caps->Gamepad.sThumbLY = caps->Gamepad.sThumbRX = caps->Gamepad.sThumbRY = (SHORT)0xFFC0;
        return ERROR_SUCCESS;
    }
    return real_getcaps(idx, flags, caps);
}

// --- Virtuelle Pads beim Spiel anmelden ------------------------------------------
// Das Spiel erkennt XInput-Pads nur ueber die DirectInput-Geraeteliste (Callback 0x522fe0).
// Dort wird der Slot auf Typ 6 gesetzt und die Tastenbelegung geladen. Fuer virtuelle Pads
// ohne echtes Geraet bauen wir genau diese Schritte nach (0x5230a5..0x52310b).
#define G_INPUT_OBJ (*(unsigned char *volatile *)0x009CFAF4)
#define SLOT(obj, i) ((obj) + 0x4E34 + (i) * 0x17C)
typedef void (__fastcall *CapsToSlotFn)(void *self, void *edx, void *caps, void *slot);
typedef void *(__fastcall *FindCfgFn)(void *self, void *edx, int kind, int idx, int pref);
typedef void (__fastcall *ApplyCfgFn)(void *self, void *edx, void *cfg, int idx, int a, int b, int c);

static void fix_input_slots(void)
{
    unsigned char *obj = G_INPUT_OBJ;
    int i;
    if (!obj) return;
    __try {
        for (i = 0; i < 4; i++) {
            unsigned char *slot = SLOT(obj, i);
            XCAPS caps;
            void *cfg;
            if (!g_connected[i] || *(int *)slot == 6) continue;
            *(int *)slot = 6;
            slot[0x2d] = 0;
            slot[0x1c] = 0x40;
            memset(slot + 0x2e, 0, 0x3f);
            strcpy((char *)slot + 0x2e, "XBOX 360 For Windows (Controller)");
            // Produkt-GUID eines kabelgebundenen Xbox-360-Pads {028E045E-0000-0000-0000-504944564944}
            // ("PIDVID"), so wie sie DirectInput liefern wuerde. Die Belegungssuche haengt daran.
            {
                static const unsigned char guid[16] = { 0x5E, 0x04, 0x8E, 0x02, 0, 0, 0, 0,
                                                        0, 0, 'P', 'I', 'D', 'V', 'I', 'D' };
                memcpy(slot + 8, guid, 16);
            }
            hook_getcaps(i, 1, &caps);
            ((CapsToSlotFn)0x0051D9B0)(obj, 0, &caps, slot);
            cfg = ((FindCfgFn)0x0051D5F0)(obj, 0, 7, i, *(int *)(obj + 0x6604));
            if (!cfg) cfg = ((FindCfgFn)0x0051D5F0)(obj, 0, 7, i, -1);
            if (cfg) ((ApplyCfgFn)0x0051D800)(obj, 0, cfg, i, *(short *)((unsigned char *)cfg + 0x5e), 0, 0);
            logf_("virtueller Pad %d angemeldet (Belegung %p)", i, cfg);
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        logf_("fix_input_slots: Zugriffsfehler");
    }
}

// Echte DirectInput-Joysticks (Geraete 4-9, z. B. ein DualSense) sollen im Skriptmodus keine
// Eingaben liefern. Aufzaehlen lassen wir sie trotzdem, sonst sucht das Spiel jeden Frame neu
// nach Geraeten (langsam). Stattdessen fangen wir die Geraete-Lesefunktion 0x5206c0 ab
// (thiscall, (index, ausgabe), ret 8) und melden fuer 4-9 "nicht verbunden".
__declspec(naked) static void stub_device_read(void)
{
    __asm {
        cmp g_mode, MODE_SCRIPT
        jne orig
        mov eax, [esp + 4]
        cmp eax, 4
        jb orig
        cmp eax, 9
        ja orig
        push edi
        mov edi, [esp + 12]
        xor eax, eax
        mov ecx, 49
        rep stosd
        pop edi
        xor eax, eax
        ret 8
    orig:
        sub esp, 13Ch
        push 005206C6h
        ret
    }
}

// --- Pruefsumme -------------------------------------------------------------------
static unsigned long long fnv(unsigned long long h, const void *p, size_t n)
{
    const unsigned char *b = (const unsigned char *)p;
    while (n--) { h ^= *b++; h *= 1099511628211ULL; }
    return h;
}

typedef struct {
    int nchars;
    unsigned long long chars, rng;
    float p[2][3];
    int pvalid[2];
} SNAP;

static void snapshot(SNAP *s)
{
    int i;
    memset(s, 0, sizeof *s);
    s->chars = s->rng = 14695981039346656037ULL;
    __try {
        for (i = 0; i < (int)(sizeof rng_addrs / sizeof *rng_addrs); i++)
            s->rng = fnv(s->rng, (const void *)rng_addrs[i], 4);
        s->nchars = G_CHAR_COUNT;
        if (s->nchars > 0 && s->nchars < 512 && G_CHAR_ARRAY) {
            const unsigned char *c = (const unsigned char *)G_CHAR_ARRAY;
            for (i = 0; i < s->nchars; i++, c += CHAR_STRIDE) {
                s->chars = fnv(s->chars, c + 0x5c, 12);
                s->chars = fnv(s->chars, c + 0x246, 2);
            }
        }
        for (i = 0; i < 2; i++) {
            const unsigned char *pc = (const unsigned char *)G_PLAYER_SLOTS[i];
            if (pc) {
                memcpy(s->p[i], pc + 0x5c, 12);
                s->pvalid[i] = 1;
            }
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        s->nchars = -1;
    }
}

// --- Screenshot (BMP, Client-Bereich des Spielfensters) ---------------------------
static void write_shot(const char *name)
{
    HWND hwnd = FindWindowA("TTalesWindow", NULL);
    RECT rc;
    HDC src, mem;
    HBITMAP bmp;
    BITMAPINFOHEADER bi;
    BITMAPFILEHEADER bf;
    int w, h, stride;
    unsigned char *pix;
    FILE *f;
    if (!hwnd || !GetClientRect(hwnd, &rc)) { logf_("Screenshot: kein Fenster"); return; }
    w = rc.right; h = rc.bottom;
    if (w <= 0 || h <= 0) return;
    src = GetDC(hwnd);
    mem = CreateCompatibleDC(src);
    bmp = CreateCompatibleBitmap(src, w, h);
    SelectObject(mem, bmp);
    BitBlt(mem, 0, 0, w, h, src, 0, 0, SRCCOPY);
    stride = (w * 3 + 3) & ~3;
    pix = (unsigned char *)malloc(stride * h);
    memset(&bi, 0, sizeof bi);
    bi.biSize = sizeof bi; bi.biWidth = w; bi.biHeight = h; bi.biPlanes = 1; bi.biBitCount = 24;
    GetDIBits(mem, bmp, 0, h, pix, (BITMAPINFO *)&bi, DIB_RGB_COLORS);
    f = open_out(name, "wb");
    if (f) {
        memset(&bf, 0, sizeof bf);
        bf.bfType = 0x4D42;
        bf.bfOffBits = sizeof bf + sizeof bi;
        bf.bfSize = bf.bfOffBits + stride * h;
        fwrite(&bf, sizeof bf, 1, f);
        fwrite(&bi, sizeof bi, 1, f);
        fwrite(pix, stride * h, 1, f);
        fclose(f);
    }
    free(pix);
    DeleteObject(bmp);
    DeleteDC(mem);
    ReleaseDC(hwnd, src);
    logf_("Screenshot %s (%dx%d)", name, w, h);
}

// --- Frame-Hooks ------------------------------------------------------------------
// Bildratenbegrenzung (LBO_FPSCAP): mit festem dt laeuft die Spielzeit sonst so schnell wie die
// Grafikkarte. Wartet bis zum naechsten 1/fps-Takt (Sleep fuer den Grossteil, Rest aktiv).
static int g_fps_cap;
static LARGE_INTEGER g_qpf, g_next_frame;

static void cap_frame_rate(void)
{
    LARGE_INTEGER now;
    LONGLONG period;
    if (g_fps_cap <= 0) return;
    period = g_qpf.QuadPart / g_fps_cap;
    QueryPerformanceCounter(&now);
    if (!g_next_frame.QuadPart || now.QuadPart - g_next_frame.QuadPart > period * 4) {
        g_next_frame.QuadPart = now.QuadPart + period;  // Start oder nach langer Pause neu takten
        return;
    }
    while (now.QuadPart < g_next_frame.QuadPart) {
        LONGLONG left_ms = (g_next_frame.QuadPart - now.QuadPart) * 1000 / g_qpf.QuadPart;
        if (left_ms > 2) Sleep((DWORD)(left_ms - 1));
        QueryPerformanceCounter(&now);
    }
    g_next_frame.QuadPart += period;
}

static void __cdecl on_frame_end(void)
{
    SNAP s;
    long F = InterlockedIncrement(&g_F);
    cap_frame_rate();
    if (g_fixed_dt) G_DT_OVERRIDE = 1.0f / 60.0f;
    if (g_mode == MODE_SCRIPT) fix_input_slots();

    snapshot(&s);
    if (g_csv) {
        fprintf(g_csv, "%ld,%ld,%d,%.6f,%d,%016llx,%016llx,%d,%.4f,%.4f,%.4f,%d,%.4f,%.4f,%.4f,%d,%lu\n",
                F, g_W, G_FRAMECOUNT, G_DT_GAME, s.nchars, s.chars, s.rng,
                s.pvalid[0], s.p[0][0], s.p[0][1], s.p[0][2],
                s.pvalid[1], s.p[1][0], s.p[1][1], s.p[1][2], g_polls_this_frame, GetTickCount() - g_t0);
        if (F % 60 == 0) fflush(g_csv);
    }
    g_polls_this_frame = 0;

    if (g_mode == MODE_SCRIPT && F % 10 == 0) {
        EnterCriticalSection(&g_cs);
        poll_live();
        LeaveCriticalSection(&g_cs);
    }
    if (g_rec && F % 60 == 0) fflush(g_rec);
    if (g_shot_every > 0 && F % g_shot_every == 0) {
        char nm[64];
        snprintf(nm, sizeof nm, "shot_F%06ld.bmp", F);
        write_shot(nm);
    }
    if (g_max_frames > 0 && F >= g_max_frames) {
        logf_("LBO_MAXFRAMES erreicht, beende Spiel");
        if (g_csv) fclose(g_csv);
        if (g_rec) fclose(g_rec);
        g_csv = g_rec = NULL;
        TerminateProcess(GetCurrentProcess(), 0);
    }
}

static void __cdecl on_world_update(void)
{
    long W = InterlockedIncrement(&g_W);
    if (W == 1) logf_("erstes Weltupdate");
}

__declspec(naked) static void stub_frame_end(void)
{
    __asm {
        pushad
        pushfd
        call on_frame_end
        popfd
        popad
        sub esp, 14h
        fldz
        push 006E0245h
        ret
    }
}

__declspec(naked) static void stub_world_update(void)
{
    __asm {
        pushad
        pushfd
        call on_world_update
        popfd
        popad
        sub esp, 38h
        push ebx
        push ebp
        push 004AEE95h
        ret
    }
}

static int patch_jmp(DWORD at, void *to, const unsigned char *expect)
{
    DWORD old;
    unsigned char *p = (unsigned char *)at;
    if (memcmp(p, expect, 5)) {
        logf_("Hook 0x%08lx: unerwartete Bytes %02x %02x %02x %02x %02x, nicht gepatcht", at, p[0], p[1], p[2], p[3], p[4]);
        return 0;
    }
    VirtualProtect(p, 5, PAGE_EXECUTE_READWRITE, &old);
    p[0] = 0xE9;
    *(DWORD *)(p + 1) = (DWORD)to - (at + 5);
    VirtualProtect(p, 5, old, &old);
    FlushInstructionCache(GetCurrentProcess(), p, 5);
    return 1;
}

static void patch_iat(DWORD slot, void *fn, void **orig)
{
    DWORD old;
    VirtualProtect((void *)slot, 4, PAGE_READWRITE, &old);
    *orig = *(void **)slot;
    *(void **)slot = fn;
    VirtualProtect((void *)slot, 4, old, &old);
}

// Das Spiel ignoriert Pad-Eingaben und haelt in 0x6e0240 an, solange sein Fenster nicht aktiv ist
// (Flag 0x94d3a0). Fuer unbeaufsichtigte Skriptlaeufe halten wir das Flag dauerhaft auf 1.
#define G_WINDOW_ACTIVE (*(volatile int *)0x0094D3A0)
static DWORD WINAPI keep_focus_thread(void *arg)
{
    (void)arg;
    for (;;) {
        G_WINDOW_ACTIVE = 1;
        Sleep(2);
    }
}

// Schutz der echten Spielstaende: Schreibzugriffe auf SaveGame*/pcconfig.txt landen im Laufordner.
// Lesen bleibt unveraendert, damit "Load Game" den echten Spielstand laden kann.
#define IAT_CREATEFILEA 0x02A1796C
#define IAT_DELETEFILEA 0x02A17990
typedef HANDLE (WINAPI *CreateFileAFn)(LPCSTR, DWORD, DWORD, LPSECURITY_ATTRIBUTES, DWORD, DWORD, HANDLE);
typedef BOOL (WINAPI *DeleteFileAFn)(LPCSTR);
static CreateFileAFn real_createfile;
static DeleteFileAFn real_deletefile;
static int g_allow_save;

static int is_protected(const char *path)
{
    char low[MAX_PATH];
    size_t i;
    if (!path || g_allow_save) return 0;
    for (i = 0; path[i] && i < MAX_PATH - 1; i++) low[i] = (char)tolower((unsigned char)path[i]);
    low[i] = 0;
    return strstr(low, "savedgame") || strstr(low, "pcconfig.txt");
}

static HANDLE WINAPI hook_createfile(LPCSTR path, DWORD access, DWORD share, LPSECURITY_ATTRIBUTES sa,
                                     DWORD disp, DWORD flags, HANDLE tmpl)
{
    if ((access & (GENERIC_WRITE | GENERIC_ALL)) && is_protected(path)) {
        char red[MAX_PATH];
        const char *base = strrchr(path, '\\');
        snprintf(red, sizeof red, "%s\\umgeleitet_%s", g_dir, base ? base + 1 : path);
        logf_("Schreibzugriff umgeleitet: %s -> %s", path, red);
        return real_createfile(red, access, share, sa, disp, flags, tmpl);
    }
    return real_createfile(path, access, share, sa, disp, flags, tmpl);
}

static BOOL WINAPI hook_deletefile(LPCSTR path)
{
    if (is_protected(path)) {
        logf_("Loeschen verhindert: %s", path);
        return TRUE;
    }
    return real_deletefile(path);
}

// Bink-Videos (Intro, Attract-Demo, Zwischensequenzen) laufen ausserhalb von 0x6e0240 und lassen
// sich per Pad nicht abbrechen. Mit LBO_NOMOVIES=1 tut BinkOpen so, als fehle die Datei.
#define IAT_BINKOPEN 0x02A17CA8
typedef void *(WINAPI *BinkOpenFn)(const char *name, unsigned flags);
static BinkOpenFn real_binkopen;
static int g_no_movies;

static void *WINAPI hook_binkopen(const char *name, unsigned flags)
{
    logf_("BinkOpen %s%s", name ? name : "(null)", g_no_movies ? " -> uebersprungen" : "");
    if (g_no_movies) return NULL;
    return real_binkopen(name, flags);
}

static void init(void)
{
    char buf[MAX_PATH];
    HMODULE xi;
    static const unsigned char pro_frame[5] = { 0x83, 0xEC, 0x14, 0xD9, 0xEE };
    static const unsigned char pro_world[5] = { 0x83, 0xEC, 0x38, 0x53, 0x55 };
    static const unsigned char pro_devread[5] = { 0x81, 0xEC, 0x3C, 0x01, 0x00 };

    InitializeCriticalSection(&g_cs);
    g_t0 = GetTickCount();
    if (!GetEnvironmentVariableA("LBO_DIR", g_dir, MAX_PATH)) GetTempPathA(MAX_PATH, g_dir);
    CreateDirectoryA(g_dir, NULL);
    g_log = open_out("lbo.log", "w");
    logf_("lbo.dll geladen, Ausgabe nach %s", g_dir);

    if (GetEnvironmentVariableA("LBO_MODE", buf, sizeof buf)) {
        if (!_stricmp(buf, "record")) g_mode = MODE_RECORD;
        else if (!_stricmp(buf, "script")) g_mode = MODE_SCRIPT;
    }
    if (GetEnvironmentVariableA("LBO_FIXEDDT", buf, sizeof buf)) g_fixed_dt = atoi(buf);
    if (GetEnvironmentVariableA("LBO_MAXFRAMES", buf, sizeof buf)) g_max_frames = atoi(buf);
    if (GetEnvironmentVariableA("LBO_SHOT_EVERY", buf, sizeof buf)) g_shot_every = atoi(buf);
    if (g_mode == MODE_SCRIPT && GetEnvironmentVariableA("LBO_SCRIPT", buf, sizeof buf)) load_script(buf);
    if (g_mode == MODE_SCRIPT && GetEnvironmentVariableA("LBO_LIVE", g_live, sizeof g_live)) {
        FILE *f = fopen(g_live, "rb");
        if (f) { fseek(f, 0, SEEK_END); g_live_pos = ftell(f); fclose(f); }  // alte Befehle ignorieren
        g_liveout = open_out("live_script.txt", "w");
    }
    if (g_mode == MODE_RECORD) g_rec = open_out("recording.txt", "w");
    logf_("Modus %d, festes dt %d, max Frames %d, Screenshot alle %d", g_mode, g_fixed_dt, g_max_frames, g_shot_every);

    g_csv = open_out("frames.csv", "w");
    if (g_csv) fprintf(g_csv, "F,W,game_frame,dt,nchars,chars_hash,rng_hash,p1,p1x,p1y,p1z,p2,p2x,p2y,p2z,polls,ms\n");

    xi = GetModuleHandleA("xinput1_3.dll");
    logf_("IAT XInputGetState=%p (xinput1_3 Ordinal 2=%p), GetCapabilities=%p (Ordinal 4=%p)",
          *(void **)IAT_XINPUT_GETSTATE, xi ? GetProcAddress(xi, (LPCSTR)2) : NULL,
          *(void **)IAT_XINPUT_GETCAPS, xi ? GetProcAddress(xi, (LPCSTR)4) : NULL);
    patch_iat(IAT_XINPUT_GETSTATE, (void *)hook_getstate, (void **)&real_getstate);
    patch_iat(IAT_XINPUT_GETCAPS, (void *)hook_getcaps, (void **)&real_getcaps);
    if (GetEnvironmentVariableA("LBO_ALLOWSAVE", buf, sizeof buf)) g_allow_save = atoi(buf);
    patch_iat(IAT_CREATEFILEA, (void *)hook_createfile, (void **)&real_createfile);
    patch_iat(IAT_DELETEFILEA, (void *)hook_deletefile, (void **)&real_deletefile);
    logf_("Spielstand-Schutz %s", g_allow_save ? "AUS (LBO_ALLOWSAVE)" : "aktiv");
    if (GetEnvironmentVariableA("LBO_NOMOVIES", buf, sizeof buf)) g_no_movies = atoi(buf);
    if (GetEnvironmentVariableA("LBO_FPSCAP", buf, sizeof buf)) g_fps_cap = atoi(buf);
    QueryPerformanceFrequency(&g_qpf);
    timeBeginPeriod(1);
    patch_iat(IAT_BINKOPEN, (void *)hook_binkopen, (void **)&real_binkopen);
    if (g_mode == MODE_SCRIPT) CreateThread(NULL, 0, keep_focus_thread, NULL, 0, NULL);

    logf_("Hook Frame-Ende: %d", patch_jmp(ADDR_FRAME_END, (void *)stub_frame_end, pro_frame));
    logf_("Hook Weltupdate: %d", patch_jmp(ADDR_WORLD_UPDATE, (void *)stub_world_update, pro_world));
    logf_("Hook Geraete-Lesen: %d", patch_jmp(ADDR_DEVICE_READ, (void *)stub_device_read, pro_devread));
}

BOOL WINAPI DllMain(HINSTANCE inst, DWORD reason, LPVOID reserved)
{
    if (reason == DLL_PROCESS_ATTACH) {
        DisableThreadLibraryCalls(inst);
        init();
    } else if (reason == DLL_PROCESS_DETACH) {
        if (g_csv) fclose(g_csv);
        if (g_rec) fclose(g_rec);
        if (g_log) { logf_("entladen"); fclose(g_log); }
    }
    return TRUE;
}
