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
#pragma pack(pop)

static SOCKET g_sock = INVALID_SOCKET;
static struct sockaddr_in g_peer;
static int g_role;   // 0 = Host (Pad 0), 1 = Client (Pad 1)
static int g_delay = 3;
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
    g_log("Netz: Rolle %s, Port %d, Partner %s:%d, Verzoegerung %d Frames",
          g_role ? "Client (Spieler 2)" : "Host (Spieler 1)", ntohs(me.sin_port), host, peer_port, g_delay);
    return 1;
}

static void send_inputs(void)
{
    PACKET p;
    int i, first;
    memset(&p, 0, sizeof p);
    p.magic = MAGIC;
    p.type = 1;
    p.last_frame = g_local_upto;
    p.count = g_local_upto < REDUNDANCY ? g_local_upto : REDUNDANCY;
    first = g_local_upto - p.count + 1;
    for (i = 0; i < p.count; i++) p.in[i] = g_local[(first + i) % RING];
    if (g_last_ck_sent) {
        p.ck_frame = g_last_ck_sent;
        p.ck = g_my_ck[g_last_ck_sent % RING];
    }
    sendto(g_sock, (const char *)&p, sizeof p, 0, (struct sockaddr *)&g_peer, sizeof g_peer);
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
    PACKET p;
    int n, i, f;
    while ((n = recv(g_sock, (char *)&p, sizeof p, 0)) == (int)sizeof p) {
        if (p.magic != MAGIC || p.type != 1 || p.count < 0 || p.count > REDUNDANCY) continue;
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
    // Lokale Eingabe dieses Frames gilt ab F + Verzoegerung
    while (g_local_upto < F + g_delay) {
        g_local_upto++;
        g_local[g_local_upto % RING] = *local_now;
    }
    send_inputs();
    receive();
    if (g_remote_upto < F) g_stalls++;
    while (g_remote_upto < F) {
        DWORD now = GetTickCount();
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
    out[1 - g_role] = g_remote[F % RING];
}

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
}
