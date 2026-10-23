// net.c: Lockstep-Eingabesynchronisation ueber UDP.
//
// Zwei Instanzen tauschen pro Frame ihre lokale Eingabe aus. Der Host steuert Pad 0 (Spieler 1),
// der Client Pad 1 (Spieler 2). Eine lokal bei Frame F abgetastete Eingabe gilt erst ab Frame
// F + Verzoegerung, damit sie rechtzeitig beim Partner ankommt. Jeder Frame wartet, bis die
// Eingabe des Partners fuer genau diesen Frame da ist (Barriere).
//
// Fuer wackelige Verbindungen (WLAN):
// - Jedes Paket enthaelt alle Eingaben ab der letzten Bestaetigung des Partners (bis 32), verlorene Pakete
//   und kurze Verlust-Serien werden so automatisch ersetzt.
// - Jedes Paket traegt Folgenummer und Zeitstempel; daraus misst jede Seite laufend Rundlaufzeit und Verlust.
// - Zeitabgleich: Jedes Paket traegt den aktuellen Frame des Absenders. Wer voraus ist, rennt sonst immer wieder in
//   Wartephasen, weil beide ihren eigenen 60-Hz-Takt halten; net_advantage() liefert den Vorsprung fuer die Bremse.
// - Die Verzoegerung ist rein lokal (jede Seite fuellt nur ihre eigene Eingabereihe) und passt sich an das
//   95-%-Perzentil der Rundlaufzeit an: rauf sofort, runter erst nach 15 s ohne Bedarf. Beide Seiten
//   belegen am Anfang (und nach jeder Schranke) dieselben ersten <Startverzoegerung> Frames neutral vor.
//
// Umgebungsvariablen:
//   LBO_NET       host | join
//   LBO_PORT      lokaler UDP-Port (Standard 47100 Host, 47101 Client)
//   LBO_PEER      Adresse des Partners, z. B. 127.0.0.1:47101
//   LBO_DELAY     Startverzoegerung in Frames (Standard 3, beide Seiten gleich)
//   LBO_ADAPTIVE  0 = feste Verzoegerung
//   LBO_TIMESYNC  0 = ohne Zeitabgleich (die DLL verlaengert sonst ihren Frame, wenn sie dem Partner voraus ist)
//   LBO_NETLOSS   Test: Prozent ausgehender Pakete verwerfen
//   LBO_NETSIM    Test: WLAN nachbilden, "Latenz,Schwankung,Spitze,Abstand,Verlust" (ms, ms, ms, ms, %),
//                 z. B. 40,30,250,8000,3: 40 ms + 0..30 ms je Paket, etwa alle 8 s 250 ms nichts, 3 % Verlust
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "net.h"

#define MAGIC 0x314F424C  // "LBO1"
#define RING 512
#define MAXIN 32          // Eingaben je Paket
#define MAXDELAY 12       // hoechste Verzoegerung (200 ms)
#define RTT_N 512         // Rundlaufzeiten der letzten etwa 8 s

