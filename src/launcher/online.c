// online.c: Verbindungsaufbau fuers Internet ohne eigenen Server.
//
// Ablauf:
//  1. Beide fragen per STUN (oeffentliche Server von Google/Cloudflare) ihre oeffentliche Adresse ab.
//  2. Daraus entsteht ein Einladungscode (oeffentliche + lokale Adresse, Zufallsschluessel).
//  3. Host und Gast tauschen ihre Codes aus (Discord, WhatsApp, ...).
//  4. Beide schicken sich gleichzeitig Pakete (UDP-Hole-Punching), bis der Partner antwortet.
//  5. Handshake: gleiche EXE, gleiche Mod-Version, Ping messen.
//  6. Der Host schickt seinen Spielstand-Ordner, damit beide Spiele denselben Stand laden.
// Danach schliesst der Launcher den Socket; die DLL im Spiel bindet denselben Port neu, die
// Zuordnung im Router bleibt dabei erhalten (Keepalive bis zum ersten Frame).
#define _WINSOCK_DEPRECATED_NO_WARNINGS
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#include <bcrypt.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "online.h"

#define LMAGIC 0x4C4E4C42u  // Launcher-Pakete "BLNL"
#define MOD_VERSION 1
enum { M_HELLO = 1, M_PING, M_PONG, M_FILE, M_HAVE, M_READY };

#pragma pack(push, 1)
typedef struct {
    unsigned magic, type, key;
    unsigned seq;
    unsigned char exe_hash[32];
    unsigned version, role;
} HELLO;
typedef struct { unsigned magic, type, key; unsigned seq; unsigned long long t; } PING;
typedef struct {
    unsigned magic, type, key;
    unsigned file, nfiles, size, chunk, nchunks;
    char name[64];
    unsigned short len;
    unsigned char data[1000];
} FILEPKT;
typedef struct { unsigned magic, type, key; unsigned file; unsigned char bits[64]; unsigned done; } HAVE;
#pragma pack(pop)

static SOCKET s_sock = INVALID_SOCKET;
static struct sockaddr_in s_peer;
static unsigned s_key_me, s_key_peer;

// --- Hilfen ---------------------------------------------------------------------------
static int recv_any(void *buf, int size, struct sockaddr_in *from, int timeout_ms)
{
    fd_set rd;
    struct timeval tv;
    int fl = sizeof *from;
    FD_ZERO(&rd);
    FD_SET(s_sock, &rd);
    tv.tv_sec = timeout_ms / 1000;
    tv.tv_usec = (timeout_ms % 1000) * 1000;
    if (select(0, &rd, NULL, NULL, &tv) <= 0) return -1;
    return recvfrom(s_sock, (char *)buf, size, 0, (struct sockaddr *)from, &fl);
}

static void send_to(const struct sockaddr_in *to, const void *buf, int len)
{
    sendto(s_sock, (const char *)buf, len, 0, (const struct sockaddr *)to, sizeof *to);
}

int online_sha256_file(const char *path, unsigned char out[32])
{
    BCRYPT_ALG_HANDLE alg;
    BCRYPT_HASH_HANDLE h;
    unsigned char buf[65536];
    DWORD n;
    HANDLE f = CreateFileA(path, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, 0, NULL);
    if (f == INVALID_HANDLE_VALUE) return 0;
    BCryptOpenAlgorithmProvider(&alg, BCRYPT_SHA256_ALGORITHM, NULL, 0);
    BCryptCreateHash(alg, &h, NULL, 0, NULL, 0, 0);
    while (ReadFile(f, buf, sizeof buf, &n, NULL) && n) BCryptHashData(h, buf, n, 0);
    BCryptFinishHash(h, out, 32, 0);
    BCryptDestroyHash(h);
    BCryptCloseAlgorithmProvider(alg, 0);
    CloseHandle(f);
    return 1;
}

