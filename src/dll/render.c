// render.c: Direct3D-9-Haken fuer den Splitscreen.
// Phase 1 (Diagnose): LBO_RENDERTRACE=F schreibt jeden wichtigen Geraeteaufruf in Frame F mit den
// aufrufenden Spielfunktionen ins Log; LBO_RENDERSTATS=N alle N Frames eine Zusammenfassung.
#include <windows.h>
#include <d3d9.h>
#include <stdio.h>
#include <string.h>
#include "render.h"

static void (*s_log)(const char *fmt, ...);
static volatile long *s_frame;
static long s_trace_frame = -1, s_stats_every;
static IDirect3DDevice9 *s_dev;

#define IAT_DIRECT3DCREATE9 0x02A17CE8

// Geraete-Vtable-Indizes (IDirect3DDevice9)
enum { V_RESET = 16, V_PRESENT = 17, V_SETRT = 37, V_SETDS = 39, V_BEGIN = 41, V_END = 42, V_CLEAR = 43, V_SETTRANSFORM = 44,
       V_SETVIEWPORT = 47, V_DRAWPRIM = 81, V_DRAWIDX = 82, V_DRAWPRIMUP = 83, V_DRAWIDXUP = 84,
       V_SETVSCONST = 94 };

typedef IDirect3D9 *(WINAPI *Create9Fn)(UINT);
typedef HRESULT (STDMETHODCALLTYPE *CreateDeviceFn)(IDirect3D9 *, UINT, D3DDEVTYPE, HWND, DWORD,
                                                    D3DPRESENT_PARAMETERS *, IDirect3DDevice9 **);
typedef HRESULT (STDMETHODCALLTYPE *PresentFn)(IDirect3DDevice9 *, const RECT *, const RECT *, HWND, const RGNDATA *);
typedef HRESULT (STDMETHODCALLTYPE *SetRTFn)(IDirect3DDevice9 *, DWORD, IDirect3DSurface9 *);
typedef HRESULT (STDMETHODCALLTYPE *SetDSFn)(IDirect3DDevice9 *, IDirect3DSurface9 *);
typedef HRESULT (STDMETHODCALLTYPE *SceneFn)(IDirect3DDevice9 *);
typedef HRESULT (STDMETHODCALLTYPE *ClearFn)(IDirect3DDevice9 *, DWORD, const D3DRECT *, DWORD, D3DCOLOR, float, DWORD);
typedef HRESULT (STDMETHODCALLTYPE *SetTransformFn)(IDirect3DDevice9 *, D3DTRANSFORMSTATETYPE, const D3DMATRIX *);
typedef HRESULT (STDMETHODCALLTYPE *SetViewportFn)(IDirect3DDevice9 *, const D3DVIEWPORT9 *);
typedef HRESULT (STDMETHODCALLTYPE *DrawPrimFn)(IDirect3DDevice9 *, D3DPRIMITIVETYPE, UINT, UINT);
typedef HRESULT (STDMETHODCALLTYPE *DrawIdxFn)(IDirect3DDevice9 *, D3DPRIMITIVETYPE, INT, UINT, UINT, UINT, UINT);
typedef HRESULT (STDMETHODCALLTYPE *DrawPrimUPFn)(IDirect3DDevice9 *, D3DPRIMITIVETYPE, UINT, const void *, UINT);
typedef HRESULT (STDMETHODCALLTYPE *DrawIdxUPFn)(IDirect3DDevice9 *, D3DPRIMITIVETYPE, UINT, UINT, UINT, const void *,
                                                 D3DFORMAT, const void *, UINT);
typedef HRESULT (STDMETHODCALLTYPE *SetVSConstFn)(IDirect3DDevice9 *, UINT, const float *, UINT);