#pragma pack(push, 1)
typedef struct {
    unsigned magic;
    unsigned type;          // 1 = Eingaben (obere 16 Bit: Epoche)
    unsigned seq;           // fortlaufende Nummer des Absenders (Verlustmessung)
    int last_frame;         // letzter enthaltener Frame (in[count-1])
    int count;
    int ack;                // hoechster lueckenlos empfangener Frame des Absenders vom Empfaenger
    int ck_frame;           // Frame der mitgeschickten Pruefsumme (0 = keine)
    unsigned long long ck;  // Pruefsumme
    int repair;             // Host: geplanter Reparatur-Frame (0 = keiner), siehe net_schedule_repair
    DWORD t_send;           // Zeit des Absenders in ms (Hochpraezisionszaehler)
    DWORD t_echo;           // t_send des zuletzt vom Partner empfangenen Pakets
    DWORD t_hold;           // ms seit dessen Bearbeitung
    DWORD t_gap;            // so lange lag es hoechstens unbeachtet im Empfangspuffer (Abstand der receive-Aufrufe)
    int cur;                // aktueller Lockstep-Frame des Absenders (Zeitabgleich)
    NETPAD in[MAXIN];
} PACKET;
typedef struct {
    unsigned magic;
    unsigned type;          // 2 = Zustandsabgleich (obere 16 Bit: Epoche), 3/4 = Schranke
    int frame;
    int len;
    unsigned char data[256];
} SYNCPACKET;
#pragma pack(pop)
#define PACKET_HDR ((int)offsetof(PACKET, in))

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
static int g_delay = 3;            // eigene, aktuelle Verzoegerung
static int g_delay0 = 3;           // Startverzoegerung (beide Seiten gleich, fuer die Vorbelegung)
static int g_adaptive = 1;
static DWORD g_lower_since;        // seit wann eine kleinere Verzoegerung reichen wuerde
static int g_delay_min = 99, g_delay_max;
static int g_partner_timeout_ms;    // 0 = nie aufgeben
static volatile LONG g_partner_lost;
static volatile DWORD g_last_rx;    // GetTickCount() des letzten Pakets vom Partner (auch Lebenszeichen)
static int g_loss;        // LBO_NETLOSS: Prozent ausgehender Pakete verwerfen (nur zum Testen)
static volatile long g_dropped;
static NETPAD g_local[RING], g_remote[RING];
static int g_local_upto = 0;    // hoechster Frame mit lokaler Eingabe
static int g_remote_upto = 0;   // hoechster lueckenlos bekannter Partner-Frame
static int g_peer_ack = 0;      // so weit hat der Partner unsere Eingaben lueckenlos
static int g_peer_started;      // Partner hat schon echte Eingaben geschickt (nicht nur die Vorbelegung)
static unsigned char g_remote_have[RING];
static unsigned long long g_my_ck[RING], g_peer_ck[RING];
static int g_my_ck_frame[RING], g_peer_ck_frame[RING];
static int g_last_ck_sent;
static long g_stalls, g_desyncs;
static int g_last_desync;              // Pruefsummen-Frame der letzten Abweichung (in der laufenden Epoche)
static int g_last_desync_parts;        // dabei verschieden: 1 = untere 32 Bit, 2 = obere 32 Bit der Pruefsumme
// Einstellungs-Abgleich (LEGO Batman 2), Paket Art 6 {MAGIC, 6, Datei-Hash, Laufzeit-Hash, Hz, Flags}: Der
// Lebenszeichen-Thread schickt es alle halbe Sekunde, receive() merkt sich das des Partners. Wirkt nicht auf
// Eingaben, Frames oder Epochen; alte Builds verwerfen die unbekannte Art.
#define CFG_WORDS 4
static unsigned g_cfg_my[CFG_WORDS], g_cfg_peer[CFG_WORDS];
static volatile LONG g_cfg_my_set, g_cfg_peer_got;
static CRITICAL_SECTION g_cfg_cs;   // g_cfg_my: Hauptthread schreibt, Lebenszeichen-Thread sendet
static int g_repair_out, g_repair_in;  // Reparatur-Frame: vom Host geplant / vom Host empfangen
static long g_frame_offset;            // Spiel-Frame = Lockstep-Frame + Versatz (nur fuer das Log)
static void (*g_log)(const char *fmt, ...);
static void (*g_wait_cb)(DWORD start);   // wird waehrend des Wartens auf den Partner aufgerufen (LEGO Batman 2: Ton anhalten)
static void (*g_notice_cb)(int kind);    // Hinweis an den Spieler (3 = Spiel des Partners startet nicht)
// Messwerte der Verbindung
static unsigned g_tx_seq;
static DWORD g_echo_t, g_echo_at, g_echo_gap;   // t_send des letzten Partnerpakets, wann bearbeitet, Pufferzeit
static DWORD g_last_recv_call;                   // letzter Aufruf von receive()
static int g_rtt[RTT_N], g_rtt_n, g_rtt_pos;
static unsigned g_rx_seq_lo, g_rx_seq_hi, g_rx_count;   // fuer die Verlustrate seit dem letzten Bericht
static long g_wait_ms_total, g_wait_long;   // Wartephasen ueber 20 ms: Summe der Zeit darueber, Anzahl
// Zeitabgleich (wie bei GGPO): Wer dem Partner zeitlich voraus ist, rennt immer wieder in Wartephasen, weil beide
// ihren eigenen 60-Hz-Takt halten. Geschaetzter Vorsprung in Frames, geglaettet; die DLL verlaengert damit ihren Takt.
static int g_cur_frame, g_peer_cur;
static unsigned g_peer_cur_seq;
static DWORD g_peer_cur_at;
static int g_rtt_med = -1;
static double g_adv;
static int g_timesync = 1;   // LBO_TIMESYNC=0 schaltet den Zeitabgleich ab (Vergleichstests)

int net_role(void) { return g_role; }



// Millisekunden aus dem Hochpraezisionszaehler (GetTickCount springt in 15,6-ms-Schritten)
static DWORD now_ms(void)
{
    static LONGLONG f;
    LARGE_INTEGER t;
    if (!f) { QueryPerformanceFrequency(&t); f = t.QuadPart; }
    QueryPerformanceCounter(&t);
    return (DWORD)(t.QuadPart * 1000 / f);
}