// --- STUN (RFC 5389, nur Binding Request) -------------------------------------------
static int stun_query(const char *host, const char *port, struct sockaddr_in *mapped)
{
    struct addrinfo hints, *res;
    unsigned char req[20], resp[512];
    struct sockaddr_in from;
    int attempt, n, i;
    memset(&hints, 0, sizeof hints);
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_DGRAM;
    if (getaddrinfo(host, port, &hints, &res)) return 0;
    memset(req, 0, sizeof req);
    req[1] = 0x01;                                  // Binding Request
    req[4] = 0x21; req[5] = 0x12; req[6] = 0xA4; req[7] = 0x42;  // Magic Cookie
    for (i = 8; i < 20; i++) req[i] = (unsigned char)rand();
    for (attempt = 0; attempt < 4; attempt++) {
        send_to((struct sockaddr_in *)res->ai_addr, req, sizeof req);
        while ((n = recv_any(resp, sizeof resp, &from, 400)) > 0) {
            if (n < 20 || resp[0] != 0x01 || resp[1] != 0x01 || memcmp(resp + 8, req + 8, 12)) continue;
            for (i = 20; i + 4 <= n;) {
                int type = resp[i] << 8 | resp[i + 1], len = resp[i + 2] << 8 | resp[i + 3];
                if ((type == 0x0020 || type == 0x0001) && len >= 8 && resp[i + 5] == 0x01) {
                    unsigned short p = (unsigned short)(resp[i + 6] << 8 | resp[i + 7]);
                    unsigned a = (unsigned)resp[i + 8] << 24 | resp[i + 9] << 16 | resp[i + 10] << 8 | resp[i + 11];
                    if (type == 0x0020) { p ^= 0x2112; a ^= 0x2112A442; }
                    memset(mapped, 0, sizeof *mapped);
                    mapped->sin_family = AF_INET;
                    mapped->sin_port = htons(p);
                    mapped->sin_addr.s_addr = htonl(a);
                    freeaddrinfo(res);
                    return 1;
                }
                i += 4 + ((len + 3) & ~3);
            }
        }
    }
    freeaddrinfo(res);
    return 0;
}

static void local_ipv4(struct in_addr *out)
{
    // Lokale Adresse, ueber die das Internet erreicht wird (kein Paket wird gesendet)
    SOCKET t = socket(AF_INET, SOCK_DGRAM, 0);
    struct sockaddr_in a, me;
    int l = sizeof me;
    memset(&a, 0, sizeof a);
    a.sin_family = AF_INET;
    a.sin_port = htons(53);
    inet_pton(AF_INET, "8.8.8.8", &a.sin_addr);
    out->s_addr = 0;
    if (connect(t, (struct sockaddr *)&a, sizeof a) == 0 && getsockname(t, (struct sockaddr *)&me, &l) == 0)
        *out = me.sin_addr;
    closesocket(t);
}

// --- Einladungscodes ------------------------------------------------------------------
// 14 Byte (oeffentliche IP+Port, lokale IP+Port, 2 Byte Schluessel) -> 23 Zeichen Base32 + Pruefzeichen
static const char B32[] = "ABCDEFGHJKLMNPQRSTUVWXYZ23456789";  // ohne I, O, 0, 1

static void encode_code(const unsigned char raw[14], char *out)
{
    unsigned acc = 0, bits = 0, sum = 0;
    int i, n = 0;
    for (i = 0; i < 14; i++) {
        acc = acc << 8 | raw[i];
        bits += 8;
        sum += raw[i] * (i + 1);
        while (bits >= 5) {
            if (n && n % 5 == 4) out[n++] = '-';
            out[n++] = B32[(acc >> (bits - 5)) & 31];
            bits -= 5;
        }
    }
    if (bits) { if (n % 5 == 4) out[n++] = '-'; out[n++] = B32[(acc << (5 - bits)) & 31]; }
    out[n++] = B32[sum % 32];
    out[n] = 0;
}

