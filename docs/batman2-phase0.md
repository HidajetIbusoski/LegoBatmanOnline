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

Weitere Schalter: `LBO_ANCHOR=1` (Levelanker: Abgleich beim ersten Frame mit mehr als 2 Figuren, Skriptzeilen `A von bis …` zählen ab dort; `compare_b2.py --anker`), `LBO_IOWAIT=1` (siehe k2).

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

### Weitere Messungen (e1, k1, k2)

`frames.csv` enthält jetzt auch die Video- und Ladezustände: `vid` (`0x11cf178`), `fmv_*` (`0x131356c`, `0x1313574`, `0x131359c`, `0x13135b0`, Videoframe `0x1313568`), NuBgProc (`bg_pend`, laufender Auftrag `bg_job`), Streaming-Liste (`ll_n`, `ll_loading`) und `rel` (Frames ab Levelanker).

- **e1 (bis Frame 9000):** Die Videozustände sind in beiden Instanzen gleich. Die erste Abweichung ist der NuBgProc-Auftrag **SampleLoader** (`0x7359b0`): Er startet bei 5041 bzw. 5034 (dort kippt auch der LCG) und vor jedem versetzten Szenenwechsel (6508 bzw. 6512). Eingereiht wird er von der Sample-Verwaltung `0x7372b0` über `0x735a20`. Die gibt Samples frei, deren Referenzzähler (`+0x18`) auf 0 gefallen ist. Das passiert, wenn ein Sound in XAudio2 **in Echtzeit** zu Ende gespielt hat.
- **k1 (Levelanker, `LBO_ANCHOR=1`, Skript `b2_anchor.txt`):** Der Anker liegt bei 20509 bzw. 20516. Ab dem Anker weicht schon bei +3 die Streaming-Liste ab (3 bzw. 11 Einträge), bei +26 die Figurenzahl, ab +1147 die Position von Spieler 1. Das Level lädt beim Anker noch nach, ein Abgleich direkt beim ersten Erscheinen der Figuren ist zu früh.
- **k2 (zusätzlich `LBO_IOWAIT=1`):** Das Frame-Ende wartet, bis `m_readThread`, `m_decodeThread` und `TextureDecode` ruhen (`QueryThreadCycleTime`). Sie waren in 6000 Frames nur 8-mal aktiv, der Menü-Versatz wurde nicht kleiner (6474 bzw. 6511). **Datei-I/O ist nicht die Ursache.**

**Zwischenstand nach k2:** Die Abweichungen gehen auf das Ton-Timing zurück.

### Virtueller Ton (`LBO_VAUDIO=1`) – Determinismus erreicht

Das Spiel nutzt XAudio2 nur für wenige Stream-Stimmen (PCM 44,1 kHz, `WAVE_FORMAT_EXTENSIBLE`). Erzeugt wird es in `0x612930` per `CoCreateInstance(IID_IXAudio2)`. Die Effekte mischt das Spiel selbst (NuSoundDecodeThread) und reicht das Ergebnis als Puffer ein. Es reagiert auf die Rückrufe `OnBufferStart`, `OnBufferEnd` und `OnStreamEnd` (eine Rückrufklasse, Vtable `0xd3923c`, im Ton-Thread) und fragt einmal pro Frame `GetState` ab (`0x6145f0`).

Die DLL hakt `CoCreateInstance`, `CreateSourceVoice` und die Stimmen-Methoden:
- `SubmitSourceBuffer`, `Start`, `Stop`, `FlushSourceBuffers`, `ExitLoop`, `SetFrequencyRatio`, `GetState`, `DestroyVoice`.

So arbeitet `LBO_VAUDIO`:
- Je Stimme führt die DLL eine eigene Warteschlange mit der Länge jedes Puffers in Samples.
- Am Frame-Ende spielt sie genau Abtastrate × Frequenzverhältnis / 60 Samples ab.
- Die dabei fälligen Rückrufe ruft sie im Hauptthread auf. Die echten Rückrufe von XAudio2 hält sie zurück.
- Danach wartet sie bis zu 50 ms, bis NuSoundDecodeThread und NuSoundStreamerWorkerThread darauf reagiert haben.
- `GetState` meldet den virtuellen Stand.

Das Abspielende hängt damit nur noch an Frames. Der echte Ton läuft weiter über XAudio2.

