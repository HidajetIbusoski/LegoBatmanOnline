# Morgenbericht 23.10.2026

## Kurz

- **"Nicht mehr synchron" in der offenen Welt:** Das lag nicht an der Verbindung. Ich habe zwei echte Ursachen gefunden und beide behoben:
  1. **Unterschiedliche Grafikeinstellungen.** Schon eine andere Bildwiederholrate (Host 120 Hz, Gast 60 Hz) oder andere Qualitätsstufen lassen die Figuren nach einigen Sekunden auseinanderlaufen. Jetzt spielen online beide mit den Grafikeinstellungen des Einladenden und mit 60 Hz. Auflösung, VSync, Lautstärke und Steuerung bleiben die eigenen.
  2. **Ein gemeinsamer Zufallsgenerator für 190 Programmteile.** Läuft ein einziger Teil (z. B. Musik oder Nachladen der Stadt) auf einem PC einen Frame früher, verschiebt das die Zufallszahlen aller anderen. Genau das stand in eurem Log von gestern: Beim Partner lief ein Verbraucher genau einen Frame später. Jetzt hat jeder Programmteil seinen eigenen Zufall; ein Ausreißer betrifft nur noch sich selbst.
- **Zwischensequenzen überspringen:** Geht jetzt bei jeder Zwischensequenz, auch beim langen Intro. START drücken (Tastatur: Enter), unten rechts erscheint die Anzeige des Spiels "Überspringen (Y)", dann Y (Tastatur: L). Das sind Anzeige und Funktion des Spiels selbst, nichts Selbstgebautes. Das Intro spart so gut 2 Minuten.
- **Verzerrte Musik:** Die Mod hat den Ton bisher mit bis zu 4,6 % Tonhöhenänderung an das Bild angeglichen, in Sprüngen. Das klang wie ein Lautstärkeregler, der hoch und runter gedreht wird. Jetzt höchstens 0,5 % (unter der Hörschwelle), langsam, und Musik in Stereo wird gar nicht mehr angefasst.
- **Neue Version:** `dist-v5\LegoBatmanOnline.zip`. Beide brauchen diese Version (der Einladende schickt jetzt seine Einstellungen mit).

## 1. Warum es in der offenen Welt auseinanderlief

Euer Log von gestern (Desync bei Spiel-Frame 52.793) zeigt: Beide Figuren standen still, die Partikel ruhten, das Spiel war im Übergang zum Laden der offenen Welt. Abgewichen ist nur ein einziger Zufallsgenerator (LCG16), und zwar so, dass die Werte beim Partner genau einen Frame später kamen. In diesem Moment zog nur noch ein Verbraucher (ein Teil des Ladebildschirms) Zufallszahlen, und der begann bei ihm einen Frame später. Die Figuren waren bei beiden noch gleich. Die Prüfsumme schlug trotzdem sofort an. Nach drei erfolglosen Reparaturen kam der Hinweis.

Statt wieder nur diesen einen Verbraucher zu suchen (so haben wir es bisher gemacht, z. B. beim Musiktakt am 20.10.), habe ich das Problem grundsätzlich angegangen:

**Zufall je Verbraucher.** Im Spiel greifen 135 Funktionen und 55 Aufrufstellen auf denselben Zufallsgenerator zu (612 Stellen im Code). Die Mod gibt jetzt jeder davon einen eigenen Zustand. Das Spiel merkt davon nichts, es bekommt weiter Zufallszahlen aus derselben Formel. Läuft ein Verbraucher auf einem PC einmal mehr oder einen Frame früher (Musik, Ladebildschirm, Nachladen der Stadt), verschiebt das nur noch seine eigenen Zahlen, nicht mehr die der Spiellogik. An jeder Schranke (Levelstart, nach jedem Laden) setzen beide Seiten alle Zustände neu. Die Liste der Stellen erzeugt `tools/gen_lcg_sites.py` aus dem Spielcode.

**Gleiche Grafikeinstellungen.** Ich habe zwei Spiele mit unterschiedlicher `pcconfig.txt` gegeneinander laufen lassen (Batman-2-Level, alles andere gleich):

| Test | Unterschied beim Gast | Ergebnis |
|---|---|---|
| CA | niedrige Shader-, Schatten- und Effektstufen | Figuren laufen ab Frame 912 nach Levelstart auseinander |
| CB | 1920×1080, 60 Hz, VSync an | ab Frame 105 auseinander |
| KV | nur VSync an | gleich |
| KR | nur 1920×1080 | gleich |
| KH | nur 60 statt 120 Hz | ab Frame 1785 auseinander |

Bei dir steht 1280×720 mit 120 Hz und hohen Effekten. Beim Partner sind die Werte vermutlich andere; seine Batman-2-Datei habe ich nicht, weil die App bisher die falsche Datei mitschickte (die von Batman 1, ebenfalls behoben).

Jetzt schickt der Launcher beim Verbinden die `pcconfig.txt` des Einladenden mit. Beide Spiele lesen daraus eine gemeinsame Datei:
- **vom Einladenden:** alle Qualitäts- und Effekt-Einstellungen
- **immer 60 Hz:** Das Spiel läuft online ohnehin mit 60 Bildern pro Sekunde
- **die eigenen:** Auflösung, Fenster, VSync, Helligkeit, Lautstärke, Untertitel, Maus und Vibration

Die echte `pcconfig.txt` wird dabei nicht verändert.