typedef HRESULT (STDMETHODCALLTYPE *ResetFn)(IDirect3DDevice9 *, D3DPRESENT_PARAMETERS *);
static ResetFn real_reset;
static int s_diag;
static HRESULT STDMETHODCALLTYPE h_reset(IDirect3DDevice9 *d, D3DPRESENT_PARAMETERS *pp);
static Create9Fn real_create9;
static CreateDeviceFn real_createdevice;
static PresentFn real_present;
static SetRTFn real_setrt;
static SetDSFn real_setds;
static SceneFn real_begin, real_end;
static ClearFn real_clear;
static SetTransformFn real_settransform;
static SetViewportFn real_setviewport;
static DrawPrimFn real_drawprim;
static DrawIdxFn real_drawidx;
static DrawPrimUPFn real_drawprimup;
static DrawIdxUPFn real_drawidxup;
static SetVSConstFn real_setvsconst;

// --- Aufrufer im Spielcode -------------------------------------------------------------
// Sucht auf dem Stack Ruecksprungadressen in den Spielcode (vor ihnen steht ein call).
static int is_ret_addr(DWORD a)
{
    const unsigned char *p = (const unsigned char *)a;
    if (a < 0x00401006 || a >= 0x00845000) return 0;
    if (p[-5] == 0xE8) return 1;                                   // call rel32
    if (p[-6] == 0xFF && (p[-5] & 0x38) == 0x10) return 1;         // call [abs]/[reg+disp32]
    if (p[-2] == 0xFF && (p[-1] & 0x38) == 0x10) return 1;         // call reg / [reg]
    if (p[-3] == 0xFF && (p[-2] & 0x38) == 0x10) return 1;         // call [reg+disp8]
    return 0;
}

static int game_callers(DWORD *out, int max)
{
    DWORD *sp = (DWORD *)&out, i;
    int n = 0;
    __try {
        for (i = 0; i < 2048 && n < max; i++)
            if (is_ret_addr(sp[i])) out[n++] = sp[i];
    } __except (EXCEPTION_EXECUTE_HANDLER) {}
    return n;
}

static int tracing(void) { return s_frame && *s_frame == s_trace_frame; }

static void trace(const char *what, const char *fmt, ...)
{
    char line[512], args[256];
    DWORD c[40];
    int n, i, len;
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(args, sizeof args, fmt, ap);
    va_end(ap);
    n = game_callers(c, 40);
    len = snprintf(line, sizeof line, "R %-10s %-30s <-", what, args);
    for (i = 0; i < n && i < 3; i++) len += snprintf(line + len, sizeof line - len, " %08lx", c[i]);
    if (n > 3) len += snprintf(line + len, sizeof line - len, " ...");
    for (i = n > 12 ? n - 9 : 3; i < n; i++) len += snprintf(line + len, sizeof line - len, " %08lx", c[i]);
    s_log("%s", line);
}

// --- Statistik ------------------------------------------------------------------------
static long st_draws, st_rt, st_vp, st_clear, st_begin, st_vsc, st_xf;

// --- Haken -----------------------------------------------------------------------------
static HRESULT STDMETHODCALLTYPE h_present(IDirect3DDevice9 *d, const RECT *a, const RECT *b, HWND w, const RGNDATA *r)
{
    long F = s_frame ? *s_frame : 0;
    if (tracing()) trace("Present", "");
    if (s_stats_every > 0 && F % s_stats_every == 0)
        s_log("Render F%ld: %ld Draws, %ld RT, %ld Viewports, %ld Clears, %ld BeginScene, %ld VSConst, %ld Transform",
              F, st_draws, st_rt, st_vp, st_clear, st_begin, st_vsc, st_xf);
    st_draws = st_rt = st_vp = st_clear = st_begin = st_vsc = st_xf = 0;
    return real_present(d, a, b, w, r);
}

static HRESULT STDMETHODCALLTYPE h_setrt(IDirect3DDevice9 *d, DWORD i, IDirect3DSurface9 *s)
{
    st_rt++;
    if (tracing()) {
        D3DSURFACE_DESC sd;
        memset(&sd, 0, sizeof sd);
        if (s) s->lpVtbl->GetDesc(s, &sd);
        trace("SetRT", "%lu %p %ux%u", i, (void *)s, sd.Width, sd.Height);
    }
    return real_setrt(d, i, s);
}

static HRESULT STDMETHODCALLTYPE h_setds(IDirect3DDevice9 *d, IDirect3DSurface9 *s)
{
    if (tracing()) trace("SetDS", "%p", (void *)s);
    return real_setds(d, s);
}

