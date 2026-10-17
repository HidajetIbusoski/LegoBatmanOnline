// online.h: Verbindungsaufbau ueber das Internet per Einladungscode, siehe online.c
#pragma once
#include <winsock2.h>

typedef struct {
    int role;                    // 0 = Host (Spieler 1), 1 = Gast (Spieler 2)
    int port;                    // lokaler UDP-Port (0 = zufaellig)
    struct sockaddr_in local, pub;            // eigene lokale und oeffentliche Adresse
    struct sockaddr_in peer_pub, peer_local;  // aus dem Code des Partners
    struct sockaddr_in peer;                  // Adresse, ueber die die Verbindung steht
    int symmetric_nat;
    int ping_ms;
} ONLINE;

int online_sha256_file(const char *path, unsigned char out[32]);
int online_prepare(ONLINE *o, char code_out[40]);       // Socket, STUN, eigener Code
int online_set_peer_code(ONLINE *o, const char *code);  // 0 = Code ungueltig
#define DIRECT_PORT 47100
int online_prepare_direct(ONLINE *o);                   // Direktmodus: fester Port, kein Code
int online_set_peer_ip(ONLINE *o, const char *text);    // "IP" oder "IP:Port"
int online_connect(ONLINE *o, const unsigned char exe_hash[32], int timeout_s);
int online_measure_ping(ONLINE *o);                     // Median-RTT in ms, -1 bei Fehler
int online_send_saves(ONLINE *o, const char *save_dir);
int online_receive_saves(ONLINE *o, const char *dest_dir);
int online_ready_barrier(ONLINE *o, int *delay);  // Gast uebernimmt die Verzoegerung des Hosts
void online_close(void);
