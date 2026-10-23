# Desync im Playtest 23.10.2026 (16:02) – Ursache und Fix

## Ursache
- Beim Host war die eigene pcconfig.txt im Moment des Starts nicht lesbar (vermutlich von einem anderen Prozess exklusiv geöffnet). Launcher und Mod fielen deshalb still auf die jeweils eigene Datei zurück, und der Gast bekam keine Einstellungen.
- Das Spiel des Hosts baute das Bild noch mit 120 Hz auf, bevor die Mod auf 60 Hz umstellte. Der Gast lief mit 60 Hz.
- Ab Frame 2957 nach Levelstart liefen Figuren und Zufall auseinander. Das ist dasselbe Muster wie im lokalen 60/120-Hz-Test KH.
- Das Netz war unauffällig: Host 0 % Verlust, Gast 0,2 %, Rundlaufzeit Median 32–35 ms.

## Fix (Team: Senior Developer, Network Engineer, Security Engineer, Reviewer)
- **Launcher:**
  - Der Host liest seine Datei robust: alle Freigaben, 15 Versuche, und bei einer Sperre steht der haltende Prozess im Log.
  - Rückfall: zuerst die zuletzt gute Kopie, sonst ein eingebauter Standard.
  - Er sendet immer genau eine Datei. Beide Seiten benutzen dieselben Bytes (`netz_basis.txt`, `LBO_HOSTCONFIG`, `LBO_CFGHASH`).
  - Die Bereit-Schranke vergleicht den SHA-256. Weicht er ab, startet kein Spiel.
  - Die Mod-Version ist jetzt 2. Ein alter Partner bekommt eine klare Meldung.
- **Mod:**
  - Keinen Rückfall auf die eigene Datei mehr.
  - `netz_pcconfig.txt` entsteht aus einer festen Vorlage mit Whitelist und geklemmten Werten.
  - Die 60 Hz sind schon beim Einlesen gepatcht (0x634660, 0x63fee7) und gelten damit vor CreateDevice.
  - Bei Frame 30 tauschen beide einen Hash von Datei und Laufzeitwerten aus. Weicht er ab, kommt sofort der Hinweis "Einstellungen verschieden" statt eines späteren Desyncs.
  - Die Prüfsumme ist in Figuren und Zufall geteilt: Bei einer Figurenabweichung gibt es keine sinnlosen Reparaturen mehr, nach dem Hinweis gar keine mehr.
- **Sicherheit (Dateiempfang):**
  - Prüfung von Größen und Längen; behoben sind ein Heap-Überlauf und ein Lesen über den Puffer hinaus.
  - Es werden nur Pakete von der Adresse des Partners angenommen.
  - Dateinamen sind auf eine Whitelist beschränkt; Speicherlecks sind behoben.
- **App:** Größenlimits für Partner-Diagnosedaten, eindeutige Ordner, `netz_basis.txt` geht mit in die Diagnose.

## Tests
Reviewer-Bericht: Notizordner `reviewer-ergebnis.md`.
- **Playtest nachgestellt:** Host-Datei gesperrt, Gast mit anderer Qualität, Auflösung, VSync und Hz. Ergebnis: "Einstellungen gleich", synchron bis 3234 Frames nach Levelstart (Abweichung damals ab 2957).
- **Normaler Ablauf:** synchron.
- **Offene Welt mit anderen eigenen Einstellungen:** 23.005 Frames, synchron.
- **Gegenprobe mit anderer Grafik:** Hinweis bei Frame 30.
- **Alte Version gegen neue:** klare Meldung, kein Start.
- **Batman 1:** Spielstandübertragung und Netztest gleich.
- **Offen:** Ein Vollbild-Start auf einem 120/144-Hz-Monitor ist ungetestet; alle Tests liefen im Fenster. Weicht die Rate trotzdem ab, meldet die Prüfung bei Frame 30 das sofort.
- **Selbstheilung:** Sie repariert nur den Zufall, nicht den Zustand der Spielobjekte. Das war vorher genauso.