`LBO_AUDIOLOG=1` zählt Methoden, Rückrufe und `GetState`-Aufrufer mit (alle 600 Frames im Log).

**Ergebnis (Lauf v1):**
- **Aufbau:** zwei Instanzen gleichzeitig, `LBO_LOADSYNC=1 LBO_VIDEODT=1 LBO_VCLOCK=1 LBO_RENDERRNG=1 LBO_VAUDIO=1 LBO_ANCHOR=1`, Skript `b2_anchor.txt`, 31.000 Frames.
- **Gleicher Ablauf:**
  - Menü- und Szenenwechsel in beiden Instanzen im selben Frame (5273, 6557, 7839).
  - Levelstart im selben Frame (20620).
  - Figuren erscheinen identisch (3/8/10/11/12/13 bei +0/+1/+3/+10/+74/+84).
- **Ab dem Levelanker 10.381 Frames lang alle Spielwerte gleich:**
  - Figuren und Positions-Prüfsumme,
  - Positionen von Spieler 1 (in 1.270 Frames bewegt, gesprungen) und Spieler 2 (Beitritt bei +3082, in 1.073 Frames bewegt),
  - alle sieben Zufallszustände, CRT-`rand`, Eingabe-Takt.
- **Absolut ab Frame 1:** Alles ist gleich außer dem LCG. Der kippt beim ersten Laden in Frame 10 und wird am Anker abgeglichen.
- **Unterschiede ohne Wirkung:** Nur interne Ladezeitpunkte (`bg_job`, `ll_n`, `ll_loading`) weichen ab. Sie wirken sich nicht auf den Spielzustand aus.

**Batman 2 ist damit unter diesen Schaltern deterministisch.** Für den Online-Lockstep reicht derselbe Ansatz wie bei Batman 1: Zustände am Levelstart vom Host übernehmen und dann nur Eingaben austauschen.

Noch nicht geprüft:
- Wie sich `LBO_VAUDIO` anhört. Der echte Ton kann bei Wartephasen kurz aussetzen.
- Ob der Abgleich am ersten Frame mit mehr als 2 Figuren auch in anderen Levels und beim Laden eines Spielstands passt.

## Netzmodus (19.10.2026)

`lbo2.dll` nutzt denselben Lockstep wie Batman 1 (`src/dll/net.c`, `src/dll/input.c`, mit in die DLL gelinkt). Mit `LBO_NET=host|join` setzt die DLL alle Determinismus-Schalter selbst:
- `FIXEDDT`, `LOADSYNC`, `VIDEODT`, `VCLOCK`, `RENDERRNG`, `VAUDIO`, `ANCHOR`, `LOOPGATE`, `IOWAIT`, `NODINPUT`, `SLOWSTART=320`, 60 fps.

Eingaben:
- Das Spiel sieht nur Pad 0 (Host) und Pad 1 (Gast) aus dem Austausch.
- Die eigene Eingabe kommt aus `input_read` (Controller oder Tastatur der App) bzw. im Test aus dem Skript (Zeilen des eigenen Pads).
- DirectInput-Geräte und die Tastatur des Spiels (Gerät 4–13) sind aus: Die Aufrufe von `0x63b6f0` bei `0x63bd59`, `0x63d608` und `0x63d8ee` melden für Gerät ≥ 4 keine Eingabe.
- Spielstände werden im Netzmodus auch beim Lesen umgeleitet: Beide sehen leere Plätze, der Host wählt „Neues Spiel“ und „Ohne zu speichern fortfahren“.

Abgleich:
1. **Frame 30** (nach dem ersten Laden): Der Host schickt die sieben Zufallszustände und den Eingabe-Takt (`net_send_sync`, `net_wait_sync`).
2. **Levelstart** (erster Frame mit mehr als 2 Figuren): Schranke `net_barrier`. Wer zuerst ankommt, wartet und verlängert dabei seine Eingaben mit neutralen Pads, damit der andere bis zur Schranke weiterläuft. Der Gast übernimmt den Zustand des Hosts. Danach zählen beide den Lockstep ab dem Anker neu (Epoche 1; ältere Pakete werden verworfen).
3. **Prüfsummen** (Figuren-Hash, Figurenzahl, LCG, `rng_a`, Park-Miller, CRT) alle 60 Frames ab dem Anker. Vorher sind kleine Frame-Versätze erlaubt.