// --- WLAN-Nachbildung (nur Tests) --------------------------------------------------------------------
typedef struct { DWORD due; int len; char buf[sizeof(SYNCPACKET) > sizeof(PACKET) ? sizeof(SYNCPACKET) : sizeof(PACKET)]; } SIMPKT;
static int g_sim, g_sim_lat, g_sim_jit, g_sim_spike, g_sim_period, g_sim_loss;
static SIMPKT g_simq[2048];
static int g_simq_n;
static DWORD g_sim_spike_at, g_sim_spike_end;
static CRITICAL_SECTION g_sim_cs;
static unsigned g_sim_rand = 12345;

static unsigned sim_rand(void) { g_sim_rand = g_sim_rand * 1103515245u + 12345u; return g_sim_rand >> 8; }

static void sim_pump(void)
{
    DWORD now = now_ms();
    int i, k = 0;
    EnterCriticalSection(&g_sim_cs);
    for (i = 0; i < g_simq_n; i++) {
        if ((int)(now - g_simq[i].due) >= 0)
            sendto(g_sock, g_simq[i].buf, g_simq[i].len, 0, (struct sockaddr *)&g_peer, sizeof g_peer);
        else g_simq[k++] = g_simq[i];
    }
    g_simq_n = k;
    LeaveCriticalSection(&g_sim_cs);
}

static DWORD WINAPI sim_thread(void *arg)
{
    (void)arg;
    for (;;) { Sleep(1); sim_pump(); }
}

static void raw_send(const void *buf, int len)
{
    if (g_loss && (rand() % 100) < g_loss) { InterlockedIncrement(&g_dropped); return; }  // Testmodus: Paketverlust
    if (!g_sim) { sendto(g_sock, (const char *)buf, len, 0, (struct sockaddr *)&g_peer, sizeof g_peer); return; }
    EnterCriticalSection(&g_sim_cs);
    {
        DWORD now = now_ms(), due;
        if (g_sim_period && (int)(now - g_sim_spike_at) >= 0) {   // naechste Funkstille (Abstand +-50 %)
            g_sim_spike_end = now + g_sim_spike;
            g_sim_spike_at = now + g_sim_period / 2 + sim_rand() % (g_sim_period + 1);
        }
        due = now + g_sim_lat + (g_sim_jit ? sim_rand() % (g_sim_jit + 1) : 0);
        if ((int)(g_sim_spike_end - now) > 0 && (int)(g_sim_spike_end + g_sim_lat - due) > 0) due = g_sim_spike_end + g_sim_lat;
        if (g_sim_loss && (int)(sim_rand() % 100) < g_sim_loss) InterlockedIncrement(&g_dropped);
        else if (g_simq_n < (int)(sizeof g_simq / sizeof g_simq[0]) && len <= (int)sizeof g_simq[0].buf) {
            g_simq[g_simq_n].due = due;
            g_simq[g_simq_n].len = len;
            memcpy(g_simq[g_simq_n].buf, buf, len);
            g_simq_n++;
        }
    }
    LeaveCriticalSection(&g_sim_cs);
}

// Bis zum ersten Frame vergehen einige Sekunden (Spielstart). In der Zeit halten wir die
// Zuordnung im Router des Partners offen, sonst wuerden seine ersten Pakete verworfen.
static volatile LONG g_started, g_ka_running_start;
static HANDLE g_keepalive;
static void send_inputs(void);

static void send_cfg(void)
{
    unsigned p[2 + CFG_WORDS];
    int i;
    if (!g_cfg_my_set || g_partner_lost) return;
    p[0] = MAGIC;
    p[1] = 6;
    EnterCriticalSection(&g_cfg_cs);
    for (i = 0; i < CFG_WORDS; i++) p[2 + i] = g_cfg_my[i];
    LeaveCriticalSection(&g_cfg_cs);
    raw_send(p, sizeof p);
}

