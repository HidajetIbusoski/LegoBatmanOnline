// input.c: liest im Netzmodus den lokalen Controller des Spielers, unabhaengig vom Spiel.
//
// Reihenfolge: 1. Xbox-Controller (XInput, erster verbundener)
//              2. DirectInput-Gamepad (DualSense/DualShock 4 mit Sony-Belegung, sonst Xbox-artig)
//              3. Tastatur, nur wenn das Spielfenster im Vordergrund ist
// Ergebnis ist immer ein Xbox-Pad-Zustand, den beide Spiele identisch bekommen.
#define DIRECTINPUT_VERSION 0x0800
#include <windows.h>
#include <dinput.h>
#include <stdio.h>
#include <string.h>
#include "input.h"

typedef DWORD (WINAPI *XGetStateFn)(DWORD, void *);
typedef HRESULT (WINAPI *DI8CreateFn)(HINSTANCE, DWORD, REFIID, LPVOID *, LPUNKNOWN);

static XGetStateFn s_xget;
static void (*s_log)(const char *fmt, ...);
static IDirectInput8A *s_di;
static IDirectInputDevice8A *s_dev;
static int s_sony;
static DWORD s_next_scan;
static char s_name[128];
static int s_last_source = -1;

#define XB_UP 0x0001
#define XB_DOWN 0x0002
#define XB_LEFT 0x0004
#define XB_RIGHT 0x0008
#define XB_START 0x0010
#define XB_BACK 0x0020
#define XB_LS 0x0040
#define XB_RS 0x0080
#define XB_LB 0x0100
#define XB_RB 0x0200
#define XB_A 0x1000
#define XB_B 0x2000
#define XB_X 0x4000
#define XB_Y 0x8000

void input_init(void *real_xinput_getstate, void (*logger)(const char *fmt, ...))
{
    s_xget = (XGetStateFn)real_xinput_getstate;
    s_log = logger;
}

static BOOL CALLBACK pick_device(const DIDEVICEINSTANCEA *inst, void *ref)
{
    DIDEVICEINSTANCEA *best = (DIDEVICEINSTANCEA *)ref;
    int is_sony = LOWORD(inst->guidProduct.Data1) == 0x054C;
    int best_sony = best->dwSize && LOWORD(best->guidProduct.Data1) == 0x054C;
    // XInput-Geraete erscheinen auch hier; die liest der XInput-Pfad, daher ueberspringen
    if (strstr(inst->tszProductName, "XBOX") || strstr(inst->tszProductName, "Xbox")) return DIENUM_CONTINUE;
    if (!best->dwSize || (is_sony && !best_sony)) *best = *inst;
    return DIENUM_CONTINUE;
}

static void scan_dinput(void)
{
    DIDEVICEINSTANCEA best;
    HWND hwnd;
    if (!s_di) {
        DI8CreateFn create = (DI8CreateFn)GetProcAddress(LoadLibraryA("dinput8.dll"), "DirectInput8Create");
        if (!create || FAILED(create(GetModuleHandleA(NULL), DIRECTINPUT_VERSION, &IID_IDirectInput8A, (void **)&s_di, NULL)))
            return;
    }
    memset(&best, 0, sizeof best);
    s_di->lpVtbl->EnumDevices(s_di, DI8DEVCLASS_GAMECTRL, pick_device, &best, DIEDFL_ATTACHEDONLY);
    if (!best.dwSize) return;
    if (FAILED(s_di->lpVtbl->CreateDevice(s_di, &best.guidInstance, &s_dev, NULL))) { s_dev = NULL; return; }
    s_dev->lpVtbl->SetDataFormat(s_dev, &c_dfDIJoystick2);
    hwnd = FindWindowA("TTalesWindow", NULL);
    if (hwnd) s_dev->lpVtbl->SetCooperativeLevel(s_dev, hwnd, DISCL_BACKGROUND | DISCL_NONEXCLUSIVE);
    s_dev->lpVtbl->Acquire(s_dev);
    s_sony = LOWORD(best.guidProduct.Data1) == 0x054C;
    snprintf(s_name, sizeof s_name, "%s", best.tszProductName);
    if (s_log) s_log("Eingabe: DirectInput-Controller \"%s\" (%s-Belegung)", s_name, s_sony ? "Sony" : "Xbox");
}

static short axis(LONG v)  // DirectInput 0..65535 -> -32768..32767
{
    LONG x = v - 32768;
    if (x > 32767) x = 32767;
    if (x < -32768) x = -32768;
    return (short)x;
}

