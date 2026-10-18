# Eigene Kamera pro Spieler / Splitscreen: Befund (18.10.2026)

## Ziel

Jeder Spieler sieht auf seinem PC die eigene Figur. Die Spieler sollen sich weit voneinander entfernen können. LEGO Batman (2008) hat keinen Splitscreen: Beide teilen eine Kamera, und das Spiel hält beide Figuren im Bild.

## Was gefunden wurde

| Stelle | Inhalt |
|---|---|
| `*(0x95f624)` | Spielkamera. Skriptbefehle `ResetGameCamera` (`0x460ec0`) und `JudderGameCamera` (`0x460ca0`) aus der Befehlstabelle bei `0x93c0b4`. |
| Kamera+`0xB8` | 4×4-Weltmatrix der Kamera, Zeilen 0–2 Drehung, Zeile 3 Augenposition (gemessen mit `LBO_CAMLOG`). |
| `0x6f1e80(m)` | Übergibt die Kamera-Weltmatrix an den Renderer (`0x72ffc0`) und berechnet die View-Projection in `0x9a7050`/`0x9a7010`. Aufgerufen von 30 Stellen, im Level zweimal pro Frame mit der Matrix der Spielkamera. |
| `0x4af2bb` | Kopiert Kamera+`0xB8` nach `*(0xa96128)` und ruft `0x6f1e80`. Das allein bestimmt das Hauptbild nicht. |

## Versuch

`LBO_OWNCAM=1` hakt sich am Anfang von `0x6f1e80` ein. Ist die Matrix die der Spielkamera, wird die Augenposition um die halbe Differenz zur eigenen Figur verschoben. Getestet wurde mit zwei Instanzen online (`tools/test_config.py --skript`, Spieler 2 tritt bei); nur der Gast bekam die eigene Kamera.

| Modus | Ergebnis |
|---|---|
| 9: Haken ohne Wirkung | 9000 Frames synchron |
| 8: alles berechnen, nichts ersetzen | synchron |
| 1: Matrix ersetzen (ab Frame 6544 im Level) | Desync ab Frame 6572: Beim Host erscheint eine Figur mehr (6 statt 5) |

**Ergebnis:** Die Spiellogik hängt davon ab, was die Kamera sieht. Figuren erscheinen abhängig vom Bild, z. B. dürfen Gegner nur außerhalb des Bildes auftauchen. Mit unterschiedlichen Kameras auf beiden PCs laufen die Spiele deshalb sofort auseinander. Eine eigene Kamera nur für die Darstellung reicht nicht.

## Mögliche Wege (nicht umgesetzt)

1. **Echter Splitscreen auf beiden PCs.** Beide PCs zeichnen pro Frame beide Ansichten in derselben Reihenfolge. Damit sieht die Logik auf beiden Seiten denselben Zustand. Dazu muss die Szenenzeichnung gefunden und zweimal mit eigenem Viewport ausgeführt werden. Das ist ein großes Reverse-Engineering-Vorhaben mit offenem Ausgang, und die Grafiklast verdoppelt sich.
2. **Darstellung auf Direct3D-Ebene ändern.** Die Logik behält die gemeinsame Kamera, nur die Matrizen beim Zeichnen werden nachträglich umgerechnet, z. B. in `SetVertexShaderConstantF`. Riskant: Man muss genau wissen, welche Register welche Matrix enthalten.
3. **Sichtbarkeits-Abfragen der Logik finden und auf die gemeinsame Kamera umbiegen.** Die Stellen sind unbekannt, und es sind wahrscheinlich viele (Spawns, KI, Effekte).

Die Grenze, die beide Figuren im Bild hält, ist nicht gefunden. Der Positionswächter (`LBO_WATCHPOS`, Hardware-Haltepunkt auf die Position von Spieler 1) fand im normalen Spiel nur einen Schreiber (`0x42198e`). Für die Grenze müsste man die Spieler gezielt auseinanderlaufen lassen.

## Nebenbefund: Desync durch den Wachhund (behoben)

Bei einem der Versuche lief das Spiel ohne Kamera-Änderung auseinander. Der Host hatte bei Frame 2078 eine zusätzliche Wachhund-Auftauung. Der Hauptthread hing dabei in der Geräteerkennung von DirectInput (Stack `0x528xxx`) und wartete nicht auf den Lade-Thread. Der aufgetaute Lade-Thread lief dann parallel zur Simulation. Dasselbe Muster zeigt die echte Sitzung vom 18.10. bei Frame 25060 und 25144, dort ohne Folgen.

In keinem protokollierten Fall hat der Hauptthread auf den Lade-Thread gewartet. Die Schwelle liegt jetzt bei 10 s statt 500 ms (`BG_WATCHDOG_MS`). In allen Tests danach gab es 0 Auftauungen und 0 Desyncs.