static DWORD WINAPI keepalive(void *arg)
{
    int tick = 0;
    (void)arg;
    while (!g_started) {
        InterlockedExchange(&g_ka_running_start, 1);
        if (!g_started) send_inputs();
        InterlockedExchange(&g_ka_running_start, 0);
        if (++tick % 3 == 0) send_cfg();
        Sleep(200);
    }
    // Danach (nur mit Partner-Zeitgrenze) ein Lebenszeichen je halbe Sekunde, auch wenn das Spiel steht
    // (Alt-Tab, Ladehaenger). So gilt der Partner nur als verloren, wenn wirklich nichts mehr ankommt.
    // Gilt er als verloren, schweigen wir auch: Kaeme er zurueck, liefe sein Spiel sonst nie weiter.
    while (!g_partner_lost) {
        if (g_partner_timeout_ms) {
            unsigned ping[2] = { MAGIC, 5 };
            raw_send(ping, sizeof ping);
        }
        send_cfg();
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
    if (GetEnvironmentVariableA("LBO_ADAPTIVE", buf, sizeof buf)) g_adaptive = atoi(buf);
    if (GetEnvironmentVariableA("LBO_TIMESYNC", buf, sizeof buf)) g_timesync = atoi(buf);
    if (GetEnvironmentVariableA("LBO_NETLOSS", buf, sizeof buf)) g_loss = atoi(buf);
    if (GetEnvironmentVariableA("LBO_NETSIM", buf, sizeof buf) &&
        sscanf(buf, "%d,%d,%d,%d,%d", &g_sim_lat, &g_sim_jit, &g_sim_spike, &g_sim_period, &g_sim_loss) >= 1) g_sim = 1;
    srand(GetCurrentProcessId());  // eigene CRT der DLL, beruehrt den Zufall des Spiels nicht
    g_sim_rand = GetCurrentProcessId() * 2654435761u;
    if (g_delay < 1) g_delay = 1;
    if (g_delay > MAXDELAY) g_delay = MAXDELAY;
    g_delay0 = g_delay;
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
    // Die ersten <Startverzoegerung> Frames haben auf beiden Seiten neutrale Eingabe
    g_local_upto = g_delay0;
    g_remote_upto = g_delay0;
    g_peer_ack = g_delay0;
    memset(g_remote_have, 0, sizeof g_remote_have);
    for (port = 1; port <= g_delay0; port++) g_remote_have[port % RING] = 1;
    if (g_loss) g_log("Netz-Test: simuliere %d %% Paketverlust", g_loss);
    if (g_sim) {
        InitializeCriticalSection(&g_sim_cs);
        g_sim_spike_at = now_ms() + g_sim_period;
        CreateThread(NULL, 0, sim_thread, NULL, 0, NULL);
        g_log("Netz-Test: WLAN-Nachbildung %d ms + 0..%d ms, Funkstille %d ms etwa alle %d ms, %d %% Verlust",
              g_sim_lat, g_sim_jit, g_sim_spike, g_sim_period, g_sim_loss);
    }
    g_last_rx = GetTickCount();
    InitializeCriticalSection(&g_cfg_cs);
    g_keepalive = CreateThread(NULL, 0, keepalive, NULL, 0, NULL);
    g_log("Netz: Rolle %s, Port %d, Partner %s:%d, Verzoegerung %d Frames%s",
          g_role ? "Client (Spieler 2)" : "Host (Spieler 1)", ntohs(me.sin_port), host, peer_port, g_delay,
          g_adaptive ? " (passt sich an)" : "");
    return 1;
}

static void send_inputs(void)
{
    PACKET p;
    int i, first, last;
    DWORD now = now_ms();
    if (g_partner_lost) return;
    memset(&p, 0, PACKET_HDR);
    p.magic = MAGIC;
    p.type = 1 | ((unsigned)g_epoch << 16);
    p.seq = ++g_tx_seq;
    // Ab der letzten Bestaetigung, hoechstens MAXIN Eingaben; mindestens die letzte (haelt Bestaetigungen am Laufen)
    first = g_peer_ack + 1;
    if (first > g_local_upto) first = g_local_upto;
    if (first < 1) first = 1;
    last = g_local_upto;
    if (last - first + 1 > MAXIN) last = first + MAXIN - 1;
    p.count = last >= first ? last - first + 1 : 0;
    p.last_frame = last;
    for (i = 0; i < p.count; i++) p.in[i] = g_local[(first + i) % RING];
    p.ack = g_remote_upto;
    if (g_last_ck_sent) {
        p.ck_frame = g_last_ck_sent;
        p.ck = g_my_ck[g_last_ck_sent % RING];
    }
    p.repair = g_repair_out;
    p.t_send = now;
    p.t_echo = g_echo_t;
    p.t_hold = g_echo_t ? now - g_echo_at : 0;
    p.t_gap = g_echo_gap;
    p.cur = g_cur_frame;
    raw_send(&p, PACKET_HDR + p.count * (int)sizeof(NETPAD));
    // Den letzten Abgleich eine Weile mitsenden, bis der Gast ihn sicher hat (Pakete koennen verloren gehen)
    if (g_sync_out.magic && g_local_upto - g_sync_out.frame < 600) raw_send(&g_sync_out, sizeof g_sync_out);
    if (g_bar_resend > 0) {
        g_bar_resend--;
        raw_send(&g_bar_out, sizeof g_bar_out);
    }
}

static void receive(void);

// Warten auf ein Paket, hoechstens ms Millisekunden
static void wait_packet(int ms)
{
    fd_set rd;
    struct timeval tv = { 0, ms * 1000 };
    FD_ZERO(&rd);
    FD_SET(g_sock, &rd);
    select(0, &rd, NULL, NULL, &tv);
}

// Neue Epoche (nach der Schranke) oder Start: Vorbelegung mit der gemeinsamen Startverzoegerung
static void reset_streams(const NETPAD *neutral)
{
    int i;
    memset(g_remote_have, 0, sizeof g_remote_have);
    memset(g_my_ck_frame, 0, sizeof g_my_ck_frame);
    memset(g_peer_ck_frame, 0, sizeof g_peer_ck_frame);
    g_last_ck_sent = 0;
    g_local_upto = g_delay0;
    g_remote_upto = g_delay0;
    g_peer_ack = g_delay0;
    for (i = 1; i <= g_delay0; i++) { g_local[i % RING] = *neutral; g_remote[i % RING] = *neutral; g_remote_have[i % RING] = 1; }
}

// Schranke, z. B. am Levelstart (LEGO Batman 2): Beide Seiten kommen hier an, auch wenn ihre Frames vorher
// durch Ladezeiten etwas auseinanderliefen. Der Host gibt seinen Zustand mit (data), der Gast bekommt ihn.
// Wer zuerst ankommt, verlaengert seine Eingaben mit neutralen Pads, damit der Partner bis zur Schranke
// weiterlaufen kann. Danach beginnt eine neue Epoche mit Frame 1 (Vorbelegung mit der Startverzoegerung).
int net_barrier(int F, void *data, int len, int timeout_ms)
{
    DWORD start = GetTickCount(), last_send = 0, last_log = start;
    NETPAD neutral;
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
        if (g_partner_timeout_ms && now - g_last_rx > (DWORD)g_partner_timeout_ms) {
            g_log("Netz: an der Schranke seit %lu ms kein Paket vom Partner", now - g_last_rx);
            InterlockedExchange(&g_partner_lost, 1);
            return 0;
        }
        // Partner braucht unsere Eingaben bis zu seinem Frame (er hat seine bis g_remote_upto geschickt)
        while (g_local_upto < g_remote_upto + 2) {
            g_local_upto++;
            g_local[g_local_upto % RING] = neutral;
        }
        if (now - last_send >= 8) { send_inputs(); last_send = now; }
        if (g_wait_cb) g_wait_cb(start);
        if (now - last_log >= 5000) { g_log("Netz: warte seit %lu ms an der Schranke", now - start); last_log = now; }
        wait_packet(2);
        receive();
    }
    if (g_role == 1) memcpy(data, g_bar_in.data, len < g_bar_in.len ? len : g_bar_in.len);
    // Neue Epoche: Puffer und Pruefsummen zuruecksetzen
    g_epoch++;
    reset_streams(&neutral);
    g_bar_resend = 600;   // Partner koennte unser Schranken-Paket noch brauchen
    g_last_desync = 0;
    g_repair_out = g_repair_in = 0;
    g_sync_out.magic = 0;   // Abgleich der alten Epoche nicht weiter senden
    g_sync_in_frame = 0;
    g_log("Netz: Schranke passiert nach %lu ms (Partner-Frame %d), Epoche %d", GetTickCount() - start, g_bar_in.frame, g_epoch);
    return 1;
}

