// b2cfg.h: pcconfig.txt von LEGO Batman 2 fuer den Netzmodus (Launcher und lbo2.dll gemeinsam).
//
// Desync 23.10.2026: Die Datei des Hosts war kurz nicht lesbar (vermutlich von einem anderen Prozess exklusiv
// geoeffnet). Der Launcher schickte darauf 0 Dateien, der Gast nahm still seine eigene, die DLL des Hosts auch, und
// ab Frame 2957 liefen die Spiele auseinander. Deshalb:
//  - robustes Lesen mit allen Freigaben und Wiederholungen (b2cfg_load),
//  - eingebauter Standard als letzter Rueckfall (k_b2cfg_default, Werte wie die pcconfig.txt des Spiels, aber
//    60 Hz und IgnoreVendorPresets 1),
//  - SHA-256 ohne bcrypt (b2_sha256), damit die DLL im DllMain keine weitere Systembibliothek laden muss.
// Nur static-Funktionen; jede Uebersetzungseinheit bekommt ihre eigene Kopie.
#pragma once
#include <windows.h>
#include <string.h>

#define B2CFG_MAX 65536   // groesste akzeptierte pcconfig.txt (echt: etwa 1,5 KB)

// Quelle der Einstellungen (Launcher -> READY-Paket, Log)
enum { B2CFG_SRC_NONE = 0, B2CFG_SRC_ECHT = 1, B2CFG_SRC_KOPIE = 2, B2CFG_SRC_STANDARD = 3 };

static const char k_b2cfg_default[] =
    "FileVersion                   5\r\n"
    "ScreenWidth                   1280\r\n"
    "ScreenHeight                  720\r\n"
    "WindowWidth                   1280\r\n"
    "WindowHeight                  720\r\n"
    "WindowLeft                    -1\r\n"
    "WindowTop                     -1\r\n"
    "ScreenRefreshRate             60\r\n"
    "VerticalSync                  0\r\n"
    "AspectRatio                   1\r\n"
    "FSAA                          0\r\n"
    "ShaderQuality                 2\r\n"
    "ShadowQuality                 3\r\n"
    "TextureQuality                1\r\n"
    "SoftParticles                 1\r\n"
    "Brightness                    10.000000\r\n"
    "SFXVolume                     7\r\n"
    "MusicVolume                   7\r\n"
    "MasterVolume                  10\r\n"
    "MusicEnabled                  1\r\n"
    "SubtitlesEnabled              0\r\n"
    "MouseSensitivity              10\r\n"
    "InvertY                       0\r\n"
    "Vibration                     0\r\n"
    "BloomEnabled                  1\r\n"
    "ExposureControlEnabled        1\r\n"
    "DoFEnabled                    1\r\n"
    "SoftShadowsEnabled            1\r\n"
    "SpeedBlurEnabled              1\r\n"
    "MotionBlurEnabled             1\r\n"
    "ProcessColourEnabled          1\r\n"
    "WaterDistortionEnabled        1\r\n"
    "EdgeAAEnabled                 0\r\n"
    "GPAAEnabled                   1\r\n"
    "SSAOEnabled                   1\r\n"
    "RingEffectEnabled             1\r\n"
    "LensFilterEnabled             1\r\n"
    "ScreenSpaceReflectionsEnabled 1\r\n"
    "UseHires                      1\r\n"
    "UseHiresPending               1\r\n"
    "ForceMultithreadedD3D         0\r\n"
    "IgnoreVendorPresets           1\r\n"
    "DisableShaderCaching          0\r\n"
    "DisableMultithreadedShaders   0\r\n"
    "AllowVendorExtensions         1\r\n";

// Plausibel: nur Text (keine Binaerdaten) und die Schluessel, an denen die Logik haengt. Eine leere oder gerade
// neu geschriebene Datei faellt hier durch und wird erneut gelesen.
static int b2cfg_plausible(const char *b, DWORD n)
{
    DWORD i;
    if (!n) return 0;
    for (i = 0; i < n; i++) {
        unsigned char c = (unsigned char)b[i];
        if (c < 32 && c != '\r' && c != '\n' && c != '\t') return 0;
        if (c >= 127) return 0;
    }
    return strstr(b, "FileVersion") != NULL && strstr(b, "ShaderQuality") != NULL;
}