static int decode_code(const char *in, unsigned char raw[14])
{
    unsigned acc = 0, bits = 0, sum = 0;
    int n = 0, i;
    char clean[64];
    int m = 0;
    for (; *in && m < 63; in++) {
        char c = *in >= 'a' && *in <= 'z' ? *in - 32 : *in;
        if (c == '0') c = 'O';
        if (c == '1') c = 'I';
        if (strchr(B32, c) && c) clean[m++] = c;
    }
    clean[m] = 0;
    if (m != 24) return 0;
    for (i = 0; i < 23; i++) {
        acc = acc << 5 | (unsigned)(strchr(B32, clean[i]) - B32);
        bits += 5;
        if (bits >= 8 && n < 14) { raw[n++] = (unsigned char)(acc >> (bits - 8)); bits -= 8; }
    }
    if (n != 14) return 0;
    for (i = 0; i < 14; i++) sum += raw[i] * (i + 1);
    return B32[sum % 32] == clean[23];
}

static void make_raw(const struct sockaddr_in *pub, const struct sockaddr_in *loc, unsigned key, unsigned char raw[14])
{
    memcpy(raw, &pub->sin_addr.s_addr, 4);
    memcpy(raw + 4, &pub->sin_port, 2);
    memcpy(raw + 6, &loc->sin_addr.s_addr, 4);
    memcpy(raw + 10, &loc->sin_port, 2);
    raw[12] = (unsigned char)key;
    raw[13] = (unsigned char)(key >> 8);
}

// --- Verbindungsaufbau ----------------------------------------------------------------
int online_prepare(ONLINE *o, char code_out[40])
{
    WSADATA wsa;
    struct sockaddr_in me, pub1, pub2;
    unsigned char raw[14];
    int l = sizeof me;
    WSAStartup(MAKEWORD(2, 2), &wsa);
    srand(GetTickCount() ^ GetCurrentProcessId());
    s_sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    memset(&me, 0, sizeof me);
    me.sin_family = AF_INET;
    me.sin_port = htons((u_short)o->port);  // 0 = zufaelliger Port
    if (bind(s_sock, (struct sockaddr *)&me, sizeof me)) {
        printf("Fehler: UDP-Port %d ist belegt.\n", o->port);
        return 0;
    }
    getsockname(s_sock, (struct sockaddr *)&me, &l);
    o->port = ntohs(me.sin_port);
    local_ipv4(&me.sin_addr);
    o->local = me;

    printf("Ermittle oeffentliche Adresse ...\n");
    if (!stun_query("stun.l.google.com", "19302", &pub1) && !stun_query("stun.cloudflare.com", "3478", &pub1)) {
        printf("Keine Antwort von STUN-Servern. Ohne Internet geht nur LAN (lokale Adresse wird verwendet).\n");
        pub1 = me;
    } else if (stun_query("stun1.l.google.com", "19302", &pub2) && pub2.sin_port != pub1.sin_port) {
        printf("Hinweis: Dein Router vergibt fuer jedes Ziel einen anderen Port (\"symmetrisches NAT\").\n"
               "Die direkte Verbindung klappt dann evtl. nicht. Ausweg: beide nutzen ein VPN wie\n"
               "Radmin VPN oder Tailscale, siehe Anleitung.\n");
        o->symmetric_nat = 1;
    }
    o->pub = pub1;
    s_key_me = (unsigned)rand() & 0xFFFF;
    make_raw(&o->pub, &o->local, s_key_me, raw);
    encode_code(raw, code_out);
    return 1;
}

// Fuer die App: Schluessel und Adressen kommen aus der verschluesselten Einladung statt aus Codes
unsigned online_key_me(void) { return s_key_me; }
void online_set_keys(unsigned me, unsigned peer) { s_key_me = me & 0xFFFF; s_key_peer = peer & 0xFFFF; }

int online_parse_endpoint(const char *text, struct sockaddr_in *out)
{
    char host[64];
    const char *colon = strrchr(text, ':');
    size_t n;
    if (!colon) return 0;
    n = (size_t)(colon - text);
    if (n == 0 || n >= sizeof host) return 0;
    memcpy(host, text, n);
    host[n] = 0;
    memset(out, 0, sizeof *out);
    out->sin_family = AF_INET;
    out->sin_port = htons((u_short)atoi(colon + 1));
    return inet_pton(AF_INET, host, &out->sin_addr) == 1;
}