static int read_dinput(NETPAD *p)
{
    DIJOYSTATE2 js;
    DWORD pov;
    unsigned short b = 0;
    const BYTE *k;
    if (!s_dev) {
        if (GetTickCount() < s_next_scan) return 0;
        s_next_scan = GetTickCount() + 3000;
        scan_dinput();
        if (!s_dev) return 0;
    }
    s_dev->lpVtbl->Poll(s_dev);
    if (FAILED(s_dev->lpVtbl->GetDeviceState(s_dev, sizeof js, &js))) {
        if (FAILED(s_dev->lpVtbl->Acquire(s_dev))) {
            s_dev->lpVtbl->Release(s_dev);
            s_dev = NULL;  // abgesteckt: spaeter neu suchen
        }
        return 0;
    }
    k = js.rgbButtons;
    if (s_sony) {
        // DualSense / DualShock 4: Viereck, Kreuz, Kreis, Dreieck, L1, R1, L2, R2, Create, Options, L3, R3
        if (k[0] & 0x80) b |= XB_X;
        if (k[1] & 0x80) b |= XB_A;
        if (k[2] & 0x80) b |= XB_B;
        if (k[3] & 0x80) b |= XB_Y;
        if (k[4] & 0x80) b |= XB_LB;
        if (k[5] & 0x80) b |= XB_RB;
        if (k[8] & 0x80) b |= XB_BACK;
        if (k[9] & 0x80) b |= XB_START;
        if (k[10] & 0x80) b |= XB_LS;
        if (k[11] & 0x80) b |= XB_RS;
        p->lt = (unsigned char)(js.lRx >> 8);
        p->rt = (unsigned char)(js.lRy >> 8);
        p->rx = axis(js.lZ);
        p->ry = (short)-axis(js.lRz) - 1;
    } else {
        // generische Pads in Xbox-Reihenfolge: A, B, X, Y, LB, RB, Back, Start, LS, RS
        static const unsigned short map[10] = { XB_A, XB_B, XB_X, XB_Y, XB_LB, XB_RB, XB_BACK, XB_START, XB_LS, XB_RS };
        int i;
        for (i = 0; i < 10; i++) if (k[i] & 0x80) b |= map[i];
        p->rx = axis(js.lRx);
        p->ry = (short)-axis(js.lRy) - 1;
        if (js.lZ > 40000) p->lt = (unsigned char)((js.lZ - 32768) >> 7);
        if (js.lZ < 25000) p->rt = (unsigned char)((32768 - js.lZ) >> 7);
    }
    p->lx = axis(js.lX);
    p->ly = (short)-axis(js.lY) - 1;
    pov = js.rgdwPOV[0];
    if (LOWORD(pov) != 0xFFFF) {
        if (pov >= 31500 || pov <= 4500) b |= XB_UP;
        if (pov >= 4500 && pov <= 13500) b |= XB_RIGHT;
        if (pov >= 13500 && pov <= 22500) b |= XB_DOWN;
        if (pov >= 22500 && pov <= 31500) b |= XB_LEFT;
    }
    p->buttons = b;
    return 1;
}

static int read_xinput(NETPAD *p)
{
    struct { DWORD packet; unsigned short buttons; unsigned char lt, rt; short lx, ly, rx, ry; } st;
    DWORD i;
    if (!s_xget) return 0;
    for (i = 0; i < 4; i++) {
        memset(&st, 0, sizeof st);
        if (s_xget(i, &st) == ERROR_SUCCESS) {
            p->buttons = st.buttons; p->lt = st.lt; p->rt = st.rt;
            p->lx = st.lx; p->ly = st.ly; p->rx = st.rx; p->ry = st.ry;
            return 1;
        }
    }
    return 0;
}

static int key(int vk) { return (GetAsyncKeyState(vk) & 0x8000) != 0; }

static int read_keyboard(NETPAD *p)
{
    // Notloesung ohne Controller. Bewegen: WASD/Pfeile, Springen: Leertaste, Angriff: J,
    // Spezial/Bauen: K, Figur wechseln: L, Start: Enter, Zurueck: Tab
    DWORD pid = 0;
    HWND fg = GetForegroundWindow();
    unsigned short b = 0;
    int x = 0, y = 0;
    GetWindowThreadProcessId(fg, &pid);
    if (pid != GetCurrentProcessId()) return 0;
    if (key('A') || key(VK_LEFT)) x -= 1;
    if (key('D') || key(VK_RIGHT)) x += 1;
    if (key('W') || key(VK_UP)) y += 1;
    if (key('S') || key(VK_DOWN)) y -= 1;
    if (key(VK_SPACE)) b |= XB_A;
    if (key('J')) b |= XB_X;
    if (key('K')) b |= XB_B;
    if (key('L')) b |= XB_Y;
    if (key(VK_RETURN)) b |= XB_START;
    if (key(VK_TAB)) b |= XB_BACK;
    if (key('Q')) b |= XB_LB;
    if (key('E')) b |= XB_RB;
    // Menues reagieren auf das Steuerkreuz
    if (y > 0) b |= XB_UP;
    if (y < 0) b |= XB_DOWN;
    if (x < 0) b |= XB_LEFT;
    if (x > 0) b |= XB_RIGHT;
    p->lx = (short)(x * 32767);
    p->ly = (short)(y * 32767);
    p->buttons = b;
    return b || x || y;
}

void input_read(NETPAD *out)
{
    NETPAD kb;
    int src;
    memset(out, 0, sizeof *out);
    memset(&kb, 0, sizeof kb);
    if (read_xinput(out)) src = 0;
    else if (read_dinput(out)) src = 1;
    else src = 2;
    if (read_keyboard(&kb)) {
        out->buttons |= kb.buttons;
        if (!out->lx && !out->ly) { out->lx = kb.lx; out->ly = kb.ly; }
    }
    out->connected = 1;
    if (src != s_last_source && s_log) {
        s_log("Eingabe: %s", src == 0 ? "Xbox-Controller (XInput)" : src == 1 ? s_name : "kein Controller, nur Tastatur");
        s_last_source = src;
    }
}
