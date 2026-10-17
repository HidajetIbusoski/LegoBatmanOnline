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
- 03:15 Launcher (lbo-launch.exe) und Hook-DLL (lbo.dll) gebaut. Injektion ohne Dateien im Spielordner funktioniert. Hooks auf Frame-Ende 0x6e0240 und Weltupdate 0x4aee90 greifen, Prüfsummen-CSV läuft.
- 03:15 Spielstand-Ordner nach runs/savebackup-0250 gesichert (pcconfig.txt wurde durch -Windowed-Läufe schon angepasst).
- 03:15 Befund: Das Spiel erkennt XInput-Pads nur über die DirectInput-Geräteliste (Callback 0x522fe0). Die DLL meldet virtuelle Pads selbst an (0x51d9b0, 0x51d5f0, 0x51d800) und blendet echte Gamepads (DualSense, Maple) im Skriptmodus aus.
