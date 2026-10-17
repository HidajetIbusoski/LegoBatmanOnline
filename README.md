# LEGO Batman Online

Online-Koop für **LEGO Batman: The Videogame (2008, PC/Steam)**. Zwei Spieler, so wie im lokalen Koop, nur über das Internet.

## Ziel

- Spielt sich wie lokaler Koop: Spieler 2 kann jederzeit einsteigen und wieder aussteigen.
- So viele Spieler wie das Original lokal erlaubt: **2**.
- Für Endnutzer: Launcher starten, auf **Hosten** oder **Beitreten** klicken, Code teilen, fertig.
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

- [ ] **Phase 0: Machbarkeit.** EXE analysieren (Imports, Kopierschutz, Input-API, Timing, Zufallszahlen). Determinismus-Test: Eingaben aufnehmen, abspielen und die Positionen vergleichen. Die statische Analyse ist fertig, siehe [docs/phase0-befunde.md](docs/phase0-befunde.md).
- [ ] **Phase 1:** Proxy-DLL, die Eingaben lokal aufzeichnet und wieder abspielt.
- [ ] **Phase 2:** Lockstep über LAN.
- [ ] **Phase 3:** Internet (NAT-Traversal, Relay, Einladungscode).
- [ ] **Phase 4:** Launcher für Endnutzer.
