# LEGO Batman 2: Phase 0 (Machbarkeit), Zwischenstand

Stand: 18.10.2026. Ziel und Vorgehen wie bei Batman 1 ([phase0-befunde.md](phase0-befunde.md)): Lockstep-Netzwerk, Eingaben statt Spielzustand synchronisieren.

## Steckbrief

- **EXE:** `D:\SteamLibrary\steamapps\common\LEGO Batman 2\LEGOBatman2.exe`, Steam-App 213330, 18,7 MB, x86 (PE32), ImageBase `0x400000`, kein ASLR, Build 25.05.2012.
- **Steam-Kopierschutzhülle (SteamStub):**
  - Der Abschnitt `.bind` enthält den Einsprung (`0x136b2ee`).
  - `.text` ist verschlüsselt (Entropie 8,00) und wird beim Start entschlüsselt.
  - Mit `SteamAppId=213330` und laufendem Steam lässt sich das Spiel direkt starten, auch mit eingeklinkter DLL.
- **Importe:**
  - Grafik: `d3d9`, `d3dx9_41`.
  - Eingabe: `XINPUT1_3` (Ordinal 2 = `XInputGetState`, IAT `0xc55334`; Ordinal 4 = `XInputGetCapabilities`, IAT `0xc55330`) und `DINPUT8`.
  - Ton: `XAPOFX1_3`, also XAudio2 statt DirectSound.

## Entpacken für die Analyse

`src/dump/dump.c` → `build/dump.dll`. Sie wird per Launcher eingeklinkt, wartet, bis `.text` entschlüsselt ist (Entropie unter 7,2; nach 0,3 s), und schreibt das Speicherabbild nach `LBO_DUMP`.

```
set SteamAppId=213330
set LBO_DUMP=...\runs\batman2\dump.bin
build\lbo-launch.exe --game "...\LEGOBatman2.exe" --dll ...\build\dump.dll --wait
```

Aus dem Abbild entsteht eine analysierbare PE-Datei: Jeder Abschnitt liegt dort an seiner RVA, und die Importnamen sind über die OriginalFirstThunks erhalten. Sie liegt unter `runs\batman2\LEGOBatman2_entpackt.exe`. Weder Abbild noch Ghidra-Projekt kommen ins Repo, die Spieldateien bleiben unverändert.

## Befunde (Capstone, statisch)

| Thema | Batman 2 | Batman 1 zum Vergleich |
|---|---|---|
| Zufalls-Init | `0x9d5630` (Aufruf aus `0xaef0bb`): `0x11a1844 = 12345`, `0x11a1840 = 0x1f3ad27f`; weitere Generatoren mit 0 über `0x51a170`, `0x77f280`, `0x51a320`, `0x77d040`; CRT `srand` `0xbd1c8b` | `0x5ae190`, gleiche Seeds |
| Park-Miller | `0x51a340` (16807, Zustand mit `0x75bd924` XOR-verschleiert, Standardzustand `0x102b358`, 12 Aufrufer) | `0x68b5f0` |
| CRT `rand` (214013) | `0xbd1ca9` | `0x742c43` |
| Zeitschritt | fest `0x13935a8` = 1/60 (1/50 bei PAL, Flag `0x139a3d4`), Kehrwert `0x13937ac` = 60, Grenze `0x139396c` = 0,1 | gemessenes dt `0xa95fe0` |
| Timer | QPC-Init `0x521840`, Auslesen `0x5218c0`, Timer-Objekt `0x523aa0` | QPC direkt im Frame-Ende |
| Hintergrundarbeit | Thread-System: `0x5510a0` startet Arbeits-Threads (Funktion `0x5458b0`, Semaphoren, Priorität, Name); Job-Warteschlange `0x556290` (Semaphore + Event) | ein Worker `0x6e2cc0` mit Events `BGWorkToDo`/`BGFreeze` |
| Einzelinstanz | `CreateMutexA` in `0x635530`; Fensterklassen `TTalesWindow`, `TTalesAdditionalWindow` | Mutex `TTales-{…}` in `0x527010` |
| Speichern | Strings `SaveGame`, `pcconfig` vorhanden (`0xd16000`, `0xeca118`) | `Saved Games`-Pfad |


## Laufzeit-Sonde und Hauptschleife (18.10.2026, abends)

