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
//
// Sicherheit (23.10.2026): Nach dem Verbinden zaehlen nur Pakete von der Adresse des Partners; Dateipakete werden
// gegen Groesse, Stueckzahl und Laenge geprueft, Dateinamen nur aus [A-Za-z0-9._ -].
#define _WINSOCK_DEPRECATED_NO_WARNINGS
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#include <bcrypt.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "online.h"

#define LMAGIC 0x4C4E4C42u  // Launcher-Pakete "BLNL"
// 2: READY mit Einstellungs-Hash, Batman 2 schickt immer genau eine pcconfig.txt (Desync 23.10.2026)
#define MOD_VERSION 2
enum { M_HELLO = 1, M_PING, M_PONG, M_FILE, M_HAVE, M_READY };
#define MAX_FILE_SIZE (1000 * 512)   // 512 Stuecke a 1000 Byte (Bitfeld in HAVE)

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
// Bereit-Meldung: t = Verzoegerung (Host gibt sie vor), cfg = SHA-256 der Netz-Einstellungen (Batman 1: Nullen),
// src = Quelle beim Host (B2CFG_SRC_*). Version 1 schickte nur PING-Groesse.
typedef struct {
    unsigned magic, type, key;
    unsigned seq;
    unsigned long long t;
    unsigned version, src;
    unsigned char cfg[32];
} READY;
#pragma pack(pop)
#define FILEPKT_HDR ((int)offsetof(FILEPKT, data))

static SOCKET s_sock = INVALID_SOCKET;
char online_error[160];   // Grund, wenn online_connect an Version, Spiel oder Rolle scheitert (sonst leer)
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

// Nach online_connect: nur Pakete von der Adresse, ueber die die Verbindung steht
static int from_peer(const struct sockaddr_in *from, const ONLINE *o)
{
    return from->sin_addr.s_addr == o->peer.sin_addr.s_addr && from->sin_port == o->peer.sin_port;
}

