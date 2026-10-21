# Netz für WLAN und schwächere PCs (21.10.2026)

Ziel: Auch mit mittelmäßigem WLAN flüssig spielbar, ohne das Spiel zu verschlechtern. Gilt für LEGO Batman 1 und 2 (gemeinsamer Netzcode `src/dll/net.c`, beide Seiten brauchen dieselbe Version).

## Was im Netzcode passiert

| Teil | Vorher | Jetzt |
|---|---|---|
| Eingaben je Paket | immer die letzten 16 | alles ab der letzten Bestätigung des Partners (bis 32); Verlust-Serien werden so lückenlos ersetzt |
| Eingabeverzögerung | einmal beim Start aus dem Median von 20 Pings (2–8 Frames) | laufend aus dem 95. Perzentil der gemessenen Rundlaufzeit (2–12 Frames); rauf sofort, runter um 1 Frame je 15 s |
| Messung | keine | Folgenummer, Zeitstempel und Echo je Paket: Rundlaufzeit, Verlust; Messungen aus Lade-Frames (Paket lag im Puffer) werden verworfen |
| Takt | jede Seite hält stur 60 Hz | Zeitabgleich: Wer dem Partner voraus ist, verlängert seinen Frame um bis zu 4 ms |
| Warten auf den Partner | alle 4 ms neu senden | alle 8 ms (weniger Last im WLAN) |

Die Verzögerung ist rein lokal: Jede Seite füllt nur ihre eigene Eingabereihe (`g_local`). Wird sie größer, gilt die aktuelle Eingabe für mehrere Frames; wird sie kleiner, wird ein Frame lang nichts Neues eingetragen. Beide Seiten belegen am Anfang und nach jeder Schranke dieselben ersten `LBO_DELAY` Frames neutral vor, deshalb dürfen die aktuellen Verzögerungen verschieden sein.

Formel: `Verzögerung = (RTT95 / 2 + 10 ms) · 60 / 1000 + 2` Frames. RTT95 30 ms ergibt 3 Frames, 120 ms 6 Frames, 200 ms 8, 330 ms 12.

Zeitabgleich (wie GGPO): Jedes Paket trägt den aktuellen Frame des Absenders. Geschätzter Partner-Frame = gemeldeter Frame + (seither vergangene Zeit + halbe RTT) · 60/1000. Der Vorsprung wird geglättet (`net_advantage`), die Bildratenbremse beider DLLs verlängert ab 0,5 Frames Vorsprung den Frame um bis zu 4 ms.

Alle 3000 Frames steht im Log eine Zeile `Netz-Qualitaet`: Median und 95. Perzentil der RTT, Verlust, Verzögerung (Spanne), Wartephasen über 20 ms, Vorsprung.

## Testschalter

- `LBO_NETSIM=Latenz,Schwankung,Funkstille,Abstand,Verlust`, z. B. `25,40,200,6000,2`: jedes ausgehende Paket 25 ms + 0–40 ms, etwa alle 6 s (±50 %) 200 ms nichts (danach kommt alles auf einmal), 2 % Verlust. Auf beiden Seiten setzen.
- `LBO_ADAPTIVE=0` feste Verzögerung, `LBO_TIMESYNC=0` ohne Zeitabgleich (Vergleich mit dem alten Verhalten).

## Messungen (zwei Instanzen auf einem PC, Batman 2 bis Frame 24.000)

Profil „mittel“ (25 + 0–40 ms, 200 ms Funkstille alle ~6 s, 2 % Verlust), Level-Abschnitt (3.500 Frames):

| Lauf | Mehrzeit gegenüber 60 fps | 101–200 ms | über 200 ms |
|---|---|---|---|
| N35 ohne WLAN | +2 % | 1 | 0 |
| W0 fest 5 Frames | +5 % | 15 | 4 |
| W2 anpassend | +3 % | 7 | 0 |

Profil „schlecht“ (40 + 0–80 ms, 300 ms alle ~5 s, 5 % Verlust), Level, Host:

| Lauf | Mehrzeit | 35–100 ms | über 200 ms |
|---|---|---|---|
| W4 alt (fest 7, ohne Zeitabgleich) | +9 % | 145 | 17 |
| W3 neu | +7 % | 20 | 4 |

Batman 1 (Profil „mittel“, 9.000 Frames): Frames 35–50 ms 35 statt 186 mit Zeitabgleich. Alle Läufe beider Spiele in allen Frames gleich.

Funkstillen, die länger sind als die Verzögerung, zeigen sich weiter als kurzes Anhalten. Sie abzufangen hieße, die Eingabeverzögerung dauerhaft auf ihre Länge zu setzen.

## Schwächere PCs (lbo2.dll)

- Die Wartestellen beim Laden drehten mit `Sleep(0)` und belegten einen ganzen Kern (40–48 % CPU des Hauptthreads in Lade-Wartephasen). Jetzt geben sie den Kern zuerst per `SwitchToThread` an die Spiel-Threads ab und schlafen dann 1 ms: 1 % CPU, Ladephasen 13 % kürzer.
- Ruhe-Messfenster für Datei- und Ton-Threads 1 ms statt `Sleep(2)` (dauerte 2–3 ms): Wartezeit der Mod im Level von ~7 auf ~2,3 ms je Frame.
- `timeBeginPeriod(1)`, damit `Sleep(1)` überall 1 ms dauert.

## Start-Fehler aus dem Playtest 20.10. abends

Zweimal kam das Spiel des Mitspielers nicht über den Start hinaus (keine einzige Eingabe, einmal fielen auch die Lebenszeichen aus). Am wahrscheinlichsten lief Steam bei ihm nicht oder wartete auf eine Anmeldung; der Kopierschutz von Batman 2 braucht Steam.

- App und Launcher starten Steam bei Bedarf und warten auf die Anmeldung (`HKCU\Software\Valve\Steam\ActiveProcess\ActiveUser`), bevor die Verbindung aufgebaut wird.
- Hinweis nach 45 s, wenn das Spiel des Partners nicht startet; eigener Hinweis, wenn er beim Start wegfällt.
- Die App schickt bei Batman 2 nach jeder schiefgegangenen Sitzung Logs, letzte Frames und App-Protokoll an den Partner (`logs/<Zeit>-partner/`).
- Behoben: 2 s Stillstand bei Frame 2 (Warten auf den Lebenszeichen-Thread, der seit dem 20.10. dauerhaft läuft).