`src/dump/probe.c` → `build/probe.dll`:
- Die Sonde biegt den `Direct3DCreate9`-Aufruf des Spiels (`0x63e59a` → Thunk `0xbcc518`) um, sobald der Code entschlüsselt ist (nach ~10 ms). Dann hakt sie `CreateDevice` und am Spielgerät `EndScene` (Vtable-Index 42).
- `Present` des Geräts ruft das Spiel nicht auf. Die Vtables liegen auf dem Heap, Direct3D ist also umhüllt: ein eigenes Hilfsgerät hätte eine andere Vtable.
- Im exklusiven Vollbild ohne Fokus zeichnet das Spiel nicht. Für Tests deshalb `-- -Windowed` übergeben.
- Ohne Bildratenbremse läuft das Spiel mit etwa 330 Bildern pro Sekunde.

Aufrufkette bei `EndScene` (außen → innen): CRT `0xbd1f3b` → `0x640455` → `0x7e7010` → `0x7e6cd0` → `0x5528c0` → `0x5bc2d0` → … → Zeichnen.

Ghidra-Projekt: `runs\batman2\ghidra` (headless analysiert, 20 MB). Dekompilieren mit `tools\ghidra\decompile.bat <ausgabe.txt> 0xADR …`.

| Funktion | Inhalt | Batman 1 |
|---|---|---|
| `0x7e7010` | Hauptschleife: Spielobjekt `*0x1389e8c` anlegen, `do { Schritt (Vtable +0x18) } while` | `0x4b01d0`-Umfeld |
| `0x7e6cd0` | Spielschritt; ruft das Frame-Ende auf (beim Laden über `0x11cf0b8` zusätzlich), prüft per `0x7e9e80` (kritischer Abschnitt `0x138b9f8`), ob der Worker beschäftigt ist | Levelschleife |
| `0x5528c0` | Frame-Ende: dt per Timer nach `0x11cf0dc`; Begrenzung auf 0,1 (`0xcf30c4`); **Überschreibung `if (0x11cf0f8 != 0) dt = 0x11cf0f8`**; Frame-Zähler `0x11cf0e4`; Zeichnen `0x5bc2d0(1, Breite 0x11cc120, Höhe 0x11cc124)`; Gesamtzeit `0x11cf104`; am Ende `ReleaseSemaphore` für einen wartenden Thread (`*0x11cc114 + 0x8050`) | `0x6e0240`, Überschreibung `0xadf690`, Zähler `0xadf68c` |

**Folgerung:** Die Engine ist dieselbe Familie wie bei Batman 1. Fester Zeitschritt, Frame-Ende-Haken und Bildratenbremse lassen sich direkt übertragen: dt-Überschreibung `0x11cf0f8 = 1/60`, Haken am Anfang von `0x5528c0`.

## Mod-DLL `lbo2.dll` (18.10.2026, nachts)

`src/dll2/lbo2.c` → `build/lbo2.dll`, gestartet wie Batman 1 über `lbo-launch.exe --game …\LEGOBatman2.exe --dll build\lbo2.dll`. Alle Haken setzt ein eigener Thread, sobald die Hülle die jeweilige Stelle entschlüsselt bzw. die Importtabelle befüllt hat (nach ~0,2 s):

| Haken | Stelle | Zweck |
|---|---|---|
| Frame-Ende | `jmp` am Anfang von `0x5528c0` (`83 EC 40 53 57`) | Frame-Zähler, festes dt (`0x11cf0f8 = 1/60`), Bildratenbremse, CSV, Live-Befehle |
| XInput | IAT `0xc55334`/`0xc55330` | Eingabeskript und Live-Eingaben (Format wie Batman 1) |
| Mutex | IAT `0xc550bc` | zweite Instanz (`LBO_MULTI=1`) |
| Direct3D | Aufruf `0x63e59a` → `CreateDevice` (Vtable 16) → `EndScene` (42), `Reset` (16) | Screenshots aus dem Backbuffer, Fenstermodus erzwingen |
| Dateien | IAT `CreateFileA/W`, `DeleteFileA`, `MoveFileA` | Spielstand-Schutz |

Erkenntnisse aus den ersten Läufen:

