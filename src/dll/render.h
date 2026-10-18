#pragma once
// Direct3D-9-Haken fuer eigene Kamera / Splitscreen (siehe render.c)
void render_init(void (*logger)(const char *, ...), volatile long *frame, int need_device);
int render_save_view(void);      // aktuelles Bild (Backbuffer) zwischenspeichern
int render_restore_view(void);   // zwischengespeichertes Bild zurueck in den Backbuffer
// Eigene Kamera nur fuer die Darstellung (nicht die Logik): m = Weltraum-Transformation mit
// VP_eigen = m * VP_spiel, view = eigene View-Matrix (beides 4x4, Zeilenvektoren). on = 0 schaltet ab.
void render_set_view(int on, const float *m, const float *view);
