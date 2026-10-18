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
#include <xmmintrin.h>
#include <mmreg.h>
#include <dsound.h>
#include "net.h"
#include "input.h"

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
static int g_net;            // 1 = Lockstep ueber UDP aktiv (net.c)
static PAD g_net_pad[2];     // synchronisierte Eingaben des aktuellen Frames
static int g_fixed_dt;
static int g_max_frames;
static int g_stutter_every, g_stutter_ms;
static long g_fake_desync;
static int g_owncam, g_owncam_player, g_camlog;
static void cam_log(long F);
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
        // Im Netzmodus darf die Tastatur nicht direkt ins Spiel (sonst Desync). 0x523c30 liefert
        // fuer die Tastatur-Geraete 10-13 keine Eingabe, solange Slot+0x28 null ist.
        if (g_net)
            for (i = 10; i < 14; i++) SLOT(obj, i)[0x28] = 0;
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
    unsigned rngv[6];
    char ids[160];  // Figuren-IDs (+0x15b0) in Array-Reihenfolge, z. B. "12:7:33"
} SNAP;

static void snapshot(SNAP *s)
{
    int i;
    memset(s, 0, sizeof *s);
    s->chars = s->rng = 14695981039346656037ULL;
    __try {
        for (i = 0; i < (int)(sizeof rng_addrs / sizeof *rng_addrs); i++) {
            s->rng = fnv(s->rng, (const void *)rng_addrs[i], 4);
            s->rngv[i] = *(const unsigned *)rng_addrs[i];
        }
        s->nchars = G_CHAR_COUNT;
        if (s->nchars > 0 && s->nchars < 512 && G_CHAR_ARRAY) {
            const unsigned char *c = (const unsigned char *)G_CHAR_ARRAY;
            for (i = 0; i < s->nchars; i++, c += CHAR_STRIDE) {
                s->chars = fnv(s->chars, c + 0x5c, 12);
                s->chars = fnv(s->chars, c + 0x246, 2);
                if (strlen(s->ids) < sizeof s->ids - 8)
                    sprintf(s->ids + strlen(s->ids), i ? ":%d" : "%d", *(const short *)(c + 0x15b0));
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
// Automatisches Laden eines Spielstands (LBO_AUTOLOAD=<slot 0-5>, nur Host): Der Host-Pad
// drueckt die Menues durch. Zeitpunkte aus der Live-Aufzeichnung r12 (festes dt, keine Videos,
// 60 fps), danach uebernimmt der echte Controller. Weil die Eingaben ueber den Lockstep laufen,
// sehen beide Spiele exakt dieselben Menuebewegungen.
static long g_script_until = 0x7FFFFFFF;

static void add_press(int from, int len, unsigned short buttons)
{
    LINE l;
    memset(&l, 0, sizeof l);
    l.dom = 'F'; l.from = from; l.to = from + len - 1; l.pad = net_role(); l.p.wButtons = buttons;
    add_line(&l);
}

static void make_autoload_script(int slot)
{
    int i, f = 4400;
    if (slot < 0 || slot > 5) return;
    if (net_role() != 0) {
        // Gast: kein Skript, aber bis zum Ende des Ladens zaehlt nur der Host (siehe net_exchange)
        g_script_until = 6260;
        logf_("Host laedt Spielstand %d automatisch, Eingaben bis Frame %ld gesperrt", slot + 1, g_script_until);
        return;
    }
    add_press(3271, 30, 0x1000);              // Titel: A
    add_press(4101, 20, 0x1000);              // Hauptmenue: "Load Game" (vorausgewaehlt, wenn es Spielstaende gibt)
    for (i = 0; i < slot % 3; i++, f += 40)   // Slot-Raster 3x2: nach rechts ...
        add_press(f, 10, 0x0008);
    for (i = 0; i < slot / 3; i++, f += 40)   // ... und nach unten
        add_press(f, 10, 0x0002);
    add_press(4661, 20, 0x1000);              // Slot waehlen
    add_press(4801, 20, 0x0001);              // Bestaetigung: hoch auf "Yes"
    add_press(5301, 20, 0x1000);              // Yes
    add_press(6201, 20, 0x1000);              // "Autosave will now use Game N": OK
    g_script_until = 6260;
    logf_("Automatisches Laden von Spielstand %d (Menue-Skript bis Frame %ld)", slot + 1, g_script_until);
}

// Lockstep: lokale Eingabe abtasten, mit dem Partner austauschen (blockiert bis beide da sind)
static void net_exchange(long F)
{
    NETPAD local, both[2];
    PAD p;
    int i;
    memset(&local, 0, sizeof local);
    if (g_nlines > 0 && F <= g_script_until) {
        // Testbetrieb oder automatisches Laden: lokale Eingabe aus dem Skript
        EnterCriticalSection(&g_cs);
        scripted_state(net_role(), &p);
        LeaveCriticalSection(&g_cs);
        local.buttons = p.wButtons; local.lt = p.bLeftTrigger; local.rt = p.bRightTrigger;
        local.lx = p.sThumbLX; local.ly = p.sThumbLY; local.rx = p.sThumbRX; local.ry = p.sThumbRY;
        local.connected = 1;
    } else {
        input_read(&local);  // echter Controller oder Tastatur des Spielers
    }
    net_frame((int)F, &local, both);
    if (g_script_until != 0x7FFFFFFF && F <= g_script_until) {
        // Waehrend des automatischen Ladens zaehlen nur die Skript-Eingaben des Hosts. Beide Seiten
        // wenden diese Regel identisch an, daher bleibt der Lockstep synchron.
        memset(&both[1], 0, sizeof both[1]);
        both[1].connected = 1;
    }
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

// --- Hintergrund-Worker synchronisieren (LBO_SYNCBG) ----------------------------
// Das Spiel laedt per Worker-Thread (0x6e2cc0) asynchron. Wann ein Job fertig wird, haengt von
// Festplatte und Scheduler ab und verschiebt Spawns um Frames. Modus 1: am Frame-Ende warten, bis
// die Job-Schlange leer ist. Modus 2: zusaetzlich den Worker ueber das Event BGFreeze (0xb03bdc)
// waehrend des Frames anhalten, damit Jobs nie parallel zur Simulation laufen.
#define G_BG_QUEUE   (*(unsigned char *volatile *)0x00B03C68)
#define G_BG_CURRENT (*(volatile int *)0x00B058C0)
#define G_BG_FREEZE  (*(HANDLE volatile *)0x00B03BDC)
static int g_sync_bg;
static volatile LONG g_frozen_since;  // GetTickCount beim Einfrieren, 0 = nicht eingefroren
static long g_bg_waits, g_bg_timeouts, g_bg_unfreezes;

static int bg_busy(void)
{
    unsigned char *q = G_BG_QUEUE;
    return G_BG_CURRENT != 0 || (q && *(volatile int *)(q + 8) != 0);
}

static void sync_background(void)
{
    DWORD start;
    static int boosted;
    if (!g_sync_bg || !G_BG_QUEUE) return;
    // Der Worker laeuft mit THREAD_PRIORITY_BELOW_NORMAL und verhungert in einer Hintergrund-
    // instanz minutenlang. Da wir ohnehin auf ihn warten, darf er normale Prioritaet haben.
    if (!boosted && *(HANDLE *)0x00B058C4) {
        SetThreadPriority(*(HANDLE *)0x00B058C4, THREAD_PRIORITY_NORMAL);
        boosted = 1;
    }
    if (g_sync_bg >= 2 && G_BG_FREEZE) {
        InterlockedExchange(&g_frozen_since, 0);
        SetEvent(G_BG_FREEZE);
    }
    if (bg_busy()) {
        g_bg_waits++;
        start = GetTickCount();
        // Sleep(1) statt Sleep(0): Sleep(0) gibt nur an Threads gleicher Prioritaet ab, der Worker
        // laeuft aber mit THREAD_PRIORITY_BELOW_NORMAL und verhungert sonst unter Last. Ein Timeout
        // wuerde den Determinismus brechen (der Job liefe parallel weiter), daher nur eine Notbremse.
        while (bg_busy()) {
            // Wurde ein Job eingereiht, waehrend der Worker eingefroren war, kann sein Wecksignal
            // "BGWorkToDo" (Auto-Reset-Event 0xb03bd8) verloren sein. Ein zusaetzliches Wecken ist
            // harmlos: findet der Worker keine Arbeit, wartet er einfach weiter.
            if (*(HANDLE *)0x00B03BD8) SetEvent(*(HANDLE *)0x00B03BD8);
            if (GetTickCount() - start > 10000) {
                g_bg_timeouts++;
                logf_("Hintergrund-Worker seit 10 s beschaeftigt, warte weiter");
                start = GetTickCount();
            }
            Sleep(1);
        }
    }
}

// Worker einfrieren, bevor das Spiel weiterrechnet. Erst ganz am Ende von on_frame_end: Unsere
// eigenen Wartezeiten (Netz, Bildratenbremse) zaehlen so nicht fuer den Wachhund. Sonst taute eine
// Ping-Spitze ueber 500 ms den Worker auf, und er liefe parallel zur Simulation weiter.
static void bg_freeze(void)
{
    if (g_sync_bg >= 2 && G_BG_FREEZE && G_BG_QUEUE) {
        ResetEvent(G_BG_FREEZE);
        InterlockedExchange(&g_frozen_since, (LONG)GetTickCount() | 1);
    }
}

// --- Rechengenauigkeit (Netzmodus) -----------------------------------------------------
// Direct3D 9 stellt die x87-FPU beim Anlegen des Geraets auf 24 Bit. Ob das auf jedem PC gleich
// passiert (Treiber, Herstellervorgaben des Spiels, eingeklinkte Overlays), ist nicht sicher; eine
// andere Genauigkeit liesse die Physik nach einigen Sekunden auseinanderlaufen. Daher setzen beide
// Seiten vor jedem Weltupdate und am Frame-Ende denselben Zustand.
static int g_fpu_fixes;

static void enforce_fpu(const char *where)
{
    unsigned short cw, want = 0x007F;   // 24 Bit, runden zum Naechsten, alle Ausnahmen maskiert
    unsigned int csr = _mm_getcsr();
    __asm fnstcw cw
    if (cw != want || (csr & 0xFFC0) != 0x1F80) {
        if (g_fpu_fixes++ < 5)
            logf_("FPU korrigiert (%s): x87 %04x -> %04x, MXCSR %08x -> %08x", where, cw, want, csr, (csr & 0x3F) | 0x1F80);
        __asm fldcw want
        _mm_setcsr((csr & 0x3F) | 0x1F80);
    }
}

// --- Hinweis bei Desync ----------------------------------------------------------------
static int g_no_dialog;

static DWORD WINAPI desync_notice(void *arg)
{
    (void)arg;
    MessageBeep(MB_ICONEXCLAMATION);
    if (g_no_dialog) return 0;
    MessageBoxW(NULL,
        L"Eure Spiele laufen nicht mehr synchron.\n\n"
        L"Ab jetzt seht ihr unterschiedliche Spielst\u00e4nde, Eingaben des Mitspielers wirken falsch.\n\n"
        L"So geht es weiter: Der Einladende speichert (falls gew\u00fcnscht) und beendet das Spiel, "
        L"der andere beendet es ebenfalls. Dann in der App erneut \"Zusammen spielen\".\n\n"
        L"Die App schickt die Fehlerdaten automatisch an deinen Mitspieler.",
        L"LEGO Batman Online \u2013 nicht mehr synchron",
        MB_OK | MB_ICONWARNING | MB_TOPMOST | MB_SETFOREGROUND | MB_SYSTEMMODAL);
    return 0;
}

// Wachhund: Wartet der Hauptthread mitten im Frame selbst auf einen Job, kaeme er nie zum
// Frame-Ende. Dann tauen wir den Worker auf. Das ist nur eine Notbremse: In allen protokollierten
// Faellen hing der Hauptthread woanders (Geraetesuche von DirectInput beim Start und mitten im
// Spiel, 0x528xxx), und der dann parallel laufende Worker liess die Spiele auseinanderlaufen.
// Daher erst nach 10 s, was keine Geraetesuche und kein Ruckler erreicht.
#define BG_WATCHDOG_MS 10000
static HANDLE g_main_thread;

// Diagnose: Wo steht der Hauptthread, wenn der Wachhund auftauen muss? Schreibt EIP und die
// Ruecksprungadressen im Spielcode vom Stack ins Log.
static void log_main_stack(void)
{
    CONTEXT c;
    DWORD i, n = 0, words[256];
    char line[600];
    int len;
    if (!g_main_thread || SuspendThread(g_main_thread) == (DWORD)-1) return;
    c.ContextFlags = CONTEXT_CONTROL;
    if (GetThreadContext(g_main_thread, &c)) {
        len = snprintf(line, sizeof line, "Wachhund: Hauptthread EIP=%08lx Stack:", c.Eip);
        if (ReadProcessMemory(GetCurrentProcess(), (void *)c.Esp, words, sizeof words, NULL) ||
            ReadProcessMemory(GetCurrentProcess(), (void *)c.Esp, words, 512, NULL)) {
            for (i = 0; i < 256 && n < 14; i++)
                if (words[i] >= 0x00401000 && words[i] < 0x00850000) {
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

static DWORD WINAPI bg_watchdog(void *arg)
{
    (void)arg;
    for (;;) {
        LONG since = g_frozen_since;
        Sleep(50);
        if (since && g_frozen_since == since && GetTickCount() - (DWORD)since > BG_WATCHDOG_MS && G_BG_FREEZE) {
            log_main_stack();
            InterlockedExchange(&g_frozen_since, 0);
            SetEvent(G_BG_FREEZE);
            g_bg_unfreezes++;
        }
    }
}

static void __cdecl on_frame_end(void)
{
    SNAP s;
    long F = InterlockedIncrement(&g_F);
    if (!g_main_thread)
        DuplicateHandle(GetCurrentProcess(), GetCurrentThread(), GetCurrentProcess(), &g_main_thread,
                        THREAD_SUSPEND_RESUME | THREAD_GET_CONTEXT | THREAD_SET_CONTEXT | THREAD_QUERY_INFORMATION, FALSE, 0);
    sync_background();
    cap_frame_rate();
    if (F % 3000 == 0 && g_sync_bg)
        logf_("Hintergrund-Sync: %ld Wartephasen, %ld Timeouts, %ld Wachhund-Auftauungen", g_bg_waits, g_bg_timeouts, g_bg_unfreezes);
    if (F == 1 || F == 1500 || F % 3000 == 0) {
        unsigned short cw;
        unsigned int csr = _mm_getcsr();
        __asm fnstcw cw
        logf_("FPU: x87-Steuerwort %04x, MXCSR %08x", cw, csr);
    }
    if (g_fixed_dt) G_DT_OVERRIDE = 1.0f / 60.0f;
    if (g_mode == MODE_SCRIPT) fix_input_slots();
    if (g_net) net_exchange(F);
    // Testhilfe LBO_STUTTER="N MS": alle N Frames MS Millisekunden haengen (wie eine Netz-Spitze)
    if (g_stutter_every > 0 && F % g_stutter_every == 0) Sleep(g_stutter_ms);

    snapshot(&s);
    if (g_net && F % 60 == 0) {
        static int notified;
        long stalls, desyncs;
        // Testhilfe LBO_FAKEDESYNC=F: ab Frame F eine falsche Pruefsumme melden
        net_checksum((int)F, (s.chars ^ s.rng) ^ (g_fake_desync && F >= g_fake_desync ? 1 : 0));
        net_stats(&stalls, &desyncs);
        if (desyncs > 0 && !notified) {
            notified = 1;
            logf_("NICHT SYNCHRON: Hinweis an den Spieler");
            CreateThread(NULL, 0, desync_notice, NULL, 0, NULL);
        }
    }
    if (g_csv) {
        fprintf(g_csv, "%ld,%ld,%d,%.6f,%d,%016llx,%016llx,%d,%.4f,%.4f,%.4f,%d,%.4f,%.4f,%.4f,%d,%lu,%08x,%08x,%08x,%08x,%08x,%08x,%s\n",
                F, g_W, G_FRAMECOUNT, G_DT_GAME, s.nchars, s.chars, s.rng,
                s.pvalid[0], s.p[0][0], s.p[0][1], s.p[0][2],
                s.pvalid[1], s.p[1][0], s.p[1][1], s.p[1][2], g_polls_this_frame, GetTickCount() - g_t0,
                s.rngv[0], s.rngv[1], s.rngv[2], s.rngv[3], s.rngv[4], s.rngv[5], s.ids);
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
    if (g_camlog > 0 && F % g_camlog == 0) cam_log(F);
    if (g_net) enforce_fpu("Frame-Ende");
    bg_freeze();
}

static void __cdecl on_world_update(void)
{
    long W = InterlockedIncrement(&g_W);
    if (W == 1) logf_("erstes Weltupdate");
    if (g_net) enforce_fpu("Weltupdate");
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

// Sucht in der Importtabelle der EXE den Eintrag, der auf fn zeigt (fuer Importe ohne festes Wissen)
static void **find_iat_slot(void *fn)
{
    void **p;
    if (!fn) return NULL;
    for (p = (void **)0x02A17000; p < (void **)0x02A1A000; p++)
        if (*p == fn) return p;
    return NULL;
}

static void patch_iat(DWORD slot, void *fn, void **orig)
{
    DWORD old;
    VirtualProtect((void *)slot, 4, PAGE_READWRITE, &old);
    *orig = *(void **)slot;
    *(void **)slot = fn;
    VirtualProtect((void *)slot, 4, old, &old);
}

// --- Eigene Kamera pro Spieler (LBO_OWNCAM) ----------------------------------------------
// Die Spiellogik behaelt die gemeinsame Kamera (Objekt *0x95f624, Matrix bei +0xB8), damit der
// Lockstep synchron bleibt. Nur fuer das Zeichnen des Hauptbilds (Aufruf 0x4af2db der
// View-Funktion 0x6f1e80) wird die Matrix so verschoben, dass sie der eigenen Figur folgt.
#define G_GAME_CAMERA   (*(unsigned char *volatile *)0x0095F624)
#define ADDR_SET_VIEW   0x006F1E80

static int player_pos(int i, float out[3])
{
    const unsigned char *pc = (const unsigned char *)G_PLAYER_SLOTS[i];
    if (!pc) return 0;
    memcpy(out, pc + 0x5c, 12);
    return 1;
}

static float g_own_matrix[16];
static long g_own_hits;

// Wird am Anfang von 0x6f1e80 (Kamera-Weltmatrix an den Renderer) aufgerufen. Ist es die Matrix der
// Spielkamera, liefert sie eine zur eigenen Figur verschobene Kopie; alle anderen Matrizen
// (Schatten, Spiegelungen, Menues) bleiben unveraendert.
static const float *__cdecl view_filter(const float *m)
{
    float me[3], other[3];
    int i;
    __try {
        const float *cam;
        if (g_owncam == 9) return m;   // Diagnose: Haken ohne Wirkung
        if (!g_owncam || !G_GAME_CAMERA || G_CHAR_COUNT <= 0) return m;   // nur im Level, nie in Menues
        cam = (const float *)(G_GAME_CAMERA + 0xB8);
        if (memcmp(m, cam, 64) != 0) return m;
        if (!player_pos(g_owncam_player, me) || !player_pos(1 - g_owncam_player, other)) return m;
        memcpy(g_own_matrix, m, 64);
        // Die gemeinsame Kamera zielt auf die Mitte beider Spieler: um die halbe Differenz zur
        // eigenen Figur verschieben (Zeile 3 der Weltmatrix ist die Augenposition)
        for (i = 0; i < 3; i++) g_own_matrix[12 + i] += (me[i] - other[i]) * 0.5f;
        if (g_own_hits++ < 3) logf_("Kamera: Ersetzung Nr. %ld bei Frame %ld", g_own_hits, g_F);
        if (g_owncam == 8) return m;   // Diagnose: alles berechnen, aber nichts ersetzen
        return g_own_matrix;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return m;
    }
}

// Alle Register und Flags bleiben erhalten: Der Aufrufer kann sich (Optimierung ueber Funktionsgrenzen)
// darauf verlassen, dass z. B. ECX/EDX den Aufruf von 0x6f1e80 unveraendert ueberstehen.
static const float *g_view_arg;

__declspec(naked) static void stub_set_view(void)
{
    __asm {
        pushad
        pushfd
        mov eax, dword ptr [esp + 40]     // Argument: 36 Byte pushad/pushfd + Ruecksprungadresse
        push eax
        call view_filter
        add esp, 4
        mov g_view_arg, eax
        popfd
        popad
        push eax
        mov eax, g_view_arg
        mov dword ptr [esp + 8], eax      // Argument ersetzen (nach push eax um 4 verschoben)
        pop eax
        // ueberschriebener Anfang von 0x6f1e80: mov eax,[esp+4]; push esi; push edi
        mov eax, dword ptr [esp + 4]
        push esi
        push edi
        push 006F1E86h
        ret
    }
}


// Diagnose LBO_WATCHPOS=1: Hardware-Haltepunkt (DR0) auf die x-Position von Spieler 1. Jede
// schreibende Codeadresse wird gezaehlt und mit cam_log ausgegeben. So findet man z. B. den Code,
// der die Spieler im gemeinsamen Bild haelt.
static int g_watchpos;
static void *g_watch_addr;
static struct { DWORD eip; long n; } g_writers[64];
static int g_nwriters;

static LONG CALLBACK watch_handler(EXCEPTION_POINTERS *e)
{
    int i;
    DWORD eip;
    if (e->ExceptionRecord->ExceptionCode != EXCEPTION_SINGLE_STEP || !(e->ContextRecord->Dr6 & 1))
        return EXCEPTION_CONTINUE_SEARCH;
    eip = e->ContextRecord->Eip;   // Instruktion nach dem Schreibzugriff
    for (i = 0; i < g_nwriters && g_writers[i].eip != eip; i++) {}
    if (i == g_nwriters && g_nwriters < 64) g_writers[g_nwriters++].eip = eip;
    if (i < 64) g_writers[i].n++;
    e->ContextRecord->Dr6 = 0;
    return EXCEPTION_CONTINUE_EXECUTION;
}

static void watch_set(void *addr)
{
    CONTEXT c;
    if (!g_main_thread || addr == g_watch_addr) return;
    if (SuspendThread(g_main_thread) == (DWORD)-1) return;
    c.ContextFlags = CONTEXT_DEBUG_REGISTERS;
    if (GetThreadContext(g_main_thread, &c)) {
        c.Dr0 = (DWORD)addr;
        c.Dr7 = (c.Dr7 & ~0x000F0003u) | 1u | (1u << 16) | (3u << 18);  // L0, nur Schreiben, 4 Byte
        SetThreadContext(g_main_thread, &c);
        g_watch_addr = addr;
        logf_("Positionswaechter auf %p", addr);
    }
    ResumeThread(g_main_thread);
}

static DWORD WINAPI watch_thread(void *arg)
{
    (void)arg;
    AddVectoredExceptionHandler(1, watch_handler);
    for (;;) {
        Sleep(500);
        __try {
            const unsigned char *pc = (const unsigned char *)G_PLAYER_SLOTS[0];
            if (pc) watch_set((void *)(pc + 0x5c));
        } __except (EXCEPTION_EXECUTE_HANDLER) {}
    }
}

static void cam_log(long F)
{
    if (g_watchpos && g_nwriters) {
        char line[1024];
        int i, len = snprintf(line, sizeof line, "Schreiber P1.x:");
        for (i = 0; i < g_nwriters && len < 980; i++) len += snprintf(line + len, sizeof line - len, " %08lx:%ld", g_writers[i].eip, g_writers[i].n);
        logf_("%s", line);
    }
    float p0[3] = {0}, p1[3] = {0};
    const float *m, *r;
    unsigned f0 = 0, f1 = 0;
    __try {
        if (!G_GAME_CAMERA) return;
        m = (const float *)(G_GAME_CAMERA + 0xB8);
        r = *(const float **)0x00A96128;
        if (player_pos(0, p0)) f0 = *(const unsigned *)((const unsigned char *)G_PLAYER_SLOTS[0] + 0x1fc);
        if (player_pos(1, p1)) f1 = *(const unsigned *)((const unsigned char *)G_PLAYER_SLOTS[1] + 0x1fc);
        logf_("Kamera F%ld: M=[%.2f %.2f %.2f %.2f | %.2f %.2f %.2f %.2f | %.2f %.2f %.2f %.2f | %.2f %.2f %.2f %.2f] "
              "R3=[%.2f %.2f %.2f] P1=(%.2f %.2f %.2f) f%x P2=(%.2f %.2f %.2f) f%x",
              F, m[0], m[1], m[2], m[3], m[4], m[5], m[6], m[7], m[8], m[9], m[10], m[11], m[12], m[13], m[14], m[15],
              r ? r[12] : 0, r ? r[13] : 0, r ? r[14] : 0, p0[0], p0[1], p0[2], f0, p1[0], p1[1], p1[2], f1);
        if (g_owncam) logf_("Kamera: %ld Hauptbild-Matrizen ersetzt", g_own_hits);
    } __except (EXCEPTION_EXECUTE_HANDLER) {}
}

// --- Ton auch ohne Fensterfokus ---------------------------------------------------------
// DirectSound schaltet Puffer ohne DSBCAPS_GLOBALFOCUS stumm, solange das Spielfenster nicht im
// Vordergrund ist. Das Spiel wird aus der App gestartet und bekommt den Fokus oft nicht; Eingaben
// funktionieren trotzdem (die Mod liest die Controller selbst), der Ton fehlte aber.
typedef HRESULT (WINAPI *DSCreate8Fn)(const GUID *, void **, void *);
typedef HRESULT (__stdcall *CreateSoundBufferFn)(void *self, const DSBUFFERDESC *desc, void **buf, void *outer);
static DSCreate8Fn real_dscreate8;
static CreateSoundBufferFn real_createbuffer;
static long g_globalfocus_buffers;

static HRESULT __stdcall hook_createbuffer(void *self, const DSBUFFERDESC *desc, void **buf, void *outer)
{
    DSBUFFERDESC d;
    if (desc && !(desc->dwFlags & DSBCAPS_PRIMARYBUFFER) && desc->dwSize <= sizeof d) {
        memset(&d, 0, sizeof d);
        memcpy(&d, desc, desc->dwSize);
        d.dwFlags |= DSBCAPS_GLOBALFOCUS;
        if (g_globalfocus_buffers++ == 0) logf_("Ton: Puffer mit DSBCAPS_GLOBALFOCUS (spielt auch ohne Fensterfokus)");
        return real_createbuffer(self, &d, buf, outer);
    }
    return real_createbuffer(self, desc, buf, outer);
}

static HRESULT WINAPI hook_dscreate8(const GUID *dev, void **out, void *outer)
{
    HRESULT hr = real_dscreate8(dev, out, outer);
    if (SUCCEEDED(hr) && out && *out && !real_createbuffer) {
        void **vtbl = *(void ***)*out;   // IDirectSound8: 0-2 IUnknown, 3 CreateSoundBuffer
        DWORD old;
        VirtualProtect(&vtbl[3], 4, PAGE_READWRITE, &old);
        real_createbuffer = (CreateSoundBufferFn)vtbl[3];
        vtbl[3] = (void *)hook_createbuffer;
        VirtualProtect(&vtbl[3], 4, old, &old);
        logf_("Ton: DirectSound8 erzeugt, CreateSoundBuffer umgeleitet");
    }
    return hr;
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

// Gast im Netzmodus (LBO_SAVEDIR): Alles unter "...\Saved Games\" kommt aus der Kopie des Host-
// Spielstands, Lesen wie Schreiben. So laden beide Spiele exakt denselben Stand, und der eigene
// Spielstand des Gastes bleibt unberuehrt.
static char g_savedir[MAX_PATH];
// LBO_PCCONFIG: pcconfig.txt (Grafik-/Toneinstellungen) aus dieser Datei statt aus dem Spielordner
// des Benutzers. Im Netzmodus muessen beide Spiele dieselben spielrelevanten Einstellungen haben.
static char g_pcconfig[MAX_PATH];
typedef DWORD (WINAPI *GetFileAttributesAFn)(LPCSTR);
typedef HANDLE (WINAPI *FindFirstFileAFn)(LPCSTR, LPWIN32_FIND_DATAA);
static GetFileAttributesAFn real_getattr;
static FindFirstFileAFn real_findfirst;

static const char *map_guest(const char *path, char *buf)
{
    char low[MAX_PATH];
    const char *hit;
    size_t i, n;
    if (!path) return path;
    for (i = 0; path[i] && i < MAX_PATH - 1; i++) low[i] = (char)tolower((unsigned char)path[i]);
    low[i] = 0;
    n = strlen(low);
    if (g_pcconfig[0] && n >= 13 && !strcmp(low + n - 13, "\\pcconfig.txt")) {
        snprintf(buf, MAX_PATH, "%s", g_pcconfig);
        return buf;
    }
    if (!g_savedir[0]) return path;
    hit = strstr(low, "\\saved games");
    if (!hit) return path;
    snprintf(buf, MAX_PATH, "%s%s", g_savedir, path + (hit - low) + strlen("\\saved games"));
    return buf;
}

static DWORD WINAPI hook_getattr(LPCSTR path)
{
    char buf[MAX_PATH];
    return real_getattr(map_guest(path, buf));
}

static HANDLE WINAPI hook_findfirst(LPCSTR path, LPWIN32_FIND_DATAA fd)
{
    char buf[MAX_PATH];
    return real_findfirst(map_guest(path, buf), fd);
}

static HANDLE WINAPI hook_createfile(LPCSTR path, DWORD access, DWORD share, LPSECURITY_ATTRIBUTES sa,
                                     DWORD disp, DWORD flags, HANDLE tmpl)
{
    char gbuf[MAX_PATH];
    const char *mapped = map_guest(path, gbuf);
    if (mapped != path) return real_createfile(mapped, access, share, sa, disp, flags, tmpl);
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
    char gbuf[MAX_PATH];
    const char *mapped = map_guest(path, gbuf);
    if (mapped != path) return real_deletefile(mapped);
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

// Einzelinstanz-Sperre (0x527010): CreateMutexA("TTales-{...}") + ERROR_ALREADY_EXISTS beendet die
// zweite Instanz. Mit LBO_MULTI=1 bekommt der Mutex die PID als Suffix, damit zwei Spiele laufen.
#define IAT_CREATEMUTEXA 0x02A178F0
typedef HANDLE (WINAPI *CreateMutexAFn)(LPSECURITY_ATTRIBUTES, BOOL, LPCSTR);
static CreateMutexAFn real_createmutex;
static int g_multi;

static HANDLE WINAPI hook_createmutex(LPSECURITY_ATTRIBUTES sa, BOOL owner, LPCSTR name)
{
    if (g_multi && name && !strncmp(name, "TTales-", 7)) {
        char uniq[256];
        snprintf(uniq, sizeof uniq, "%s-lbo%lu", name, GetCurrentProcessId());
        logf_("Einzelinstanz-Mutex umbenannt: %s", uniq);
        return real_createmutex(sa, owner, uniq);
    }
    return real_createmutex(sa, owner, name);
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
    // Netzmodus (LBO_NET=host|join): Pads 0 und 1 kommen aus dem Lockstep-Austausch. Die lokale
    // Eingabe stammt vom echten Pad 0 oder, falls LBO_SCRIPT gesetzt ist, aus dem Skript.
    g_net = net_init(logf_);
    if (g_net) {
        g_mode = MODE_SCRIPT;
        g_connected[0] = g_connected[1] = 1;
    }
    if (GetEnvironmentVariableA("LBO_FIXEDDT", buf, sizeof buf)) g_fixed_dt = atoi(buf);
    if (GetEnvironmentVariableA("LBO_MAXFRAMES", buf, sizeof buf)) g_max_frames = atoi(buf);
    if (GetEnvironmentVariableA("LBO_STUTTER", buf, sizeof buf)) sscanf(buf, "%d %d", &g_stutter_every, &g_stutter_ms);
    if (GetEnvironmentVariableA("LBO_FAKEDESYNC", buf, sizeof buf)) g_fake_desync = atol(buf);
    if (GetEnvironmentVariableA("LBO_CAMLOG", buf, sizeof buf)) g_camlog = atoi(buf);
    if (GetEnvironmentVariableA("LBO_WATCHPOS", buf, sizeof buf) && (g_watchpos = atoi(buf)) != 0)
        CreateThread(NULL, 0, watch_thread, NULL, 0, NULL);
    if (GetEnvironmentVariableA("LBO_OWNCAM", buf, sizeof buf)) {
        g_owncam = atoi(buf);
        // Eigene Figur: im Netz die eigene Rolle (Host = Spieler 1), sonst LBO_OWNCAM_PLAYER
        g_owncam_player = g_net ? net_role() : (GetEnvironmentVariableA("LBO_OWNCAM_PLAYER", buf, sizeof buf) ? atoi(buf) : 0);
        if (g_owncam) {
            static const unsigned char pro_view[5] = { 0x8B, 0x44, 0x24, 0x04, 0x56 };
            logf_("Kamera: eigene Kamera fuer Spieler %d, Hook %d", g_owncam_player + 1,
                  patch_jmp(ADDR_SET_VIEW, (void *)stub_set_view, pro_view));
        }
    }
    if (GetEnvironmentVariableA("LBO_NODIALOG", buf, sizeof buf)) g_no_dialog = atoi(buf);
    if (GetEnvironmentVariableA("LBO_SHOT_EVERY", buf, sizeof buf)) g_shot_every = atoi(buf);
    if (g_mode == MODE_SCRIPT && GetEnvironmentVariableA("LBO_SCRIPT", buf, sizeof buf)) load_script(buf);
    if (g_net && GetEnvironmentVariableA("LBO_AUTOLOAD", buf, sizeof buf)) make_autoload_script(atoi(buf));
    if (g_mode == MODE_SCRIPT && GetEnvironmentVariableA("LBO_LIVE", g_live, sizeof g_live)) {
        FILE *f = fopen(g_live, "rb");
        if (f) { fseek(f, 0, SEEK_END); g_live_pos = ftell(f); fclose(f); }  // alte Befehle ignorieren
        g_liveout = open_out("live_script.txt", "w");
    }
    if (g_mode == MODE_RECORD) g_rec = open_out("recording.txt", "w");
    logf_("Modus %d, festes dt %d, max Frames %d, Screenshot alle %d", g_mode, g_fixed_dt, g_max_frames, g_shot_every);

    g_csv = open_out("frames.csv", "w");
    if (g_csv) fprintf(g_csv, "F,W,game_frame,dt,nchars,chars_hash,rng_hash,p1,p1x,p1y,p1z,p2,p2x,p2y,p2z,polls,ms,rng_ad3b6c,rng_95fd58,rng_95fd5c,rng_969730,rng_95dedc,rng_95e15c,char_ids\n");

    xi = GetModuleHandleA("xinput1_3.dll");
    logf_("IAT XInputGetState=%p (xinput1_3 Ordinal 2=%p), GetCapabilities=%p (Ordinal 4=%p)",
          *(void **)IAT_XINPUT_GETSTATE, xi ? GetProcAddress(xi, (LPCSTR)2) : NULL,
          *(void **)IAT_XINPUT_GETCAPS, xi ? GetProcAddress(xi, (LPCSTR)4) : NULL);
    patch_iat(IAT_XINPUT_GETSTATE, (void *)hook_getstate, (void **)&real_getstate);
    patch_iat(IAT_XINPUT_GETCAPS, (void *)hook_getcaps, (void **)&real_getcaps);
    if (GetEnvironmentVariableA("LBO_ALLOWSAVE", buf, sizeof buf)) g_allow_save = atoi(buf);
    {
        void **ds = find_iat_slot(GetProcAddress(GetModuleHandleA("dsound.dll"), (LPCSTR)11));  // DirectSoundCreate8
        if (ds) patch_iat((DWORD)ds, (void *)hook_dscreate8, (void **)&real_dscreate8);
        else logf_("Ton: DirectSoundCreate8 nicht in der Importtabelle gefunden");
    }
    patch_iat(IAT_CREATEFILEA, (void *)hook_createfile, (void **)&real_createfile);
    patch_iat(IAT_DELETEFILEA, (void *)hook_deletefile, (void **)&real_deletefile);
    logf_("Spielstand-Schutz %s", g_allow_save ? "AUS (LBO_ALLOWSAVE)" : "aktiv");
    GetEnvironmentVariableA("LBO_SAVEDIR", g_savedir, sizeof g_savedir);
    if (GetEnvironmentVariableA("LBO_PCCONFIG", g_pcconfig, sizeof g_pcconfig))
        logf_("Einstellungen (pcconfig.txt) aus %s", g_pcconfig);
    if (g_savedir[0] || g_pcconfig[0]) {
        void **slot;
        patch_iat(0x02A178F4, (void *)hook_getattr, (void **)&real_getattr);
        slot = find_iat_slot(GetProcAddress(GetModuleHandleA("kernel32.dll"), "FindFirstFileA"));
        if (slot) patch_iat((DWORD)slot, (void *)hook_findfirst, (void **)&real_findfirst);
        if (g_savedir[0])
            logf_("Spielstand-Ordner umgeleitet nach %s (FindFirstFileA %s)", g_savedir, slot ? "umgeleitet" : "nicht gefunden");
    }
    input_init((void *)real_getstate, logf_);
    if (GetEnvironmentVariableA("LBO_MULTI", buf, sizeof buf)) g_multi = atoi(buf);
    patch_iat(IAT_CREATEMUTEXA, (void *)hook_createmutex, (void **)&real_createmutex);
    if (GetEnvironmentVariableA("LBO_NOMOVIES", buf, sizeof buf)) g_no_movies = atoi(buf);
    if (GetEnvironmentVariableA("LBO_FPSCAP", buf, sizeof buf)) g_fps_cap = atoi(buf);
    if (GetEnvironmentVariableA("LBO_SYNCBG", buf, sizeof buf)) g_sync_bg = atoi(buf);
    if (g_sync_bg >= 2) CreateThread(NULL, 0, bg_watchdog, NULL, 0, NULL);
    logf_("Hintergrund-Sync Modus %d", g_sync_bg);
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