- **Fokus:** Ohne aktives Fenster wartet das Spiel in `0x5bc170` mit `Sleep(10)`, solange `0x1153e18` (gesetzt von `WM_ACTIVATE`, Fensterprozedur `0x640a20`) 0 ist. Die DLL hält das Flag im Skriptmodus auf 1.
- **Fenstermodus:** `-Windowed` und `-Fullscreen` setzen nur `0x1365920`, das nie gelesen wird. Das Spiel läuft immer im exklusiven Vollbild und minimiert sich bei Fokusverlust. `LBO_WINDOWED=1` setzt bei `CreateDevice`/`Reset` `Windowed=1`.
- **Screenshots:** GDI (`BitBlt`, `PrintWindow`) liefert nur Weiß. Die DLL kopiert deshalb bei Bedarf in `EndScene` den Backbuffer.
- **Spielstand:** `%APPDATA%\Warner Bros. Interactive Entertainment\LEGO Batman 2\SavedGames\Slot1\GAME1.LEGOBatman2SaveGameData` (Autosave beim Start eines neuen Spiels), daneben `pcconfig.txt`. Ohne `LBO_ALLOWSAVE=1` leitet die DLL Schreibzugriffe dorthin (außer `CachedShaders`) in den Ausgabeordner um. Die leeren Ordner `SavedGames\Slot1` legt das Spiel trotzdem an.
- **Zwischensequenzen** lassen sich in einem neuen Spiel per Pad nicht überspringen (wohl weil sie noch nicht als gesehen gespeichert sind). Das Video der Gotham-City-News lässt sich überspringen.

## Spielerplätze und Figuren

| Adresse | Inhalt | Batman 1 |
|---|---|---|
| `*0x1398fe8` | Figurenarray, Abstand **`0x17f0`**, höchstens `0x40` Plätze (Schleife in `0xa4c4d0`) | `*0xab364c`, `0x1648` |
| `*0x1394b88` | Anzahl Figuren | `0xab3648` |
| `0x138f4e0[0..1]` | Spieler-Figur 1 und 2 (Getter `0x81e300`) | `0xab3960` |
| `0x13999dc` / `0x13999e0` | gesteuerte Figur Spieler 1 / 2, ggf. das Fahrzeug (`+0x7f0`); 0 = nicht beigetreten. Gefüllt in `0x8af4f0`, Skriptvariable `Player2Active` = `0x13999e0 != 0` | `Player2Active` `0xab3984` |

Felder einer Figur:

| Offset | Inhalt |
|---|---|
| `+0x5c/+0x60/+0x64` | Position x, y, z (zur Laufzeit bestätigt: x/z beim Laufen, y beim Springen; wie Batman 1) |
| `+0x218` | ≠ 0: Platz belegt |
| `+0x4f6` | short, Figuren-ID |
| `+0x176a` | Byte, 1 = von einem Spieler gesteuert (bei Spieler 2 erst nach dem Beitritt) |
| `+0x7f0` | gefahrenes Fahrzeug (wenn `+0x218` und `+0xd24` Bit 0) |

**Eingabe:** Batman 2 liest XInput direkt (`0x63b6f0`, Pads 0–3, danach DirectInput-Geräte). Neue Pads sucht es aber nur beim Start und nach `WM_DEVICECHANGE` (`0x63c1b0`, angestoßen über `0x1153e9c`). Ein im Spiel neu „eingesteckter“ virtueller Pad wird erst nach dieser Nachricht erkannt; die DLL schickt sie bei `connect`. Spieler 2 tritt mit Start auf Pad 2 bei (getestet: `0x13999e0` zeigt danach auf Figur 1, deren `+0x176a` wird 1). Im Spiel wird nur jedes zugeordnete Pad einmal pro Frame abgefragt.

## Thread-System

`0x5510a0` startet Threads über die Tabelle `0x102dbe0` (Abstand `0xb4`: Funktion `+0`, Argument `+4`, Name `+8`, Handle `+0xa8`, Thread-ID `+0xb0`). Startfunktion ist immer `0x5458b0`. Zur Laufzeit im ersten Level:

| Name | Funktion | Bedeutung für den Lockstep |
|---|---|---|
| `RENDER` | `0x59a870` | ruht (`ForceMultithreadedD3D 0`) |
| `m_readThread`, `m_decodeThread` | `0x567680`, `0x563ab0` | Datei-Streaming |
| `TextureDecode`, 8 × `NuUbershaderBytecodeCache::Work`, `NuShaderManagerGen::WorkerThread` | | Grafik, unkritisch |
| `NuFmvStream` | `0x62aa60` | Videos |
| **`NuBgProc`** | `0x5522a0` → Schleife `0x546ff0` | Hintergrund-Aufträge, siehe unten |
| `NuSoundDecodeThread`, `NuSoundStreamerWorkerThread` | | Ton |
| `SaveCore` | `0x540f70` | Speichern |

