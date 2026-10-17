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

## Ghidra-Analyse (Phase 0b)

Stand 2026-10-17. Werkzeug: REA 6.5.0 mit Ghidra 12.1.4, Default-Autoanalyse, keine PDB geladen. Alle Funktionsnamen (`FUN_xxxxxxxx`) stammen von Ghidra. Alle Adressen sind virtuelle Adressen bei ImageBase 0x400000.

**Evidence-IDs** sind auf 12 Hex-Zeichen gekürzt (`ev_…`). Das vollständige Bundle mit 63 Records liegt außerhalb des Repos, weil es dekompilierten Spielcode enthält: `%APPDATA%\Claude\side-session-notes\local_ad5cdc57-…\phase0-ghidra-evidence.json`.

Alles hier ist **statische** Analyse. Kein Befund wurde zur Laufzeit beobachtet.

### 1. Main-Loop und Zeitschritt

**Beobachtet**

| Adresse | Was dort passiert | Evidence |
|---|---|---|
| `0x6e3c10` | Timer-Init: Prozessaffinität kurz auf CPU 0, `QueryPerformanceCounter` → Startwert `0xb05498`, `QueryPerformanceFrequency` → `0xb039b0` | `ev_b118903cad2c` |
| `0x6e3b80` | Liest QPC und gibt Ticks seit Start zurück (64 Bit, Differenz zu `0xb05498`). **Einziger** Spiel-Aufrufer von QPC (die dritte Referenz `0x747af4` ist CRT `__security_init_cookie`) | `ev_97e68053e48f`, `ev_b118903cad2c` |
| `0x6d5090` | Ticks → Sekunden (Division durch die QPC-Frequenz) | `ev_c97db25e90d2` |
| `0x6e0240` | **Frame-Ende.** Misst die Zeit seit dem letzten Frame per `0x6e3b80`, schreibt sie als Float-Sekunden nach `0xadf688` und gibt sie zurück. Wenn `0xadf690 != 0`, wird stattdessen dieser feste Wert genommen. Ruft am Ende `0x6de440` auf (Eingabe-Polling, siehe 3). | `ev_bfa818df5e34` |
| `0x6db310` | Frame-Limiter: Busy-Wait, bis `n / Bildrate` verstrichen ist. Modus `0xadf654` (Init 5 → 1 VBlank), Bildrate `0xadf664` (Init 60.0, gesetzt in `0x6d5210`) | `ev_bfa818df5e34`, `ev_905f042e6ad2` |
| `0x4b01d0` | **Level-Hauptschleife.** Pro Iteration: `0x4aee90` (Weltupdate), danach `dt = 0x6e0240()` → `0xa95fe0`, begrenzt auf `0xa9601c` (außer wenn `0xacb714` gesetzt ist) | `ev_7d4af6542e1f` |
| `0x4aee90` | **Weltupdate.** Reicht `dt` (`0xa95fe0`) an rund 20 Subsysteme weiter, zieht ihn von Timern ab und rechnet Animationen mit `dt * 60` | `ev_a6268d6f3d97` |
| `0x431d80` | Spiel-Init: `0xa95fe0 = 1/60` (NTSC, „BASLUS-21785“) bzw. `1/50` (PAL, „BESLES-55135“), `0xa95fe4 = 1/60` (Startwert je Level), `0xa9601c = 0.1` (Maximal-dt) | `ev_905f042e6ad2`, `ev_743e850abb3e`, `ev_f2e450b7f848` |
| `0x535f30` | **Einziger** Aufrufer von `timeGetTime`. Gehört zur Sound-Streaming-Verwaltung (Puffer-Slots, Lautstärke −7000) | `ev_faa00eade19d`, `ev_b118903cad2c` |
| `0x539ac0` | `timeSetEvent(500 ms, periodisch)` mit einem Callback aus einem Objekt (Sound-Kontext, Zweck nicht geklärt) | `ev_d3c7a751c1ec`, `ev_905f042e6ad2` |
| `0x67b5e0`, `0x67b630`, `0x67b680` | Einzige Spiel-Aufrufer von `GetTickCount`: ein Zustandsautomat (`0xad341c`), der nach 1000 ms weiterschaltet. Wahrscheinlich eine Status-Anzeige wie „Speichern…“ (Folgerung, nicht belegt). `0x747ae8` ist CRT. | `ev_02afda5cbee3`, `ev_56034bc64fe6` |

