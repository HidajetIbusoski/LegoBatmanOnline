# Nachtarbeit 2026-10-17

Hidajet hat Claude am 2026-10-17 um 02:35 beauftragt, über Nacht selbständig weiterzuarbeiten und morgens einen Bericht zu liefern. Diese Datei ist der **gemeinsame Zustand** für alle Sitzungen dieser Nacht. Jede Sitzung, auch eine per Zeitplan fortgesetzte, liest sie zuerst und trägt ihren Fortschritt unten ein.

## Ziel der Nacht

Phase 1 vorbereiten und den Determinismus-Test aus Phase 0 so weit wie möglich durchziehen:

1. **Launcher** `lbo-launch.exe`: startet `LEGOBatman.exe` angehalten im Spielordner, lädt `lbo.dll` per `LoadLibraryA`-Remote-Thread und lässt das Spiel weiterlaufen. So landet **keine Datei im Spielordner**.
2. **Hook-DLL** `lbo.dll`:
   - Hook auf Frame-Ende `0x6e0240` (Frame-Zähler, Barriere-Platz für später).
   - Festes dt über `0xadf690 = 1/60` (abschaltbar).
   - XInput über IAT-Einträge `0x2a17c38` (GetState) und `0x2a17c3c` (GetCapabilities) abfangen: Aufnahme und Wiedergabe pro Frame.
   - Prüfsumme pro Frame: Figuren (`*(0xab364c)`, Stride 0x1648, Anzahl `0xab3648`, Felder `+0x5c..+0x67`, `+0x246`) und PRNG-Zustände. Ergebnis als CSV-Log.
3. **Test:** Lauf A nimmt ein Eingabeskript auf (oder spielt ein festes Skript ab), Lauf B spielt es ab. Danach die Prüfsummen-Logs vergleichen.

## Regeln

