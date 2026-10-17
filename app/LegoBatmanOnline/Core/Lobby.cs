using System.Collections.ObjectModel;
using System.ComponentModel;
using System.IO;
using System.Runtime.CompilerServices;
using System.Security.Cryptography;
using System.Text.Json.Nodes;
using System.Windows.Threading;

namespace LegoBatmanOnline.Core;

public sealed class FriendItem : INotifyPropertyChanged
{
    private bool _online;
    private string _name = "";
    private FriendState _state;
    public string Id { get; init; } = "";
    public string Code => Identity.FormatCode(Id);
    public string Name { get => _name; set { _name = value; Raise(); } }
    public bool Online { get => _online; set { _online = value; Raise(); Raise(nameof(CanInvite)); Raise(nameof(StatusText)); } }
    public FriendState State
    {
        get => _state;
        set { _state = value; Raise(); Raise(nameof(CanInvite)); Raise(nameof(IsIncoming)); Raise(nameof(IsFriend)); Raise(nameof(StatusText)); }
    }
    public bool CanInvite => Online && State == FriendState.Friend;
    public bool IsIncoming => State == FriendState.RequestReceived;
    public bool IsFriend => State == FriendState.Friend;
    public string StatusText => State switch
    {
        FriendState.RequestSent => "Anfrage gesendet",
        FriendState.RequestReceived => "möchte dein Freund sein",
        _ => Online ? "online" : "offline",
    };
    public event PropertyChangedEventHandler? PropertyChanged;
    private void Raise([CallerMemberName] string? n = null) => PropertyChanged?.Invoke(this, new(n));
}

public sealed class Invitation
{
    public string Sid { get; init; } = "";
    public FriendItem From { get; init; } = null!;
    public int Slot { get; init; }
    public string Text => Slot >= 0
        ? $"{From.Name} lädt dich ein (Spielstand {Slot + 1})"
        : $"{From.Name} lädt dich ein";
}

/// <summary>Ablauf der App: Freunde, Einladungen, Verbindungsaufbau und Spielstart.</summary>
public sealed class Lobby : INotifyPropertyChanged
{
    private readonly Profile _profile;
    private readonly Identity _me;
    private readonly Messenger _msg;
    private readonly Dispatcher _ui;
    private readonly AppOptions _opt;
    private string? _launcher;
    private CancellationTokenSource? _sessionCts;

    // laufende Einladung
    private string? _sid;
    private FriendItem? _peer;
    private Endpoint? _myEp;
    private int _sessionSlot = -1;
    private bool _isHost;

    public ObservableCollection<FriendItem> Friends { get; } = new();
    public ObservableCollection<SaveSlot> Saves { get; } = new();
    public ObservableCollection<string> Log { get; } = new();

    public string MyCode => Identity.FormatCode(_me.Id);
    public string Name { get => _profile.Name; set { _profile.Name = Messenger.Clean(value); _profile.Save(); Raise(); _ = _msg.PublishProfileAsync(); } }
    public bool NeedsName => string.IsNullOrWhiteSpace(_profile.Name);

    private bool _brokerOnline;
    public bool BrokerOnline { get => _brokerOnline; private set { _brokerOnline = value; Raise(); Raise(nameof(BrokerText)); } }
    public string BrokerText => BrokerOnline ? "Vermittler verbunden" : "Verbinde mit Vermittler …";

    public string? GamePath => _profile.GamePath;
    public string GameText => GamePath != null ? "LEGO Batman gefunden" : "LEGO Batman nicht gefunden";
    public bool GameOk => GamePath != null;
    public bool GameMissing => GamePath == null;

    private SaveSlot? _selectedSave;
    public SaveSlot? SelectedSave { get => _selectedSave; set { _selectedSave = value; Raise(); if (value != null) { _profile.LastSlot = value.Index; _profile.Save(); } } }
    private bool _autoLoad = true;
    public bool AutoLoad { get => _autoLoad; set { _autoLoad = value; Raise(); } }

    private Invitation? _incoming;
    public Invitation? Incoming { get => _incoming; private set { _incoming = value; Raise(); Raise(nameof(HasIncoming)); } }
    public bool HasIncoming => Incoming != null;