// Dateinamen vom Host: nur [A-Za-z0-9._ -], kein "..", nicht leer (keine Pfade, NTFS-Streams, Geraetenamen mit ':')
static int valid_name(const char *n)
{
    size_t i, l = strlen(n);
    if (!l || l >= 64 || strstr(n, "..")) return 0;
    for (i = 0; i < l; i++) {
        char c = n[i];
        if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '.' || c == '_' || c == ' ' || c == '-'))
            return 0;
    }
    return 1;
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
        if (in.version != MOD_VERSION) {
            snprintf(online_error, sizeof online_error, "Ihr habt verschiedene Versionen der Mod (%u und %u). Bitte beide die neueste installieren.",
                     MOD_VERSION, in.version);
            printf("Fehler: %s\n", online_error);
            return 0;
        }
        if (memcmp(in.exe_hash, exe_hash, 32)) {
            snprintf(online_error, sizeof online_error, "Die Spielversionen unterscheiden sich. Beide brauchen die aktuelle Steam-Version.");
            printf("Fehler: %s\n", online_error);
            return 0;
        }
        if (in.role == o->role) {
            snprintf(online_error, sizeof online_error, "Beide sind %s. Einer muss hosten, einer beitreten.", o->role ? "Gast" : "Host");
            printf("Fehler: %s\n", online_error);
            return 0;
        }
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
            if (n != (int)sizeof in || !from_peer(&from, o) || in.magic != LMAGIC || in.key != s_key_peer) continue;
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
        if (n == (int)sizeof in && from_peer(&from, o) && in.magic == LMAGIC && in.key == s_key_peer && in.type == M_PING) {
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

// --- Dateien uebertragen (Host -> Gast) -----------------------------------------------
// Datei 0..n-1 nacheinander, danach eine Abschluss-Runde (file == nfiles, ohne Daten). Der Gast meldet per HAVE,
// welche Stuecke er hat, und "done", wenn eine Datei vollstaendig ist. Eine leere Datei ist ein Stueck mit Laenge 0.
static int send_files(ONLINE *o, int nfiles, char names[][64], unsigned char **data, const unsigned *sizes)
{
    int i;
    printf("Sende %d Datei(en) an den Gast ...\n", nfiles);
    for (i = 0; i <= nfiles; i++) {
        unsigned nch = i < nfiles ? (sizes[i] + 999) / 1000 : 1;
        unsigned char have[64];
        DWORD start = GetTickCount();
        if (nch == 0) nch = 1;
        memset(have, 0, sizeof have);
        for (;;) {
            unsigned c;
            HAVE in;
            struct sockaddr_in from;
            int n, done = 0;
            for (c = 0; c < nch; c++) {
                FILEPKT p;
                if (have[c / 8] & (1 << (c % 8))) continue;
                memset(&p, 0, sizeof p);
                p.magic = LMAGIC; p.type = M_FILE; p.key = s_key_me;
                p.file = i; p.nfiles = nfiles; p.chunk = c; p.nchunks = nch;
                if (i < nfiles) {
                    p.size = sizes[i];
                    strcpy(p.name, names[i]);
                    p.len = (unsigned short)(c + 1 < nch ? 1000 : sizes[i] - c * 1000);
                    memcpy(p.data, data[i] + c * 1000, p.len);
                }
                send_to(&o->peer, &p, FILEPKT_HDR + p.len);
            }
            while ((n = recv_any(&in, sizeof in, &from, 60)) > 0) {
                if (n < 12 || !from_peer(&from, o) || in.magic != LMAGIC || in.key != s_key_peer) continue;
                if (in.type == M_READY) {
                    // Gast wartet schon an der Bereit-Schranke: Nur in der Abschluss-Runde heisst das "fertig" (seine
                    // letzte Meldung ging verloren). Vorher waere es ein Gast ohne Empfang (alte Version), dann
                    // entscheidet der Hash-Vergleich an der Schranke bzw. die Zeitgrenze hier.
                    if (i == nfiles) { printf("Dateien uebertragen.\n"); return 1; }
                    continue;
                }
                if (n != (int)sizeof in || in.type != M_HAVE) continue;
                if (in.file > (unsigned)i && in.file <= (unsigned)nfiles) { done = 1; break; }   // Gast ist schon bei der naechsten
                if (in.file != (unsigned)i) continue;
                memcpy(have, in.bits, sizeof have);
                if (in.done) { done = 1; break; }
            }
            if (done) break;
            if (GetTickCount() - start > 30000) {
                printf("Fehler: Uebertragung abgebrochen (Gast bestaetigt Datei %d von %d nicht).\n", i + 1, nfiles);
                return 0;
            }
        }
    }
    printf("Dateien uebertragen.\n");
    return 1;
}

int online_send_saves(ONLINE *o, const char *save_dir, const char *file_pattern)
{
    char pattern[MAX_PATH], path[MAX_PATH];
    WIN32_FIND_DATAA fd;
    HANDLE fh;
    char names[16][64];
    unsigned char *data[16];
    unsigned sizes[16];
    int nfiles = 0, i, ok;
    snprintf(pattern, sizeof pattern, "%s\\%s", save_dir, file_pattern);
    fh = FindFirstFileA(pattern, &fd);
    if (fh != INVALID_HANDLE_VALUE) {
        do {
            FILE *f;
            if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) continue;
            if (nfiles >= 16 || fd.nFileSizeLow > MAX_FILE_SIZE || !valid_name(fd.cFileName)) {
                printf("Datei %s wird nicht gesendet (zu gross, zu viele oder ungueltiger Name).\n", fd.cFileName);
                continue;
            }
            snprintf(path, sizeof path, "%s\\%s", save_dir, fd.cFileName);
            f = fopen(path, "rb");
            if (!f) { printf("Datei %s nicht lesbar (Fehler %lu), wird nicht gesendet.\n", fd.cFileName, GetLastError()); continue; }
            data[nfiles] = (unsigned char *)malloc(fd.nFileSizeLow + 1);
            if (!data[nfiles]) { fclose(f); continue; }
            sizes[nfiles] = (unsigned)fread(data[nfiles], 1, fd.nFileSizeLow, f);
            fclose(f);
            if (sizes[nfiles] != fd.nFileSizeLow) printf("Datei %s nur teilweise gelesen (%u von %lu Bytes).\n", fd.cFileName, sizes[nfiles], fd.nFileSizeLow);
            strcpy(names[nfiles], fd.cFileName);
            nfiles++;
        } while (FindNextFileA(fh, &fd));
        FindClose(fh);
    }
    ok = send_files(o, nfiles, names, data, sizes);
    for (i = 0; i < nfiles; i++) free(data[i]);
    return ok;
}

