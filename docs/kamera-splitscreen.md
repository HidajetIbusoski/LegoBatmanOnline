# Eigene Kamera pro Spieler (Splitscreen-Ersatz)

Stand 18.10.2026. Online hat jeder Spieler seinen eigenen Bildschirm. Statt eines geteilten Bildes bekommt deshalb jeder PC eine eigene Kamera, die der eigenen Figur folgt. In der App heißt der Schalter „Eigene Kamera für meine Figur“ (`LBO_OWNCAM=5`), er ist standardmäßig an.

## Warum nicht einfach die Kamera des Spiels ändern

LEGO Batman (2008) hat nur eine gemeinsame Kamera, und die Spiellogik hängt davon ab, was sie sieht. Figuren erscheinen zum Beispiel abhängig vom Bild. Bei allen Eingriffen in die Spielkamera liefen die Spiele im Lockstep auseinander:

| Versuch | Ergebnis |
|---|---|
| Kamera im Logik-Update ersetzen (Haken an `0x6f1e80`) | Desync ab Frame 6572: Beim Host erscheint eine Figur mehr |
| Kamera nur während des Zeichnens am Frame-Ende setzen (`0x6fb1c0`) | wirkungslos, die Zeichenlisten enthalten fertige Matrizen; zusätzliche Aufrufe störten die Logik |
| Doppelt zeichnen nur auf einer Seite | Desync: Das Zeichnen hat Nebenwirkungen auf die Logik |
| Doppelt zeichnen auf beiden Seiten | synchron, aber ohne Wirkung auf das Bild (siehe oben) |

Achtung: Bis zum 18.10. fand `write_shot` per `FindWindowA` bei zwei Spielen auf einem PC dasselbe Fenster, Bildvergleiche aus dieser Zeit sind wertlos. Jetzt sucht es das Fenster des eigenen Prozesses.

## Lösung: Kamera auf dem Weg zur Grafikkarte tauschen

`render.c` hängt sich in `IDirect3DDevice9::SetVertexShaderConstantF` ein. Die Shader bekommen:

| Register | Inhalt |
|---|---|
| `c0`–`c3` | View-Projection der Kamera, transponiert |
| `c12`–`c15` | View-Matrix, transponiert |
| `c16`–`c19` | Weltmatrix des Objekts |

Ermittelt wurde das mit `LBO_RENDERTRACE`, einem Aufruf-Trace eines ganzen Frames. Ablauf pro Frame:

1. **Am Anfang des Frame-Endes**, vor dem Zeichnen, wird die View-Projection der Spielkamera (`0x9a7050`) festgehalten. Der Schatten-Durchgang überschreibt diese globale Matrix später mit der Lichtkamera.
2. **`c0`:** Ein Upload, der genau dieser Matrix entspricht, wird durch `M · VP` ersetzt. Die Lichtkamera und andere Matrizen bleiben unverändert.
3. **`c12`:** Der direkt davor hochgeladene Block mit letzter Zeile `0 0 0 1` ist die View der Spielkamera. Er und alle gleichen Uploads danach werden durch die eigene View ersetzt.

Die Spiellogik sieht davon nichts, der Lockstep bleibt synchron. Das ist mit zwei Instanzen geprüft, mit eigener Kamera auf einer und auf beiden Seiten: 0 Desyncs, Frames bitgleich.

## Wie die eigene Kamera entsteht (`update_view_shift` in `lbo.c`)

- **Ausgangspunkt:** die Kamera-Weltmatrix `C` der Spielkamera (Kamera-Objekt `*(0x95f624)` + `0xB8`). Zeilen 0–2 sind die Basis, Zeile 2 ist die Blickrichtung, Zeile 3 die Augenposition `E`.
- **Folgt die Spielkamera den Spielern?** Die Mitte beider Figuren muss innerhalb von etwa 25° der Blickrichtung liegen. Sonst, etwa bei Zwischensequenzen, der Levelauswahl oder festen Kameras, wird nichts verändert. Ein- und Ausblenden laufen weich über `g_alpha`.
- **Drehung:** Die Basis wird um das Auge gedreht (Rodrigues), sodass die eigene Figur an fester Bildposition steht: Mitte, leicht unterhalb, wie das Spiel sie rahmt.
- **Heranfahren:** Die Kamera fährt auf der Sichtlinie bis zum Normalabstand heran. Der wird gelernt, solange beide Figuren nah beieinander stehen. Die Sichtlinie ist frei, weil die Spielkamera beide Figuren sieht, deshalb landet die Kamera nicht in Wänden.
- **Matrizen:** `V' = C'^{-1}` und `M = V' · C`.
- **Kontrolle:** Mit `LBO_CAMLOG` wird protokolliert, wo die eigene Figur im eigenen Bild steht. Gemessen wurden x = 0,00 und y = −0,12 bei Abstand 3,5.

## Bekannte Grenzen

- **Verdeckung:** Die eigene Figur kann von Geländern oder Gittern verdeckt sein, die die Spielkamera nicht stören.
- **Sichtbarkeitsprüfung:** Das Spiel prüft weiter mit der gemeinsamen Kamera, was gezeichnet wird. Was nur die eigene Kamera sehen würde, kann am Bildrand fehlen.
- **Abstandsgrenze:** Ob und wo das Spiel die Spieler bei großem Abstand stoppt, ist nicht gefunden. Im Test in der Bathöhle trennten sie sich auf über 10 Einheiten ohne Grenze. Der beobachtete Teleport von Spieler 2 war ein Sturz ins Wasser mit Wiedereinstieg (`0x43b970` → `0x5beb00`).
- **Zusätzliche Szenen:** Der Spiegel bzw. die Reflexion und HUD-Markierungen über Figuren werden mit der Spielkamera berechnet.

## Werkzeuge

| Variable | Wirkung |
|---|---|
| `LBO_OWNCAM=5` | eigene Kamera (Modi 1–4, 8, 9 waren Versuche, siehe oben) |
| `LBO_RENDERTRACE=F` | Direct3D-Aufrufe in Frame F mit Aufrufkette und Shader-Konstanten |
| `LBO_RENDERSTATS=N` | alle N Frames eine Zusammenfassung |
| `LBO_CAMLOG=N` | Kameramatrix, Spielerpositionen und Lage der eigenen Figur |
| `LBO_CAMTRACE=F` | alle Aufrufe von `0x72ffc0` (Kamera an den Renderer) in Frame F |
| `LBO_WATCHPOS=1\|2` | Hardware-Haltepunkt auf die Position von Spieler 1 bzw. 2; zählt die schreibenden Codeadressen |
| `LBO_OWNCAM_SHIFT=x` | zusätzliche Verschiebung zur Sichtkontrolle |

Skripte:
- `tools/test_config.py --skripte split_host.txt split_join.txt`: Trennungsszenario, Spieler 2 tritt bei, dann laufen beide auseinander.
- `tools/test_cam.py`: Einzelspiel in der Bathöhle. Achtung: Der Start im Einzelspiel ist nicht reproduzierbar, Läufe lassen sich dort nicht direkt vergleichen.