**Folgerungen**

- Das Spiel läuft mit **variablem Zeitschritt**. Es gibt genau einen Simulationsschritt pro gerendertem Frame, kein Sub-Stepping. Der Schritt ist die gemessene QPC-Zeit, nach oben auf 0,1 s begrenzt. Bei 60 fps ist `dt` also nur ungefähr 1/60 und schwankt von Frame zu Frame.
- Für Lockstep genügt ein Eingriff an einer Stelle. `dt` muss auf beiden Rechnern eine Konstante sein. Am saubersten geht das über den eingebauten Override `0xadf690`: Steht dort `1/60` als Float, nimmt `0x6e0240` diesen Wert statt der Messung. Eine andere Möglichkeit ist ein Hook auf den Rückgabewert von `0x6e0240`. QPC selbst müssen wir **nicht** umbiegen, weil es nur über `0x6e3b80` in die Simulation gelangt.
- `timeGetTime` (Sound) und `GetTickCount` (UI-Timer) fließen nach dieser Analyse nicht in die Weltsimulation.

**Unbekannt**

- `0x729680` → `0x7017a0` kann `dt` ein zweites Mal überschreiben (Flags `0x29f3e10`/`0x29f3f5c`, referenziert auch `0xadf690`, `ev_6f8780337abc`). Vermutlich gehört das zu Videos oder Cutscenes, geprüft ist es nicht.
- Wer den VBlank-Zähler `0x9d5844` hochzählt, ist offen (`ev_9b3b128a9506`).

### 2. Zufall

**Beobachtet**

| Generator | Funktion(en) | Zustand | Seed | Evidence |
|---|---|---|---|---|
| LCG (Numerical Recipes, `x*0x19660d + 0x3c6ef35f`), liefert Float in [0,1) | `0x68b530` (54 Aufrufer), `0x68b590` (nur Schritt, 9 Aufrufer) | **`0xad3b6c`** | **0**, gesetzt über `0x68b4d0(0)` | `ev_40fd21fd060c`, `ev_b1e87a8f5866`, `ev_986e5a9d285e` |
| gleicher LCG mit Zustand per Zeiger | `0x68b4f0` (22 Aufrufer), `0x68b570` | Zustand liegt beim Aufrufer (z. B. in Objekten) | je Objekt, nicht untersucht | `ev_be9df4485dae`, `ev_54a60cf82af8` |
| Park-Miller (16807) mit XOR-Maske `0x75bd924` | `0x68b5f0`, Float-Wrapper `0x68b650` | `0x95fd58` (Seed **0x1f3ad27f**), Default `0x969730` (Seed **0**) | fest | `ev_e52d6840d8f5`, `ev_647339a34ec6` |
| 16-Bit-LCG (`x*0x24cd + 1`) | `0x5ae160` | `0x95fd5c` | **12345** (0x3039) | `ev_2d1ade9257ef` |
| MSVC-CRT `rand` (`x*0x343fd + 0x269ec3`, pro Thread) | `_rand` `0x742c43`, `srand` = `0x742c36` | `_tiddata->_holdrand` | **0** | `ev_9245861a4461`, `ev_308788572fb8` |
| weitere Seed-Variablen | `0x560b00`, `0x583f00` | `0x95dedc`, `0x95e15c` | **0** | `ev_308788572fb8` |