// Direktverbindung ohne Code (VPN wie Radmin/Tailscale, LAN oder Portweiterleitung):
// Der Host lauscht auf festem Port, der Gast gibt die IP des Hosts ein.
int online_prepare_direct(ONLINE *o)
{
    WSADATA wsa;
    struct sockaddr_in me;
    WSAStartup(MAKEWORD(2, 2), &wsa);
    srand(GetTickCount() ^ GetCurrentProcessId());
    s_sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    memset(&me, 0, sizeof me);
    me.sin_family = AF_INET;
    me.sin_port = htons((u_short)o->port);
    if (bind(s_sock, (struct sockaddr *)&me, sizeof me)) {
        printf("Fehler: UDP-Port %d ist belegt.\n", o->port);
        return 0;
    }
    {
        int l = sizeof me;
        getsockname(s_sock, (struct sockaddr *)&me, &l);
        o->port = ntohs(me.sin_port);
    }
    s_key_me = s_key_peer = 0;  // ohne Code gibt es keinen Schluessel
    memset(&o->peer_pub, 0, sizeof o->peer_pub);
    memset(&o->peer_local, 0, sizeof o->peer_local);
    return 1;
}

int online_set_peer_ip(ONLINE *o, const char *text)
{
    char host[64];
    const char *colon = strrchr(text, ':');
    int port = DIRECT_PORT;
    size_t n = colon ? (size_t)(colon - text) : strlen(text);
    if (n == 0 || n >= sizeof host) return 0;
    memcpy(host, text, n);
    host[n] = 0;
    if (colon) port = atoi(colon + 1);
    memset(&o->peer_pub, 0, sizeof o->peer_pub);
    o->peer_pub.sin_family = AF_INET;
    o->peer_pub.sin_port = htons((u_short)port);
    return inet_pton(AF_INET, host, &o->peer_pub.sin_addr) == 1 && port > 0 && port < 65536;
}

int online_set_peer_code(ONLINE *o, const char *code)
{
    unsigned char raw[14];
    if (!decode_code(code, raw)) return 0;
    memset(&o->peer_pub, 0, sizeof o->peer_pub);
    memset(&o->peer_local, 0, sizeof o->peer_local);
    o->peer_pub.sin_family = o->peer_local.sin_family = AF_INET;
    memcpy(&o->peer_pub.sin_addr.s_addr, raw, 4);
    memcpy(&o->peer_pub.sin_port, raw + 4, 2);
    memcpy(&o->peer_local.sin_addr.s_addr, raw + 6, 4);
    memcpy(&o->peer_local.sin_port, raw + 10, 2);
    s_key_peer = raw[12] | raw[13] << 8;
    return 1;
}