    private string _sessionText = "";
    public string SessionText { get => _sessionText; private set { _sessionText = value; Raise(); } }
    private bool _sessionActive;
    public bool SessionActive { get => _sessionActive; private set { _sessionActive = value; Raise(); Raise(nameof(Idle)); } }
    public bool Idle => !SessionActive;

    private string _hint = "";
    public string Hint { get => _hint; set { _hint = value; Raise(); } }

    public Lobby(Profile profile, Identity me, AppOptions opt, Dispatcher ui)
    {
        _profile = profile;
        _me = me;
        _opt = opt;
        _ui = ui;
        _msg = new Messenger(me, () => string.IsNullOrWhiteSpace(_profile.Name) ? "Spieler" : _profile.Name);
        _msg.ConnectionChanged += on => _ui.BeginInvoke(() => BrokerOnline = on);
        _msg.PresenceChanged += (id, on) => _ui.BeginInvoke(() => { var f = Friends.FirstOrDefault(x => x.Id == id); if (f != null) f.Online = on; });
        _msg.MessageReceived += (from, pub, c) => _ui.BeginInvoke(() => OnMessage(from, pub, c));
        foreach (var f in profile.Friends)
            Friends.Add(new FriendItem { Id = f.Id, Name = f.Name, State = f.State });
    }

    public async Task StartAsync()
    {
        _profile.GamePath ??= GameBridge.FindGame();
        _profile.Save();
        Raise(nameof(GamePath)); Raise(nameof(GameText)); Raise(nameof(GameOk)); Raise(nameof(GameMissing));
        RefreshSaves();
        try { _launcher = GameBridge.PrepareBinaries(); }
        catch (Exception e) { AddLog("Mod-Dateien fehlen: " + e.Message); }
        if (!_profile.ShortcutsCreated && !_opt.Test)
        {
            try { GameBridge.CreateShortcuts(); } catch { }
            _profile.ShortcutsCreated = true;
            _profile.Save();
        }
        if (_opt.Test)
            File.WriteAllText(Path.Combine(Profile.BaseDir, "app", _opt.Profile, "code.txt"), _me.Id);
        if (_opt.Slot >= 0) SelectedSave = Saves.FirstOrDefault(s => s.Index == _opt.Slot) ?? SelectedSave;
        await _msg.StartAsync();
        foreach (var f in Friends) await _msg.WatchAsync(f.Id);
        if (_opt.AutoInvite != null) _ = AutoInviteAsync(_opt.AutoInvite);
    }

    public void RefreshSaves()
    {
        Saves.Clear();
        foreach (var s in GameBridge.ListSaves()) Saves.Add(s);
        SelectedSave = Saves.FirstOrDefault(s => s.Index == _profile.LastSlot) ?? Saves.FirstOrDefault();
    }

    public void SetGamePath(string path)
    {
        _profile.GamePath = path;
        _profile.Save();
        Raise(nameof(GamePath)); Raise(nameof(GameText)); Raise(nameof(GameOk)); Raise(nameof(GameMissing));
    }

    private void AddLog(string line)
    {
        Log.Add($"{DateTime.Now:HH:mm:ss}  {line}");
        while (Log.Count > 200) Log.RemoveAt(0);
    }

    // --- Freunde --------------------------------------------------------------------
    public async Task<string> AddFriendAsync(string code)
    {
        var id = Identity.ParseCode(code);
        if (id == null) return "Der Code ist ungültig (12 Zeichen, z. B. ABCD-EFGH-JKLM).";
        if (id == _me.Id) return "Das ist dein eigener Code.";
        if (Friends.Any(f => f.Id == id && f.State == FriendState.Friend)) return "Ihr seid schon befreundet.";
        if (!_msg.IsConnected) return "Keine Verbindung zum Vermittler. Bitte kurz warten.";
        var prof = await _msg.LookupAsync(id, TimeSpan.FromSeconds(8));
        if (prof == null) return "Zu diesem Code wurde niemand gefunden. Hat dein Freund die App schon einmal gestartet?";
        var item = Upsert(id, prof.Value.Name, prof.Value.Pub, FriendState.RequestSent);
        await _msg.SendAsync(id, prof.Value.Pub, new JsonObject { ["t"] = "friend_req" });
        await _msg.WatchAsync(id);
        return $"Anfrage an {item.Name} gesendet.";
    }

