# Phase 1: Werkzeuge (Launcher, Hook-DLL, Tests)

Stand 2026-10-17, in der Nacht entstanden. Alles läuft ohne Änderungen am Spielordner.

## Bauen

```bat
build.bat
```

Das Skript baut mit MSVC x86 (VS 2026 Community) nach `build\`:

- `lbo-launch.exe`: startet `LEGOBatman.exe` angehalten, lädt `lbo.dll` per `LoadLibraryA`-Remote-Thread und lässt das Spiel dann weiterlaufen.
- `lbo.dll`: die Hook-DLL.

## Starten

```sh
tools/start_live.sh <run> [skript]      # Skriptmodus mit Live-Steuerung über runs/live.txt
tools/run_test.sh <run> <skript> <frames>  # unbeaufsichtigter Testlauf mit festen Determinismus-Einstellungen
tools/run_net.sh <name> <frames> [delay]   # Lockstep-Test: Host und Client lokal über 127.0.0.1
python tools/compare_runs.py runs/A runs/B [--by-w]  # Prüfsummen zweier Läufe vergleichen
python tools/live.py runs/<run> --wait 1 "rel 20 0 1000 0 0 0 0 0 0"  # Live-Befehl senden und Screenshot holen
```

Ausgaben landen in `runs/<run>/` (per `.gitignore` ausgeschlossen):

- `lbo.log`: Protokoll der DLL
- `frames.csv`: eine Zeile pro Frame-Ende, mit Weltupdate-Zähler, dt, Figurenzahl, Prüfsummen, Positionen von Spieler 1 und 2, allen 6 PRNG-Zuständen und den Figuren-IDs
- Screenshots
- umgeleitete Spielstand- und Config-Schreibzugriffe

## Umgebungsvariablen der DLL

| Variable | Wirkung |
|---|---|
| `LBO_DIR` | Ausgabeordner |
| `LBO_MODE` | `off` (nur loggen), `record` (echte XInput-Eingaben aufnehmen), `script` (Eingaben aus Skript) |
| `LBO_SCRIPT` | Eingabeskript (Format siehe Kopf von `src/dll/lbo.c`) |
| `LBO_LIVE` | Befehlsdatei für Live-Steuerung: `rel`, `shot`, `peek`, `poke`, `clear`, `quit` |
| `LBO_FIXEDDT=1` | festes dt 1/60 über den eingebauten Override `0xadf690` |
| `LBO_FPSCAP=60` | Bildrate begrenzen, damit Spielzeit = Echtzeit |
| `LBO_SYNCBG=2` | Lade-Worker nur am Frame-Ende laufen lassen. **Nötig für Determinismus.** |
| `LBO_NOMOVIES=1` | Bink-Videos überspringen |
| `LBO_MULTI=1` | Einzelinstanz-Sperre umgehen (zwei Spiele gleichzeitig) |
| `LBO_NET=host\|join`, `LBO_PEER`, `LBO_PORT`, `LBO_DELAY` | Lockstep über UDP (Phase 2, Prototyp) |
| `LBO_MAXFRAMES`, `LBO_SHOT_EVERY` | Laufende begrenzen, Screenshots |
| `LBO_ALLOWSAVE=1` | Spielstand-Schutz abschalten (Standard: Schreibzugriffe umgeleitet) |

## Was die DLL im Spiel verändert (nur im Speicher)

| Stelle | Art | Zweck |
|---|---|---|
| `0x6e0240` Frame-Ende | Inline-Hook (5 Byte) | Frame-Zähler, festes dt, Worker-Sync, Lockstep-Barriere, Prüfsumme |
| `0x4aee90` Weltupdate | Inline-Hook | Weltupdate-Zähler W |
| `0x5206c0` Gerät lesen | Inline-Hook | echte Joysticks (Geräte 4–9) im Skriptmodus als „nicht verbunden“ melden |
| IAT `XInputGetState`/`GetCapabilities` | IAT | virtuelle Pads (Skript, Aufnahme, Netz) |
| IAT `CreateFileA`/`DeleteFileA` | IAT | Spielstand-Schutz |
| IAT `BinkOpen` | IAT | Videos überspringen |
| IAT `CreateMutexA` | IAT | Mehrfachinstanzen |

Zusätzlich meldet die DLL virtuelle Xbox-360-Pads beim Spiel an. Dafür ruft sie die Spielfunktionen `0x51d9b0`, `0x51d5f0` und `0x51d800` auf, mit PID/VID `0x028E045E` im Slot. Außerdem hält sie das Fensterfokus-Flag `0x94d3a0` auf 1.

## Wichtige Erkenntnisse aus den Tests

- **Determinismus:** Mit `LBO_FIXEDDT=1 LBO_SYNCBG=2` und gleichem Eingabeskript liefen alle Läufe bitgleich (r17–r23, r25): Menüs, Laden des Spielstands, Bathöhle, Spieler-2-Drop-in, ~5000 Frames Gameplay.
- Ohne `LBO_SYNCBG` verschieben asynchrone Ladejobs Spawns um Frames, dann weichen Zufall und Positionen ab.
- Das Pad-Polling (`0x6de440`) hängt am Present-Zähler `0x9d08d0` und nicht am Frame.
- Ohne Fokus verwirft das Spiel Pad-Eingaben und wartet in `0x6e0240`.
- Zwei Instanzen auf einem PC laufen, die zweite aber stark gebremst.
