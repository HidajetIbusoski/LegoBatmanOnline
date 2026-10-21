# Morgenbericht 21.10.2026

## Kurz

- **WLAN:** Die Mod misst die Verbindung jetzt laufend und passt die Eingabeverzögerung an. Dazu kommt ein Zeitabgleich zwischen beiden PCs. Mit einem simulierten schlechten WLAN gibt es im Level spürbares Ruckeln (35–100 ms) noch in 20 statt 145 Frames, Standbilder über 200 ms 4 statt 17.
- **Schwächere PCs:** Die Mod wartet im Level nur noch ~2,3 statt ~7 ms je Frame. Beim Laden verbrennt sie keinen CPU-Kern mehr (1 % statt 40–48 %), die Ladephasen sind 13 % kürzer.
- **Fehler aus eurem Abendtest (20.10., 23:32/23:33):** Beide Male kam das Spiel deines Mitspielers nicht über den Start hinaus, vermutlich weil Steam bei ihm nicht lief. App und Launcher starten Steam jetzt selbst und warten auf die Anmeldung. Außerdem gibt es einen klaren Hinweis, wenn das Spiel des Partners nicht startet.
- **Spielstart schneller:** Der absichtliche Langsamstart (bisher ~15 s zähe Logos) ist weg. Logos und Titelbild dauern ~9–11 statt ~24 s.
- **Neue Version:** `dist-v4\LegoBatmanOnline.zip`. Beide brauchen diese Version, das Netzprotokoll hat sich geändert.

## 1. Was im Abendtest schiefging

Dein Spiel lief, vom Mitspieler kam aber keine einzige Eingabe:
- **23:32:** Nach ~8 s kamen auch keine Lebenszeichen mehr; nach 30 s lief dein Spiel allein weiter.
- **23:33:** Lebenszeichen kamen weiter, Eingaben nie. Sein Spielprozess lief also, das Spiel startete aber nicht, und deins wartete ohne Hinweis.

Batman 2 hat einen Steam-Kopierschutz. Läuft Steam nicht oder ist niemand angemeldet, startet das Spiel Steam und beendet sich, oder es wartet auf ein Steam-Fenster. Das passt zu beiden Fällen. Sicher weiß ich es nicht, weil die App bisher nur bei Desyncs Logs an den Partner schickte.

Jetzt:
- App und Launcher prüfen vor dem Verbindungsaufbau, ob Steam läuft und angemeldet ist, starten es sonst und warten bis zu 2 Minuten ("Warte auf Steam …").
- Kommt das Spiel des Partners nach 45 s nicht in Gang, erscheint ein Hinweis. Fällt er beim Start weg, ein eigener Hinweis.
- Nach jeder Batman-2-Sitzung, die nicht bis ins Level kam oder abbrach, schickt die App Log, letzte Frames und App-Protokoll an den Partner (`%LOCALAPPDATA%\LegoBatmanOnline\logs\<Zeit>-partner`). Beim nächsten Problem habe ich also beide Seiten.
- Mein Fehler von gestern behoben: Bei Frame 2 stand das Spiel jedes Mal ~2,3 s (Warten auf einen Hilfs-Thread).

## 2. WLAN

Bisher wurde die Eingabeverzögerung einmal beim Start aus dem Ping bestimmt und blieb fest. WLAN schwankt aber. Neu:
- **Verzögerung passt sich an:** Grundlage ist das 95. Perzentil der laufend gemessenen Rundlaufzeit. Gutes Netz: ~3 Frames, mittelmäßiges WLAN: 5–6, schlechtes: 9–10, höchstens 12 (200 ms).
- **Zeitabgleich:** Bisher hielt jede Seite stur ihren 60-Hz-Takt und rannte immer wieder in Wartephasen. Jetzt bremst die Seite, die vorne liegt, um höchstens 4 ms je Frame.
- **Bestätigungen:** Jedes Paket enthält alle Eingaben, die der Partner noch nicht bestätigt hat. Auch längere Verlust-Serien werden so ersetzt.
- **Weniger Last beim Warten:** Neu gesendet wird alle 8 statt alle 4 ms.
- **Messwerte im Log:** alle 3000 Frames eine Zeile `Netz-Qualitaet` mit Rundlaufzeit, Verlust, Verzögerung und Wartephasen.

Getestet habe ich mit einer WLAN-Nachbildung auf diesem PC (Latenz, Schwankung, Funklöcher, Verlust), immer zwei Spiele gegeneinander:

| Profil (Level, 3.500 Frames) | alt | neu |
|---|---|---|
| mittel: 25 + 0–40 ms, 200 ms Funkloch alle ~6 s, 2 % Verlust | +5 % Zeit, 19 Frames über 100 ms | +3 % Zeit, 7 Frames über 100 ms |
| schlecht: 40 + 0–80 ms, 300 ms alle ~5 s, 5 % Verlust | 145 Ruckel-Frames, 17 Standbilder | 20 Ruckel-Frames, 4 Standbilder |

Zum Vergleich ohne WLAN: +2 % Zeit. Batman 2 und Batman 1 liefen in allen Läufen in allen Werten gleich, auch während sich die Verzögerung änderte.

Grenze: Funklöcher, die länger sind als die Verzögerung, bleiben ein kurzes Anhalten. Das ginge nur mit dauerhaft höherer Eingabeverzögerung weg.

## 3. Schnellerer Spielstart

Der Langsamstart (320 Frames mit je +50 ms) sollte einen seltenen 1-Frame-Versatz im Intro verhindern. Den gleicht inzwischen die Schranke am Levelstart aus. Ohne Langsamstart:
- **Test S2:** Genau dieser Versatz trat auf (Gast einen Frame früher), nach der Schranke war alles gleich.
- **Test S1:** Kein Versatz, alles gleich.

Logos und Titelbild dauern jetzt ~9–11 s statt ~24 s. Außerdem stand das Spiel bei Frame 2 bisher immer ~2,3 s (mein Fehler vom 20.10.); jetzt wartet es dort nur noch auf den gleichzeitig startenden Partner.

## 4. Schwächere PCs

Im Lockstep bestimmt der langsamere PC das Tempo beider Spiele. Deshalb habe ich die Last der Mod gesenkt, ohne am Spiel selbst etwas zu verschlechtern:
- **Warteschleifen beim Laden** gaben den Prozessor nicht ab und belegten einen ganzen Kern. Jetzt überlassen sie ihn den Spiel-Threads, auf die sie warten: Der Hauptthread rechnet beim Laden 1 % statt 40–48 % der Zeit, Ladephasen sind 13 % kürzer. Auf einem Vierkerner dürfte das mehr bringen als hier.
- **Wartezeit pro Frame** im Level von ~7 auf ~2,3 ms (bei 16,7 ms je Frame): kürzere, genau gemessene Ruhefenster statt `Sleep(2)`.

## 5. Commits

- `163903b` Netz für WLAN, weniger Last für schwache PCs, Start-Fehler aus dem Playtest
- Langsamstart aus, Doku, Morgenbericht (siehe `git log`)

Details: `docs/netz-wlan.md`.

## 6. Für euren nächsten Test

1. Beide entpacken `dist-v4\LegoBatmanOnline.zip` und starten die App daraus. Die alte Version kann mit der neuen nicht spielen.
2. Zusammen spielen wie gewohnt. Läuft Steam bei einem nicht, startet die App es.
3. Nach dem Spielen liegen bei Problemen die Daten beider Seiten in `%LOCALAPPDATA%\LegoBatmanOnline\logs`.