Was im Netz zusätzlich nötig war (lokale Netztests n1–n14, Host und Gast auf einem PC):
- **Erstes Laden:** Dort bearbeitet der Hauptthread die Streaming-Liste selbst. `wait_loads` wartet deshalb nur, wenn die Lade-Schleife `0x7d48c0` läuft oder eingereiht ist (vorher 30-s-Zeitgrenzen und 3 Frames Versatz).
- **Startphase:** Bis Frame 300 wartet das Frame-Ende auf alle NuBgProc-Aufträge außer der Streaming-Schleife.
- **Nach dem Frame-Ende:** Ein zweiter Haken (Aufrufe von `0x5528c0` bei `0x7e6e03` und `0x7e6f98`) greift nach dem Zeichnen und vor der Logik. Solange Einträge im Übergang sind, wartet er zwei Durchläufe der Lade-Schleife ab (Zähler über den Aufruf `0x557850` bei `0x7ebb90`). Bei leerer Liste wartet er, bis sich der Auftrag beendet hat.
- **Verlangsamung:** In den ersten 900 Frames dauert jeder Frame 50 ms länger (`LBO_SLOWSTART`). Danach dauert er 40 ms länger, solange ein Ladeauftrag läuft (`LBO_LOADSLOW`). Logos, Intro-Animationen und `StreamingLevel::Unload` (`0x7ddc60`, räumt über `0x7ebef0` die Liste ab) starten bzw. enden, sobald Daten da sind. Mit der Pause sind sie praktisch immer im selben Frame fertig. Ohne sie gab es in einem von zwei bis drei Läufen 1 Frame Versatz.
- **Diagnose:**
  - `LBO_PROBE=Adr,…` (`*Zeiger+Versatz` möglich) schreibt Adressen je Frame nach `probes.csv`.
  - `LBO_BREAKAT=Adr` protokolliert beim ersten Treffer den Aufrufstapel.
  - Das Hauptthread-Handle braucht `THREAD_SET_CONTEXT`, sonst greifen Hardware-Haltepunkte nicht.

Launcher und App:
- `lbo-launch --connect … --b2` sucht `LEGOBatman2.exe` und lädt `lbo2.dll`.
- Er setzt `SteamAppId=213330` (Steam muss laufen) und lässt den Spielstand-Austausch weg.
- In der App steht beim Freund der Knopf „Batman 2“. Die Einladung trägt `game=b2`.
- Testoption `--auto-b2` (mit `--auto-invite`).

**Ergebnis (Stand 19.10.2026): Verbindung, Abgleich, Schranke und Lockstep funktionieren. Deterministisch ist der Netzmodus noch nicht.**

| Lauf | Aufbau | Ergebnis |
|---|---|---|
| n7, n10, n11, z1–z4 | 400–1500 Frames | identisch |
| n9, w1–w4, x3/x4, y1 | 400–1500 Frames | 1 Frame Versatz um Frame 280 (Intro). Mit `LBO_SLOWSTART` behoben |
| n12, n13 | 31.000 Frames, mit Verlangsamung | erste Abweichung erst beim Start des neuen Spiels (5278) bzw. in Menü und Zwischensequenz (4266/6564) |
| n14 | 31.000 Frames, mit Schranke am Levelstart | Schranke passiert. Danach weicht die Streaming-Liste ab +17 ab, die Figurenzahl ab +51, dann folgt Desync |

Der lokale Paartest ohne Netz (v1) war über 10.381 Frames ab dem Anker identisch. Im Netz unterscheidet sich vor allem das Timing, weil Host und Gast unterschiedlich lange aufeinander warten. Ladevorgänge, die über mehrere Frames mit dem Hauptthread zusammenarbeiten, enden dann in verschiedenen Frames. Betroffen sind das Nachladen der Level-Figuren, `StreamingLevel::Unload` und Intro-Animationen. Warten und Verlangsamen senkt die Häufigkeit, schließt diese Wettläufe aber nicht sicher aus.

### Frame-gebundene Lade-Threads (`LBO_GATE`, experimentell)

**Nachstellungstest ohne Netz:**
- Zwei Instanzen mit allen Schaltern, ungebremst (`LBO_FPSCAP=0`), eine mit `LBO_JITTER=30` (zufällige Pause bis 30 ms je Frame).
- Die schnelle Instanz läuft anders: Neues Spiel bei 6833 statt 5273. Die verzögerte folgt fast genau dem normalen 60-fps-Ablauf.
- Was im Hintergrund pro Frame fertig wird, hängt also an der Echtzeit.