static HRESULT STDMETHODCALLTYPE h_begin(IDirect3DDevice9 *d)
{
    st_begin++;
    if (tracing()) trace("BeginScene", "");
    return real_begin(d);
}

static HRESULT STDMETHODCALLTYPE h_end(IDirect3DDevice9 *d)
{
    if (tracing()) trace("EndScene", "");
    return real_end(d);
}

static HRESULT STDMETHODCALLTYPE h_clear(IDirect3DDevice9 *d, DWORD n, const D3DRECT *r, DWORD f, D3DCOLOR c, float z, DWORD s)
{
    st_clear++;
    if (tracing()) trace("Clear", "n=%lu flags=%lx col=%08lx", n, f, c);
    return real_clear(d, n, r, f, c, z, s);
}

static HRESULT STDMETHODCALLTYPE h_settransform(IDirect3DDevice9 *d, D3DTRANSFORMSTATETYPE t, const D3DMATRIX *m)
{
    st_xf++;
    if (tracing()) trace("Transform", "%d", (int)t);
    return real_settransform(d, t, m);
}

static HRESULT STDMETHODCALLTYPE h_setviewport(IDirect3DDevice9 *d, const D3DVIEWPORT9 *v)
{
    st_vp++;
    if (tracing()) trace("Viewport", "%lu %lu %lux%lu", v->X, v->Y, v->Width, v->Height);
    return real_setviewport(d, v);
}

static long tr_draws;
static void draw_trace(const char *what, UINT count)
{
    st_draws++;
    if (tracing() && tr_draws++ < 400) trace(what, "%u", count);
}

static HRESULT STDMETHODCALLTYPE h_drawprim(IDirect3DDevice9 *d, D3DPRIMITIVETYPE t, UINT s, UINT c)
{
    draw_trace("DrawPrim", c);
    return real_drawprim(d, t, s, c);
}

static HRESULT STDMETHODCALLTYPE h_drawidx(IDirect3DDevice9 *d, D3DPRIMITIVETYPE t, INT b, UINT mi, UINT nv, UINT si, UINT c)
{
    draw_trace("DrawIdx", c);
    return real_drawidx(d, t, b, mi, nv, si, c);
}

static HRESULT STDMETHODCALLTYPE h_drawprimup(IDirect3DDevice9 *d, D3DPRIMITIVETYPE t, UINT c, const void *v, UINT st)
{
    draw_trace("DrawPrimUP", c);
    return real_drawprimup(d, t, c, v, st);
}

static HRESULT STDMETHODCALLTYPE h_drawidxup(IDirect3DDevice9 *d, D3DPRIMITIVETYPE t, UINT mi, UINT nv, UINT c, const void *i,
                                             D3DFORMAT f, const void *v, UINT st)
{
    draw_trace("DrawIdxUP", c);
    return real_drawidxup(d, t, mi, nv, c, i, f, v, st);
}

// --- Eigene Kamera auf Grafikkarten-Ebene ------------------------------------------------
// Die Shader bekommen die Kamera als Konstanten: c0..c3 = View-Projection (transponiert), c12..c15 =
// View (transponiert). Entsprechen die Werte genau der gemeinsamen Kamera des Spiels (globale Matrizen
// 0x9a7050 bzw. 0x9a6fd0), wird stattdessen die um d verschobene Kamera hochgeladen:
// verschobene Kamera C' = C * T(d)  =>  V' = T(-d) * V  =>  VP' = T(-d) * VP.
// Die Spiellogik bleibt unberuehrt, daher kein Einfluss auf den Lockstep.
static float s_world_xf[16];                // M (Weltraum): VP' = M * VP
static float s_own_view[16];                // eigene View-Matrix (Zeilenvektoren)
static int s_shift_on;
static long s_shift_hits[2];
static float s_cam_vp[16];                  // View-Projection der Spielkamera, vor dem Zeichnen festgehalten
static float s_cam_v_upload[16], s_last_c12[16];
static int s_have_cam_v, s_have_last_c12;

