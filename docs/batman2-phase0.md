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

## Erste Einschätzung

- **Vorteil:** Batman 2 rechnet die Logik offenbar schon mit festem 60-Hz-Schritt. Den Zeitschritt müsste man dann nicht mehr erzwingen, nur noch die Bildrate takten.
- **Hauptrisiko:** Statt eines Lade-Threads gibt es mehrere Arbeits-Threads mit einer Job-Warteschlange. Bei Batman 1 war der Lade-Thread die einzige Quelle für Nichtdeterminismus. Hier muss geklärt werden, welche Jobs Spielzustand ändern und wie man sie an feste Zeitpunkte bindet.
- **Splitscreen:** Batman 2 hat eingebauten dynamischen Splitscreen. Online zeichnen beide PCs dasselbe Bild, das ist für den Lockstep unkritisch.

## Nächste Schritte

1. Vollständige Ghidra-Analyse (läuft headless in `runs\batman2\ghidra`).
2. Hauptschleife und Frame-Ende finden. Prüfen, wie der feste Schritt mit der Echtzeit verrechnet wird: gibt es Unterschritte, und wird mit der Echtzeit nachgeholt?
3. Spielerplätze, Figurenarray und Positionen, Eingabekette (XInput, DirectInput, Tastatur).
4. Job-System: welche Jobs, welche Threads, Warten und Synchronisation.
5. Determinismus-Test wie in Phase 0 von Batman 1: zweimal dasselbe Eingabeskript, Prüfsummen vergleichen.