int online_send_blob(ONLINE *o, const char *name, const void *data, unsigned size)
{
    char names[1][64];
    unsigned char *d[1];
    if (size > MAX_FILE_SIZE || !valid_name(name)) return 0;
    strcpy(names[0], name);
    d[0] = (unsigned char *)data;
    return send_files(o, 1, names, d, &size);
}

typedef int (*STOREFN)(void *ctx, const char *name, const unsigned char *data, unsigned size);

// Empfang. want_nfiles >= 0: genau so viele Dateien, want_name: nur dieser Name (sonst Abbruch). max_size <= 512000.
static int recv_files(ONLINE *o, unsigned max_size, int want_nfiles, const char *want_name, STOREFN store, void *ctx)
{
    int file = 0, nfiles = -1;
    unsigned char have[64];
    unsigned char *buf = NULL;
    unsigned size = 0, nch = 0;
    char name[64] = "";
    DWORD start = GetTickCount(), last_have = 0;
    if (max_size > MAX_FILE_SIZE) max_size = MAX_FILE_SIZE;
    memset(have, 0, sizeof have);
    printf("Empfange Dateien des Hosts ...\n");
    while (GetTickCount() - start < 40000) {
        FILEPKT p;
        struct sockaddr_in from;
        int n = recv_any(&p, sizeof p, &from, 50);
        HAVE h;
        int complete = 0, k;
        if (n >= FILEPKT_HDR && from_peer(&from, o) && p.magic == LMAGIC && p.key == s_key_peer && p.type == M_FILE &&
            p.file == (unsigned)file && p.len <= 1000 && n == FILEPKT_HDR + p.len && p.nfiles <= 16) {
            if (nfiles < 0) {
                nfiles = (int)p.nfiles;
                if (want_nfiles >= 0 && nfiles != want_nfiles) {
                    printf("Fehler: Der Host schickt %d Datei(en), erwartet %d (alte Version beim Host?).\n", nfiles, want_nfiles);
                    return 0;
                }
            }
            if (p.nfiles != (unsigned)nfiles) {
                // passt nicht zum ersten Paket: verwerfen
            } else if (p.file >= p.nfiles) {
                if (p.chunk == 0) { nch = 1; have[0] |= 1; }   // Abschluss-Runde
            } else if (p.size <= max_size && p.nchunks == (p.size ? (p.size + 999) / 1000 : 1) && p.chunk < p.nchunks) {
                p.name[63] = 0;
                if (!buf) {
                    if (!valid_name(p.name) || (want_name && _stricmp(p.name, want_name))) {
                        printf("Fehler: Der Host schickt eine unerwartete Datei (%s).\n", valid_name(p.name) ? p.name : "ungueltiger Name");
                        return 0;
                    }
                    size = p.size; nch = p.nchunks;
                    strcpy(name, p.name);
                    buf = (unsigned char *)calloc(size + 1, 1);
                    if (!buf) { printf("Fehler: kein Speicher fuer %s.\n", name); return 0; }
                }
                // Jedes Stueck muss zu Groesse, Name und seiner Position passen
                if (p.size == size && p.nchunks == nch && !strcmp(p.name, name) &&
                    p.len == (p.chunk + 1 < nch ? 1000u : size - p.chunk * 1000)) {
                    memcpy(buf + p.chunk * 1000, p.data, p.len);
                    have[p.chunk / 8] |= (unsigned char)(1 << (p.chunk % 8));
                }
            }
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
            if (buf) {
                if (!store(ctx, name, buf, size)) { free(buf); return 0; }
                printf("  %s (%u Bytes)\n", name, size);
            }
            free(buf); buf = NULL; nch = 0;
            memset(have, 0, sizeof have);
            file++;
            if (file > nfiles) { printf("Dateien empfangen.\n"); return 1; }
        }
    }
    free(buf);
    printf("Fehler: Dateien des Hosts nicht vollstaendig empfangen (%s).\n",
           nfiles < 0 ? "nichts angekommen" : "Zeitgrenze 40 s");
    return 0;
}

