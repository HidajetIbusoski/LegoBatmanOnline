# Nachtbericht 17.10.2026

Hidajet, guten Morgen. Hier ist, was über Nacht passiert ist (ca. 02:35–07:15).

## Kurzfassung

1. **Das Spiel ist deterministisch.** Mit festem Zeitschritt, gleichen Eingaben und einer Synchronisation des Lade-Threads laufen wiederholte Läufe bitgenau gleich, über bis zu 12.500 Frames inklusive Menü, Spielstand-Laden, Bathöhle, Einstieg von Spieler 2 und Kämpfen.
2. **Ein Lockstep-Prototyp funktioniert.** Zwei Spielinstanzen auf deinem PC tauschen Eingaben über UDP aus. Sie bleiben über 9000 Frames identisch, auch bei 20 % simuliertem Paketverlust.
3. **Dein Spielstand ist unberührt.** Der SHA-256-Hash ist vor und nach allen Tests gleich (`4b29e1ab…`). Alle Schreibzugriffe des Spiels wurden umgeleitet. Eine Sicherung liegt zusätzlich in `runs/savebackup-0250/`.

Damit ist die Kernfrage aus Phase 0 beantwortet: **Lockstep-Online-Koop ist machbar.**

## Was gemacht wurde

| Zeit | Ergebnis | Commit |
|---|---|---|
| 02:35 | Plan, Fortschrittsprotokoll, Zeitplan-Fortsetzung | – |
| ~03:00 | Launcher `lbo-launch.exe` und Hook-DLL `lbo.dll`. Das Spiel wird gestartet und die DLL injiziert, **ohne Dateien im Spielordner** | `c9abc90` |
| ~03:35 | Virtueller Xbox-Pad, Fokus-Problem, Spielstand-Schutz, Video-Skip, 60-fps-Limit | `b0db6b6` |
| 04:05 | Erster Determinismus-Test: identisch bis zum Laden der Bathöhle, dann Abweichung. Ursache gefunden: der asynchrone Lade-Worker | `a72e271` |
| 04:20–04:45 | Mit Worker-Sync sind **4 von 4 Läufen bitgleich** | `680ad0f` |
| 05:00 | Spieler 2 steigt per zweitem Pad ein, ebenfalls bitgleich | – |
| 05:20 | Lockstep-Prototyp `src/dll/net.c`, zwei Instanzen gleichzeitig möglich | `2917a40` |
| 06:40 | Zwei Fehler im Worker-Sync behoben. **Netztest Host/Client: 9000 Frames identisch** | `91e60f6` |
| 07:05 | Test mit 20 % Paketverlust: identisch | `d4c5e75` |

Das ausführliche Protokoll steht in [nachtarbeit.md](nachtarbeit.md), die Bedienung in [phase1-werkzeuge.md](phase1-werkzeuge.md) und die Befunde (aktualisiert) in [phase0-befunde.md](phase0-befunde.md).

## Wichtigste technische Erkenntnisse

- **Zeitschritt:** Das Spiel misst dt per QPC (variabel). Der eingebaute Override `0xadf690 = 1/60` macht ihn fest. Zusätzlich braucht es ein 60-fps-Limit, sonst läuft die Spielzeit so schnell, wie die Grafikkarte rendert.
- **Die einzige Störquelle für den Determinismus war der Lade-Thread** (`0x6e2cc0`). Je nachdem, wie schnell die Festplatte ist, erscheinen Figuren einen Frame früher oder später. Die DLL lässt seine Jobs nur noch am Frame-Ende laufen und wartet darauf (`LBO_SYNCBG=2`). Dabei sind zwei Stolperfallen aufgetaucht, beide sind behoben:
  - Der Thread hat niedrige Priorität und verhungert unter Last.
  - Ein Wecksignal kann verloren gehen.