Tests mit dem neuen Stand:
- **Gemischte Einstellungen:** Host mit deinen echten Einstellungen, Gast mit niedriger Qualität, 1920×1080, 60 Hz und VSync. Zwei Läufe, beide bis zum Ende in allen Werten gleich (KM1, KM2).
- **Offene Welt:** Erstmals lokal getestet, mit eurem Spielstand von gestern. Zwei Testspiele laden ihn über "Spiel laden" (nur im Testordner, siehe unten). Danach 22.465 Frames, gut 6 Minuten, durch Gotham mit Laufen, Springen und Angreifen: beide gleich, 0 Desyncs.
- **Ungleiches Tempo:** Dasselbe mit künstlich ungleichem Tempo beim Gast (bis 30 ms Pause je Frame), mit alter und mit neuer Zufallslogik: beide gleich.
- **Ende-zu-Ende über den Launcher:** Die Einstellungen kommen beim Gast an, beide lesen eine identische Datei.

Ehrlich gesagt: Den Ein-Frame-Versatz von gestern konnte ich auf einem PC nicht nachstellen, auch mit der alten Logik nicht. Er hängt am Zeitverhalten zweier verschiedener PCs. Die neue Logik verhindert, dass so ein Versatz die Spiellogik erreicht oder einen Fehlalarm auslöst. Der echte Beweis ist euer nächster Test.

## 2. Zwischensequenzen überspringen

Das Spiel kann jede Zwischensequenz überspringen, schaltet das aber nur frei, wenn das Level es erlaubt. Beim ersten Durchspielen ist das Intro gesperrt. Die Mod setzt diese Freigabe jetzt immer. Ergebnis:
- START während einer Zwischensequenz zeigt unten rechts die Anzeige des Spiels "Überspringen (Y)".
- Y (Tastatur der App: L) überspringt. Das sind Grafik, Schrift und Taste des Spiels selbst.
- Die Tasten laufen wie alle Eingaben über den Lockstep, beide Spiele springen im selben Frame weiter.
- Gegenprobe ohne die Freischaltung: keine Anzeige, das Intro läuft weiter.
- Mit Skip beginnt das erste Level bei Frame ~11.900 statt ~18.700, also gut 2 Minuten früher.

Abschalten ließe es sich mit `LBO_CUTSKIP=0`.

## 3. Verzerrte Musik

Die Mod gleicht den echten Ton an die virtuelle Spieluhr an, damit Sprache lippensynchron bleibt. Dafür hat sie die Abspielgeschwindigkeit verstellt: zuletzt um bis zu 4,6 %, in Sprüngen, und bei größerem Abstand hat sie Stimmen kurz angehalten. Auf Musik klingt das genau wie dein Beispiel mit dem Lautstärkeregler. Neu:
- Stereo- und Surround-Stimmen (die Musik) werden gar nicht mehr verstellt.
- Mono-Stimmen höchstens um 0,5 %, und nur langsam (0,05 % je Frame). Das hört man nicht, holt aber 300 ms in einer Minute auf.
- Angehalten wird erst ab 400 ms Abstand (vorher 150–250 ms), die Ton-Pause bei Hängern erst ab 300 ms (vorher 60 ms).

Den Klang selbst konnte ich nachts nicht anhören. Bitte beim nächsten Spielen darauf achten.

## 4. Kleineres

- **Steam-Start beim Partner:** Gestern Abend um 23:55 beendete sich sein Spiel nach 1 s wieder. Steam galt als bereit, weil ein alter Eintrag in der Registry stand. Jetzt prüfen App und Launcher, ob genau dieser Steam-Prozess läuft. Nach einem eigenen Steam-Start warten sie mindestens 20 s. Beendet sich das Spiel trotzdem in den ersten 20 s, startet der Launcher es einmal neu.
- **Fehlerdaten:** Die App schickt bei Batman 2 jetzt die richtige `pcconfig.txt` mit.
- **Neue Werkzeuge:** `tools/first_diff_b2.py` vergleicht zwei Läufe. Neue Testskripte: `b2_anchor_skip.txt` (Intro übersprungen, Paartest in ~5 statt ~9 min) und `b2_openworld.txt` (offene Welt mit mitgebrachtem Spielstand, `LBO_SAVEFIND=1`, nur im Testordner).
- **Ein Ausreißer im Kontrolltest:** Ein Lauf (K0) wich bei Frame 1785 genau wie der 60/120-Hz-Test ab, obwohl beide 120 Hz hatten. Vermutlich hat das Spiel die Rate dort aus dem Anzeigemodus übernommen. Die Mod setzt deshalb online auch den Wert im Spiel auf 60 Hz und schreibt ins Log, wenn er sich ändert. Die Wiederholung (K2) und alle späteren Läufe waren gleich.

## 5. Neue Version und Mail

- `dist-v5\LegoBatmanOnline.zip` (62 MB). Beide müssen sie nutzen, denn der Verbindungsablauf hat sich geändert.
- Die Mail an ibusoskih@gmail.com konnte ich nicht schicken: Mir steht hier kein Mail-Zugang zur Verfügung, und 62 MB sind für die meisten Postfächer ohnehin zu groß. Die ZIP liegt im Ordner `dist-v5`, und ich habe sie dir hier im Chat angehängt.

## 6. Für euren nächsten Test

1. Beide installieren die neue Version (dist-v5).
2. Wer einlädt, bestimmt die Grafik. Wenn es bei deinem Partner ruckelt, lädt besser er ein, mit seinen Einstellungen.
3. Falls doch "nicht synchron" erscheint: Die App schickt die Fehlerdaten jetzt mit beiden Einstellungsdateien. Bitte die App danach eine Minute offen lassen.