// Wird am Anfang des Frame-Endes aufgerufen, bevor gezeichnet wird. Die globalen Matrizen enthalten dann
// noch die Spielkamera; der Schatten-Durchgang ueberschreibt sie spaeter mit der Lichtkamera.
// m: Weltraum-Transformation (eigene View = Spiel-View * ... ; VP' = m * VP), view: eigene View-Matrix.
void render_set_view(int on, const float *m, const float *view)
{
    s_shift_on = on;
    if (on) {
        memcpy(s_world_xf, m, 64);
        memcpy(s_own_view, view, 64);
        memcpy(s_cam_vp, (const void *)0x009A7050, 64);
    }
    s_have_cam_v = s_have_last_c12 = 0;   // View-Matrix wird beim ersten passenden c0 neu erkannt
}

// Vergleicht einen hochgeladenen 4x4-Block (transponiert) mit einer Zeilenmatrix des Spiels
static int equals_transposed(const float *u, const float *m)
{
    int i, j;
    for (i = 0; i < 4; i++)
        for (j = 0; j < 4; j++)
            if (u[i * 4 + j] != m[j * 4 + i]) return 0;
    return 1;
}

// Hochgeladen wird VP transponiert (U = VP^T). Gesucht (M*VP)^T = VP^T * M^T: U'[i][j] = sum_k U[i][k] * M[j][k]
static void transform_vp_upload(float *u)
{
    float r[16];
    int i, j, k;
    for (i = 0; i < 4; i++)
        for (j = 0; j < 4; j++) {
            float s = 0;
            for (k = 0; k < 4; k++) s += u[i * 4 + k] * s_world_xf[j * 4 + k];
            r[i * 4 + j] = s;
        }
    memcpy(u, r, 64);
}

// Eigene View transponiert in den Upload schreiben
static void own_view_upload(float *u)
{
    int i, j;
    for (i = 0; i < 4; i++)
        for (j = 0; j < 4; j++) u[i * 4 + j] = s_own_view[j * 4 + i];
}

static long tr_vsc;
static HRESULT STDMETHODCALLTYPE h_setvsconst(IDirect3DDevice9 *d, UINT r, const float *c, UINT n)
{
    st_vsc++;
    if (tracing() && tr_vsc++ < 3000) {
        if (tr_vsc == 1) {
            const float *vp = s_shift_on ? s_cam_vp : (const float *)0x009A7050;   // Momentaufnahme bzw. global
            s_log("VP  [%.3f %.3f %.3f %.3f | %.3f %.3f %.3f %.3f | %.3f %.3f %.3f %.3f | %.3f %.3f %.3f %.3f]",
                  vp[0], vp[1], vp[2], vp[3], vp[4], vp[5], vp[6], vp[7], vp[8], vp[9], vp[10], vp[11], vp[12], vp[13], vp[14], vp[15]);
        }
        if (n >= 4)
            s_log("VSC c%u x%u [%.3f %.3f %.3f %.3f | %.3f %.3f %.3f %.3f | %.3f %.3f %.3f %.3f | %.3f %.3f %.3f %.3f]", r, n,
                  c[0], c[1], c[2], c[3], c[4], c[5], c[6], c[7], c[8], c[9], c[10], c[11], c[12], c[13], c[14], c[15]);
        else
            s_log("VSC c%u x%u [%.3f %.3f %.3f %.3f]", r, n, c[0], c[1], c[2], c[3]);
    }
    if (s_shift_on && n <= 64 && ((r <= 0 && r + n >= 4) || (r <= 12 && r + n >= 16))) {
        float buf[64 * 4];
        HRESULT hr;
        int fix_view_now = 0;
        memcpy(buf, c, n * 16);
        if (r <= 12 && r + n >= 16) {
            float *v = buf + (12 - r) * 4;
            memcpy(s_last_c12, v, 64);   // Original merken
            s_have_last_c12 = 1;
            if (s_have_cam_v && memcmp(v, s_cam_v_upload, 64) == 0) {
                own_view_upload(v);
                if (s_shift_hits[1]++ == 0) s_log("Render: eigene Kamera fuer c12 (View) aktiv");
            }
        }
        if (r <= 0 && r + n >= 4 && equals_transposed(buf - r * 4, s_cam_vp)) {
            transform_vp_upload(buf - r * 4);
            if (s_shift_hits[0]++ == 0) s_log("Render: eigene Kamera fuer c0 (View-Projection) aktiv");
            // Die View der Spielkamera wird direkt vorher nach c12 geladen: jetzt als solche erkennen und
            // als eigene View nachladen (letzte Zeile 0 0 0 1 = echte View-Matrix, nicht die Lichtkamera)
            if (!s_have_cam_v && s_have_last_c12 && s_last_c12[12] == 0 && s_last_c12[13] == 0 &&
                s_last_c12[14] == 0 && s_last_c12[15] == 1) {
                memcpy(s_cam_v_upload, s_last_c12, 64);
                s_have_cam_v = 1;
                fix_view_now = !(r <= 12 && r + n >= 16);
            }
        }
        hr = real_setvsconst(d, r, buf, n);
        if (fix_view_now) {
            float v[16];
            own_view_upload(v);
            real_setvsconst(d, 12, v, 4);
            if (s_shift_hits[1]++ == 0) s_log("Render: eigene Kamera fuer c12 (View) aktiv");
        }
        return hr;
    }
    return real_setvsconst(d, r, c, n);
}

