# App (Phase 4)

`app/LegoBatmanOnline` ist eine WPF-App (.NET 10). Sie wird als **eine** selbstständige EXE veröffentlicht, in der `lbo-launch.exe` und `lbo.dll` eingebettet sind. Beim Start entpackt die App beide nach `%LOCALAPPDATA%\LegoBatmanOnline\bin\<hash>\`. Ein Ordner pro Version, damit ein laufendes Spiel (es sperrt die DLL) spätere Updates nicht blockiert.

## Bedienkonzept: gemeinsame Welten

Zwei Freunde teilen eine **Welt**: einen benannten Spielstand. Jeder kann jederzeit **alleine** darin weiterspielen. Sind beide online, spielen sie **zusammen** im selben Stand. Nach jedem Spiel gleicht die App den Stand ab. Er liegt verschlüsselt beim Vermittler, damit ihn der andere auch bekommt, wenn man selbst offline ist. Bei einem Konflikt (beide haben getrennt gespielt) gewinnt der zuletzt gespeicherte Stand. Der unterlegene Stand wird gesichert.

## Aufbau

| Datei | Aufgabe |
|---|---|
| `Core/Identity.cs` | P-256-Schlüsselpaar. Freundescode = 12 Zeichen Base32 aus SHA-256 des öffentlichen Schlüssels, Anzeige `XXXX-XXXX-XXXX` |
| `Core/Profile.cs` | `%LOCALAPPDATA%\LegoBatmanOnline\app\<profil>\profil.json`: Name, privater Schlüssel (DPAPI), Freunde, Welten, Spielpfad |
| `Core/World.cs` | Welt-Daten, Ordner, Prüfsumme, Verpacken und Verschlüsseln, Sicherungen |
| `Core/Messenger.cs` | Vermittlung über öffentliche MQTT-Dienste, Ende-zu-Ende-Verschlüsselung |
| `Core/GameBridge.cs` | Spiel finden (Steam-Registry + `libraryfolders.vdf`), Spielstände auflisten, Launcher aufrufen (`--stun`, `--connect`, `--solo`), Verknüpfungen anlegen |
| `Core/Lobby.cs` | Ablauf: Freunde, Welten, Abgleich, Einladungen, Sitzung; Testoptionen |
| `MainWindow.xaml` | Oberfläche |

Welt-Ordner: `%LOCALAPPDATA%\LegoBatmanOnline\welten\<profil>\<welt-id>\`
- `spielstand\`: SaveGame-Dateien. Das Spiel liest und speichert hier: Die DLL leitet alle Zugriffe auf `Saved Games` per `LBO_SAVEDIR` um.
- `sicherungen\<zeit>_ersetzt\`: Stände, die durch einen neueren ersetzt wurden. Höchstens 20 werden aufbewahrt.

## Vermittler

Die App verbindet sich **gleichzeitig** mit `broker.hivemq.com`, `broker.emqx.io` und `test.mosquitto.org` (Port 1883) und sendet über alle. Zwei Spieler finden sich auch dann, wenn ein Dienst bei einem von ihnen ausfällt. Doppelte Nachrichten werden über die Nonce aussortiert.

Themen unter `lbobatman/v1/`:

- `profile/<id>`: `{pub, name}`, retained. Beim Nachschlagen prüft die App, dass der Hash von `pub` die ID ergibt.
- `presence/<id>`: `on`, `busy` (spielt gerade) oder `off`. Retained, Last Will `off`. Die App erneuert den Status alle 30 s, damit ein verspäteter Last Will einer alten Verbindung nicht hängen bleibt. Über alle Dienste gilt: einer meldet `busy` → `busy`, sonst einer `on` → `on`.
- `inbox/<id>`: Umschlag `{from, pub, n, c}`. Inhalt mit AES-GCM verschlüsselt. Schlüssel per statischem ECDH (SHA-256 mit Präfix `lego-batman-online-v1`), AAD `from|to`. Nachrichten älter als 600 s werden verworfen.
- `world/<thema>`: Spielstand einer Welt, retained. `thema` = SHA-256(`thema|` + Weltschlüssel), die ersten 32 Hex-Zeichen. Fremde kennen das Thema nicht. Inhalt: `0x01 | nonce(12) | AES-GCM(gzip(JSON)) | tag(16)`, AAD `welt|<id>`. Das JSON enthält `{ts, sha, by, name, files, times}`. Ein Stand ist etwa 40 KB groß, komprimiert deutlich weniger.

Die Dienste sehen nur Codes, Namen und Online-Status. Anfragen, Einladungen, IP-Adressen und Spielstände sind verschlüsselt.

## Abgleich

- **Lokaler Stand:** `VersionTs` (Unix-ms), `VersionSha` (Prüfsumme über Namen und Inhalt der SaveGame-Dateien) und `VersionBy`.
- **Nach jedem eigenen Spiel** (alleine oder als Host) und beim App-Start: Hat sich die Prüfsumme geändert, gilt `VersionTs = max(letzte Dateiänderung, VersionTs + 1)`. Die App lädt den Stand hoch.
- **Empfang:**
  - Gleiche Prüfsumme: nichts zu tun.
  - Der empfangene Stand ist neuer (größeres `ts`; bei Gleichstand gewinnt die größere Prüfsumme): Die App sichert den lokalen Stand und übernimmt den empfangenen.
  - Der eigene Stand ist neuer: Die App lädt ihn erneut hoch.
  - Läuft gerade ein Spiel aus diesem Ordner, wartet der empfangene Stand bis zum Spielende und wird dann noch einmal verglichen.
- **Nach dem Verbinden** wartet die App 6 s auf hinterlegte Stände und lädt dann die eigenen hoch, sofern beim Vermittler nichts Neueres liegt.
- **Gast beim Zusammenspielen:** Er spielt mit der Kopie des Hosts (`gast-spielstand`), sein Welt-Ordner bleibt unberührt. Den neuen Stand bekommt er danach über den Vermittler.

## Protokoll (Inbox)

```
A → B  friend_req                       B zeigt "Annehmen" (gleichzeitige Anfragen → sofort Freunde)
B → A  friend_ok                        nur gültig, wenn A vorher angefragt hat
A → B  world {id, wname, key}           Welt teilen; nur von Freunden
A → B  invite {sid, slot, world, wname} nur von Freunden; wer schon spielt, antwortet invite_no {busy}
B → A  invite_no {sid} | invite_yes {sid, ep}    B ermittelt vorher per STUN seine Adresse
A → B  go {sid, ep, slot}               A ermittelt nach invite_yes seine Adresse
A      lbo-launch --connect host ... --savedir <welt>\spielstand [--slot N]
B      lbo-launch --connect join ... [--slot N]
       cancel {sid}                     jederzeit von beiden Seiten
