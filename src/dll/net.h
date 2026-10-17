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
