// net.c: Lockstep-Eingabesynchronisation ueber UDP (Phase 2, Prototyp).
//
// Zwei Instanzen tauschen pro Frame ihre lokale Eingabe aus. Der Host steuert Pad 0 (Spieler 1),
// der Client Pad 1 (Spieler 2). Eine lokal bei Frame F abgetastete Eingabe gilt erst ab Frame
// F + Verzoegerung, damit sie rechtzeitig beim Partner ankommt. Jeder Frame wartet, bis die
// Eingabe des Partners fuer genau diesen Frame da ist (Barriere). Jedes Paket enthaelt die
// letzten 16 Eingaben, verlorene Pakete werden so automatisch ersetzt.
//
// Umgebungsvariablen:
//   LBO_NET    host | join
//   LBO_PORT   lokaler UDP-Port (Standard 47100 Host, 47101 Client)
//   LBO_PEER   Adresse des Partners, z. B. 127.0.0.1:47101
//   LBO_DELAY  Eingabeverzoegerung in Frames (Standard 3)
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "net.h"

#define MAGIC 0x314F424C  // "LBO1"
#define RING 512
#define REDUNDANCY 16

#pragma pack(push, 1)
typedef struct { NETPAD in[REDUNDANCY]; } INPUTS;
typedef struct {
    unsigned magic;
    unsigned type;          // 1 = Eingaben
    int last_frame;         // letzter enthaltener Frame (in[count-1])
    int count;
    int ck_frame;           // Frame der mitgeschickten Pruefsumme (0 = keine)
    unsigned long long ck;  // Pruefsumme
    NETPAD in[REDUNDANCY];
} PACKET;
typedef struct {
    unsigned magic;
    unsigned type;          // 2 = Zustandsabgleich
    int frame;
    int len;
    unsigned char data[256];
} SYNCPACKET;
#pragma pack(pop)

static SYNCPACKET g_sync_out, g_sync_in;   // zuletzt gesendeter / empfangener Abgleich
static volatile int g_sync_in_frame;
// Schranke (net_barrier): Typ 3 = Zustand des Hosts, Typ 4 = Gast ist angekommen. Danach beginnt eine
// neue Epoche; Eingabepakete tragen sie im oberen Teil von "type", aeltere werden verworfen.
static int g_epoch;
static SYNCPACKET g_bar_out, g_bar_in;
static volatile int g_bar_seen;    // Host: Gast angekommen; Gast: Zustand des Hosts erhalten
static int g_bar_resend;           // Schranken-Paket noch so oft mitsenden
static SOCKET g_sock = INVALID_SOCKET;
static struct sockaddr_in g_peer;
static int g_role;   // 0 = Host (Pad 0), 1 = Client (Pad 1)
static int g_delay = 3;
static int g_partner_timeout_ms;    // 0 = nie aufgeben
static volatile LONG g_partner_lost;
static volatile DWORD g_last_rx;    // GetTickCount() des letzten Pakets vom Partner (auch Lebenszeichen)
static int g_loss;        // LBO_NETLOSS: Prozent ausgehender Pakete verwerfen (nur zum Testen)
static long g_dropped;
static NETPAD g_local[RING], g_remote[RING];
static int g_local_upto = 0;    // hoechster Frame mit lokaler Eingabe
static int g_remote_upto = 0;   // hoechster lueckenlos bekannter Partner-Frame
static unsigned char g_remote_have[RING];
static unsigned long long g_my_ck[RING], g_peer_ck[RING];
static int g_my_ck_frame[RING], g_peer_ck_frame[RING];
static int g_last_ck_sent;
static long g_stalls, g_desyncs;
static void (*g_log)(const char *fmt, ...);

int net_role(void) { return g_role; }