**`LBO_GATE=1`:**
- Die Threads aus `LBO_GATESET` (Standard `2,15` = m_readThread, NuBgProc) laufen nur im Fenster nach dem Frame-Ende, bis sie 4 ms ruhen (höchstens 300 ms).
- Während Logik und Zeichnen sind sie per `SuspendThread` angehalten.
- Ein Wächter gibt sie frei, wenn der Hauptthread im Spielcode wartet: weniger als 300.000 Zyklen in 2 ms, oder 100 ms ohne Fortschritt bei aktivem Warten auf eine Sperre.
- Gewollte Pausen (Bildratenbremse, Netz, Jitter) zählen nicht (`g_in_wait`).

**Erfahrungen:**
- **Hänger:** Gehören TextureDecode und m_decodeThread zum Takt, hängt der Hauptthread beim Zeichnen des Ladebildschirms (`0x5707c0` → `0x5e1830`, Grafiksperre). Eigene Diagnose-Zugriffe auf die Streaming-Liste blockierten am kritischen Abschnitt, den ein angehaltener Thread hielt; deshalb jetzt `TryEnterCriticalSection`. Mit `2,15` und dem verbesserten Wächter gibt es keine Hänger mehr (22 Freigaben in 6000 Frames).
- **Zu langsam:** Ton- und Video-Threads (14, 16, 17) fragen ständig ab und ruhen nie. Mit ihnen läuft jedes Fenster in die Zeitgrenze, und das Spiel schafft nur etwa 3 Frames/s.
- **Wirkung:** Je 4 kurze Paare (600 Frames, schnell gegen verzögert) mit Gating 2 Abweichungen bei Frame 284, ohne Gating 0. Der Wettlauf ist also selten und liegt nicht in NuBgProc oder m_readThread.

`LBO_GATE` bleibt experimentell und ist im Netzmodus aus. Den Ersatz beschreibt der nächste Abschnitt.

### Wettlaufsuche mit dem Paartest (19.10.2026, Fortsetzung)

Methode: Paare bis ins Level (31.000 Frames), schnelle Instanz (`LBO_FPSCAP=0`) gegen verzögerte (`LBO_JITTER=30`), beide mit `LBO_RNGRESET=30`. Erste Abweichung per `tools/compare_b2.py`, dann Speicherabzüge (`LBO_MEMDUMP=von,bis` + `tools/memdiff_b2.py`), Schreib-Wächter (`LBO_WATCHADDR`, schreibt Codeadresse und Aufrufer je Frame nach `rngtrace.txt`) oder Schreib-Haltepunkt (`LBO_BREAKW`, `LBO_BREAKFROM`).

Gefundene und behobene Wettläufe:

| Stelle | Ursache | Lösung |
|---|---|---|
| Streaming-Schleife `0x7eb950` | dreht im NuBgProc-Thread ohne Takt und ändert die Ladeliste zu beliebigen Zeitpunkten | `LBO_LOOPGATE`: wartet am Schleifenanfang `0x7eb979` auf eine Freigabe. Nach dem Frame-Ende gibt `loop_window` einzelne Durchläufe frei, bis die Liste zwei Durchläufe lang gleich bleibt und nichts mehr lädt |
| Aufräum-Schleife `0x7ebef0` (`StreamingLevel::Unload`) | endete je nach Tempo einen Frame früher oder später (Frame 7851) | gleiche Freigabe am Schleifenanfang `0x7ebf19`, ab Frame 300 (beim Start wartet der Hauptthread selbst auf sie) |
| andere NuBgProc-Aufträge (SampleLoader `0x7359b0`, Lade-Auftrag beim ersten Laden) | liefen in den nächsten Spielschritt hinein (Frame 284, Anker +2) | `bg_settle` nach dem Frame-Ende ab Frame 30: warten, bis NuBgProc nichts tut, an einer Freigabe steht, auf einen Frame wartet oder 5 ms lang weder rechnet noch liest |
| Lese-Pipeline (`m_readThread`) | wartet auf die Platte, rechnet dabei nicht, die CPU-Ruhe täuscht | `stream_busy` in `wait_io`: Leser wartet auf sein Ereignis und keine Anfrage ist offen |
| Maus und Tastatur per DirectInput (`0x633860`, `0x6338f0`) | ob Poll/Acquire gelingt, hängt am Fensterfokus; jeder Erfolg schaltet Puffer um (`0x13658bc`, `0x13655a4`) | Abschalt-Bits des Spiels `0x136555c` (1 Maus, 2 Tastatur), `LBO_NODINPUT=1`, im Netz immer |
| Speicher-Thread `SaveCore` (`0x540f70`) | schaut nur alle 100 ms in seinen Auftragsring und hält „Speichern…“ eine Mindestzeit in Echtzeit. Der Spielstand-Bildschirm erschien bei Frame 4842 oder 4879, danach griff ein Tastendruck im Menü oder nicht | mit `LBO_IOWAIT`: Pause 1 ms (`0x5410c2`), Mindestzeit übersprungen (`0x540e2b` jnp → jmp), Frame-Ende wartet auf leeren Ring (`0x102cab8`/`0x102cabc`) und `0x11cc19a = 0` |