int online_connect(ONLINE *o, const unsigned char exe_hash[32], int timeout_s)
{
    HELLO h, in;
    struct sockaddr_in from;
    DWORD start = GetTickCount(), last = 0;
    int got_hello = 0, got_ack = 0, n;
    memset(&h, 0, sizeof h);
    h.magic = LMAGIC; h.type = M_HELLO; h.key = s_key_me; h.version = MOD_VERSION; h.role = o->role;
    memcpy(h.exe_hash, exe_hash, 32);
    printf("Verbinde ... (bis zu %d s, beide muessen gleichzeitig verbinden)\n", timeout_s);
    while (GetTickCount() - start < (DWORD)timeout_s * 1000) {
        if (GetTickCount() - last > 100) {
            h.seq = got_hello;  // seq=1 heisst: "ich habe dich schon gehoert"
            if (got_hello) send_to(&s_peer, &h, sizeof h);
            else {
                // Direktmodus-Host kennt den Gast noch nicht (Adresse 0) und wartet nur
                if (o->peer_pub.sin_addr.s_addr) send_to(&o->peer_pub, &h, sizeof h);
                if (o->peer_local.sin_addr.s_addr) send_to(&o->peer_local, &h, sizeof h);
            }
            last = GetTickCount();
        }
        n = recv_any(&in, sizeof in, &from, 50);
        if (n != (int)sizeof in || in.magic != LMAGIC || in.type != M_HELLO || in.key != s_key_peer) continue;
        if (!got_hello) {
            s_peer = from;  // die Adresse, ueber die es tatsaechlich klappt
            printf("Verbunden mit %s:%d\n", inet_ntoa(from.sin_addr), ntohs(from.sin_port));
        }
        got_hello = 1;
        if (in.version != MOD_VERSION) { printf("Fehler: unterschiedliche Mod-Versionen (%u vs. %u).\n", MOD_VERSION, in.version); return 0; }
        if (memcmp(in.exe_hash, exe_hash, 32)) { printf("Fehler: Die Spielversionen unterscheiden sich (LEGOBatman.exe). Beide brauchen die aktuelle Steam-Version.\n"); return 0; }
        if (in.role == o->role) { printf("Fehler: Beide sind %s. Einer muss hosten, einer beitreten.\n", o->role ? "Gast" : "Host"); return 0; }
        if (in.seq) got_ack = 1;
        if (got_ack) {
            int i;
            h.seq = 1;
            for (i = 0; i < 5; i++) send_to(&s_peer, &h, sizeof h);  // Partner soll uns sicher hoeren
            o->peer = s_peer;
            return 1;
        }
    }
    printf("Keine Verbindung zustande gekommen.\n");
    return 0;
}

int online_measure_ping(ONLINE *o)
{
    // 20 Pings, Median der Rundlaufzeit in ms. Beide Seiten antworten auf Pings.
    PING p, in;
    struct sockaddr_in from;
    int rtt[20], got = 0, i, j, n;
    LARGE_INTEGER f, t;
    DWORD start = GetTickCount();
    QueryPerformanceFrequency(&f);
    for (i = 0; i < 20 && GetTickCount() - start < 8000;) {
        QueryPerformanceCounter(&t);
        memset(&p, 0, sizeof p);
        p.magic = LMAGIC; p.type = M_PING; p.key = s_key_me; p.seq = i; p.t = (unsigned long long)t.QuadPart;
        send_to(&o->peer, &p, sizeof p);
        while ((n = recv_any(&in, sizeof in, &from, 150)) > 0) {
            if (n != (int)sizeof in || in.magic != LMAGIC || in.key != s_key_peer) continue;
            if (in.type == M_PING) { in.type = M_PONG; in.key = s_key_me; send_to(&o->peer, &in, sizeof in); continue; }
            if (in.type == M_PONG && in.seq == (unsigned)i) {
                QueryPerformanceCounter(&t);
                rtt[got++] = (int)((t.QuadPart - (LONGLONG)in.t) * 1000 / f.QuadPart);
                break;
            }
        }
        i++;
    }
    // noch kurz auf Pings des Partners antworten
    start = GetTickCount();
    while (GetTickCount() - start < 1500) {
        n = recv_any(&in, sizeof in, &from, 50);
        if (n == (int)sizeof in && in.magic == LMAGIC && in.key == s_key_peer && in.type == M_PING) {
            in.type = M_PONG; in.key = s_key_me; send_to(&o->peer, &in, sizeof in);
        }
    }
    if (!got) return -1;
    for (i = 0; i < got; i++)
        for (j = i + 1; j < got; j++)
            if (rtt[j] < rtt[i]) { int x = rtt[i]; rtt[i] = rtt[j]; rtt[j] = x; }
    // 80. Perzentil statt Median: Bei WLAN streut die Rundlaufzeit stark, der Median unterschaetzt sie. Im Spiel
    // passt sich die Verzoegerung danach ohnehin laufend an (net.c), das hier ist nur der Startwert.
    o->ping_ms = rtt[(got - 1) * 4 / 5];
    return o->ping_ms;
}