```

`ep` = `{port, pub, loc, key}` aus `lbo-launch --stun`. `--connect` bindet denselben lokalen Port erneut, damit die NAT-Zuordnung aus der STUN-Abfrage weiter gilt. Der Launcher meldet `STATUS verbinde|verbunden|ping|spielstand|spiel|beendet` und `ERROR …`. Die App zeigt das im Sitzungsfenster an. `slot` ist der zuletzt geänderte Spielstand der Welt (automatisch laden).

Alleine spielen: `lbo-launch --solo --savedir <welt>\spielstand`. Ohne Netz und ohne automatisches Laden. Im Menü sind nur die Spielstände der Welt sichtbar.

## Testen

`tools/test_worlds.py [exe] [teile]` mit zwei Instanzen und den Profilen `wA` und `wB`:

1. A fügt B hinzu und erstellt die Welt „Gotham“ aus Spielstand 1. B bekommt Welt und Stand.
2. Zusammen spielen aus dem Welt-Ordner, 7500 Frames, Vergleich von Host und Gast.
3. Konflikt: B ändert den Stand. A übernimmt den neueren und sichert seinen eigenen.
4. B lädt hoch und beendet die App. A startet danach allein und holt den Stand vom Vermittler.

| Option | Wirkung |
|---|---|
| `--profil X` | eigenes Profil |
| `--test` | setzt `LBO_MULTI=1`, `LBO_ALLOWSAVE=0`, schreibt `code.txt`, keine Verknüpfungen |
| `--name N` | Name setzen |
| `--auto-accept` | Anfragen und Einladungen sofort annehmen |
| `--auto-invite CODE` | Freund hinzufügen |
| `--auto-world NAME` | nach der Freundschaft die Welt NAME anlegen (Start: `--slot N`) |
| `--auto-play` | sobald der Partner online ist: zusammen spielen |
| `--max-frames N` | Spiel nach N Frames beenden |

Ergebnis vom 17.10.2026: Alle vier Teile bestanden, mit Debug-Build und veröffentlichter EXE. Teil 2 wurde nur mit dem Debug-Build ausgeführt. Ergebnisse:
- B hatte die Welt nach 11 s.
- Beim Zusammenspielen waren 7500 Frames bei Host und Gast identisch.
- Der Konflikt wurde nach 2 s aufgelöst, mit einer Sicherung.
- Die Übergabe über den Vermittler funktionierte, obwohl B offline war.
- Der echte Spielstand blieb unverändert.

`--solo` wurde von Hand mit 900 Frames geprüft: Der Spielstand-Ordner wurde umgeleitet.

`tools/window_shot.ps1 <pid> <png>` fotografiert ein Fenster, auch wenn es verdeckt ist.