- Alle Seeds setzt eine einzige Funktion: `0x5ae190`, aufgerufen nur aus der Spiel-Init `0x431d80` (`ev_73a4eb6d5d13`, `ev_b12ee38ee0cf`). Alle Werte sind Konstanten. Kein Seed hängt von Zeit, QPC oder `GetTickCount` ab.
- Ein Byte-Scan nach den Konstanten von glibc-LCG (`0x41c64e6d`) und Mersenne-Twister-Init (`0x6c078965`) fand keine Treffer (`ev_fd58dc644aa2`, `ev_9c3407b69d10`). Den Numerical-Recipes-LCG gibt es nur in den vier Funktionen oben (`ev_c5e48332f8f6`), den MSVC-`rand` nur in `_rand` (`ev_d35e35c4cc96`).
- CRT-`rand` hat nur 6 Aufrufer, alle in Rendering oder Effekten: Rauschtextur `0x6e8d00`, Shader-Konstanten `0x7289d0`, Partikel `0x55ccc0` und `0x666a30` (`ev_9245861a4461`, `ev_905f042e6ad2`).

**Folgerungen**

- Der Zufall ist **deterministisch**: eigene PRNGs, feste Seeds, einmal beim Start gesetzt. Die wichtigste Seed-Variable ist **`0xad3b6c`**.
- Gefahr für Lockstep: Die Zustände laufen ab dem Programmstart weiter, auch durch Menüs, Intro und Effekte. Starten zwei Spieler unterschiedlich oft ins Menü, stehen ihre PRNGs beim Levelstart woanders. Deshalb beim Levelstart alle Zustände vom Host übernehmen: `0xad3b6c`, `0x95fd58`, `0x95fd5c`, `0x969730`, `0x95dedc`, `0x95e15c` sowie `_holdrand` des Hauptthreads. Danach gehören sie in die Prüfsumme.
- Effekte, die nur fürs Rendering gedacht sind, ziehen aus demselben globalen LCG `0xad3b6c` wie die Spiellogik (zum Beispiel `0x666a30`). Hängt deren Aufrufzahl von Bildschirmauflösung oder Sichtbarkeit ab, divergiert der Zustand. Beide Seiten müssen deshalb mit gleicher Auflösung laufen, bis ein Test das Gegenteil zeigt.

### 3. Eingabe und Spielerzuordnung

**Beobachtet (Ebenen von unten nach oben)**

1. **Gerät lesen:** `0x5206c0(geräteIndex, out)`. Index 0–3 ist XInput und wird nur gelesen, wenn der Slot-Typ 6 ist. Aufgerufen wird `XInputGetState` (Thunk `0x7954b2`, einzige Aufrufstelle `0x520764`) und `XInputGetCapabilities` beim ersten Mal. Index 4–9 ist DirectInput-Joystick: `vtbl+0x64` = `Poll`, `vtbl+0x24` = **`GetDeviceState`** mit 0x110 Bytes (`DIJOYSTATE2`), `vtbl+0x1c` = `Acquire`. Index 10–13 sind virtuelle Geräte (`0x51f980`), darunter die Tastatur. Die Gerätedaten liegen im Input-Objekt ab `+0x4e34`, Stride 0x17c. (`ev_901efb754a9d`, `ev_b118903cad2c`, `ev_da99cce9e233`)
2. **In ein PS2-artiges Pad umsetzen:** `0x523c30` → `0x520f60` macht aus Rohdaten 8 Analog-Bytes (0x80 = Mitte) und eine 32-Bit-Tastenmaske. Maus (`0x520b70`) und Tastatur (`0x51eb90`, Puffer `+0x68b0`) fließen hier mit ein. (`ev_182ed8c4264d`, `ev_84b4a7d78e50`)
3. **Physische Pads:** `0x6de440` wird einmal pro Frame aus `0x6e0240` aufgerufen. Es geht über **14 Geräte** (`0x51f2a0` gibt 14 zurück) und füllt die Tabelle **`0xaf8870`** (Stride 0x84): `+0` verbunden, `+4` zugeordneter logischer Port (−1 = frei), `+8..+0xF` Analogwerte, `+0x10` Tasten. (`ev_9a491b5a782e`, `ev_3cfac00d8b5a`)
4. **Logische Ports:** Tabelle **`0xaecb64`**, Stride 12, Anzahl `0x99f2dc`. Feld `[0]` ist der Geräteindex, `[1]` das Aktiv-Flag. `0x6d5df0(port)` liefert das Gerät. Zuordnung in `0x6de440`:
   - Einzelspieler-Modus (`0xb038f8 == 0`): Port 0 bekommt das erste verbundene Gerät mit Eingabe. Es rastet ein, sobald Maske `0x840` gedrückt wird.
   - Mehrspieler-Modus: Ein aktiver Port ohne verbundenes Gerät übernimmt das erste freie Gerät, auf dem **Start (`0x800`)** gedrückt wird. Ein Port ohne Aktiv-Flag verliert sein Gerät.
