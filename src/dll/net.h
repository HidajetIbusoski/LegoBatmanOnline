// net.h: Lockstep-Eingabesynchronisation ueber UDP, siehe net.c
#pragma once

#pragma pack(push, 1)
typedef struct {
    unsigned short buttons;
    unsigned char lt, rt;
    short lx, ly, rx, ry;
    unsigned char connected;
    unsigned char reserved;
} NETPAD;
#pragma pack(pop)

int net_init(void (*logger)(const char *fmt, ...));  // 1 = Netzmodus aktiv (LBO_NET gesetzt)
int net_role(void);                                  // 0 = Host/Pad 0, 1 = Client/Pad 1
void net_frame(int F, const NETPAD *local_now, NETPAD out[2]);  // blockiert bis beide Eingaben fuer F da sind
void net_checksum(int F, unsigned long long ck);
void net_stats(long *stalls, long *desyncs);
void net_report(void);   // Rundlaufzeit, Verlust, Verzoegerung und Wartezeit seit dem letzten Bericht ins Log
// Zustandsabgleich (LEGO Batman 2): Der Host schickt fuer Frame F einen Datenblock (hoechstens 256 Byte),
// der Gast wartet an Frame F darauf. Rueckgabe des Gasts: Frame des empfangenen Blocks, 0 bei Zeitueberschreitung.
void net_send_sync(int F, const void *data, int len);
int net_wait_sync(int F, void *data, int len, int timeout_ms);
// Schranke mit Zustand vom Host; danach zaehlen beide Seiten die Frames neu ab 1 (siehe net.c)
int net_barrier(int F, void *data, int len, int timeout_ms);
// Partner weg (LEGO Batman 2): Kommt laenger als ms kein Paket (auch kein Lebenszeichen, das ein eigener
// Thread jede halbe Sekunde schickt), gilt er als verloren. Ab dann liefert net_frame fuer ihn neutrale
// Eingaben, wartet nicht mehr und sendet selbst nichts mehr. 0 = nie aufgeben (Standard, Batman 1).
void net_set_partner_timeout(int ms);
int net_partner_lost(void);
// Selbstheilung (LEGO Batman 2): Der Host plant bei einer Abweichung einen Reparatur-Frame und kuendigt ihn in
// jedem Eingabepaket an; dort uebernimmt der Gast per net_send_sync/net_wait_sync den Zustand des Hosts.
void net_schedule_repair(int frame);   // nur Host
int net_repair_frame(void);            // Host: eigener Plan, Gast: zuletzt angekuendigter (0 = keiner)
int net_last_desync(void);             // Pruefsummen-Frame der letzten Abweichung dieser Epoche (0 = keine)
void net_set_frame_offset(long offset);   // fuer das Log: Spiel-Frame = Lockstep-Frame + offset
void net_set_wait_callback(void (*cb)(unsigned long start));   // waehrend net_frame auf den Partner wartet
void net_set_notice_callback(void (*cb)(int kind));   // Hinweis an den Spieler: 3 = Spiel des Partners startet nicht
int net_delay(void);                                   // aktuelle eigene Eingabeverzoegerung in Frames
int net_peer_started(void);                            // Partner hat schon echte Eingaben geschickt
double net_advantage(void);   // geschaetzter zeitlicher Vorsprung vor dem Partner in Frames (positiv: wir sind voraus)
