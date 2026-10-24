# Desync im Playtest 23.10.2026 (18:04) – Ursache und Fix

Team: Programmierer, Code-Reviewer, Netzwerk-Engineer, kritischer Tester (Notizen und Berichte im Sitzungs-Notizordner).

## Was passiert war

- Die Einstellungen waren gleich: "Einstellungen gleich" bei Frame 30. Das Netz war unauffällig (0 % Verlust, Rundlaufzeit Median 21 ms).
- Nach 823 Frames im Level wich die Figur des Partners um 0,0001 ab. Zufall und alle anderen Werte waren bis dahin gleich.
- Schon vor dem Level liefen die Spiele 1 Frame versetzt. Der Host ging einen Frame früher ins Levelladen (F2079 gegen F2080) und erreichte den Levelstart einen Frame später (F2814 gegen F2813).
- Dasselbe Muster hatte der Desync vom 22.10. in der offenen Welt.

## Ursache

Die Musik im Menü und in den Zwischenszenen läuft in Echtzeit (Ton-Streamer), und die Zwischenszenen wechseln im Takt dieser Musik. Entsteht die Musikstimme auf einem PC einen Frame später (Ladezeit), wechseln dort alle folgenden Szenen einen Frame später.

Die Eingaben vor dem Level gelten je absolutem Frame. Sie treffen dann auf den beiden PCs verschiedene Spielmomente, zum Beispiel beim Überspringen einer Zwischensequenz. Der Zustand, der daraus entsteht, bleibt bis ins Level verschieden. Die Schranke am Levelstart gleicht nur Zufall und Eingabe-Takt ab und konnte das nicht reparieren. Einige hundert Frames später laufen die Figuren dann auseinander.

Auf einem einzelnen PC tritt der Versatz nur selten auf. Deshalb waren die lokalen Tests fast immer synchron. Der frühere Ausreißer K0 (22.10.) war genau dieser Fall.

## Fix

1. **Schranke an jedem Szenenstart** (`LBO_SCENEBARRIER`, Standard an)
   - Beide Spiele treffen sich im ersten Frame, in dem nach einem kurzen Szenenwechsel (höchstens 30 Frames ohne Figuren) wieder Figuren da sind.
   - Dort übernimmt der Gast vom Host: Frame-Zähler, Spielzeit, Logik-Zeit und die virtuelle Uhr. Danach vergleichen beide ihren Zustand.
   - Lange Ladephasen sind ausgenommen, dort ist der erste Frame mit Figuren ladezeitabhängig; zuständig bleiben Levelanker und Schranke nach dem Laden.
   - Sieht nur eine Seite eine Schranke, wird sie übersprungen und laut geloggt. Einen Hänger gibt es dann nicht.
2. **Abgleich an jeder Schranke:**
   - Frame-Zähler (0x11CF0E4 samt Kopie, Fristen), Logik-Tick, Spielzeit, Logik-Zeit und virtuelle Uhr vom Host
   - Zustandsvergleich mit lauter Logzeile bei Abweichung
3. **Gleiche Controller-Zuordnung:** Im Online-Modus wird `Mappings.dat` nicht gelesen, beide nutzen die eingebaute Zuordnung. Ihr Hash wird bei Frame 30 verglichen.
4. **Fensterfokus:** Im Online-Modus schreibt die Fensterprozedur des Spiels immer "aktiv".
5. **Start unter Last:** UDP-Fehlermeldungen beim Verbindungsaufbau führten dazu, dass ein Partner fälschlich als verloren galt. Das ist behoben.
6. **Diagnose für den nächsten Fall:**
   - beide Pads je Frame in frames.csv
   - Pad-Hash in der Prüfsumme; die DESYNC-Zeile meldet "Eingaben verschieden" oder "Figuren verschieden"
   - Grafikkarte, GPU-Klasse und QPF im Log
   - dwPacketNumber = Lockstep-Frame
7. Die Schranke beim Level-Init (`LBO_INITBARRIER`) ist abschaltbar, Standard aus. Sie richtete sich an einem Ladeschritt aus, der selbst ladezeitabhängig ist (Test TF2O).

## Nachweis (lokale Paartests, eingebaute Testschalter)

- **Reproduktion:** Host `LBO_TESTPROGSHIFT=1,5034,1`, Gast `LBO_TESTPROGSHIFT=2,6500` erzeugt den natürlichen Versatz in den Zwischenszenen.
  - Ohne Fix (`LBO_SCENEBARRIER=0`): Desync bei rel 1785, bit-gleich mit dem früheren Ausreißer.
  - Mit Fix: zweimal synchron.
- **Offene Welt:** dreimal 23.005 Frames synchron, auch mit künstlichem Versatz.
- **Weitere Läufe:** Level ohne Schalter synchron; Launcher-Ablauf synchron; Batman 1 identisch (Ergebnisse des Testers).

## Offen

- Ein Versatz, der mitten in einer langen Ladephase entsteht, ist nicht durch eine Schranke gedeckt. In keinem Lauf hat er zu einem Desync geführt.
- Den endgültigen Beweis liefert der nächste Playtest auf zwei PCs. Bei einem neuen Desync zeigen die Logs jetzt, ob Eingaben, Figuren, Zähler oder der Zustand an einer Schranke abwichen.
