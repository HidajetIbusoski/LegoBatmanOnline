# LEGO Batman Online

Online-Koop für **LEGO Batman: The Videogame (2008, PC/Steam)**. Zwei Spieler, so wie im lokalen Koop, nur über das Internet.

## Ziel

- Spielt sich wie lokaler Koop: Spieler 2 kann jederzeit einsteigen und wieder aussteigen.
- So viele Spieler wie das Original lokal erlaubt: **2**.
- Für Endnutzer: App starten, Freund per Code hinzufügen, gemeinsame Welt anlegen, **Zusammen spielen** oder **Alleine spielen**.
- Stabile Verbindung: direkt per UDP zwischen den Spielern, Relay als Fallback.

## Technischer Ansatz (Entwurf)

Beide Spieler starten das Spiel selbst. Die Mod synchronisiert **Eingaben, nicht den Spielzustand** (Lockstep, wie bei Emulator-Netplay):

1. Eine Proxy-DLL wird vom Spiel geladen (welche, hängt von den Imports der EXE ab).
2. Sie hakt sich in das Auslesen der Controller und in den Frame-Takt ein.
3. Jeder Frame: lokale Eingabe senden, die Eingabe des Partners abwarten, beide Instanzen bekommen exakt dieselben Eingaben für Spieler 1 und 2.
4. Ein kleiner Eingabepuffer (2–3 Frames) gleicht Latenz aus.
5. Desync-Erkennung über Prüfsummen (z. B. Positionen der Figuren).

Das funktioniert nur, wenn das Spiel bei gleichen Eingaben und gleichem Zeitschritt deterministisch läuft. Genau das klärt Phase 0.

## Phasen

- [x] **Phase 0: Machbarkeit.** EXE analysiert, Determinismus auf einem PC bestätigt, siehe [docs/phase0-befunde.md](docs/phase0-befunde.md).
- [x] **Phase 1:** Hook-DLL (per Launcher injiziert statt Proxy-DLL, damit nichts im Spielordner landet). Sie zeichnet Eingaben auf, spielt Skripte ab und vergleicht Prüfsummen, siehe [docs/phase1-werkzeuge.md](docs/phase1-werkzeuge.md).
- [x] **Phase 2:** Lockstep (`src/dll/net.c`): lokal mit zwei Instanzen bestätigt, auch mit 20 % Paketverlust. Echte Controller (XInput, DirectInput/DualSense) und Tastatur werden gelesen.
- [x] **Phase 3:** Internet ohne eigenen Server: STUN, Einladungscodes, Hole Punching, Spielstand-Übertragung (`src/launcher/online.c`). Direktmodus per IP für VPN.
- [x] **Phase 4:** App für Endnutzer (`app/LegoBatmanOnline`, WPF): Freundescodes, Freundesliste mit Online-Status und gemeinsame Welten (ein geteilter Spielstand, alleine oder zusammen spielbar, Abgleich über den Vermittler). Siehe [docs/app.md](docs/app.md).
- [ ] **Offen:** Test zwischen zwei echten PCs in verschiedenen Haushalten (Determinismus über verschiedene Hardware, NAT), DualSense-Belegung mit echtem Spieler.

## Release bauen

```
powershell -ExecutionPolicy Bypass -File tools\make_release.ps1
```

Ergebnis: `dist\LegoBatmanOnline.zip` mit einer einzelnen `LegoBatmanOnline.exe` (enthält Launcher und Mod-DLL, kein .NET nötig), `ANLEITUNG.txt` und dem Konsolen-Launcher unter `Extras\`.