void net_send_sync(int F, const void *data, int len)
{
    if (len > 256) len = 256;
    memset(&g_sync_out, 0, sizeof g_sync_out);
    g_sync_out.magic = MAGIC;
    g_sync_out.type = 2 | ((unsigned)g_epoch << 16);
    g_sync_out.frame = F;
    g_sync_out.len = len;
    memcpy(g_sync_out.data, data, len);
    send_inputs();
    g_log("Netz: Abgleich fuer Frame %d gesendet (%d Byte)", F, len);
}

int net_wait_sync(int F, void *data, int len, int timeout_ms)
{
    DWORD start = GetTickCount(), last_send = 0;
    receive();
    while (g_sync_in_frame != F) {
        DWORD now = GetTickCount();
        // Ein Abgleich fuer einen spaeteren Frame bedeutet: Der Host hat diesen Punkt anders erreicht
        if (g_sync_in_frame > F) break;
        if (now - start > (DWORD)timeout_ms) { g_log("Netz: kein Abgleich fuer Frame %d erhalten", F); return 0; }
        if (g_partner_lost || (g_partner_timeout_ms && now - g_last_rx > (DWORD)g_partner_timeout_ms)) {
            g_log("Netz: kein Abgleich fuer Frame %d, Partner antwortet nicht", F);
            return 0;
        }
        if (now - last_send >= 8) { send_inputs(); last_send = now; }
        if (g_wait_cb) g_wait_cb(start);
        wait_packet(2);
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
        unsigned long long d = g_my_ck[f % RING] ^ g_peer_ck[f % RING];
        int parts = ((unsigned)d ? 1 : 0) | ((unsigned)(d >> 32) ? 2 : 0);
        g_desyncs++;
        if (f > g_last_desync) { g_last_desync = f; g_last_desync_parts = parts; }
        // Die ersten 30 einzeln, danach jede 100. (nach dem Hinweis kam sonst alle 20 Frames eine Zeile)
        if (g_desyncs <= 30 || g_desyncs % 100 == 0)
            g_log("DESYNC bei Frame %d (Spiel-Frame %ld): lokal %016llx, Partner %016llx (verschieden: %s)", f, f + g_frame_offset,
              g_my_ck[f % RING], g_peer_ck[f % RING], parts == 3 ? "beide Teile" : parts == 2 ? "oberer Teil" : "unterer Teil");
    }
}