**NuBgProc** entspricht dem BG-Worker von Batman 1. Warteschlange `*0x11cc114` (angelegt in `0x556290`), Einreihen mit `0x5522b0(Name, Funktion, Fertig-Funktion, …, Frame-Versatz)`. Aufträge:

- `SampleLoader`, `SpeechLoader`
- `StaticLoadSplashSequence`
- `StreamingLevel::Unload`
- `DoDynamicLoad`
- `CutsceneResourcePacket::DoLoad`
- `0x7d48c0` aus dem Spielschritt `0x7e6cd0`

Ein Auftrag mit Frame-Versatz wartet im Worker, bis das Frame-Ende `0x5528c0` den Zielframe erreicht (`+0x804c`, Semaphore `+0x8050`).

**Eingebauter Synchronmodus:** Ist das Byte `*0x11cc114 + 0x8058` gesetzt, führt `0x5522b0` den Auftrag sofort im aufrufenden Thread aus (`LBO_BGSYNC=1`). Das **hängt** schon im Startbildschirm (Frame 172): Der Auftrag `0x7d48c0` ruft die Lade-Schleife `0x7eb950` auf. Die läuft, bis alle offenen Ladeeinträge (Liste mit kritischem Abschnitt `0x138b9f8`) fertig sind, und die werden erst fertig, wenn der Hauptthread weiterläuft.

**Warten am Frame-Ende** wie bei Batman 1 (`LBO_BGSYNC=2`): Das Frame-Ende wartet, bis die Warteschlange leer ist und kein Auftrag läuft (`+0x44`, laufender Auftrag `0x11cc118`), höchstens 3 s. In einem Lauf bis ins erste Level gab es 29 Zeitgrenzen in 7 Gruppen (Frames 169, 2189, 2310, 29261, 29438, 30128, 41805), jeweils mehrere Frames hintereinander. Das sind dieselben Aufträge, die den Hauptthread brauchen. Bei Batman 1 reichte es, den Worker am Frame-Ende festzuhalten. Hier braucht es eine feinere Lösung, siehe unten.

## Determinismus-Test (18./19.10.2026)

Werkzeuge:
- `tools/scripts/b2_det.txt`: Eingabeskript, aufgezeichnet aus einem Live-Lauf (Intro, „Neues Spiel“, erstes Level, Spieler 2 tritt bei, beide laufen, springen, schlagen).
- `tools/compare_b2.py <lauf1> <lauf2>`: meldet je Spalte von `frames.csv` den ersten abweichenden Frame.
- `frames.csv` je Frame:
  - Spielframe, dt
  - sieben Zufallszustände
  - Figurenzahl und Prüfsumme über alle Figurenpositionen
  - Positionen von Spieler 1 und 2

**Lauf d1** (zwei Instanzen gleichzeitig, festes dt, 60 fps, Videos an, ohne Sync, 30.000 Frames): **nicht deterministisch**.

| Was | Abweichung |
|---|---|
| CRT-`rand`, 16-Bit-LCG, Park-Miller-Zustand | ab Frame 3 / 11 / 10 (Intro) |
| Start des neuen Spiels | Instanz B 36 Frames früher (5237 statt 5273) |
| Levelstart (Figuren > 2) | B 30 Frames früher (20571 statt 20601) |
| Figuren beim Levelstart | A: 3/8/9/11/12/13 bei +0/+1/+2/+3/+49/+73, B: 3/8/10/11/12/13 bei +0/+1/+3/+11/+65/+69 |

Ursachen:
1. **Videos:** Während eines Videos setzt `0x5bb230` das dt auf die Videouhr, also auf Echtzeit. Wie viele Frames ein Video dauert, hängt deshalb von der Rechnerlast ab. `LBO_NOMOVIES=1` lässt die Videosuche `0x628d60` (Aufruf in NuFMVPlayV `0x62be30`) leer ausgehen. Ohne Videos dauerte die Phase zwischen „Neues Spiel“ und Level aber 23.600 statt rund 1.000 Frames. Das Spiel wartet dann offenbar auf etwas anderes, ungeklärt.
2. **Asynchrones Laden:** Wann Figuren erscheinen, hängt von der Ladezeit ab. Das ist dasselbe Problem wie der BG-Worker bei Batman 1, nur verteilt auf NuBgProc und die Streaming-Threads.
3. **Zufallszustände** laufen schon vor dem Level auseinander. Das ist erwartet und wird online wie bei Batman 1 beim Levelstart vom Host übernommen.