static void hook_vtbl(void **vtbl, int idx, void *fn, void **orig)
{
    DWORD old;
    VirtualProtect(&vtbl[idx], 4, PAGE_EXECUTE_READWRITE, &old);
    *orig = vtbl[idx];
    vtbl[idx] = fn;
    VirtualProtect(&vtbl[idx], 4, old, &old);
}

static HRESULT STDMETHODCALLTYPE h_createdevice(IDirect3D9 *d3d, UINT a, D3DDEVTYPE t, HWND w, DWORD f,
                                                D3DPRESENT_PARAMETERS *pp, IDirect3DDevice9 **out)
{
    HRESULT hr = real_createdevice(d3d, a, t, w, f, pp, out);
    s_log("Render: CreateDevice %08lx, Flags %08lx, %ux%u, Fenster %d", hr, f,
          pp ? pp->BackBufferWidth : 0, pp ? pp->BackBufferHeight : 0, pp ? pp->Windowed : -1);
    if (SUCCEEDED(hr) && out && *out && !real_reset) {
        void **v = *(void ***)*out;
        s_dev = *out;
        hook_vtbl(v, V_RESET, (void *)h_reset, (void **)&real_reset);
        hook_vtbl(v, V_SETVSCONST, (void *)h_setvsconst, (void **)&real_setvsconst);
        if (!s_diag) return hr;
        hook_vtbl(v, V_PRESENT, (void *)h_present, (void **)&real_present);
        hook_vtbl(v, V_SETRT, (void *)h_setrt, (void **)&real_setrt);
        hook_vtbl(v, V_SETDS, (void *)h_setds, (void **)&real_setds);
        hook_vtbl(v, V_BEGIN, (void *)h_begin, (void **)&real_begin);
        hook_vtbl(v, V_END, (void *)h_end, (void **)&real_end);
        hook_vtbl(v, V_CLEAR, (void *)h_clear, (void **)&real_clear);
        hook_vtbl(v, V_SETTRANSFORM, (void *)h_settransform, (void **)&real_settransform);
        hook_vtbl(v, V_SETVIEWPORT, (void *)h_setviewport, (void **)&real_setviewport);
        hook_vtbl(v, V_DRAWPRIM, (void *)h_drawprim, (void **)&real_drawprim);
        hook_vtbl(v, V_DRAWIDX, (void *)h_drawidx, (void **)&real_drawidx);
        hook_vtbl(v, V_DRAWPRIMUP, (void *)h_drawprimup, (void **)&real_drawprimup);
        hook_vtbl(v, V_DRAWIDXUP, (void *)h_drawidxup, (void **)&real_drawidxup);
    }
    return hr;
}

static IDirect3D9 *WINAPI h_create9(UINT sdk)
{
    IDirect3D9 *d = real_create9(sdk);
    if (d && !real_createdevice)
        hook_vtbl(*(void ***)d, 16, (void *)h_createdevice, (void **)&real_createdevice);
    return d;
}