static void receive(void)
{
    union { PACKET p; SYNCPACKET s; unsigned u[2 + CFG_WORDS]; } u;
    int n, i, f;
    DWORD call = now_ms(), gap = g_last_recv_call ? call - g_last_recv_call : 1000;
    g_last_recv_call = call;
    while ((n = recv(g_sock, (char *)&u, sizeof u, 0)) > 0) {
        unsigned kind;
        if (n < 8 || u.u[0] != MAGIC) continue;
        g_last_rx = GetTickCount();
        kind = u.u[1] & 0xFFFF;
        if (kind == 2 && n == (int)sizeof(SYNCPACKET)) {
            if ((int)(u.s.type >> 16) == g_epoch && u.s.frame != g_sync_in_frame && u.s.len >= 0 && u.s.len <= 256) {
                g_sync_in = u.s;
                g_sync_in_frame = u.s.frame;
            }
            continue;
        }
        if ((kind == 3 || kind == 4) && n == (int)sizeof(SYNCPACKET)) {
            // Schranke: Typ 3 kommt vom Host (fuer den Gast), Typ 4 vom Gast (fuer den Host)
            if ((kind == 3 && g_role == 1) || (kind == 4 && g_role == 0)) {
                if (!g_bar_seen || u.s.frame != g_bar_in.frame) g_bar_in = u.s;
                if (g_bar_out.magic && g_bar_resend > 600) g_bar_seen = 1;   // nur, solange wir an der Schranke stehen
            }
            continue;
        }
        if (kind == 6 && n == (int)sizeof(unsigned) * (2 + CFG_WORDS)) {
            int i;
            for (i = 0; i < CFG_WORDS; i++) g_cfg_peer[i] = u.u[2 + i];
            InterlockedExchange(&g_cfg_peer_got, 1);
            continue;
        }
        if (kind != 1 || n < PACKET_HDR) continue;
        {
            PACKET *p = &u.p;
            if (p->count < 0 || p->count > MAXIN || n != PACKET_HDR + p->count * (int)sizeof(NETPAD)) continue;
            // Messwerte: Folgenummern und Rundlaufzeit (auch aus Paketen alter Epochen)
            if (!g_rx_count || (int)(p->seq - g_rx_seq_lo) < 0) g_rx_seq_lo = p->seq;
            if (!g_rx_count || (int)(p->seq - g_rx_seq_hi) > 0) g_rx_seq_hi = p->seq;
            g_rx_count++;
            if ((int)(p->t_send - g_echo_t) > 0 || !g_echo_t) { g_echo_t = p->t_send; g_echo_at = call; g_echo_gap = gap; }
            // Nur Messungen, bei denen keine Seite laenger als 25 ms nicht nachgesehen hat: Sonst laege das Paket in
            // einem Lade-Frame (bis Sekunden) im Puffer und die Rundlaufzeit waere viel zu gross (Test W1: 594 ms)
            if (p->t_echo && p->t_gap <= 25 && gap <= 25) {
                // Ankunft im Puffer liegt irgendwo im Abstand der receive-Aufrufe: im Mittel die Haelfte abziehen
                int rtt = (int)(call - p->t_echo - p->t_hold) - (int)(p->t_gap + gap) / 2;
                if (rtt >= 0 && rtt < 5000) {
                    g_rtt[g_rtt_pos] = rtt;
                    g_rtt_pos = (g_rtt_pos + 1) % RTT_N;
                    if (g_rtt_n < RTT_N) g_rtt_n++;
                }
            }
            if ((int)(p->type >> 16) != g_epoch) continue;
            if (!g_peer_cur_at || (int)(p->seq - g_peer_cur_seq) > 0) { g_peer_cur = p->cur; g_peer_cur_seq = p->seq; g_peer_cur_at = call; }
            if (g_role == 1 && p->repair > g_repair_in) g_repair_in = p->repair;
            if (p->ack > g_peer_ack && p->ack <= g_local_upto) g_peer_ack = p->ack;
            if (g_epoch == 0 && p->last_frame > g_delay0) g_peer_started = 1;
            for (i = 0; i < p->count; i++) {
                f = p->last_frame - p->count + 1 + i;
                if (f <= g_remote_upto || f > g_remote_upto + RING / 2) continue;   // schon bekannt oder unplausibel
                g_remote[f % RING] = p->in[i];
                g_remote_have[f % RING] = 1;
            }
            while (g_remote_have[(g_remote_upto + 1) % RING] && g_remote_upto + 1 <= p->last_frame) {
                g_remote_upto++;
                g_remote_have[(g_remote_upto + RING / 2) % RING] = 0;  // Platz fuer spaetere Frames
            }
            if (p->ck_frame && g_peer_ck_frame[p->ck_frame % RING] != p->ck_frame) {
                g_peer_ck_frame[p->ck_frame % RING] = p->ck_frame;
                g_peer_ck[p->ck_frame % RING] = p->ck;
                compare_checksum(p->ck_frame);
            }
        }
    }
}