    public async Task AcceptFriendAsync(FriendItem f)
    {
        var data = _profile.Find(f.Id);
        if (data == null) return;
        f.State = data.State = FriendState.Friend;
        _profile.Save();
        await _msg.SendAsync(f.Id, Convert.FromBase64String(data.PublicKey), new JsonObject { ["t"] = "friend_ok" });
        await _msg.WatchAsync(f.Id);
    }

    public void RemoveFriend(FriendItem f)
    {
        Friends.Remove(f);
        _profile.Friends.RemoveAll(x => x.Id == f.Id);
        _profile.Save();
    }

    private FriendItem Upsert(string id, string name, byte[] pub, FriendState state)
    {
        var data = _profile.Find(id);
        if (data == null) { data = new Friend { Id = id }; _profile.Friends.Add(data); }
        data.Name = name;
        data.PublicKey = Convert.ToBase64String(pub);
        if (!(data.State == FriendState.Friend && state != FriendState.Friend)) data.State = state;
        _profile.Save();
        var item = Friends.FirstOrDefault(x => x.Id == id);
        if (item == null) { item = new FriendItem { Id = id }; Friends.Add(item); }
        item.Name = name;
        item.State = data.State;
        return item;
    }

    private byte[]? PubOf(string id) => _profile.Find(id) is { } f ? Convert.FromBase64String(f.PublicKey) : null;

    // --- Einladen ---------------------------------------------------------------------
    public async Task InviteAsync(FriendItem f)
    {
        if (SessionActive || !CheckReady()) return;
        var pub = PubOf(f.Id);
        if (pub == null) return;
        _sid = RandomNumberGenerator.GetHexString(8);
        _peer = f;
        _isHost = true;
        _sessionSlot = AutoLoad && SelectedSave != null ? SelectedSave.Index : -1;
        SessionActive = true;
        SessionText = $"Warte auf {f.Name} …";
        AddLog($"Einladung an {f.Name} gesendet");
        await _msg.SendAsync(f.Id, pub, new JsonObject { ["t"] = "invite", ["sid"] = _sid, ["slot"] = _sessionSlot });
        _ = TimeoutAsync(_sid, 120, "Keine Antwort auf die Einladung.");
    }

    public async Task AcceptInviteAsync()
    {
        var inv = Incoming;
        if (inv == null || !CheckReady()) return;
        Incoming = null;
        _sid = inv.Sid;
        _peer = inv.From;
        _isHost = false;
        _sessionSlot = inv.Slot;
        SessionActive = true;
        SessionText = "Ermittle deine Adresse …";
        _myEp = await GameBridge.StunAsync(_launcher!);
        if (_myEp == null) { EndSession("Adresse konnte nicht ermittelt werden (STUN)."); return; }
        WarnNat(_myEp);
        await _msg.SendAsync(_peer.Id, PubOf(_peer.Id)!, new JsonObject { ["t"] = "invite_yes", ["sid"] = _sid, ["ep"] = EpJson(_myEp) });
        SessionText = $"Warte auf {_peer.Name} …";
        _ = TimeoutAsync(_sid, 30, "Der Host hat nicht geantwortet.");
    }

    public async Task DeclineInviteAsync()
    {
        var inv = Incoming;
        Incoming = null;
        if (inv == null) return;
        var pub = PubOf(inv.From.Id);
        if (pub != null) await _msg.SendAsync(inv.From.Id, pub, new JsonObject { ["t"] = "invite_no", ["sid"] = inv.Sid });
    }

    public async Task CancelSessionAsync()
    {
        _sessionCts?.Cancel();
        if (_peer != null && _sid != null && PubOf(_peer.Id) is { } pub)
            await _msg.SendAsync(_peer.Id, pub, new JsonObject { ["t"] = "cancel", ["sid"] = _sid });
        EndSession("Abgebrochen.");
    }

    private bool CheckReady()
    {
        if (GamePath == null) { Hint = "LEGO Batman wurde nicht gefunden. Bitte unten den Spielordner wählen."; return false; }
        if (_launcher == null) { Hint = "Die Mod-Dateien fehlen. Bitte die App neu herunterladen."; return false; }
        return true;
    }