static int store_to_dir(void *ctx, const char *name, const unsigned char *data, unsigned size)
{
    char path[MAX_PATH];
    FILE *f;
    size_t w;
    snprintf(path, sizeof path, "%s\\%s", (const char *)ctx, name);
    f = fopen(path, "wb");
    if (!f) { printf("Fehler: %s kann nicht geschrieben werden (Fehler %lu).\n", path, GetLastError()); return 0; }
    w = fwrite(data, 1, size, f);
    if (fclose(f) != 0 || w != size) { printf("Fehler: %s nicht vollstaendig geschrieben.\n", path); return 0; }
    return 1;
}

int online_receive_saves(ONLINE *o, const char *dest_dir)
{
    CreateDirectoryA(dest_dir, NULL);
    return recv_files(o, MAX_FILE_SIZE, -1, NULL, store_to_dir, (void *)dest_dir);
}

typedef struct { unsigned char *buf; unsigned cap, len; } RXBUF;

static int store_to_blob(void *ctx, const char *name, const unsigned char *data, unsigned size)
{
    RXBUF *b = (RXBUF *)ctx;
    (void)name;
    if (size > b->cap) return 0;
    memcpy(b->buf, data, size);
    b->len = size;
    return 1;
}

int online_receive_blob(ONLINE *o, const char *name, void *buf, unsigned cap, unsigned *len)
{
    RXBUF b;
    b.buf = (unsigned char *)buf; b.cap = cap; b.len = 0;
    *len = 0;
    if (!recv_files(o, cap, 1, name, store_to_blob, &b)) return 0;
    *len = b.len;
    return 1;
}

int online_ready_barrier(ONLINE *o, int *delay, const unsigned char my_cfg[32], unsigned my_src,
                         unsigned char peer_cfg[32], unsigned *peer_src)
{
    // Beide melden "bereit" mit dem Hash ihrer Einstellungen; danach starten beide ihr Spiel. Weichen die Hashes
    // ab, startet keiner (beide sehen den Hash des anderen: wer die Abweichung zuerst sieht, schickt seine Meldung
    // noch fuenfmal, damit der Partner sie auch bekommt).
    READY p, in;
    struct sockaddr_in from;
    DWORD start = GetTickCount(), last = 0;
    int got = 0, n;
    memset(&p, 0, sizeof p);
    p.magic = LMAGIC; p.type = M_READY; p.key = s_key_me;
    p.t = (unsigned long long)*delay;  // Host gibt die Verzoegerung vor
    p.version = MOD_VERSION;
    p.src = my_src;
    if (my_cfg) memcpy(p.cfg, my_cfg, 32);
    while (GetTickCount() - start < 30000) {
        if (GetTickCount() - last > 50) { p.seq = got; send_to(&o->peer, &p, sizeof p); last = GetTickCount(); }
        n = recv_any(&in, sizeof in, &from, 20);
        if (n < 12 || !from_peer(&from, o) || in.magic != LMAGIC || in.key != s_key_peer || in.type != M_READY) continue;
        if (n == (int)sizeof(PING) || (n == (int)sizeof in && in.version != MOD_VERSION)) {
            printf("Fehler: Dein Mitspieler hat eine andere Version von LEGO Online (Bereit-Meldung Version %u, hier %u).\n",
                   n == (int)sizeof in ? in.version : 1, MOD_VERSION);
            return -2;
        }
        if (n != (int)sizeof in) continue;
        got = 1;
        if (peer_cfg) memcpy(peer_cfg, in.cfg, 32);
        if (peer_src) *peer_src = in.src;
        if (memcmp(in.cfg, p.cfg, 32)) {
            int i;
            p.seq = 1;
            for (i = 0; i < 5; i++) send_to(&o->peer, &p, sizeof p);
            return -1;
        }
        if (o->role == 1 && in.t >= 1 && in.t <= 16) *delay = (int)in.t;
        if (in.seq) {
            int i;
            p.seq = 1;
            for (i = 0; i < 5; i++) send_to(&o->peer, &p, sizeof p);
            return 1;
        }
    }
    return 0;
}

void online_close(void)
{
    if (s_sock != INVALID_SOCKET) closesocket(s_sock);
    s_sock = INVALID_SOCKET;
}