// Bis zum ersten Frame vergehen einige Sekunden (Spielstart). In der Zeit halten wir die
// Zuordnung im Router des Partners offen, sonst wuerden seine ersten Pakete verworfen.
static volatile LONG g_started;
static HANDLE g_keepalive;
static void send_inputs(void);
static DWORD WINAPI keepalive(void *arg)
{
    (void)arg;
    while (!g_started) {
        send_inputs();
        Sleep(200);
    }
    // Danach (nur mit Partner-Zeitgrenze) ein Lebenszeichen je halbe Sekunde, auch wenn das Spiel steht
    // (Alt-Tab, Ladehaenger). So gilt der Partner nur als verloren, wenn wirklich nichts mehr ankommt.
    // Gilt er als verloren, schweigen wir auch: Kaeme er zurueck, liefe sein Spiel sonst nie weiter.
    while (!g_partner_lost) {
        if (g_partner_timeout_ms) {
            unsigned ping[2] = { MAGIC, 5 };
            sendto(g_sock, (const char *)ping, sizeof ping, 0, (struct sockaddr *)&g_peer, sizeof g_peer);
        }
        Sleep(500);
    }
    return 0;
}

int net_init(void (*logger)(const char *fmt, ...))
{
    char buf[128], host[64];
    int port, peer_port;
    WSADATA wsa;
    u_long nb = 1;
    struct sockaddr_in me;
    g_log = logger;
    if (!GetEnvironmentVariableA("LBO_NET", buf, sizeof buf)) return 0;
    if (!_stricmp(buf, "host")) g_role = 0;
    else if (!_stricmp(buf, "join")) g_role = 1;
    else return 0;
    port = g_role ? 47101 : 47100;
    if (GetEnvironmentVariableA("LBO_PORT", buf, sizeof buf)) port = atoi(buf);
    if (GetEnvironmentVariableA("LBO_DELAY", buf, sizeof buf)) g_delay = atoi(buf);
    if (GetEnvironmentVariableA("LBO_NETLOSS", buf, sizeof buf)) g_loss = atoi(buf);
    srand(GetCurrentProcessId());  // eigene CRT der DLL, beruehrt den Zufall des Spiels nicht
    if (g_delay < 1) g_delay = 1;
    if (g_delay > REDUNDANCY / 2) g_delay = REDUNDANCY / 2;
    strcpy(host, "127.0.0.1");
    peer_port = g_role ? 47100 : 47101;
    if (GetEnvironmentVariableA("LBO_PEER", buf, sizeof buf)) {
        char *colon = strrchr(buf, ':');
        if (colon) { *colon = 0; peer_port = atoi(colon + 1); }
        strncpy(host, buf, sizeof host - 1);
    }
    WSAStartup(MAKEWORD(2, 2), &wsa);
    g_sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    memset(&me, 0, sizeof me);
    me.sin_family = AF_INET;
    me.sin_port = htons((u_short)port);
    me.sin_addr.s_addr = htonl(INADDR_ANY);
    if (bind(g_sock, (struct sockaddr *)&me, sizeof me)) {
        g_log("Netz: bind auf Port %d fehlgeschlagen (%d)", port, WSAGetLastError());
        return 0;
    }
    ioctlsocket(g_sock, FIONBIO, &nb);
    memset(&g_peer, 0, sizeof g_peer);
    g_peer.sin_family = AF_INET;
    g_peer.sin_port = htons((u_short)peer_port);
    inet_pton(AF_INET, host, &g_peer.sin_addr);
    // Die ersten <Verzoegerung> Frames haben auf beiden Seiten neutrale Eingabe
    g_local_upto = g_delay;
    g_remote_upto = g_delay;
    memset(g_remote_have, 0, sizeof g_remote_have);
    for (port = 1; port <= g_delay; port++) g_remote_have[port % RING] = 1;
    if (g_loss) g_log("Netz-Test: simuliere %d %% Paketverlust", g_loss);
    g_last_rx = GetTickCount();
    g_keepalive = CreateThread(NULL, 0, keepalive, NULL, 0, NULL);
    g_log("Netz: Rolle %s, Port %d, Partner %s:%d, Verzoegerung %d Frames",
          g_role ? "Client (Spieler 2)" : "Host (Spieler 1)", ntohs(me.sin_port), host, peer_port, g_delay);
    return 1;
}