    private async Task TimeoutAsync(string sid, int seconds, string text)
    {
        await Task.Delay(TimeSpan.FromSeconds(seconds));
        if (_sid == sid && SessionActive && _sessionCts == null) EndSession(text);
    }

    private void EndSession(string text)
    {
        AddLog(text);
        Hint = text;
        _sid = null;
        _peer = null;
        _myEp = null;
        _sessionCts = null;
        SessionActive = false;
        SessionText = "";
        RefreshSaves();
    }

    private static JsonObject EpJson(Endpoint e) => new()
    {
        ["port"] = e.Port, ["pub"] = e.Public, ["loc"] = e.Local, ["key"] = e.Key,
    };

    private static Endpoint? ParseEp(JsonNode? n) => n == null ? null :
        new Endpoint((int)n["port"]!, (string)n["pub"]!, (string)n["loc"]!, (int)n["key"]!, false);

    private void WarnNat(Endpoint e)
    {
        if (e.SymmetricNat)
            AddLog("Hinweis: Dein Router erschwert direkte Verbindungen (symmetrisches NAT). Klappt es nicht, nutzt Radmin VPN.");
    }

    // --- Eingehende Nachrichten ---------------------------------------------------------
    private async void OnMessage(string from, byte[] pub, JsonObject c)
    {
        var type = (string?)c["t"];
        var name = (string?)c["name"] ?? "Spieler";
        var known = _profile.Find(from);
        switch (type)
        {
            case "friend_req":
                if (known?.State == FriendState.Friend) { await _msg.SendAsync(from, pub, new JsonObject { ["t"] = "friend_ok" }); break; }
                if (known?.State == FriendState.RequestSent)
                {
                    // Beide haben sich gleichzeitig hinzugefuegt
                    var both = Upsert(from, name, pub, FriendState.Friend);
                    await AcceptFriendAsync(both);
                    break;
                }
                var req = Upsert(from, name, pub, FriendState.RequestReceived);
                AddLog($"{name} möchte dein Freund sein");
                if (_opt.AutoAccept) await AcceptFriendAsync(req);
                break;
            case "friend_ok":
                if (known == null) break;  // nur wer angefragt wurde, kann bestaetigen
                Upsert(from, name, pub, FriendState.Friend);
                await _msg.WatchAsync(from);
                AddLog($"{name} hat deine Anfrage angenommen");
                if (_opt.AutoInvite != null && Identity.ParseCode(_opt.AutoInvite) == from) _ = AutoInviteAsync(_opt.AutoInvite);
                break;
            case "invite":
                if (known?.State != FriendState.Friend) break;  // nur Freunde duerfen einladen
                if (SessionActive)
                {
                    await _msg.SendAsync(from, pub, new JsonObject { ["t"] = "invite_no", ["sid"] = (string?)c["sid"], ["busy"] = true });
                    break;
                }
                var f = Friends.First(x => x.Id == from);
                Incoming = new Invitation { Sid = (string)c["sid"]!, From = f, Slot = (int?)c["slot"] ?? -1 };
                AddLog(Incoming.Text);
                if (_opt.AutoAccept) await AcceptInviteAsync();
                break;
            case "invite_no":
                if ((string?)c["sid"] == _sid && _isHost)
                    EndSession((bool?)c["busy"] == true ? $"{name} spielt gerade schon." : $"{name} hat abgelehnt.");
                break;
            case "cancel":
                if ((string?)c["sid"] == _sid) { _sessionCts?.Cancel(); EndSession($"{name} hat abgebrochen."); }
                if (Incoming?.Sid == (string?)c["sid"]) Incoming = null;
                break;
            case "invite_yes":
                if ((string?)c["sid"] != _sid || !_isHost || _peer?.Id != from) break;
                {
                    var peerEp = ParseEp(c["ep"]);
                    SessionText = "Ermittle deine Adresse …";
                    _myEp = await GameBridge.StunAsync(_launcher!);
                    if (_myEp == null || peerEp == null) { EndSession("Adresse konnte nicht ermittelt werden (STUN)."); break; }
                    WarnNat(_myEp);
                    await _msg.SendAsync(from, pub, new JsonObject { ["t"] = "go", ["sid"] = _sid, ["ep"] = EpJson(_myEp), ["slot"] = _sessionSlot });
                    await PlayAsync(peerEp);
                }
                break;
            case "go":
                if ((string?)c["sid"] != _sid || _isHost || _peer?.Id != from || _myEp == null) break;
                {
                    var peerEp = ParseEp(c["ep"]);
                    if (peerEp == null) break;
                    _sessionSlot = (int?)c["slot"] ?? -1;
                    await PlayAsync(peerEp);
                }
                break;
        }
    }