5. **Spieler-Pads:** `0x5a2a80` kopiert die Pad-Zeiger aus `0xa96384[0..1]` nach **`0xa96388` (Spieler 1)** und **`0xa963e8` (Spieler 2)**. Stride 0x60, es gibt genau 2 Einträge. Jede Spielfigur zeigt über `+0x112c` auf ihren Eintrag. (`ev_6c939daa52ee`, `ev_fb3e9fba199d`)
6. **Drop-in/Drop-out:** `0x5a3510` läuft jedes Weltupdate über beide Spieler, `0x5a3290(i)` liefert ihren Status:
   - 1: Menü
   - 2: Slot hat eine Figur, die ein Spieler steuert (Figur `+0x1fc` Bit 0x80)
   - 3: Slot hat eine Figur, die die KI steuert. Das ist der Fall, wenn Spieler 2 nicht beigetreten ist.

   Bei Status 3 wird nur die Start-Maske (`0x95f724` = 0x800) in den Pad-Eintrag `+8` (`0xa96390` + 0x60·i) gesammelt. Bei Status 2 öffnet Start das Pausenmenü (`0x5bd740(pad)`, merkt den Spieler in `0x960308`). Das globale Flag „Spieler 2 aktiv“ ist **`0xab3984`** (73 Referenzen, auch als Skriptvariable `Player2Active` exportiert in `0x4b01d0`). (`ev_da99cce9e233`, `ev_dfa0284c9546`, `ev_66fda3af0d39`, `ev_c831fbb183ae`)
- Tastenmasken in `0x95f720…`: Start 0x800, D-Pad 0x1000/0x2000/0x4000/0x8000 (`ev_c831fbb183ae`). Das passt zum PS2-Pad-Layout.

**Folgerungen**

- Zuordnung: Spieler 1 ist logischer Port 0, Spieler 2 logischer Port 1. Welches Gerät auf welchem Port landet, ist **dynamisch**: Es entscheidet, wer zuerst Start drückt. Die Zuordnung hängt nur vom Pad-Inhalt (verbunden + Tasten) ab und ist damit selbst deterministisch, solange beide Seiten dieselben Pad-Daten sehen.
- **Bester Lockstep-Einhakpunkt:** in `0x6e0240` direkt vor `0x6de440` (Frame-Grenze) auf die Eingabe der Gegenseite warten, dann die physischen Pad-Records gleich befüllen. Alternativ in der Proxy-DLL: `XInputGetState` für Index 0 und 1 auf beiden Seiten identisch liefern („Controller 0 = P1, Controller 1 = P2“) und DirectInput-Joysticks sowie Tastatur im Online-Modus ausblenden. Die Eingaben von Frame N+1 liest das Spiel am Ende von Frame N.
- Drop-in für P2 heißt: Start auf einem freien Gerät, solange die P2-Figur KI-gesteuert ist. Das läuft über dieselbe Pad-Kette und braucht keinen Sonderfall, wenn die Pads synchron sind.

**Unbekannt**

- Welcher Code die gesammelte Start-Maske (`0xa96390`) bei Status 3 auswertet und die KI-Figur an Spieler 2 übergibt, ist nicht gefunden. Drop-out ist ebenfalls nicht lokalisiert.
- `0x6de440` pollt nur, wenn sich `0x52fec0()` gegenüber `0xb03940` ändert. Was dieser Zähler ist, ist offen. Wenn er an Fensterfokus oder Nachrichten hängt, können Polls ausfallen.
- Die Tastaturbelegung für Geräte 10–13 (`0x51f650`) ist nicht im Detail untersucht.