static void send_inputs(void)
{
    PACKET p;
    int i, first;
    if (g_partner_lost) return;
    memset(&p, 0, sizeof p);
    p.magic = MAGIC;
    p.type = 1 | ((unsigned)g_epoch << 16);
    p.last_frame = g_local_upto;
    p.count = g_local_upto < REDUNDANCY ? g_local_upto : REDUNDANCY;
    first = g_local_upto - p.count + 1;
    for (i = 0; i < p.count; i++) p.in[i] = g_local[(first + i) % RING];
    if (g_last_ck_sent) {
        p.ck_frame = g_last_ck_sent;
        p.ck = g_my_ck[g_last_ck_sent % RING];
    }
    if (g_loss && (rand() % 100) < g_loss) { g_dropped++; return; }  // Testmodus: Paketverlust
    sendto(g_sock, (const char *)&p, sizeof p, 0, (struct sockaddr *)&g_peer, sizeof g_peer);
    // Den letzten Abgleich eine Weile mitsenden, bis der Gast ihn sicher hat (Pakete koennen verloren gehen)
    if (g_sync_out.magic && g_local_upto - g_sync_out.frame < 600)
        sendto(g_sock, (const char *)&g_sync_out, sizeof g_sync_out, 0, (struct sockaddr *)&g_peer, sizeof g_peer);
    if (g_bar_resend > 0) {
        g_bar_resend--;
        sendto(g_sock, (const char *)&g_bar_out, sizeof g_bar_out, 0, (struct sockaddr *)&g_peer, sizeof g_peer);
    }
}

static void receive(void);

// Schranke, z. B. am Levelstart (LEGO Batman 2): Beide Seiten kommen hier an, auch wenn ihre Frames vorher
// durch Ladezeiten etwas auseinanderliefen. Der Host gibt seinen Zustand mit (data), der Gast bekommt ihn.
// Wer zuerst ankommt, verlaengert seine Eingaben mit neutralen Pads, damit der Partner bis zur Schranke
// weiterlaufen kann. Danach beginnt eine neue Epoche mit Frame 1 (gleiche Verzoegerung wie am Anfang).
int net_barrier(int F, void *data, int len, int timeout_ms)
{
    DWORD start = GetTickCount(), last_send = 0, last_log = start;
    NETPAD neutral;
    int i;
    if (len > 256) len = 256;
    memset(&neutral, 0, sizeof neutral);
    neutral.connected = 1;
    memset(&g_bar_out, 0, sizeof g_bar_out);
    g_bar_out.magic = MAGIC;
    g_bar_out.type = g_role == 0 ? 3 : 4;
    g_bar_out.frame = F;
    g_bar_out.len = g_role == 0 ? len : 0;
    if (g_role == 0) memcpy(g_bar_out.data, data, len);
    g_bar_resend = 1 << 30;
    g_bar_seen = 0;   // Pakete von vor unserer Ankunft zaehlen nicht, der Partner sendet weiter
    g_log("Netz: an der Schranke (Frame %d), warte auf Partner", F);
    while (!g_bar_seen) {
        DWORD now = GetTickCount();
        if (now - start > (DWORD)timeout_ms) { g_log("Netz: Partner kam nicht an der Schranke an"); return 0; }
        // Partner braucht unsere Eingaben bis zu seinem Frame (er hat seine bis g_remote_upto geschickt)
        while (g_local_upto < g_remote_upto + 2) {
            g_local_upto++;
            g_local[g_local_upto % RING] = neutral;
        }
        if (now - last_send >= 4) { send_inputs(); last_send = now; }
        if (now - last_log >= 5000) { g_log("Netz: warte seit %lu ms an der Schranke", now - start); last_log = now; }
        {
            fd_set rd;
            struct timeval tv = { 0, 2000 };
            FD_ZERO(&rd);
            FD_SET(g_sock, &rd);
            select(0, &rd, NULL, NULL, &tv);
        }
        receive();
    }
    if (g_role == 1) memcpy(data, g_bar_in.data, len < g_bar_in.len ? len : g_bar_in.len);
    // Neue Epoche: Puffer und Pruefsummen zuruecksetzen
    g_epoch++;
    memset(g_remote_have, 0, sizeof g_remote_have);
    memset(g_my_ck_frame, 0, sizeof g_my_ck_frame);
    memset(g_peer_ck_frame, 0, sizeof g_peer_ck_frame);
    g_last_ck_sent = 0;
    g_local_upto = g_delay;
    g_remote_upto = g_delay;
    for (i = 1; i <= g_delay; i++) { g_local[i % RING] = neutral; g_remote_have[i % RING] = 1; }
    g_bar_resend = 600;   // Partner koennte unser Schranken-Paket noch brauchen
    g_log("Netz: Schranke passiert nach %lu ms (Partner-Frame %d), Epoche %d", GetTickCount() - start, g_bar_in.frame, g_epoch);
    return 1;
}