// --- Eigene Ansicht sichern und vor der Ausgabe zurueckschreiben (Splitscreen) --------------
static IDirect3DSurface9 *s_saved;
static int s_copy_errors;

static void release_saved(void)
{
    if (s_saved) { s_saved->lpVtbl->Release(s_saved); s_saved = NULL; }
}

static HRESULT STDMETHODCALLTYPE h_reset(IDirect3DDevice9 *d, D3DPRESENT_PARAMETERS *pp)
{
    release_saved();   // D3DPOOL_DEFAULT-Flaechen muessen vor Reset freigegeben sein
    return real_reset(d, pp);
}

// StretchRect zwischen zwei Render-Targets; manche Treiber verlangen, dass keine Szene offen ist
static int copy_surface(IDirect3DSurface9 *from, IDirect3DSurface9 *to)
{
    HRESULT hr = s_dev->lpVtbl->StretchRect(s_dev, from, NULL, to, NULL, D3DTEXF_NONE);
    if (FAILED(hr)) {
        s_dev->lpVtbl->EndScene(s_dev);
        hr = s_dev->lpVtbl->StretchRect(s_dev, from, NULL, to, NULL, D3DTEXF_NONE);
        s_dev->lpVtbl->BeginScene(s_dev);
    }
    if (FAILED(hr) && s_copy_errors++ < 5) s_log("Render: StretchRect fehlgeschlagen (%08lx)", hr);
    return SUCCEEDED(hr);
}

int render_save_view(void)
{
    IDirect3DSurface9 *bb;
    D3DSURFACE_DESC bd, sd;
    int ok;
    if (!s_dev || FAILED(s_dev->lpVtbl->GetBackBuffer(s_dev, 0, 0, D3DBACKBUFFER_TYPE_MONO, &bb))) return 0;
    bb->lpVtbl->GetDesc(bb, &bd);
    if (s_saved) {
        s_saved->lpVtbl->GetDesc(s_saved, &sd);
        if (sd.Width != bd.Width || sd.Height != bd.Height || sd.Format != bd.Format) release_saved();
    }
    if (!s_saved && FAILED(s_dev->lpVtbl->CreateRenderTarget(s_dev, bd.Width, bd.Height, bd.Format,
                                                             D3DMULTISAMPLE_NONE, 0, FALSE, &s_saved, NULL))) {
        if (s_copy_errors++ < 5) s_log("Render: Zwischenspeicher %ux%u nicht angelegt", bd.Width, bd.Height);
        bb->lpVtbl->Release(bb);
        return 0;
    }
    ok = copy_surface(bb, s_saved);
    bb->lpVtbl->Release(bb);
    return ok;
}

int render_restore_view(void)
{
    IDirect3DSurface9 *bb;
    int ok;
    if (!s_dev || !s_saved || FAILED(s_dev->lpVtbl->GetBackBuffer(s_dev, 0, 0, D3DBACKBUFFER_TYPE_MONO, &bb))) return 0;
    ok = copy_surface(s_saved, bb);
    bb->lpVtbl->Release(bb);
    return ok;
}

void render_init(void (*logger)(const char *, ...), volatile long *frame, int need_device)
{
    char buf[32];
    DWORD old;
    s_log = logger;
    s_frame = frame;
    if (GetEnvironmentVariableA("LBO_RENDERTRACE", buf, sizeof buf)) s_trace_frame = atol(buf);
    if (GetEnvironmentVariableA("LBO_RENDERSTATS", buf, sizeof buf)) s_stats_every = atol(buf);
    s_diag = s_trace_frame >= 0 || s_stats_every > 0;
    if (!s_diag && !need_device) return;
    VirtualProtect((void *)IAT_DIRECT3DCREATE9, 4, PAGE_READWRITE, &old);
    real_create9 = *(Create9Fn *)IAT_DIRECT3DCREATE9;
    *(void **)IAT_DIRECT3DCREATE9 = (void *)h_create9;
    VirtualProtect((void *)IAT_DIRECT3DCREATE9, 4, old, &old);
    s_log("Render: Haken aktiv (Trace Frame %ld, Statistik alle %ld)", s_trace_frame, s_stats_every);
}