Die Bewegung selbst war in beiden Instanzen gleich: Spieler 1 stand nach dem Levelstart in beiden Läufen bei (−0,0361; −0,3564; −5,6282). Die Figurenphysik ist also vermutlich deterministisch. Belegt ist das erst, wenn das Laden synchronisiert ist.

## Lade-Synchronisation und Abgleich (19.10.2026)

Ursachen für Abweichungen und was die DLL dagegen tut:

| Ursache | Fund | Gegenmittel (`lbo2.dll`) |
|---|---|---|
| Streaming-Laden | `0x7eb950` (Auftrag `0x7d48c0`) ist eine **Dauerschleife** über die Eintragsliste `*0x11be084` (kritischer Abschnitt `0x138b9f8`). Eintrag: `+8` nächster, `+0x14` Zustand (1 leer, 2 lädt, 3/4 geladen, 5 entlädt), Byte `+0x1c` gewünscht, `+0x20` Gruppe (Filter `0x1199108`/`0x119910c`). Deshalb lief „warten bis NuBgProc leer“ immer in die Zeitgrenze. | `LBO_LOADSYNC=1`: Das Frame-Ende wartet, bis kein gewünschter Eintrag mehr lädt. Entladen (5) zählt nicht, es braucht das Zeichnen im Hauptthread. |
| Video-dt | `0x5528c0` setzt dt bei `0x552b89` auf die Videouhr (Echtzeit) | `LBO_VIDEODT=1`: Dieser Speicherbefehl wird zu `fstp st(0)`, dt bleibt 1/60. Danach startet das neue Spiel in beiden Instanzen im selben Frame (vorher 36 Frames Versatz). |
| Gemessene Zeit | Effekte nutzen den Spiel-Timer (QPC nur in `0x521840`/`0x5218c0`) | `LBO_VCLOCK=1`: QPC liefert im Hauptthread Start + Frame × 1/60 (+1 Tick je Aufruf) |
| Zufall beim Zeichnen | CRT-`rand` aus `0x5e0470` (Aufrufe `0x5e0d52`, `0x5e0db8`) und `rng_a` (`0x598619`) im Effekt-Takt | `LBO_RENDERRNG=1`: Diese Aufrufe gehen an eigene Generatoren |
| Musik-Takt | `0x94cdf0` (Objekt `0x11ad524`, über `0x9edb10`) zieht alle 723 Frames aus dem Logik-LCG `0x11a1844`, gesteuert von der Musik in Echtzeit; dasselbe Muster haben `0x94cbc0` und `0x94d110` | ebenfalls `LBO_RENDERRNG`: Die Adressen `0x11a1844` in diesen drei Funktionen zeigen auf eigene Zustände |
| Eingabe-Takt | `0x63bcd0` liest pro Frame nur ein Gerät, reihum (Countdown `0x1154400`, Gerät `0x13666c8`, Flag `0x13655a7`). Jedes Pad wird alle 5 Frames gelesen. | Diese drei Werte gehören zum Abgleich (`LBO_RNGRESET`) |
| Programmstart | Das erste Laden (132 Einträge, Frame ~10) braucht den Hauptthread; Zufallszustände laufen ab Frame 6 auseinander | Abgleichpunkt nötig: online übernimmt der Gast die Zustände vom Host, lokal `LBO_RNGRESET=<Frame>` |

Diagnose-Hilfen: `LBO_TRACERNG=von,bis` schreibt die Aufrufer von `rng_a` und CRT-`rand` je Frame nach `rngtrace.txt`. `LBO_WATCHLCG=1` setzt zusätzlich in allen Threads einen Hardware-Haltepunkt auf den LCG (Schreibzugriffe mit Codeadresse und Thread).

Leere Ordner `SavedGames\SlotN` zeigt das Spiel als „beschädigt“ an und fragt dann nach einem Speicherplatz. Im Schutzmodus legt die DLL sie deshalb gar nicht erst an (`CreateDirectoryA`, `SHCreateDirectoryExA`). Ohne diese Umleitung blieben drei Testpaare im Speichermenü stehen; ihre scheinbar gleichen Ergebnisse waren deshalb wertlos.

