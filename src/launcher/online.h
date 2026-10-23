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
unsigned online_key_me(void);
void online_set_keys(unsigned me, unsigned peer);
int online_parse_endpoint(const char *text, struct sockaddr_in *out);  // "IP:Port"
#define DIRECT_PORT 47100
int online_prepare_direct(ONLINE *o);                   // Direktmodus: fester Port, kein Code
int online_set_peer_ip(ONLINE *o, const char *text);    // "IP" oder "IP:Port"
int online_connect(ONLINE *o, const unsigned char exe_hash[32], int timeout_s);
extern char online_error[160];   // Grund, wenn online_connect an Version, Spiel oder Rolle scheitert (sonst leer)
int online_measure_ping(ONLINE *o);                     // Median-RTT in ms, -1 bei Fehler
// pattern: Dateimuster im Ordner, z. B. "SaveGame*" (Batman 1)
int online_send_saves(ONLINE *o, const char *save_dir, const char *pattern);
int online_receive_saves(ONLINE *o, const char *dest_dir);
// Batman 2: genau eine Datei aus dem Speicher (Host) bzw. in den Speicher (Gast verlangt genau eine Datei name)
int online_send_blob(ONLINE *o, const char *name, const void *data, unsigned size);
int online_receive_blob(ONLINE *o, const char *name, void *buf, unsigned cap, unsigned *len);
// Bereit-Schranke: Gast uebernimmt die Verzoegerung des Hosts; beide tauschen den SHA-256 ihrer Einstellungen
// (NULL = Nullen, Batman 1) und die Quelle (B2CFG_SRC_*). Rueckgabe 1 = ok, 0 = Zeitgrenze,
// -1 = Einstellungen verschieden (peer_cfg/peer_src gesetzt), -2 = Partner hat eine andere Version.
int online_ready_barrier(ONLINE *o, int *delay, const unsigned char my_cfg[32], unsigned my_src,
                         unsigned char peer_cfg[32], unsigned *peer_src);
void online_close(void);