### 4. Figuren-Strukturen (Grundlage für Prüfsummen)

**Beobachtet**

- **Spieler-Slots:** `0xab3960` ist ein Array von 8 Zeigern auf Figuren. `[0]` = P1 (`0xab3960`), `[1]` = P2 (`0xab3964`), Anzahl der Spieler-Slots in `0xab3988`. (`ev_6c939daa52ee`, `ev_a6268d6f3d97`)
- **Alle Figuren:** Zeiger `0xab364c` auf ein Array mit Stride **0x1648**, Anzahl in `0xab3648` (Schleife in `0x42f4e0`, 158 Referenzen auf `0xab364c`). (`ev_cbf669816525`, `ev_7c6c3553a89b`)
- Felder einer Figur, wie sie in `0x42ad10`, `0x42f4e0`, `0x5a3510` und `0x4b5ed0` verwendet werden:

| Offset | Typ | Verwendung |
|---|---|---|
| `+0x5c/+0x60/+0x64` | float x,y,z | Position (x und z werden mit sin/cos von `+0x246` versetzt) |
| `+0x8c` | vec3 | Wird für die Geschwindigkeit von `+0x5c` abgezogen und durch `dt` geteilt (`0x5a3510`) |
| `+0x80/+0x84/+0x88` | vec3 | Position für Effekte, Text und Sound (`"!!!FROZEN!!!"`, `Riddler_MindControl_Zap`) |
| `+0xb8` | 4x4-Matrix | Weltmatrix (wird als 0x40 Bytes kopiert) |
| `+0x190..+0x198` | vec3 | Fallback-Position in `0x4b5ed0` |
| `+0x246` | u16 | Blickrichtung (Winkel 0–0xFFFF) |
| `+0x1fc` | Flags | Bit 0x80 = spielergesteuert |
| `+0x112c` | Zeiger | Spieler-Pad-Eintrag (`0xa96388 + 0x60·i`) |
| `+0x15b0` | short | Figuren-ID |
| `+0x9db` | byte | Aktions-/Zustandscode |

**Folgerungen**

- Vorschlag für die Desync-Prüfsumme pro Frame: über alle `0xab3648` Figuren ab `*(0xab364c)` jeweils `+0x5c..+0x67` und `+0x246` hashen, dazu die PRNG-Zustände aus Abschnitt 2 und den Frame-Zähler `0xadf68c` (wird in `0x6e0240` pro Frame erhöht).
- Dass `+0x5c` die „echte“ Position und `+0x8c` die des Vorframes ist, ist eine Folgerung aus der Geschwindigkeitsrechnung. Das muss zur Laufzeit bestätigt werden (Speicher lesen, Figur bewegen).

## Empfehlung: Ist Lockstep-Determinismus realistisch?

**Ja, mit Auflagen.** Die zwei Kernvoraussetzungen sind erfüllt:

- Der Zufall ist geseedet und zeitunabhängig.
- Die Zeit fließt an genau einer Stelle in die Simulation (`0x6e0240` → `0xa95fe0`), und dort gibt es sogar einen eingebauten Override für festes `dt`.

Damit die Simulation auf beiden Rechnern gleich läuft, braucht es:

1. **Festes dt:** `0xadf690 = 1/60` auf beiden Seiten, Simulation strikt im Frame-Lockstep. Ein Frame wird erst gerechnet, wenn die Eingabe beider Seiten da ist, Barriere in `0x6e0240` vor `0x6de440`.
2. **Identische Pad-Daten:** Proxy liefert für Index 0/1 dieselben Daten auf beiden Seiten, inklusive „verbunden“. DirectInput-Joysticks und Tastatur sind im Online-Modus ausgeblendet.
3. **Synchroner Levelstart:** Barriere, bis beide Seiten fertig geladen haben. Der Host schickt alle PRNG-Zustände, beide setzen sie und starten im selben Frame.
4. **Prüfsumme pro Frame** (Abschnitt 4) und bei Abweichung sofort melden.

Risiken, die erst der Test zeigt:

