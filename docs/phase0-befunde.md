# Phase 0: Befunde

Stand 2026-10-17, Steam-Version (App 21000) aus `D:\SteamLibrary\steamapps\common\Lego Batman`.

## Statische Analyse der EXE

| | `LEGOBatman.exe` | `testapp.exe` |
|---|---|---|
| SHA-256 | `f9ef801c…aa960716` | `3a7e029d…86cfb1e2` |
| Build | 2025-07-04 (PDB `c:\TT\legobatman1_clean\Batman_Resub2\...`) | 2008-09-05 |
| Architektur | x86, 32 Bit | x86, 32 Bit |
| ASLR | **aus** (DllCharacteristics 0, ImageBase 0x400000) | aus |
| Kopierschutz | **keiner**. `.text` ist unverschlüsselt (Entropie 5,9), es gibt keine `.bind`-Sektion und keine Steam-API. | SteamStub (`.bind`, `.text` mit Entropie 8,0) |

`testapp.exe` ist der alte Build mit Kopierschutz. Er ist für uns nutzlos, wir arbeiten nur mit `LEGOBatman.exe`.

### Relevante Imports

- **Eingabe:** `DINPUT8.dll!DirectInput8Create` (Tastatur, ältere Gamepads) und `XINPUT1_3.dll` Ordinal 2/3/4 (`XInputGetState`, `XInputSetState`, `XInputGetCapabilities`).
- **Zeit:** `WINMM!timeGetTime`, `timeSetEvent`/`timeKillEvent`, dazu `KERNEL32!QueryPerformanceCounter`/`Frequency` und `GetTickCount`.
- **Sonstiges:** d3d9 + d3dx9_35, DSOUND, binkw32 (Videos), Registry über ADVAPI32 (Sprache, Pfad). Eine Netzwerk-API wird nicht importiert.

## Folgerungen

1. **Einstieg per Proxy-DLL ist einfach.** `xinput1_3.dll` und `dinput8.dll` sind keine KnownDLLs, deshalb lädt Windows sie zuerst aus dem Spielordner. Für den Nutzer heißt das: eine DLL ins Spielverzeichnis kopieren, fertig.
2. **Feste Adressen.** Ohne ASLR liegen Code und globale Daten bei jedem Start an derselben Adresse. Das erleichtert Hooks und das Auslesen des Spielzustands für Prüfsummen erheblich.
3. **Kein DRM und keine Steam-API.** Es gibt nichts, was Hooks blockiert oder beim Patchen anschlägt.
4. **Die Eingabe läuft über zwei Wege.** Für den Lockstep müssen XInput **und** DirectInput (Tastatur) abgefangen werden. Alternativ zwingen wir beide Spieler im Online-Modus auf Controller-Eingabe.
5. **Zeitquellen** sind `timeGetTime`, QPC und `GetTickCount`. Für den Determinismus-Test müssen alle drei auf eine gemeinsame, rein Frame-gesteuerte Uhr umgebogen werden.

## Offen (dynamisch zu klären)

- [ ] Läuft das Spiel mit festem oder variablem Zeitschritt? (Ghidra: Main-Loop, Verwendung von QPC/timeGetTime)
- [ ] Woher kommt der Zufall? Eigener PRNG-Seed oder Zeit?
- [ ] Determinismus-Test: gleiche Eingaben plus gleiche Uhr ergeben in zwei Läufen gleiche Figurenpositionen.
- [ ] Wie verteilt das Spiel die Eingaben auf Spieler 1 und 2 (Controller-Index ↔ Spieler, Drop-in)?
