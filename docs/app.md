# App (Phase 4)

`app/LegoBatmanOnline` ist eine WPF-App (.NET 10). Sie wird als **eine** selbstständige EXE veröffentlicht, in der `lbo-launch.exe` und `lbo.dll` eingebettet sind. Beim Start entpackt die App beide nach `%LOCALAPPDATA%\LegoBatmanOnline\bin\<hash>\`. Ein Ordner pro Version, damit ein laufendes Spiel (es sperrt die DLL) spätere Updates nicht blockiert.

## Aufbau

| Datei | Aufgabe |
|---|---|
| `Core/Identity.cs` | P-256-Schlüsselpaar. Freundescode = 12 Zeichen Base32 aus SHA-256 des öffentlichen Schlüssels, Anzeige `XXXX-XXXX-XXXX` |
| `Core/Profile.cs` | `%LOCALAPPDATA%\LegoBatmanOnline\app\<profil>\profil.json`: Name, privater Schlüssel (DPAPI), Freunde, Spielpfad, letzter Spielstand |
| `Core/Messenger.cs` | Vermittlung über öffentliche MQTT-Dienste, Ende-zu-Ende-Verschlüsselung |
| `Core/GameBridge.cs` | Spiel finden (Steam-Registry + `libraryfolders.vdf`), Spielstände auflisten, Launcher aufrufen (`--stun`, `--connect`), Verknüpfungen anlegen |
| `Core/Lobby.cs` | Ablauf: Freunde, Einladungen, Sitzung; Testoptionen |
| `MainWindow.xaml` | Oberfläche |

## Vermittler

Die App verbindet sich **gleichzeitig** mit `broker.hivemq.com`, `broker.emqx.io` und `test.mosquitto.org` (Port 1883) und sendet über alle. Zwei Spieler finden sich auch dann, wenn ein Dienst bei einem von ihnen ausfällt. Doppelte Nachrichten werden über die Nonce aussortiert.

Themen unter `lbobatman/v1/`:

- `profile/<id>`: `{pub, name}`, retained. Beim Nachschlagen prüft die App, dass der Hash von `pub` die ID ergibt.
- `presence/<id>`: `on`/`off`, retained, Last Will `off`. Online, sobald ein Dienst `on` meldet.
- `inbox/<id>`: Umschlag `{from, pub, n, c}`. Inhalt mit AES-GCM verschlüsselt. Schlüssel per statischem ECDH (SHA-256 mit Präfix `lego-batman-online-v1`), AAD `from|to`. Nachrichten älter als 600 s werden verworfen.

Die Dienste sehen nur Codes, Namen und Online-Status. Anfragen, Einladungen und IP-Adressen sind verschlüsselt.

## Protokoll

```
A → B  friend_req                       B zeigt "Annehmen" (gleichzeitige Anfragen → sofort Freunde)
B → A  friend_ok                        nur gültig, wenn A vorher angefragt hat
A → B  invite {sid, slot}               nur von Freunden; wer schon spielt, antwortet invite_no {busy}
B → A  invite_no {sid} | invite_yes {sid, ep}    B ermittelt vorher per STUN seine Adresse
A → B  go {sid, ep, slot}               A ermittelt nach invite_yes seine Adresse
beide  lbo-launch --connect host|join ... [--slot N]
       cancel {sid}                     jederzeit von beiden Seiten
```

`ep` = `{port, pub, loc, key}` aus `lbo-launch --stun`. `--connect` bindet denselben lokalen Port erneut, damit die NAT-Zuordnung aus der STUN-Abfrage weiter gilt. Der Launcher meldet `STATUS verbinde|verbunden|ping|spielstand|spiel|beendet` und `ERROR …`. Die App zeigt das im Sitzungsfenster an.

## Testen

`tools/test_app.py [exe] [slot] [maxframes]` startet zwei Instanzen mit den Profilen `testA` und `testB`:

| Option | Wirkung |
|---|---|
| `--profil X` | eigenes Profil |
| `--test` | setzt `LBO_MULTI=1`, `LBO_ALLOWSAVE=0`, schreibt `code.txt`, keine Verknüpfungen |
| `--name N` | Name setzen |
| `--auto-accept` | Anfragen und Einladungen sofort annehmen |
| `--auto-invite CODE` | Freund hinzufügen und einladen, sobald er online ist |
| `--slot N` | Spielstand wählen |
| `--max-frames N` | Spiel nach N Frames beenden |

Ergebnis vom 17.10.2026 (Debug-Build und veröffentlichte EXE): Beide Spiele liefen nach 7 s, 7500 Frames bei Host und Gast identisch. Spielstand 1 wurde automatisch geladen, der echte Spielstand blieb unverändert.

`tools/window_shot.ps1 <pid> <png>` fotografiert ein Fenster, auch wenn es verdeckt ist.