static int rtt_percentile(int pct)
{
    int tmp[RTT_N], i, j, n = g_rtt_n;
    if (!n) return -1;
    memcpy(tmp, g_rtt, n * sizeof(int));
    // Einfuegesortierung reicht bei 512 Werten alle halbe Sekunde
    for (i = 1; i < n; i++) { int v = tmp[i]; for (j = i - 1; j >= 0 && tmp[j] > v; j--) tmp[j + 1] = tmp[j]; tmp[j + 1] = v; }
    return tmp[(n - 1) * pct / 100];
}

// Eigene Verzoegerung an die Verbindung anpassen: halbe Rundlaufzeit (95. Perzentil) plus 10 ms Reserve, in
// Frames aufgerundet, plus ein Frame. Rauf sofort, runter um einen Frame je 15 s, in denen weniger reicht.
static void adapt_delay(int F)
{
    int p95, need;
    DWORD now;
    if (F % 30 || g_rtt_n < 30) return;
    g_rtt_med = rtt_percentile(50);
    if (!g_adaptive) return;
    p95 = rtt_percentile(95);
    need = (p95 / 2 + 10) * 60 / 1000 + 2;
    if (need < 2) need = 2;
    if (need > MAXDELAY) need = MAXDELAY;
    now = GetTickCount();
    if (need > g_delay) {
        g_log("Netz: Verzoegerung %d -> %d Frames (Rundlaufzeit 95 %%: %d ms)", g_delay, need, p95);
        g_delay = need;
        g_lower_since = 0;
    } else if (need < g_delay) {
        if (!g_lower_since) g_lower_since = now;
        else if (now - g_lower_since > 15000) {
            g_log("Netz: Verzoegerung %d -> %d Frames (Rundlaufzeit 95 %%: %d ms)", g_delay, g_delay - 1, p95);
            g_delay--;
            g_lower_since = now;
        }
    } else g_lower_since = 0;
    if (g_delay < g_delay_min) g_delay_min = g_delay;
    if (g_delay > g_delay_max) g_delay_max = g_delay;
}

void net_frame(int F, const NETPAD *local_now, NETPAD out[2])
{
    DWORD start = GetTickCount(), last_send = 0, last_log = start;
    static int notified_nostart;
    if (!g_started) {
        // Der Lebenszeichen-Thread darf ab jetzt keine Eingaben mehr senden (er laeuft fuer Pings weiter)
        DWORD t0 = GetTickCount();
        InterlockedExchange(&g_started, 1);
        while (g_ka_running_start && GetTickCount() - t0 < 500) Sleep(1);
    }
    adapt_delay(F);
    g_cur_frame = F;
    // Lokale Eingabe dieses Frames gilt ab F + Verzoegerung. Wurde die Verzoegerung kleiner, wird hier nichts
    // Neues eingetragen, bis F + Verzoegerung den Stand wieder erreicht; wurde sie groesser, gilt die aktuelle
    // Eingabe fuer mehrere Frames.
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
        // Spiel des Partners kommt nicht ueber den Start hinaus (er lebt, schickt aber keine Eingaben)
        if (!notified_nostart && g_epoch == 0 && !g_peer_started && now - start > 45000) {
            notified_nostart = 1;
            g_log("Netz: Spiel des Partners startet nicht (seit %lu ms keine Eingaben, Lebenszeichen kommen)", now - start);
            if (g_notice_cb) g_notice_cb(3);
        }
        if (now - last_send >= 8) { send_inputs(); last_send = now; }
        if (g_wait_cb) g_wait_cb(start);
        if (now - last_log >= 5000) {
            g_log("Netz: warte seit %lu ms auf Partner-Eingabe fuer Frame %d (bekannt bis %d)", now - start, F, g_remote_upto);
            last_log = now;
        }
        wait_packet(2);
        receive();
    }
    {
        DWORD waited = GetTickCount() - start;
        if (waited > 20) { g_wait_ms_total += waited; g_wait_long++; }
    }
    // Zeitabgleich: Partner-Frame jetzt = sein gemeldeter Frame + seither vergangene Zeit + halbe Rundlaufzeit
    if (g_peer_cur_at && g_rtt_med >= 0 && !g_partner_lost && g_peer_cur > 0) {
        double pnow = g_peer_cur + ((double)(now_ms() - g_peer_cur_at) + g_rtt_med / 2.0) * 60.0 / 1000.0;
        double adv = F - pnow;
        if (adv > 3) adv = 3;   // Laden oder Schranke beim Partner: nicht ueberbewerten
        if (adv < -3) adv = -3;
        g_adv = g_adv * 0.95 + adv * 0.05;
    }
    out[g_role] = g_local[F % RING];
    if (g_partner_lost && g_remote_upto < F) memset(&out[1 - g_role], 0, sizeof out[1 - g_role]);
    else out[1 - g_role] = g_remote[F % RING];
}