    private async Task PlayAsync(Endpoint peerEp)
    {
        _sessionCts = new CancellationTokenSource();
        var sid = _sid;
        SessionText = "Verbinde …";
        var env = new Dictionary<string, string>();
        if (_opt.Test)
        {
            env["LBO_MULTI"] = "1";
            env["LBO_ALLOWSAVE"] = "0";
            if (_opt.MaxFrames > 0) env["LBO_MAXFRAMES"] = _opt.MaxFrames.ToString();
        }
        var code = await GameBridge.ConnectAndPlayAsync(_launcher!, _isHost, _myEp!, peerEp, _sessionSlot,
            line => _ui.BeginInvoke(() => OnLauncherLine(line)), env, _sessionCts.Token);
        if (_sid == sid) EndSession(code == 0 ? "Spiel beendet." : "Verbindung fehlgeschlagen.");
    }

    private void OnLauncherLine(string line)
    {
        if (line.StartsWith("STATUS "))
        {
            SessionText = line[7..] switch
            {
                "verbinde" => "Verbinde …",
                "verbunden" => "Verbunden",
                "ping" => "Messe Ping …",
                "spielstand" => _isHost ? "Sende Spielstand …" : "Empfange Spielstand …",
                "spiel" => _isHost
                    ? (_sessionSlot >= 0 ? $"Spiel läuft – Spielstand {_sessionSlot + 1} wird automatisch geladen" : "Spiel läuft – du bedienst die Menüs")
                    : "Spiel läuft – drücke START, sobald ihr im Spiel seid",
                "beendet" => "Spiel beendet",
                var s => s,
            };
        }
        else if (line.StartsWith("ERROR ")) AddLog(line[6..]);
        else if (line.StartsWith("Ping:") || line.StartsWith("Verbunden")) AddLog(line);
    }

    private async Task AutoInviteAsync(string code)
    {
        // Testhilfe: Freund hinzufuegen und einladen, sobald er online ist
        var id = Identity.ParseCode(code);
        if (id == null) return;
        while (!_msg.IsConnected) await Task.Delay(500);
        if (!Friends.Any(f => f.Id == id)) AddLog(await AddFriendAsync(code));
        for (int i = 0; i < 120; i++)
        {
            var f = Friends.FirstOrDefault(x => x.Id == id);
            if (f is { CanInvite: true } && !SessionActive) { await InviteAsync(f); return; }
            await Task.Delay(1000);
        }
    }

    public async Task ShutdownAsync()
    {
        _sessionCts?.Cancel();
        await _msg.DisposeAsync();
    }

    public event PropertyChangedEventHandler? PropertyChanged;
    private void Raise([CallerMemberName] string? n = null) => PropertyChanged?.Invoke(this, new(n));
}

public sealed class AppOptions
{
    public string Profile { get; set; } = "standard";
    public bool Test { get; set; }
    public bool AutoAccept { get; set; }
    public string? AutoInvite { get; set; }
    public string? Name { get; set; }
    public int MaxFrames { get; set; }
    public int Slot { get; set; } = -1;

    public static AppOptions Parse(string[] args)
    {
        var o = new AppOptions();
        for (int i = 0; i < args.Length; i++)
        {
            switch (args[i])
            {
                case "--profil" when i + 1 < args.Length: o.Profile = args[++i]; break;
                case "--test": o.Test = true; break;
                case "--auto-accept": o.AutoAccept = true; break;
                case "--auto-invite" when i + 1 < args.Length: o.AutoInvite = args[++i]; break;
                case "--name" when i + 1 < args.Length: o.Name = args[++i]; break;
                case "--slot" when i + 1 < args.Length: o.Slot = int.Parse(args[++i]); break;
                case "--max-frames" when i + 1 < args.Length: o.MaxFrames = int.Parse(args[++i]); break;
            }
        }
        return o;
    }
}