// --- Spielstand uebertragen (Host -> Gast) ---------------------------------------------
int online_send_saves(ONLINE *o, const char *save_dir)
{
    char pattern[MAX_PATH], path[MAX_PATH];
    WIN32_FIND_DATAA fd;
    HANDLE fh;
    char names[16][64];
    unsigned char *data[16];
    unsigned sizes[16];
    int nfiles = 0, i;
    snprintf(pattern, sizeof pattern, "%s\\SaveGame*", save_dir);
    fh = FindFirstFileA(pattern, &fd);
    if (fh != INVALID_HANDLE_VALUE) {
        do {
            FILE *f;
            if (nfiles >= 16 || fd.nFileSizeLow > 1000 * 64 * 8 || strlen(fd.cFileName) >= 64) continue;
            snprintf(path, sizeof path, "%s\\%s", save_dir, fd.cFileName);
            f = fopen(path, "rb");
            if (!f) continue;
            data[nfiles] = (unsigned char *)malloc(fd.nFileSizeLow + 1);
            sizes[nfiles] = (unsigned)fread(data[nfiles], 1, fd.nFileSizeLow, f);
            fclose(f);
            strcpy(names[nfiles], fd.cFileName);
            nfiles++;
        } while (FindNextFileA(fh, &fd));
        FindClose(fh);
    }
    printf("Sende %d Spielstand-Datei(en) an den Gast ...\n", nfiles);
    // Datei 0..n-1 nacheinander; Gast meldet per HAVE, welche Stuecke fehlen. Leere Liste: nfiles=0.
    for (i = 0; i <= nfiles; i++) {
        unsigned nch = i < nfiles ? (sizes[i] + 999) / 1000 : 1;
        unsigned char have[64];
        DWORD start = GetTickCount();
        memset(have, 0, sizeof have);
        for (;;) {
            unsigned c;
            HAVE in;
            struct sockaddr_in from;
            int n, all = 1;
            for (c = 0; c < nch; c++) {
                FILEPKT p;
                if (have[c / 8] & (1 << (c % 8))) continue;
                all = 0;
                memset(&p, 0, sizeof p);
                p.magic = LMAGIC; p.type = M_FILE; p.key = s_key_me;
                p.file = i; p.nfiles = nfiles; p.chunk = c; p.nchunks = nch;
                if (i < nfiles) {
                    p.size = sizes[i];
                    strcpy(p.name, names[i]);
                    p.len = (unsigned short)(c + 1 < nch ? 1000 : sizes[i] - c * 1000);
                    memcpy(p.data, data[i] + c * 1000, p.len);
                }
                send_to(&o->peer, &p, (int)(sizeof p - sizeof p.data + p.len));
            }
            while ((n = recv_any(&in, sizeof in, &from, 60)) > 0) {
                if (n >= 12 && in.magic == LMAGIC && in.key == s_key_peer && in.type == M_READY) {
                    // Der Gast ist schon fertig und wartet; seine letzte Meldung ging verloren
                    for (; i < nfiles; i++) free(data[i]);
                    printf("Spielstand uebertragen.\n");
                    return 1;
                }
                if (n != (int)sizeof in || in.magic != LMAGIC || in.key != s_key_peer || in.type != M_HAVE || in.file != (unsigned)i) continue;
                memcpy(have, in.bits, sizeof have);
                if (in.done) { all = 1; break; }
            }
            if (all && i <= nfiles) {
                // fertig, wenn der Gast "done" fuer diese Datei gemeldet hat
                int k, complete = 1;
                for (k = 0; k < (int)nch; k++) if (!(have[k / 8] & (1 << (k % 8)))) complete = 0;
                if (complete) break;
            }
            if (GetTickCount() - start > 30000) { printf("Fehler: Spielstand-Uebertragung abgebrochen.\n"); return 0; }
        }
        if (i < nfiles) free(data[i]);
    }
    printf("Spielstand uebertragen.\n");
    return 1;
}