// Liest path ganz nach buf (NUL-terminiert, hoechstens cap-1 Bytes). Freigaben READ|WRITE|DELETE, damit ein
// anderer Prozess mit offener Datei nicht stoert; bis zu tries Versuche mit wachsender Pause (15 Versuche etwa
// 2,7 s). Fehlt die Datei, nach 3 Versuchen aufgeben. on_err (optional) bekommt jeden Fehlversuch.
// Rueckgabe 1 = gelesen und plausibel; *err = letzter Fehler (ERROR_INVALID_DATA: leer/unplausibel/zu gross).
static int b2cfg_load(const wchar_t *path, char *buf, DWORD cap, DWORD *len, DWORD *err, int tries,
                      void (*on_err)(int attempt, DWORD err, const wchar_t *path))
{
    int i;
    *len = 0;
    *err = ERROR_FILE_NOT_FOUND;
    if (!path || !path[0] || cap < 2) return 0;
    for (i = 0; i < tries; i++) {
        HANDLE h = CreateFileW(path, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, NULL,
                               OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
        if (h != INVALID_HANDLE_VALUE) {
            DWORD total = 0, got = 0;
            BOOL ok = TRUE;
            while (total < cap - 1 && (ok = ReadFile(h, buf + total, cap - 1 - total, &got, NULL)) != 0 && got)
                total += got;
            *err = ok ? ERROR_INVALID_DATA : GetLastError();
            if (ok && total == cap - 1) {   // noch mehr Daten hinter cap-1: zu gross
                char x;
                if (ReadFile(h, &x, 1, &got, NULL) && got) ok = FALSE;
            }
            CloseHandle(h);
            buf[total] = 0;
            if (ok && b2cfg_plausible(buf, total)) { *len = total; *err = 0; return 1; }
            if (!*err) *err = ERROR_INVALID_DATA;
        } else {
            *err = GetLastError();
        }
        if (on_err) on_err(i + 1, *err, path);
        if ((*err == ERROR_FILE_NOT_FOUND || *err == ERROR_PATH_NOT_FOUND) && i >= 2) break;
        if (i + 1 < tries) Sleep(40 + 20 * i);
    }
    buf[0] = 0;
    return 0;
}

// --- SHA-256 (FIPS 180-4) -----------------------------------------------------------------------------
static const unsigned k_b2_sha_k[64] = {
    0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5, 0x3956c25b, 0x59f111f1, 0x923f82a4, 0xab1c5ed5,
    0xd807aa98, 0x12835b01, 0x243185be, 0x550c7dc3, 0x72be5d74, 0x80deb1fe, 0x9bdc06a7, 0xc19bf174,
    0xe49b69c1, 0xefbe4786, 0x0fc19dc6, 0x240ca1cc, 0x2de92c6f, 0x4a7484aa, 0x5cb0a9dc, 0x76f988da,
    0x983e5152, 0xa831c66d, 0xb00327c8, 0xbf597fc7, 0xc6e00bf3, 0xd5a79147, 0x06ca6351, 0x14292967,
    0x27b70a85, 0x2e1b2138, 0x4d2c6dfc, 0x53380d13, 0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85,
    0xa2bfe8a1, 0xa81a664b, 0xc24b8b70, 0xc76c51a3, 0xd192e819, 0xd6990624, 0xf40e3585, 0x106aa070,
    0x19a4c116, 0x1e376c08, 0x2748774c, 0x34b0bcb5, 0x391c0cb3, 0x4ed8aa4a, 0x5b9cca4f, 0x682e6ff3,
    0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208, 0x90befffa, 0xa4506ceb, 0xbef9a3f7, 0xc67178f2 };

#define B2_ROR(x, n) (((x) >> (n)) | ((x) << (32 - (n))))

static void b2_sha256_block(unsigned st[8], const unsigned char *p)
{
    unsigned w[64], a, b, c, d, e, f, g, h, t1, t2;
    int i;
    for (i = 0; i < 16; i++)
        w[i] = (unsigned)p[4 * i] << 24 | (unsigned)p[4 * i + 1] << 16 | (unsigned)p[4 * i + 2] << 8 | p[4 * i + 3];
    for (; i < 64; i++) {
        unsigned s0 = B2_ROR(w[i - 15], 7) ^ B2_ROR(w[i - 15], 18) ^ (w[i - 15] >> 3);
        unsigned s1 = B2_ROR(w[i - 2], 17) ^ B2_ROR(w[i - 2], 19) ^ (w[i - 2] >> 10);
        w[i] = w[i - 16] + s0 + w[i - 7] + s1;
    }
    a = st[0]; b = st[1]; c = st[2]; d = st[3]; e = st[4]; f = st[5]; g = st[6]; h = st[7];
    for (i = 0; i < 64; i++) {
        t1 = h + (B2_ROR(e, 6) ^ B2_ROR(e, 11) ^ B2_ROR(e, 25)) + ((e & f) ^ (~e & g)) + k_b2_sha_k[i] + w[i];
        t2 = (B2_ROR(a, 2) ^ B2_ROR(a, 13) ^ B2_ROR(a, 22)) + ((a & b) ^ (a & c) ^ (b & c));
        h = g; g = f; f = e; e = d + t1; d = c; c = b; b = a; a = t1 + t2;
    }
    st[0] += a; st[1] += b; st[2] += c; st[3] += d; st[4] += e; st[5] += f; st[6] += g; st[7] += h;
}

static void b2_sha256(const void *data, size_t n, unsigned char out[32])
{
    unsigned st[8] = { 0x6a09e667, 0xbb67ae85, 0x3c6ef372, 0xa54ff53a, 0x510e527f, 0x9b05688c, 0x1f83d9ab, 0x5be0cd19 };
    const unsigned char *p = (const unsigned char *)data;
    unsigned long long bits = (unsigned long long)n * 8;
    unsigned char tail[128];
    size_t end, i;
    for (; n >= 64; n -= 64, p += 64) b2_sha256_block(st, p);
    memset(tail, 0, sizeof tail);
    memcpy(tail, p, n);
    tail[n] = 0x80;
    end = n < 56 ? 64 : 128;
    for (i = 0; i < 8; i++) tail[end - 1 - i] = (unsigned char)(bits >> (8 * i));
    b2_sha256_block(st, tail);
    if (end == 128) b2_sha256_block(st, tail + 64);
    for (i = 0; i < 8; i++) {
        out[4 * i] = (unsigned char)(st[i] >> 24); out[4 * i + 1] = (unsigned char)(st[i] >> 16);
        out[4 * i + 2] = (unsigned char)(st[i] >> 8); out[4 * i + 3] = (unsigned char)st[i];
    }
}

static void b2_hex(const unsigned char h[32], char out[65])
{
    static const char hx[] = "0123456789abcdef";
    int i;
    for (i = 0; i < 32; i++) { out[2 * i] = hx[h[i] >> 4]; out[2 * i + 1] = hx[h[i] & 15]; }
    out[64] = 0;
}