**Ergebnis (Lauf d11):** zwei Instanzen gleichzeitig, `LBO_LOADSYNC=1 LBO_VIDEODT=1 LBO_VCLOCK=1 LBO_RENDERRNG=1 LBO_RNGRESET=4000`, Skript `b2_det.txt`, 30.000 Frames. **Noch nicht deterministisch, aber deutlich näher dran:**

| | d1 (ohne alles) | d11 |
|---|---|---|
| Start „Neues Spiel“ | 36 Frames Versatz | gleicher Frame (5273) |
| CRT-`rand`, Park-Miller, Eingabe-Takt nach dem Abgleich | weichen ab | gleich über 26.000 Frames |
| Ende Nachrichten-Video | – | 2 Frames Versatz (6511/6513) |
| Levelstart | 30 Frames Versatz | 4 Frames Versatz (20510/20514) |
| Figuren beim Levelstart | 3/8/9/11/12/13 bei +0/+1/+2/+3/+49/+73 bzw. 3/8/10/11/12/13 bei +0/+1/+3/+11/+65/+69 | 3/8/10/11/12/13 bei +0/+1/+3/+12/+55/+66 bzw. 3/8/11/12/13 bei +0/+1/+3/+40/+58 |
| LCG `0x11a1844` | ab Frame 11 | ab Frame 5034 (ein zusätzlicher Zug in einer Instanz) |

Offene Quellen:
1. **Video-Ende:** Das Nachrichten-Video endet trotz festem dt nicht im selben Frame. Vermutlich entscheidet die Decoderzeit (`NuFmvStream`, `0x5ba670`), wann es fertig ist.
2. **Weitere Ladewege:** Figuren erscheinen weiterhin nach Ladezeit. Die Streaming-Liste deckt nicht alles ab: NuBgProc-Aufträge (`DoDynamicLoad`, `CutsceneResourcePacket::DoLoad`, `SampleLoader`) und `m_readThread` laufen weiter frei.
3. **LCG ab Frame 5034:** Im Trace von d7 war `0x94ced9` die einzige zeitabhängige Schreibstelle; die ist jetzt umgeleitet. Der verbleibende Zug ist noch nicht gefunden (nächster Schritt: `LBO_WATCHLCG` in einem Lauf mit Spielgeschehen).

## Zufallsgeneratoren

Neben `0x11a1840` (Park-Miller-Startwert), `0x11a1844` (16-Bit-LCG, 615 Zugriffe) und `0x102b358` (Park-Miller-Standardzustand) gibt es drei weitere Zustände mit Settern: `0x11be45c` (`0x51a170`), `0x1195b50` (`0x77f280`) und `0x1195b30` (`0x77d040`). Dazu kommt der CRT-`rand`-Zustand des Hauptthreads (`_getptd()+0x14`). Sie laufen schon im Intro auseinander (CRT ab Frame 3, LCG ab Frame 11). Online müssen sie wie bei Batman 1 beim Levelstart vom Host übernommen werden. Lokal setzt `LBO_RNGRESET=<Frame>` sie zu einem festen Frame auf feste Werte, zusammen mit dem Eingabe-Takt.

## Erste Einschätzung

- **Zeitschritt:** Das Frame-Ende misst dt mit dem Timer. Die Überschreibung `0x11cf0f8 = 1/60` wirkt wie bei Batman 1 und ist zur Laufzeit bestätigt.
- **Hauptrisiko bestätigt:** Ladeaufträge (NuBgProc, Streaming) arbeiten über mehrere Frames mit dem Hauptthread zusammen. Einfaches Festhalten am Frame-Ende wie bei Batman 1 reicht nicht.
- **Splitscreen:** Batman 2 hat eingebauten dynamischen Splitscreen. Online zeichnen beide PCs dasselbe Bild, das ist für den Lockstep unkritisch.

## Nächste Schritte

1. **Restliche Ladewege und Video-Ende** synchronisieren (siehe „Offene Quellen“), dann d11 wiederholen, bis Figuren und Positionen gleich bleiben.
2. **Abgleichpunkt für online:** Beim Start des Levels (bzw. beim Verlassen des Hauptmenüs) überträgt der Host Zufallszustände, Eingabe-Takt und ggf. weitere Zähler an den Gast; danach Lockstep wie bei Batman 1.
3. Netzcode und App-Anbindung für Batman 2 (Adresstabelle pro Spiel).
4. Längere Tests in weiteren Levels (Fahrzeuge, Charakterwechsel, Speichern ist im Test umgeleitet).