- Spieldateien nie verändern, nichts in den Spielordner schreiben, nichts aus dem Spielordner ins Repo kopieren.
- Logs und Aufnahmen gehen nach `C:\Users\hidaj\LegoBatmanOnline\runs\` (per `.gitignore` ausgeschlossen).
- Build: `build.bat` mit `vcvars32.bat` aus VS 2026 Community.
- Regelmäßig committen (`main`, Co-Authored-By-Zeile).
- **Heartbeat:** Eine aktive Sitzung schreibt alle paar Minuten die aktuelle Zeit nach `runs/heartbeat.txt`. Ist der Heartbeat jünger als 20 Minuten, arbeitet schon jemand, und eine per Zeitplan gestartete Sitzung beendet sich sofort.

## Fortschritt

- 02:35 Plan angelegt, Zeitplan-Fortsetzung eingerichtet (stündlich 03–08 Uhr, Abschlussbericht 08:30).
- ~03:00 Launcher (lbo-launch.exe) und Hook-DLL (lbo.dll) gebaut. Injektion ohne Dateien im Spielordner funktioniert. Hooks auf Frame-Ende 0x6e0240 und Weltupdate 0x4aee90 greifen, Prüfsummen-CSV läuft.
- ~03:00 Spielstand-Ordner nach runs/savebackup-0250 gesichert (pcconfig.txt wurde durch -Windowed-Läufe schon angepasst).
- ~03:00 Befund: Das Spiel erkennt XInput-Pads nur über die DirectInput-Geräteliste (Callback 0x522fe0). Die DLL meldet virtuelle Pads selbst an (0x51d9b0, 0x51d5f0, 0x51d800) und blendet echte Gamepads (DualSense, Maple) im Skriptmodus aus.
- 03:35 Weitere Befunde und Werkzeuge:
  - Belegung: Die Tastenbelegung hängt an der Produkt-PID/VID im Slot (`+8`). Mit `0x028E045E` (Xbox 360) greift der eingebaute Eintrag, danach funktionieren A/B im Menü.
  - Fokus: Ohne aktives Fenster verwirft das Spiel Pad-Eingaben und wartet in `0x6e0240` (Flag `0x94d3a0`). Die DLL hält das Flag im Skriptmodus auf 1.
  - Echte Joysticks (DualSense, „Maple") liefern im Skriptmodus nichts mehr (Hook auf `0x5206c0`).
  - Spielstand-Schutz: Schreibzugriffe auf `SaveGame*` und `pcconfig.txt` werden in den Lauf-Ordner umgeleitet. Grund: Nach „Load Game" ist Autosave auf Slot 1 aktiv. Der Original-Spielstand ist nachweislich unverändert (SHA-256 `4b29e1ab…`).
  - Bink-Videos (`BinkOpen`) lassen sich mit `LBO_NOMOVIES=1` überspringen. Das Intro ist dagegen eine Echtzeitsequenz.
  - Mit festem dt läuft die Spielzeit so schnell, wie die GPU Bilder liefert (~1000 fps). Deshalb gibt es jetzt `LBO_FPSCAP=60`.
  - Das Pad-Polling in `0x6de440` hängt am Present-Zähler `0x9d08d0`, nicht am Frame. Für den Lockstep muss pro Simulationsschritt genau einmal gepollt werden.
- 04:05 **Erster Determinismus-Test** (r13/r14, festes dt, gleiches Skript): bitgleich bis F 6668, dann Abweichung beim Betreten der Bathöhle (Figur spawnt einen Frame früher, PRNG kippt). r15/r16 weichen schon beim Übergang Intro→Titel um einen Frame ab. Ursache: asynchroner Lade-Worker `0x6e2cc0` (Job-Schlange `0xb03c68`, Event „BGFreeze" `0xb03bdc`).
- 04:20 **Mit `LBO_SYNCBG=2`** (Worker nur am Frame-Ende laufen lassen): r17 und r18 über 9000 Frames **vollständig identisch** (Positionen, alle 6 PRNG-Zustände, Figurenliste, Weltupdate-Zähler).
- 04:45 r19 und r20 (je 12.500 Frames) ebenfalls identisch, auch mit r17. **4 von 4 Läufen bitgleich.** Original-Spielstand weiterhin unverändert.
- 05:00 Zwei-Spieler-Test: Spieler 2 steigt per virtuellem Pad 1 (Start) in der Bathöhle ein. r21 = r22 bitgleich (10.500 Frames).
- 05:10 Einzelinstanz-Sperre (`CreateMutexA` in `0x527010`) per `LBO_MULTI=1` umgangen. Zwei Instanzen laufen gleichzeitig, die zweite gebremst. r25 (parallel gestartet) = r21.
- 05:20 **Lockstep-Prototyp** (`src/dll/net.c`): UDP, Host = Pad 0, Client = Pad 1, Eingabeverzögerung 3 Frames, 16-fache Redundanz pro Paket, Prüfsummen-Abgleich alle 60 Frames.
- 05:40 Zwei Fehler im Worker-Sync gefunden und behoben: (1) Der Worker läuft mit Priorität „unter normal" und verhungerte in der Hintergrundinstanz. Er wird jetzt auf normal gehoben. (2) Ein während des Freeze eingereihter Job konnte sein Wecksignal `BGWorkToDo` (`0xb03bd8`) verlieren. Die DLL weckt jetzt nach. Timeouts gibt es keine mehr (lieber warten als desynchronisieren).
- 06:40 **Netztest n03: Host und Client über 9000 Frames bitgleich, 0 Desyncs.** Der Host steuert die Menüs und lädt den Spielstand, der Client steigt als Spieler 2 ein, beide laufen herum. Jede Instanz kennt nur ihre eigenen Eingaben, der Rest kommt per UDP.
- 07:05 **Netztest n04 mit 20 % simuliertem Paketverlust** (1802 Pakete verworfen): Host = Client bitgleich, 0 Desyncs, und identisch mit dem verlustfreien Lauf n03. README und phase0-befunde.md aktualisiert.
- 07:15 Morgenbericht geschrieben (docs/nachtbericht-2026-10-17.md). Geplante Tasks pausiert. Nachtarbeit beendet.