void net_send_sync(int F, const void *data, int len)
{
    if (len > 256) len = 256;
    memset(&g_sync_out, 0, sizeof g_sync_out);
    g_sync_out.magic = MAGIC;
    g_sync_out.type = 2;
    g_sync_out.frame = F;
    g_sync_out.len = len;
    memcpy(g_sync_out.data, data, len);
    send_inputs();
    g_log("Netz: Abgleich fuer Frame %d gesendet (%d Byte)", F, len);
}

static void receive(void);

int net_wait_sync(int F, void *data, int len, int timeout_ms)
{
    DWORD start = GetTickCount(), last_send = 0;
    receive();
    while (g_sync_in_frame != F) {
        DWORD now = GetTickCount();
        // Ein Abgleich fuer einen spaeteren Frame bedeutet: Der Host hat diesen Punkt anders erreicht
        if (g_sync_in_frame > F) break;
        if (now - start > (DWORD)timeout_ms) { g_log("Netz: kein Abgleich fuer Frame %d erhalten", F); return 0; }
        if (now - last_send >= 4) { send_inputs(); last_send = now; }
        {
            fd_set rd;
            struct timeval tv = { 0, 2000 };
            FD_ZERO(&rd);
            FD_SET(g_sock, &rd);
            select(0, &rd, NULL, NULL, &tv);
        }
        receive();
    }
    if (len > g_sync_in.len) len = g_sync_in.len;
    memcpy(data, g_sync_in.data, len);
    g_log("Netz: Abgleich fuer Frame %d uebernommen (Host-Frame %d)", F, g_sync_in.frame);
    return g_sync_in.frame;
}

static void compare_checksum(int f)
{
    if (g_my_ck_frame[f % RING] == f && g_peer_ck_frame[f % RING] == f && g_my_ck[f % RING] != g_peer_ck[f % RING]) {
        g_desyncs++;
        g_log("DESYNC bei Frame %d: lokal %016llx, Partner %016llx", f, g_my_ck[f % RING], g_peer_ck[f % RING]);
    }
}