- **Controller:** XInput-Pads erkennt das Spiel nur über die DirectInput-Geräteliste, mit Tastenbelegung je nach Pad-Modell. Die DLL meldet virtuelle Xbox-360-Pads selbst an. Ohne Fensterfokus ignoriert das Spiel Pad-Eingaben. Die DLL hält das Fokus-Flag gesetzt.
- **Zweite Instanz:** Das Spiel blockiert normalerweise einen zweiten Start (Mutex). `LBO_MULTI=1` umgeht das, die zweite Instanz läuft aber langsamer.
- Gefunden habe ich an deinem PC einen **DualSense** und ein Gerät namens **„Maple“** als DirectInput-Joysticks. Im Testmodus werden beide ignoriert.

## Was nicht geklappt hat oder offen ist

- **Nur auf einem PC getestet.** Ob zwei verschiedene Rechner (Intel/AMD, andere Grafikkarte) gleich rechnen, ist offen. Das ist der wichtigste nächste Test.
- **Echte Controller im Netzmodus:** Der Prototyp liest lokal nur XInput-Pad 0. Dein **DualSense** läuft über DirectInput und würde im Netzmodus nicht erkannt. Abhilfe ist Steam Input oder DS4Windows, oder besser die Eingabe direkt aus der Gerätetabelle des Spiels übernehmen (noch zu bauen).
- **Tastatur und Maus** werden im Netzmodus noch nicht abgeschaltet. Tippt jemand, entsteht ein Desync (die Prüfsumme würde das melden).
- **Drop-out von Spieler 2** ist noch nicht getestet, nur der Einstieg.
- Im Netzmodus muss der Host die Menüs bedienen, beide Spiele starten gemeinsam ab dem Titel. Ein Einstieg mitten im Spiel ist noch nicht möglich.
- `pcconfig.txt` (Fenstergröße) wurde von den ersten beiden Testläufen überschrieben, bevor der Schutz aktiv war. Inhaltlich steht dort deine Auflösung 1920×1061 im Fenstermodus. Falls das Spiel jetzt im Fenster statt im Vollbild startet, bitte einmal im Spielmenü umstellen.
- **Nutzungslimit:** Gegen 05:00 war das Limit erreicht, die Arbeit stand bis zum Reset um 06:40. Die geplanten Fortsetzungen um 03:22 und 04:22 haben sich korrekt beendet, weil die Hauptsitzung noch lief (Heartbeat). Die um 05:22 und 06:22 sind am Limit gescheitert. Ab 06:41 ging es in dieser Sitzung weiter. Beide geplanten Tasks sind jetzt pausiert, du findest sie unter „Scheduled“ in der Seitenleiste.

## Nächste Schritte (Vorschlag)

1. **Test über zwei echte PCs im LAN** (`LBO_NET=host`/`join`, `LBO_PEER=<IP>:47100`). Dabei Prüfsummen beobachten.
2. Lokale Eingabe aus beliebigen Controllern (DualSense) übernehmen, Tastatur und Maus im Netzmodus sperren.
3. Drop-out testen, Wiederverbindung, sauberer Start-Handshake.
4. Danach Phase 3: Internet (NAT-Traversal, Relay, Einladungscode).

## Zum Selbst-Ausprobieren

Bauen in PowerShell:

```powershell
C:\Users\hidaj\LegoBatmanOnline\build.bat
```

Danach im Ordner `C:\Users\hidaj\LegoBatmanOnline` in Git Bash:

```bash
tools/run_test.sh demo tools/scripts/determinismus_batcave_2p.txt 9000
```

Das startet das Spiel und lässt es automatisch den Spielstand laden. Batman und Robin laufen dann ferngesteuert durch die Bathöhle. Schreibzugriffe auf deinen Spielstand werden umgeleitet.

```bash
tools/run_net.sh demo 9000 3
```

Das startet zwei Instanzen im Lockstep. Danach vergleichen:

```bash
python tools/compare_runs.py runs/demo_host runs/demo_join
```