Ein Versuch, SaveCore ganz frame-gebunden laufen zu lassen (eigene Freigabe statt Sleep, virtuelle Uhr für seinen Thread), hing an anderen Wartestellen und wurde verworfen.

Zwei weitere Quellen, behoben:
- **Nullpunkt der virtuellen Uhr:** Er lag beim ersten QPC-Aufruf, also um einen zufälligen Echtzeit-Betrag gegen den Startwert der Spieluhr (`0x11c2d60`) verschoben. Jetzt gilt Spielzeit = (Frame + D)/60 s, D auf volle 3600 Frames aufgerundet (Sprung nur vorwärts).
- **Ton-Nachlieferung:** Gestreamte Stimmen bekommen ihren nächsten Puffer erst nach BufferEnd. Kam er nach dem nächsten `va_tick`, lief die virtuelle Stimme leer. `va_tick` wartet jetzt, bis jede BufferEnd ohne Stream-Ende mit einem Submit beantwortet ist (höchstens 200 ms; 325 Wartephasen und 5 Zeitgrenzen in 30.000 Frames).

**Ergebnis (Paare F7, F9):** Start, Intro-Video, Menü mit Spielstand-Bildschirm, „Neues Spiel“ und beide Zwischensequenzen sind in beiden Instanzen identisch. Vorher lief es spätestens bei Frame 284 oder im Menü auseinander.

Weitere Befunde (Fortsetzung):
- **Installer-Fehler:** Die Haken werden gesetzt, während die Steam-Hülle noch entschlüsselt. Die Zeichen-Zufall-Patches waren mit `&&` verkettet. Schlug einer fehl, scheiterte die Kette beim nächsten Durchlauf schon am ersten (bereits gepatchten) Eintrag. Die Musik-LCG-Umleitungen (`0x94cec4` usw.) waren deshalb nie aktiv, und der Installer lief jedes Mal 120 s bis zur Zeitgrenze. Jetzt hat jeder Patch einen eigenen Merker (`ONCE`), der Installer ist nach etwa 200 ms fertig. Seitdem trat der seltene Versatz bei Frame 284 (erste Musikstimme bei 284 statt 283) in 14 Läufen nicht mehr auf, vorher in etwa 2 von 12.
- **`rng_b` ist reine Optik:** Alle Nutzer (`0x790500`–`0x7930e0`, `0x7adad0`, `0x7ae500`, `0x7ae7d0`) arbeiten auf Partikel-Systemen. Die Abweichung ab 13349 kommt daher, dass Partikel-Systeme je nach Sichtbarkeit in der Logik (`0x7d8d20` → vtbl+0x30 `0x7b10b0`) oder beim Zeichnen (`0x7c0270` → `0x7b0f90` bei `0x7c053c`) weitergerechnet werden. Für den Lockstep ist das harmlos. Versuche, das umzulenken (eigener Zustand, Update nur in der Logik, Schalter `0x7c5070` aus), änderten an der Logik nichts und wurden wieder entfernt.
- **Gelesen, nicht die Ursache:** QPC-Aufrufe je Frame, Rohes dt `0x11cf0d8`, x87-Steuerwort und MXCSR (CSV-Spalten `qpc`, `rawdt`, `fpucw`, `mxcsr`: in beiden Instanzen Frame für Frame gleich), RDTSC (keine echten Aufrufe), Ton-Nachlieferungen (Zeitgrenzen in beiden Instanzen an denselben Stellen), Ton-Position (`0x614070` liest die virtuellen SamplesPlayed im Hauptthread).