static void receive(void)
{
    union { PACKET p; SYNCPACKET s; } u;
    PACKET p;
    int n, i, f;
    while ((n = recv(g_sock, (char *)&u, sizeof u, 0)) > 0) {
        if (n >= 8 && u.p.magic == MAGIC) g_last_rx = GetTickCount();
        if (n == (int)sizeof(SYNCPACKET) && u.s.magic == MAGIC && u.s.type == 2) {
            if (u.s.frame != g_sync_in_frame && u.s.len >= 0 && u.s.len <= 256) {
                g_sync_in = u.s;
                g_sync_in_frame = u.s.frame;
            }
            continue;
        }
        if (n == (int)sizeof(SYNCPACKET) && u.s.magic == MAGIC && (u.s.type == 3 || u.s.type == 4)) {
            // Schranke: Typ 3 kommt vom Host (fuer den Gast), Typ 4 vom Gast (fuer den Host)
            if ((u.s.type == 3 && g_role == 1) || (u.s.type == 4 && g_role == 0)) {
                if (!g_bar_seen || u.s.frame != g_bar_in.frame) g_bar_in = u.s;
                if (g_bar_out.magic && g_bar_resend > 600) g_bar_seen = 1;   // nur, solange wir an der Schranke stehen
            }
            continue;
        }
        if (n != (int)sizeof p) continue;
        p = u.p;
        if (p.magic != MAGIC || (p.type & 0xFFFF) != 1 || (int)(p.type >> 16) != g_epoch || p.count < 0 || p.count > REDUNDANCY) continue;
        for (i = 0; i < p.count; i++) {
            f = p.last_frame - p.count + 1 + i;
            if (f <= g_remote_upto - RING / 2) continue;  // uralt
            g_remote[f % RING] = p.in[i];
            g_remote_have[f % RING] = 1;
        }
        while (g_remote_have[(g_remote_upto + 1) % RING] &&
               g_remote_upto + 1 <= p.last_frame) {
            g_remote_upto++;
            g_remote_have[(g_remote_upto + RING / 2) % RING] = 0;  // Platz fuer spaetere Frames
        }
        if (p.ck_frame && g_peer_ck_frame[p.ck_frame % RING] != p.ck_frame) {
            g_peer_ck_frame[p.ck_frame % RING] = p.ck_frame;
            g_peer_ck[p.ck_frame % RING] = p.ck;
            compare_checksum(p.ck_frame);
        }
    }
}

void net_frame(int F, const NETPAD *local_now, NETPAD out[2])
{
    DWORD start = GetTickCount(), last_send = 0, last_log = start;
    if (!g_started) {
        InterlockedExchange(&g_started, 1);
        if (g_keepalive) WaitForSingleObject(g_keepalive, 2000);
    }
    // Lokale Eingabe dieses Frames gilt ab F + Verzoegerung
    while (g_local_upto < F + g_delay) {
        g_local_upto++;
        g_local[g_local_upto % RING] = *local_now;
    }
    send_inputs();
    receive();
    if (g_remote_upto < F && !g_partner_lost) g_stalls++;
    while (g_remote_upto < F && !g_partner_lost) {
        DWORD now = GetTickCount();
        if (g_partner_timeout_ms && now - g_last_rx > (DWORD)g_partner_timeout_ms) {
            g_log("Netz: seit %lu ms kein Paket vom Partner (wartet seit %lu ms), Spiel laeuft alleine weiter",
                  now - g_last_rx, now - start);
            InterlockedExchange(&g_partner_lost, 1);
            break;
        }
        if (now - last_send >= 4) { send_inputs(); last_send = now; }
        if (now - last_log >= 5000) {
            g_log("Netz: warte seit %lu ms auf Partner-Eingabe fuer Frame %d (bekannt bis %d)", now - start, F, g_remote_upto);
            last_log = now;
        }
        {
            fd_set rd;
            struct timeval tv = { 0, 2000 };
            FD_ZERO(&rd);
            FD_SET(g_sock, &rd);
            select(0, &rd, NULL, NULL, &tv);
        }
        receive();
    }
    out[g_role] = g_local[F % RING];
    if (g_partner_lost && g_remote_upto < F) memset(&out[1 - g_role], 0, sizeof out[1 - g_role]);
    else out[1 - g_role] = g_remote[F % RING];
}

void net_set_partner_timeout(int ms) { g_partner_timeout_ms = ms; }

int net_partner_lost(void) { return g_partner_lost != 0; }

void net_checksum(int F, unsigned long long ck)
{
    g_my_ck[F % RING] = ck;
    g_my_ck_frame[F % RING] = F;
    g_last_ck_sent = F;
    compare_checksum(F);
}

void net_stats(long *stalls, long *desyncs)
{
    *stalls = g_stalls;
    *desyncs = g_desyncs;
    if (g_loss) g_log("Netz-Test: %ld Pakete absichtlich verworfen (%d %%)", g_dropped, g_loss);
}