int online_receive_saves(ONLINE *o, const char *dest_dir)
{
    int file = 0, nfiles = -1;
    unsigned char have[64];
    unsigned char *buf = NULL;
    unsigned size = 0, nch = 0;
    char name[64] = "";
    DWORD start = GetTickCount(), last_have = 0;
    CreateDirectoryA(dest_dir, NULL);
    memset(have, 0, sizeof have);
    printf("Empfange den Spielstand des Hosts ...\n");
    while (GetTickCount() - start < 40000) {
        FILEPKT p;
        struct sockaddr_in from;
        int n = recv_any(&p, sizeof p, &from, 50);
        HAVE h;
        int complete = 0, k;
        if (n > 0 && p.magic == LMAGIC && p.key == s_key_peer && p.type == M_FILE && p.file == (unsigned)file && p.chunk < 512) {
            if (nfiles < 0) nfiles = (int)p.nfiles;
            if (!buf && p.file < p.nfiles) {
                size = p.size; nch = p.nchunks;
                memcpy(name, p.name, sizeof name); name[63] = 0;
                buf = (unsigned char *)calloc(size + 1, 1);
            }
            if (p.file >= p.nfiles) nch = 1;
            if (buf && p.chunk * 1000 + p.len <= size) memcpy(buf + p.chunk * 1000, p.data, p.len);
            have[p.chunk / 8] |= (unsigned char)(1 << (p.chunk % 8));
        }
        if (nch) {
            complete = 1;
            for (k = 0; k < (int)nch; k++) if (!(have[k / 8] & (1 << (k % 8)))) complete = 0;
        }
        if (complete || GetTickCount() - last_have > 100) {
            memset(&h, 0, sizeof h);
            h.magic = LMAGIC; h.type = M_HAVE; h.key = s_key_me; h.file = file; h.done = complete;
            memcpy(h.bits, have, sizeof have);
            send_to(&o->peer, &h, sizeof h);
            last_have = GetTickCount();
        }
        if (complete) {
            int i;
            for (i = 0; i < 3; i++) send_to(&o->peer, &h, sizeof h);
            if (buf && strchr(name, '\\') == NULL && strchr(name, '/') == NULL && strstr(name, "..") == NULL) {
                char path[MAX_PATH];
                FILE *f;
                snprintf(path, sizeof path, "%s\\%s", dest_dir, name);
                f = fopen(path, "wb");
                if (f) { fwrite(buf, 1, size, f); fclose(f); }
                printf("  %s (%u Bytes)\n", name, size);
            }
            free(buf); buf = NULL; nch = 0;
            memset(have, 0, sizeof have);
            file++;
            if (file > nfiles) { printf("Spielstand empfangen.\n"); return 1; }
        }
    }
    printf("Fehler: Spielstand nicht vollstaendig empfangen.\n");
    return 0;
}

int online_ready_barrier(ONLINE *o, int *delay)
{
    // Beide melden "bereit"; danach starten beide ihr Spiel.
    PING p, in;
    struct sockaddr_in from;
    DWORD start = GetTickCount(), last = 0;
    int got = 0, n;
    memset(&p, 0, sizeof p);
    p.magic = LMAGIC; p.type = M_READY; p.key = s_key_me;
    p.t = (unsigned long long)*delay;  // Host gibt die Verzoegerung vor
    while (GetTickCount() - start < 30000) {
        if (GetTickCount() - last > 50) { p.seq = got; send_to(&o->peer, &p, sizeof p); last = GetTickCount(); }
        n = recv_any(&in, sizeof in, &from, 20);
        if (n == (int)sizeof in && in.magic == LMAGIC && in.key == s_key_peer && in.type == M_READY) {
            got = 1;
            if (o->role == 1 && in.t >= 1 && in.t <= 16) *delay = (int)in.t;
            if (in.seq) {
                int i;
                p.seq = 1;
                for (i = 0; i < 5; i++) send_to(&o->peer, &p, sizeof p);
                return 1;
            }
        }
    }
    return 0;
}

void online_close(void)
{
    if (s_sock != INVALID_SOCKET) closesocket(s_sock);
    s_sock = INVALID_SOCKET;
}