**Ton als Uhr der Zwischensequenzen:** Die Zwischensequenzen laufen nach der Ton-Position (`0x614070` liest die gespielten Samples der Stream-Stimme im Hauptthread). Gestreamte Musik und Sprache (44,1 kHz) bekommen ihren nächsten Puffer erst nach BufferEnd vom Streamer, in Echtzeit. Kam er zu spät, blieb die virtuelle Stimme stehen. Ab Frame 12525 liefen dann Lichtwerte, die Animationszeit `0x1389120` und die Kamera in der 5. Stelle auseinander, bei 14119 startete das Laden einen Frame anders, und der Levelstart lag 5 Frames auseinander.
- `va_tick` wartet höchstens 200 ms, bis jede BufferEnd einer 44,1-kHz-Stimme, die schon einmal im selben Frame nachgeliefert hat, mit einem Submit beantwortet ist. Kommt nichts, gilt der Stream als zu Ende (`dry`), und es wird nicht erneut gewartet.
- Fehler unterwegs: Eine feste 2-s-Grenze für alle Stimmen ließ das Bild an Stream-Enden 2 s stehen. Mit Soundeffekten (22 kHz) im Warten gab es im Level 322 Hänger von etwa 220 ms. Und eine Zeitgrenze bei einem Soundeffekt löschte die ausstehende Musik-Nachlieferung mit; genau das war der Wettlauf bei 14119.

**Zeichnen zieht aus `rng_a`:** Die Ansicht-Zeichenfunktion `0x575480` zieht am Ende zwei Zahlen aus dem Logik-Generator (`0x575c61/69`). Wie viele Ansichten gezeichnet werden, hängt am Zeichnen. Bei 12215 kam je nach Tempo eine dritte Ansicht ein oder zwei Frames früher dazu. Alle 25 `rng_a`-Aufrufe im Zeichenbereich `0x570000`–`0x5f0000` bekommen den eigenen Render-Zustand.

**Wartezeit der Mod:** CSV-Spalte `wait_ms` und eine Logzeile je 3000 Frames zeigen die Zeit in den Wartestellen. `wait_io` lief zweimal je Frame mit fester 3-ms-Pause (zusammen etwa 8 ms). Jetzt läuft es einmal mit 2-ms-Pause, insgesamt etwa 3 ms plus Ton (im Level 4–6 ms). Ungebremst schafft die schnelle Instanz 132 statt 78 fps.

**Startphase:** Der 1-Frame-Versatz des Intros (erste Musikstimme bei 283/284) kommt aus dem Ablaufskript der Zwischensequenz (`0xa1f420`, Objekt +0x6c), dessen Schritte auf Echtzeit-Fertigstellungen warten. Mit `LBO_SLOWSTART=320` (50 ms extra je Frame in den ersten 320 Frames) trat er in 8 Kurzläufen und 6 vollen Läufen nicht auf. Im Netzmodus ist das jetzt Standard (statt 900 Frames).

**Netzmodus:** `LOOPGATE`, `IOWAIT` und `NODINPUT` sind jetzt fest an, `LBO_LOADSLOW` entfällt.

**Stand 20.10.2026 (Paartest schnell gegen verzögert, je 31.000 Frames, `LBO_SLOWSTART=320`):** Mit dem aktuellen Stand waren 4 von 5 vollen Paaren (F34, F35, F37, F38) in allen Logikwerten gleich: Zufallsgeneratoren, Figuren, Positionen beider Spieler, Levelanker. Einzelne LCG-Abweichungen in nur einem Frame (7610, 20398) sind Momentaufnahmen, während ein Hintergrund-Auftrag zieht; ab dem nächsten Frame ist der LCG wieder gleich.

Offen (F36, reproduzierbar mit denselben Werten): `rng_a` kippt bei 14681. Dort feuert ein Ereignis-Handler (`0x4747a0` → `0x433790` → `0x74bb30`) in der schnellen Instanz einmal mehr. Vorläufer: Bei 9530 ist der Stream-Merker `0x1360b44/45` (gesetzt u. a. von `0x614140`) in der schnellen Instanz 1, in der verzögerten 0. Ab 9538 weicht die Animationszeit `0x1389120` ab, ab 9552 die Kamera der Zwischensequenz in der letzten Stelle. Ein Schreib-Wächter auf `0x1360b44` fand in vier weiteren Läufen keinen Schreibzugriff im Fenster 9520–9540.