void net_set_partner_timeout(int ms) { g_partner_timeout_ms = ms; }

void net_set_frame_offset(long offset) { g_frame_offset = offset; }

void net_set_wait_callback(void (*cb)(DWORD start)) { g_wait_cb = cb; }

void net_set_notice_callback(void (*cb)(int kind)) { g_notice_cb = cb; }

int net_last_desync(void) { return g_last_desync; }

int net_last_desync_parts(void) { return g_last_desync_parts; }

void net_set_cfg(const unsigned v[4])
{
    int i;
    if (g_sock == INVALID_SOCKET) return;
    EnterCriticalSection(&g_cfg_cs);
    for (i = 0; i < CFG_WORDS; i++) g_cfg_my[i] = v[i];
    LeaveCriticalSection(&g_cfg_cs);
    InterlockedExchange(&g_cfg_my_set, 1);
    send_cfg();
}

int net_peer_cfg(unsigned v[4])
{
    int i;
    receive();
    if (!g_cfg_peer_got) return 0;
    for (i = 0; i < CFG_WORDS; i++) v[i] = g_cfg_peer[i];
    return 1;
}

int net_wait_peer_cfg(unsigned v[4], unsigned need_flag, int timeout_ms)
{
    DWORD start = GetTickCount(), last_send = 0;
    for (;;) {
        DWORD now = GetTickCount();
        if (net_peer_cfg(v) && (v[3] & need_flag) == need_flag) return 1;
        if (now - start > (DWORD)timeout_ms || g_partner_lost) return 0;
        if (now - last_send >= 100) { send_inputs(); send_cfg(); last_send = now; }
        if (g_wait_cb) g_wait_cb(start);
        wait_packet(2);
    }
}

void net_schedule_repair(int frame) { g_repair_out = frame; }

int net_repair_frame(void) { return g_role == 0 ? g_repair_out : g_repair_in; }

int net_partner_lost(void) { return g_partner_lost != 0; }

int net_delay(void) { return g_delay; }

double net_advantage(void) { return g_timesync ? g_adv : 0; }

int net_peer_started(void) { return g_peer_started || g_epoch > 0; }

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

// Bericht ueber die Verbindung seit dem letzten Aufruf (alle 3000 Frames)
void net_report(void)
{
    {
        unsigned expect = g_rx_count ? g_rx_seq_hi - g_rx_seq_lo + 1 : 0;
        if (expect < g_rx_count) expect = g_rx_count;   // doppelt angekommene Pakete
        g_log("Netz-Qualitaet: Rundlaufzeit Median %d ms, 95 %% %d ms; Verlust %.1f %% (%u von %u Paketen); "
              "Verzoegerung %d Frames (seit letztem Bericht %d-%d); %ld Wartephasen ueber 20 ms (zusammen %ld ms); Vorsprung %.2f Frames",
              rtt_percentile(50), rtt_percentile(95), expect ? 100.0 * (expect - g_rx_count) / expect : 0.0,
              expect - g_rx_count, expect, g_delay, g_delay_min == 99 ? g_delay : g_delay_min,
              g_delay_max ? g_delay_max : g_delay, g_wait_long, g_wait_ms_total, g_adv);
        g_rx_count = 0;
        g_delay_min = 99;
        g_delay_max = 0;
        g_wait_ms_total = 0;
        g_wait_long = 0;
    }
    if (g_loss || g_sim) g_log("Netz-Test: %ld Pakete absichtlich verworfen", g_dropped);
}
