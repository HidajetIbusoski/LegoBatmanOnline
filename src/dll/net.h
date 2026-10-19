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
// Zustandsabgleich (LEGO Batman 2): Der Host schickt fuer Frame F einen Datenblock (hoechstens 256 Byte),
// der Gast wartet an Frame F darauf. Rueckgabe des Gasts: Frame des empfangenen Blocks, 0 bei Zeitueberschreitung.
void net_send_sync(int F, const void *data, int len);
int net_wait_sync(int F, void *data, int len, int timeout_ms);
// Schranke mit Zustand vom Host; danach zaehlen beide Seiten die Frames neu ab 1 (siehe net.c)
int net_barrier(int F, void *data, int len, int timeout_ms);