Die Zählung nach dem Anker taugt erst, wenn der Vorlauf gleich ist: Mit unterschiedlichem Vorlauf weichen auch nach dem Rücksetzen der Zufallszustände die geladenen Daten ab.

### Lokale Netztests (20.10.2026)

Host und Gast auf diesem PC (UDP über 127.0.0.1), beide mit Skript-Eingaben, je 31.000 Frames (Intro, Zwischensequenzen, erstes Level):

| Lauf | Bedingung | Ergebnis |
|---|---|---|
| N20 | Gast mit `LBO_JITTER=20` | alle Logikwerte gleich, 0 Desyncs, Schranke am Levelstart bei beiden Frame 20380 |
| N21 | ohne Jitter | alle Logikwerte gleich, 0 Desyncs, 59 fps in Zwischensequenz und Level |
| N22 | Gast bei Frame 22.000 beendet | Host wartet 30 s, zeigt den Hinweis und spielt allein weiter |
| N23 | Gast 40 s angehalten (`LBO_PAUSE`), dann beendet | kein falscher Abbruch während der Pause, danach weiter gleich; nach dem Beenden Abbruch nach 30 s |

**Abbruchschutz:** Ein eigener Thread schickt jede halbe Sekunde ein Lebenszeichen. Erst wenn 30 s gar nichts mehr ankommt, gilt der Partner als verloren: Spieler 2 bekommt neutrale Eingaben, die eigene Seite sendet nichts mehr (käme der Partner zurück, liefe er so ebenfalls in die Zeitgrenze statt endlos zu warten). Ein Hinweisfenster erklärt das. Bei einem Desync (Prüfsumme alle 60 Frames) erscheint ebenfalls ein Hinweis.

**FPU:** Im Netzmodus wird der x87-Zustand (0x7f) und MXCSR (0x1f80) je Frame festgesetzt, wie bei Batman 1. In allen Tests war keine Korrektur nötig; es sichert gegen Treiber oder Overlays ab, die die Genauigkeit verstellen.

Noch nicht getestet: zwei echte PCs über das Internet.

## Zufallsgeneratoren

Neben `0x11a1840` (Park-Miller-Startwert), `0x11a1844` (16-Bit-LCG, 615 Zugriffe) und `0x102b358` (Park-Miller-Standardzustand) gibt es drei weitere Zustände mit Settern: `0x11be45c` (`0x51a170`), `0x1195b50` (`0x77f280`) und `0x1195b30` (`0x77d040`). Dazu kommt der CRT-`rand`-Zustand des Hauptthreads (`_getptd()+0x14`). Sie laufen schon im Intro auseinander (CRT ab Frame 3, LCG ab Frame 11). Online müssen sie wie bei Batman 1 beim Levelstart vom Host übernommen werden. Lokal setzt `LBO_RNGRESET=<Frame>` sie zu einem festen Frame auf feste Werte, zusammen mit dem Eingabe-Takt.

## Erste Einschätzung

- **Zeitschritt:** Das Frame-Ende misst dt mit dem Timer. Die Überschreibung `0x11cf0f8 = 1/60` wirkt wie bei Batman 1 und ist zur Laufzeit bestätigt.
- **Hauptrisiko bestätigt und gelöst:** Der Nichtdeterminismus kam vor allem vom Ton. Das Abspielende in Echtzeit steuerte das Freigeben und Nachladen von Samples und damit Szenenwechsel und Figuren. Dazu kamen Video-dt, Zeichen-Zufall, Musik-Takt und Streaming. Mit virtuellem Ton und den übrigen Schaltern ist der Ablauf gleich.
- **Splitscreen:** Batman 2 hat eingebauten dynamischen Splitscreen. Online zeichnen beide PCs dasselbe Bild, das ist für den Lockstep unkritisch.

## Nächste Schritte

1. **Laden im Netz deterministisch machen** (siehe „Netzmodus“, nächster Ansatz).
2. **Ton anhören** mit `LBO_VAUDIO=1` (Aussetzer?).
3. Längere Tests in weiteren Levels (Fahrzeuge, Charakterwechsel, Speichern ist im Test umgeleitet).