- x87-Gleitkomma auf Intel vs. AMD. Werden `fsin`/`fcos` direkt benutzt, können die Ergebnisse je nach Hersteller abweichen.
- Lade-Threads (`CreateThread` ist importiert): Objekte könnten in unterschiedlichen Frames erscheinen.
- Effekte, deren PRNG-Verbrauch von Auflösung oder Sichtbarkeit abhängt.
- Das dt-Override in Cutscenes (`0x7017a0`).

Nächster Schritt ist der Determinismus-Test mit zwei lokalen Instanzen. Mit festem dt, aufgezeichneter Eingabe und Prüfsumme pro Frame zeigt sich, ob und ab welchem Frame die beiden Läufe auseinanderlaufen.

## Laufzeit-Bestätigung (Nacht 2026-10-17)

Getestet mit der Hook-DLL aus Phase 1 (siehe [phase1-werkzeuge.md](phase1-werkzeuge.md) und [nachtarbeit.md](nachtarbeit.md)). Das sind jetzt **beobachtete** Ergebnisse, keine statischen Folgerungen mehr.

- **Determinismus bestätigt.** Mit festem dt (`0xadf690 = 1/60`), synchronisiertem Lade-Worker und gleichen Eingaben laufen wiederholte Läufe bitgleich. Geprüft wurden Figurenpositionen, alle 6 PRNG-Zustände und die Figurenliste über bis zu 12.500 Frames: Menüs, Laden des Spielstands, Bathöhle, Drop-in von Spieler 2, Kämpfen, Springen, Respawn.
- **Einzige gefundene Störquelle: der asynchrone Lade-Worker** `0x6e2cc0` (Schlange `0xb03c68`, Events „BGWorkToDo“ `0xb03bd8` und „BGFreeze“ `0xb03bdc`). Ohne Synchronisation spawnen Figuren je nach Ladezeit einen Frame früher oder später, und der 16-Bit-PRNG `0x95fd5c` läuft auseinander. Die Lösung ist, Jobs nur am Frame-Ende laufen zu lassen und darauf zu warten.
- **Figurenfelder bestätigt.** `+0x5c/+0x60/+0x64` ist die Position (x, y, z). y ändert sich beim Springen, x/z beim Laufen.
- **Eingabe.** XInput-Pads erkennt das Spiel nur über die DirectInput-Geräteliste. Die Belegung hängt an PID/VID im Slot (`+8`). Ohne Fensterfokus werden Pad-Eingaben verworfen (`0x94d3a0`). Gepollt wird pro Present (`0x9d08d0`).
- **Lockstep-Prototyp funktioniert lokal.** Zwei Instanzen auf einem PC tauschen Eingaben über UDP aus. Host und Client blieben über 9000 Frames bitgleich, 0 Desyncs.

## Offen (dynamisch zu klären)

- [x] Läuft das Spiel mit festem oder variablem Zeitschritt? → **variabel**, einziger Einhakpunkt `0x6e0240`/`0xadf690` (Abschnitt 1)
- [x] Woher kommt der Zufall? → **eigene PRNGs, feste Seeds** in `0x5ae190`, Haupt-Seed `0xad3b6c` (Abschnitt 2)
- [x] Determinismus-Test: gleiche Eingaben plus gleiche Uhr ergeben in zwei Läufen gleiche Figurenpositionen. → **ja**, mit Worker-Sync (siehe oben)
- [x] Wie verteilt das Spiel die Eingaben auf Spieler 1 und 2? → logische Ports 0/1, dynamische Gerätezuordnung per Start (Abschnitt 3). Offen ist noch, welcher Code Drop-in und Drop-out der P2-Figur auslöst.
- [x] Laufzeit-Bestätigung der Figurenfelder und des dt-Overrides `0xadf690`. → Position `+0x5c` und dt-Override bestätigt; dass `+0x8c` der Vorframe ist, bleibt ungeprüft.
- [ ] Determinismus **zwischen verschiedenen PCs** (Intel/AMD, andere GPU). Bisher nur auf einem Rechner getestet.
