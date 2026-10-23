using System.Collections.ObjectModel;
using System.ComponentModel;
using System.IO;
using System.Runtime.CompilerServices;
using System.Security.Cryptography;
using System.Text.Json.Nodes;
using System.Windows.Threading;

namespace LegoBatmanOnline.Core;

public abstract class Bindable : INotifyPropertyChanged
{
    public event PropertyChangedEventHandler? PropertyChanged;
    protected void Raise([CallerMemberName] string? n = null) => PropertyChanged?.Invoke(this, new(n));
}

public sealed class FriendItem : Bindable
{
    private string _presence = "off";
    private string _name = "";
    private FriendState _state;
    public string Id { get; init; } = "";
    public string Code => Identity.FormatCode(Id);
    public string Name { get => _name; set { _name = value; Raise(); Raise(nameof(Initial)); } }
    public string Initial => string.IsNullOrWhiteSpace(Name) ? "?" : Name.Trim()[..1].ToUpperInvariant();
    /// <summary>"on", "busy" (spielt gerade) oder "off"</summary>
    public string Presence
    {
        get => _presence;
        set { _presence = value; Raise(); Raise(nameof(Online)); Raise(nameof(Busy)); Raise(nameof(StatusText)); }
    }
    public bool Online => Presence != "off";
    public bool Busy => Presence == "busy";
    public FriendState State
    {
        get => _state;
        set { _state = value; Raise(); Raise(nameof(IsIncoming)); Raise(nameof(IsFriend)); Raise(nameof(StatusText)); }
    }
    public bool IsIncoming => State == FriendState.RequestReceived;
    public bool IsFriend => State == FriendState.Friend;
    public string StatusText => State switch
    {
        FriendState.RequestSent => "Anfrage gesendet",
        FriendState.RequestReceived => "möchte dein Freund sein",
        _ => Presence switch { "busy" => "spielt gerade", "on" => "online", _ => "offline" },
    };
}

public sealed class WorldItem : Bindable
{
    private FriendItem? _partner;
    private string _syncText = "";
    public WorldInfo Info { get; }
    public WorldItem(WorldInfo info) => Info = info;

    public FriendItem? Partner
    {
        get => _partner;
        set
        {
            if (_partner != null) _partner.PropertyChanged -= OnPartner;
            _partner = value;
            if (_partner != null) _partner.PropertyChanged += OnPartner;
            Refresh();
        }
    }
    private void OnPartner(object? s, PropertyChangedEventArgs e) => Refresh();

    public string Name => Info.Name;
    public string PartnerText => $"mit {Partner?.Name ?? "?"}";
    public bool PartnerOnline => Partner?.Online == true;
    public bool CanPlayTogether => Partner is { Presence: "on", IsFriend: true };
    public string PartnerStatus => Partner?.Presence switch { "busy" => "spielt gerade", "on" => "online", _ => "offline" };
    public bool HasSave => Info.VersionTs > 0;
    public string VersionText => HasSave
        ? $"Stand vom {DateTimeOffset.FromUnixTimeMilliseconds(Info.VersionTs).LocalDateTime:dd.MM.yyyy, HH:mm} · zuletzt {Info.VersionBy}"
        : "Noch kein Spielstand – startet im Spiel ein neues Spiel und speichert";
    public string SyncText { get => _syncText; set { _syncText = value; Raise(); } }

    public void Refresh()
    {
        foreach (var n in new[] { nameof(Name), nameof(PartnerText), nameof(PartnerOnline), nameof(CanPlayTogether),
                     nameof(PartnerStatus), nameof(HasSave), nameof(VersionText) })
            Raise(n);
    }
}

public sealed class Invitation
{
    public string Sid { get; init; } = "";
    public FriendItem From { get; init; } = null!;
    public string? WorldName { get; init; }
    /// <summary>"b2" = LEGO Batman 2, sonst LEGO Batman</summary>
    public string? Game { get; init; }
    public string Text => Game == "b2"
        ? $"{From.Name} lädt dich zu LEGO Batman 2 ein"
        : WorldName != null
        ? $"{From.Name} möchte mit dir in „{WorldName}“ spielen"
        : $"{From.Name} lädt dich ein";
}

/// <summary>Startpunkt einer neuen Welt: neues Spiel oder ein vorhandener eigener Spielstand.</summary>
public sealed record WorldSeed(int Slot, string Title, string Sub);

/// <summary>Ablauf der App: Freunde, gemeinsame Welten, Einladungen, Verbindungsaufbau und Spielstart.</summary>
public sealed class Lobby : Bindable
{
    private readonly Profile _profile;
    private readonly Identity _me;
    private readonly Messenger _msg;
    private readonly Dispatcher _ui;
    private readonly AppOptions _opt;
    private string? _launcher;
    private CancellationTokenSource? _sessionCts;

    // laufende Sitzung
    private string? _sid;
    private FriendItem? _peer;
    private Endpoint? _myEp;
    private int _sessionSlot = -1;
    private bool _isHost;
    private bool _solo;
    private WorldItem? _world;
    private bool _batman2;   // Sitzung mit LEGO Batman 2 (ohne Welt und Spielstand)

    // Abgleich der Welten
    private readonly Dictionary<string, WorldVersion> _pending = new();   // wartet aufs Spielende
    private readonly Dictionary<string, long> _remoteSeen = new();         // neuester Stand beim Vermittler
    private bool _autoWorldDone;

    public ObservableCollection<FriendItem> Friends { get; } = new();
    public ObservableCollection<WorldItem> Worlds { get; } = new();
    public ObservableCollection<SaveSlot> Saves { get; } = new();
    public ObservableCollection<string> Log { get; } = new();

    // Launcher: Spielekatalog und gewaehltes Spiel
    public ObservableCollection<GameItem> Games { get; } = GameCatalog.Create();
    private GameItem? _selectedGame;
    public GameItem? SelectedGame
    {
        get => _selectedGame;
        set
        {
            if (_selectedGame == value) return;
            if (_selectedGame != null) _selectedGame.Selected = false;
            _selectedGame = value;
            if (value != null) value.Selected = true;
            Raise(); Raise(nameof(IsBatman1)); Raise(nameof(IsBatman2)); Raise(nameof(ShowGameMissing));
        }
    }
    public bool IsBatman1 => SelectedGame?.Key == "batman1";
    public bool IsBatman2 => SelectedGame?.Key == "batman2";
    public bool ShowGameMissing => IsBatman1 && GameMissing;
    public void RememberGame(string key)
    {
        if (_profile.LastGame == key) return;
        _profile.LastGame = key;
        try { _profile.Save(); } catch { }
    }

    public string MyCode => Identity.FormatCode(_me.Id);
    public string Name
    {
        get => _profile.Name;
        set { _profile.Name = Messenger.Clean(value); _profile.Save(); Raise(); _ = _msg.PublishProfileAsync(); }
    }
    public bool NeedsName => string.IsNullOrWhiteSpace(_profile.Name);

    private bool _brokerOnline;
    public bool BrokerOnline { get => _brokerOnline; private set { _brokerOnline = value; Raise(); Raise(nameof(BrokerText)); } }
    public string BrokerText => BrokerOnline ? "Vermittler verbunden" : "Verbinde mit Vermittler …";

    public string? GamePath => _profile.GamePath;
    public string GameText => GamePath != null ? "LEGO Batman gefunden" : "LEGO Batman nicht gefunden";
    public bool GameOk => GamePath != null;
    public bool GameMissing => GamePath == null;

    private bool _autoLoad = true;
    public bool AutoLoad { get => _autoLoad; set { _autoLoad = value; Raise(); } }
    private Invitation? _incoming;
    public Invitation? Incoming { get => _incoming; private set { _incoming = value; Raise(); Raise(nameof(HasIncoming)); } }
    public bool HasIncoming => Incoming != null;

    private string _sessionText = "";
    public string SessionText { get => _sessionText; private set { _sessionText = value; Raise(); } }
    private bool _sessionActive;
    public bool SessionActive
    {
        get => _sessionActive;
        private set
        {
            if (_sessionActive == value) return;
            _sessionActive = value;
            Raise(); Raise(nameof(Idle));
            _ = _msg.SetBusyAsync(value);  // Freunde sehen "spielt gerade"
        }
    }
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
        _msg.ConnectionChanged += on => _ui.BeginInvoke(() => OnConnection(on));
        _msg.PresenceChanged += (id, state) => _ui.BeginInvoke(() =>
        {
            var f = Friends.FirstOrDefault(x => x.Id == id);
            if (f == null) return;
            f.Presence = state;
            TryAutoPlay();
        });
        _msg.MessageReceived += (from, pub, c) => _ui.BeginInvoke(() => OnMessage(from, pub, c));
        _msg.WorldReceived += (topic, data) => _ui.BeginInvoke(() => OnWorldData(topic, data));
        foreach (var f in profile.Friends)
            Friends.Add(new FriendItem { Id = f.Id, Name = f.Name, State = f.State });
        foreach (var w in profile.Worlds)
            Worlds.Add(new WorldItem(w) { Partner = Friends.FirstOrDefault(f => f.Id == w.Partner) });
    }

    public async Task StartAsync()
    {
        _profile.GamePath ??= GameBridge.FindGame();
        _profile.Save();
        Raise(nameof(GamePath)); Raise(nameof(GameText)); Raise(nameof(GameOk)); Raise(nameof(GameMissing)); Raise(nameof(ShowGameMissing));
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
        // Wurde seit dem letzten Mal ausserhalb der App gespielt (oder ist sie abgestuerzt)?
        foreach (var w in Worlds) TakeLocalChanges(w);
        await _msg.StartAsync();
        foreach (var f in Friends) await _msg.WatchAsync(f.Id);
        foreach (var w in Worlds) await _msg.WatchWorldAsync(WorldStore.Topic(w.Info));
        if (_opt.AutoInvite != null) _ = AutoInviteAsync(_opt.AutoInvite);
    }

    public void RefreshSaves()
    {
        Saves.Clear();
        foreach (var s in GameBridge.ListSaves()) Saves.Add(s);
    }

    public IEnumerable<WorldSeed> Seeds()
    {
        yield return new WorldSeed(-1, "Neues Spiel", "Ihr fangt gemeinsam von vorne an");
        foreach (var s in GameBridge.ListSaves())
            yield return new WorldSeed(s.Index, $"Meinen Spielstand „{s.Title}“ übernehmen", s.SavedText);
    }

    public void SetGamePath(string path)
    {
        _profile.GamePath = path;
        _profile.Save();
        Raise(nameof(GamePath)); Raise(nameof(GameText)); Raise(nameof(GameOk)); Raise(nameof(GameMissing)); Raise(nameof(ShowGameMissing));
    }

    private void AddLog(string line)
    {
        Log.Add($"{DateTime.Now:HH:mm:ss}  {line}");
        while (Log.Count > 200) Log.RemoveAt(0);
    }

    private void OnConnection(bool on)
    {
        BrokerOnline = on;
        if (on) _ = SyncAllAsync();
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
        RefreshWorlds();
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
        RefreshWorlds();
        return item;
    }

    private void RefreshWorlds()
    {
        foreach (var w in Worlds)
        {
            var p = Friends.FirstOrDefault(f => f.Id == w.Info.Partner);
            if (w.Partner != p) w.Partner = p; else w.Refresh();
        }
    }

    private byte[]? PubOf(string id) => _profile.Find(id) is { } f ? Convert.FromBase64String(f.PublicKey) : null;

    // --- Welten -----------------------------------------------------------------------
    private string SavesDir(WorldItem w) => WorldStore.SavesDir(_opt.Profile, w.Info.Id);

    public async Task<string> CreateWorldAsync(string name, FriendItem partner, WorldSeed seed)
    {
        name = Messenger.Clean(name);
        if (name.Length == 0) return "Bitte gib der Welt einen Namen.";
        if (!partner.IsFriend) return "Ihr müsst erst befreundet sein.";
        var info = WorldStore.Create(name, partner.Id);
        var w = new WorldItem(info) { Partner = partner };
        var dir = SavesDir(w);
        Directory.CreateDirectory(dir);
        if (seed.Slot >= 0)
        {
            var src = Path.Combine(GameBridge.SaveDir, $"SaveGame{seed.Slot}.LEGO Batman_SavedGame");
            var dst = Path.Combine(dir, Path.GetFileName(src));
            File.Copy(src, dst, true);
            File.SetLastWriteTimeUtc(dst, File.GetLastWriteTimeUtc(src));
        }
        _profile.Worlds.Add(info);
        Worlds.Add(w);
        TakeLocalChanges(w);
        _profile.Save();
        await _msg.WatchWorldAsync(WorldStore.Topic(info));
        await PublishWorldAsync(w);
        await ShareWorldAsync(w);
        AddLog($"Welt „{name}“ mit {partner.Name} erstellt");
        return "";
    }

    private async Task ShareWorldAsync(WorldItem w)
    {
        if (PubOf(w.Info.Partner) is not { } pub) return;
        await _msg.SendAsync(w.Info.Partner, pub, new JsonObject
        {
            ["t"] = "world", ["id"] = w.Info.Id, ["wname"] = w.Info.Name, ["key"] = w.Info.Key,
        });
    }

    public async Task RemoveWorldAsync(WorldItem w)
    {
        if (_world == w && SessionActive) return;
        Worlds.Remove(w);
        _profile.Worlds.Remove(w.Info);
        _profile.Save();
        await _msg.UnwatchWorldAsync(WorldStore.Topic(w.Info));
        AddLog($"Welt „{w.Name}“ entfernt (Dateien bleiben unter {WorldStore.Dir(_opt.Profile, w.Info.Id)})");
    }

    public string BackupFolder(WorldItem w)
    {
        var d = WorldStore.BackupDir(_opt.Profile, w.Info.Id);
        Directory.CreateDirectory(d);
        return d;
    }

    /// <summary>Hat sich der Spielstand im Weltordner geaendert, wird er zum neuen Stand der Welt.</summary>
    private bool TakeLocalChanges(WorldItem w)
    {
        var dir = SavesDir(w);
        var sha = WorldStore.Hash(dir);
        if (sha == "" || sha == w.Info.VersionSha) return false;
        var written = new DateTimeOffset(DateTime.SpecifyKind(WorldStore.LastWrite(dir), DateTimeKind.Utc)).ToUnixTimeMilliseconds();
        w.Info.VersionTs = Math.Max(written, w.Info.VersionTs + 1);
        w.Info.VersionSha = sha;
        w.Info.VersionBy = string.IsNullOrWhiteSpace(_profile.Name) ? "Spieler" : _profile.Name;
        _profile.Save();
        w.Refresh();
        return true;
    }

    private async Task PublishWorldAsync(WorldItem w)
    {
        if (w.Info.VersionTs == 0 || WorldStore.SaveFiles(SavesDir(w)).Length == 0) return;
        w.SyncText = "wird hochgeladen …";
        var ok = await _msg.PublishWorldAsync(WorldStore.Topic(w.Info), WorldStore.Pack(w.Info, SavesDir(w)));
        if (ok) _remoteSeen[w.Info.Id] = Math.Max(_remoteSeen.GetValueOrDefault(w.Info.Id), w.Info.VersionTs);
        w.SyncText = ok ? "abgeglichen" : "noch nicht hochgeladen (keine Verbindung)";
    }

    /// <summary>Nach dem Verbinden: kurz auf die hinterlegten Staende warten, dann eigene neuere hochladen.</summary>
    private async Task SyncAllAsync()
    {
        await Task.Delay(TimeSpan.FromSeconds(6));
        foreach (var w in Worlds.ToArray())
            if (!InUse(w) && _remoteSeen.GetValueOrDefault(w.Info.Id) <= w.Info.VersionTs) await PublishWorldAsync(w);
    }

    private bool InUse(WorldItem w) => SessionActive && _world == w && (_solo || _isHost);

    private void OnWorldData(string topic, byte[] data)
    {
        var w = Worlds.FirstOrDefault(x => WorldStore.Topic(x.Info) == topic);
        if (w == null) return;
        var v = WorldStore.Unpack(w.Info, data);
        if (v == null) return;
        _remoteSeen[w.Info.Id] = Math.Max(_remoteSeen.GetValueOrDefault(w.Info.Id), v.Ts);
        if (v.Name.Length > 0 && v.Name != w.Info.Name && v.Ts >= w.Info.VersionTs) { w.Info.Name = v.Name; _profile.Save(); w.Refresh(); }
        if (v.Sha == w.Info.VersionSha)
        {
            if (v.Ts > w.Info.VersionTs) { w.Info.VersionTs = v.Ts; w.Info.VersionBy = v.By; _profile.Save(); w.Refresh(); }
            w.SyncText = "abgeglichen";
            return;
        }
        // Neuester gewinnt; bei gleicher Zeit entscheidet die Pruefsumme, damit beide dasselbe waehlen
        bool remoteNewer = v.Ts > w.Info.VersionTs ||
                           (v.Ts == w.Info.VersionTs && string.CompareOrdinal(v.Sha, w.Info.VersionSha) > 0);
        if (!remoteNewer)
        {
            if (!InUse(w)) _ = PublishWorldAsync(w);  // unserer ist neuer: beim Vermittler ersetzen
            return;
        }
        if (InUse(w))
        {
            _pending[w.Info.Id] = v;
            w.SyncText = $"neuer Stand von {v.By} wird nach dem Spiel übernommen";
            return;
        }
        Apply(w, v);
    }

    private void Apply(WorldItem w, WorldVersion v)
    {
        bool hadLocal = w.Info.VersionTs > 0 && WorldStore.SaveFiles(SavesDir(w)).Length > 0;
        if (hadLocal) WorldStore.Backup(_opt.Profile, w.Info.Id, "ersetzt");
        WorldStore.Replace(_opt.Profile, w.Info.Id, v);
        w.Info.VersionTs = v.Ts;
        w.Info.VersionSha = WorldStore.Hash(SavesDir(w));
        w.Info.VersionBy = v.By;
        _profile.Save();
        w.Refresh();
        w.SyncText = "abgeglichen";
        AddLog(hadLocal
            ? $"„{w.Name}“: neuerer Stand von {v.By} übernommen, dein alter Stand ist gesichert"
            : $"„{w.Name}“: Stand von {v.By} übernommen");
    }

    private async Task AfterGameAsync(WorldItem w)
    {
        if (TakeLocalChanges(w))
        {
            AddLog($"„{w.Name}“: neuer Spielstand gespeichert");
            // Wurde waehrenddessen beim Partner auch gespielt, gewinnt trotzdem der neueste
            if (_pending.Remove(w.Info.Id, out var p) && p.Ts > w.Info.VersionTs) { Apply(w, p); return; }
            await PublishWorldAsync(w);
        }
        else if (_pending.Remove(w.Info.Id, out var p))
        {
            if (p.Ts > w.Info.VersionTs) Apply(w, p);
        }
    }

    // --- Spielen ----------------------------------------------------------------------
    public async Task PlayTogetherAsync(WorldItem w)
    {
        if (SessionActive || !CheckReady()) return;
        if (w.Partner is not { IsFriend: true } f || PubOf(f.Id) is not { } pub) return;
        if (!w.CanPlayTogether)
        {
            Hint = w.Partner.Busy ? $"{f.Name} spielt gerade. Sobald er fertig ist, könnt ihr zusammen spielen."
                                  : $"{f.Name} ist offline. Du kannst alleine weiterspielen.";
            return;
        }
        _sid = RandomNumberGenerator.GetHexString(8);
        _peer = f;
        _isHost = true;
        _solo = false;
        _batman2 = false;
        _world = w;
        TakeLocalChanges(w);
        _sessionSlot = AutoLoad ? WorldStore.NewestSlot(SavesDir(w)) : -1;
        SessionActive = true;
        SessionText = $"Warte auf {f.Name} …";
        AddLog($"Einladung an {f.Name} für „{w.Name}“ gesendet");
        await _msg.SendAsync(f.Id, pub, new JsonObject
        {
            ["t"] = "invite", ["sid"] = _sid, ["slot"] = _sessionSlot, ["world"] = w.Info.Id, ["wname"] = w.Info.Name,
        });
        _ = TimeoutAsync(_sid, 120, "Keine Antwort auf die Einladung.");
    }

    /// <summary>
    /// LEGO Batman 2 zusammen spielen. Noch ohne Welten und Spielstaende: Beide starten ohne Spielstand,
    /// der Host waehlt "Neues Spiel", der Gast steigt im Level mit Start ein.
    /// </summary>
    public async Task PlayBatman2Async(FriendItem f)
    {
        if (SessionActive || _launcher == null) return;
        if (!f.IsFriend || PubOf(f.Id) is not { } pub) return;
        if (f.Presence != "on")
        {
            Hint = f.Busy ? $"{f.Name} spielt gerade." : $"{f.Name} ist offline.";
            return;
        }
        _sid = RandomNumberGenerator.GetHexString(8);
        _peer = f;
        _isHost = true;
        _solo = false;
        _batman2 = true;
        _world = null;
        _sessionSlot = -1;
        SessionActive = true;
        var sid = _sid;
        if (!await GameBridge.EnsureSteamAsync(t => { SessionText = t; AddLog(t); }))
        {
            if (_sid == sid) EndSession("LEGO Batman 2 braucht ein laufendes, angemeldetes Steam.");
            return;
        }
        if (_sid != sid || !SessionActive) return;
        SessionText = $"Warte auf {f.Name} …";
        AddLog($"Einladung zu LEGO Batman 2 an {f.Name} gesendet");
        await _msg.SendAsync(f.Id, pub, new JsonObject { ["t"] = "invite", ["sid"] = _sid, ["slot"] = -1, ["game"] = "b2" });
        _ = TimeoutAsync(_sid, 120, "Keine Antwort auf die Einladung.");
    }

    public async Task PlaySoloAsync(WorldItem w)
    {
        if (SessionActive || !CheckReady()) return;
        _sid = RandomNumberGenerator.GetHexString(8);
        _peer = null;
        _isHost = false;
        _solo = true;
        _world = w;
        TakeLocalChanges(w);
        SessionActive = true;
        SessionText = $"Du spielst alleine in „{w.Name}“. Lade im Spiel unter „Load Game“ euren Spielstand.";
        _sessionCts = new CancellationTokenSource();
        var sid = _sid;
        var env = TestEnv();
        await GameBridge.PlaySoloAsync(_launcher!, SavesDir(w), line => _ui.BeginInvoke(() => OnLauncherLine(line)),
            env, _sessionCts.Token);
        if (_sid == sid) EndSession("Spiel beendet.");
    }

    public async Task AcceptInviteAsync()
    {
        var inv = Incoming;
        if (inv == null || (inv.Game != "b2" && !CheckReady()) || _launcher == null) return;
        Incoming = null;
        _sid = inv.Sid;
        _peer = inv.From;
        _isHost = false;
        _solo = false;
        _batman2 = inv.Game == "b2";
        _world = null;  // der Gast spielt mit der Kopie des Hosts, sein Weltordner bleibt unberuehrt
        SessionActive = true;
        if (_batman2)
        {
            var sid = _sid;
            if (!await GameBridge.EnsureSteamAsync(t => { SessionText = t; AddLog(t); }))
            {
                if (_sid == sid) EndSession("LEGO Batman 2 braucht ein laufendes, angemeldetes Steam.");
                return;
            }
            if (_sid != sid || !SessionActive) return;
        }
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
        Hint = "";
        return true;
    }

    private async Task TimeoutAsync(string sid, int seconds, string text)
    {
        await Task.Delay(TimeSpan.FromSeconds(seconds));
        if (_sid == sid && SessionActive && _sessionCts == null) EndSession(text);
    }

    private void EndSession(string text)
    {
        var w = _world;
        bool wrote = w != null && (_solo || _isHost);
        AddLog(text);
        Hint = text;
        _sid = null;
        _peer = null;
        _myEp = null;
        _sessionCts = null;
        _world = null;
        _solo = false;
        SessionActive = false;
        SessionText = "";
        if (wrote) _ = AfterGameAsync(w!);
        TryAutoPlay();
    }

    private Dictionary<string, string> TestEnv()
    {
        var env = new Dictionary<string, string>();
        if (_opt.Test)
        {
            env["LBO_MULTI"] = "1";
            env["LBO_WINDOWED"] = "1";
            env["LBO_ALLOWSAVE"] = "0";
            if (_opt.MaxFrames > 0) env["LBO_MAXFRAMES"] = _opt.MaxFrames.ToString();
        }
        return env;
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
                await TryAutoWorldAsync();
                break;
            case "world":
                if (known?.State != FriendState.Friend) break;  // nur Freunde koennen Welten teilen
                await OnWorldShared(from, name, c);
                break;
            case "invite":
                if (known?.State != FriendState.Friend) break;  // nur Freunde duerfen einladen
                if (SessionActive)
                {
                    await _msg.SendAsync(from, pub, new JsonObject { ["t"] = "invite_no", ["sid"] = (string?)c["sid"], ["busy"] = true });
                    break;
                }
                var f = Friends.First(x => x.Id == from);
                Incoming = new Invitation
                {
                    Sid = (string)c["sid"]!, From = f, Game = (string?)c["game"] == "b2" ? "b2" : null,
                    WorldName = Messenger.Clean((string?)c["wname"]) is { Length: > 0 } wn ? wn : null,
                };
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
            case "diag":
                if (known?.State == FriendState.Friend) OnDiag(name, c);
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

    private async Task OnWorldShared(string from, string fromName, JsonObject c)
    {
        var id = (string?)c["id"];
        var key = (string?)c["key"];
        var wname = Messenger.Clean((string?)c["wname"]);
        if (id == null || id.Length < 16 || id.Length > 64 || !id.All(Uri.IsHexDigit) || key == null) return;
        try { if (Convert.FromBase64String(key).Length != 32) return; } catch { return; }
        if (Worlds.Any(x => x.Info.Id == id)) return;
        var info = new WorldInfo { Id = id, Name = wname.Length > 0 ? wname : "Gemeinsame Welt", Key = key, Partner = from };
        var w = new WorldItem(info) { Partner = Friends.FirstOrDefault(f => f.Id == from) };
        Directory.CreateDirectory(SavesDir(w));
        _profile.Worlds.Add(info);
        _profile.Save();
        Worlds.Add(w);
        w.SyncText = "lade Spielstand …";
        AddLog($"{fromName} hat die Welt „{info.Name}“ mit dir geteilt");
        await _msg.WatchWorldAsync(WorldStore.Topic(info));
    }

    private async Task PlayAsync(Endpoint peerEp)
    {
        _sessionCts = new CancellationTokenSource();
        var sid = _sid;
        SessionText = "Verbinde …";
        string? saveDir = _isHost && _world != null && !_batman2 ? SavesDir(_world) : null;
        var peer = _peer;
        _logDir = null;
        _desyncShown = false;
        _ = WatchDesyncAsync(sid);
        var code = await GameBridge.ConnectAndPlayAsync(_launcher!, _isHost, _myEp!, peerEp, _sessionSlot, saveDir, _batman2,
            line => _ui.BeginInvoke(() => OnLauncherLine(line)), TestEnv(), _sessionCts.Token);
        var logDir = _logDir;
        bool b2 = _batman2;
        if (_sid == sid)
            EndSession(_desyncShown ? "Das Spiel war nicht mehr synchron. Die Fehlerdaten gehen an deinen Mitspieler."
                     : code == 0 ? "Spiel beendet." : "Verbindung fehlgeschlagen.");
        if (peer != null) await SendDiagAsync(peer, logDir, b2, code);
    }

    // --- Desync: Hinweis und Fehlerdaten -------------------------------------------------
    private string? _logDir;
    private bool _desyncShown;

    /// <summary>Waehrend des Spiels das Log der Mod beobachten und einen Desync sofort anzeigen.</summary>
    private async Task WatchDesyncAsync(string? sid)
    {
        while (_sid == sid && SessionActive)
        {
            await Task.Delay(2000);
            if (_logDir == null || _desyncShown) continue;
            try
            {
                var log = ReadShared(Path.Combine(_logDir, "lbo.log"));
                bool cfgDiff = log.Contains("EINSTELLUNGEN VERSCHIEDEN");
                if (!log.Contains("NICHT SYNCHRON") && !cfgDiff) continue;
                _desyncShown = true;
                if (cfgDiff)
                {
                    SessionText = "⚠ Eure Einstellungen sind verschieden. Beendet das Spiel und ladet neu ein.";
                    AddLog("Einstellungen verschieden – die Spiele würden auseinanderlaufen");
                }
                else
                {
                    SessionText = "⚠ Eure Spiele sind nicht mehr synchron. Speichert (Einladender) und beendet das Spiel, dann neu starten.";
                    AddLog("Desync erkannt – die Spiele laufen auseinander");
                }
                AlertRequested?.Invoke();
            }
            catch { }
        }
    }

    /// <summary>Wird ausgeloest, wenn die App den Spieler deutlich warnen soll (Fenster nach vorn).</summary>
    public event Action? AlertRequested;

    private static string ReadShared(string path)
    {
        using var fs = new FileStream(path, FileMode.Open, FileAccess.Read, FileShare.ReadWrite | FileShare.Delete);
        using var r = new StreamReader(fs, System.Text.Encoding.Latin1);
        return r.ReadToEnd();
    }

    /// <summary>
    /// Fehlerdaten an den Partner: nach einem Desync Log-Auszug und frames.csv rund um die erste Abweichung. Bei
    /// LEGO Batman 2 auch, wenn die Sitzung schiefging (nie im Level, Partner verloren, Start gescheitert): Anfang und
    /// Ende des Logs, die letzten Frames und das App-Protokoll. Im Playtest 20.10. abends fehlten genau diese Daten.
    /// </summary>
    private async Task SendDiagAsync(FriendItem peer, string? logDir, bool b2, int code)
    {
        try
        {
            if (PubOf(peer.Id) is not { } pub) return;
            string log = "";
            try { if (logDir != null) log = ReadShared(Path.Combine(logDir, "lbo.log")); } catch { }
            // Neuere DLL: "DESYNC bei Frame <Lockstep> (Spiel-Frame <F>)"; frames.csv zaehlt Spiel-Frames
            var m = System.Text.RegularExpressions.Regex.Match(log, @"DESYNC bei Frame (\d+)(?: \(Spiel-Frame (\d+)\))?");
            bool trouble = b2 && (code != 0 || log.Length == 0 || !log.Contains("Levelanker") || log.Contains("Partner verloren")
                                  || log.Contains("startet nicht") || log.Contains("kam nicht an der Schranke")
                                  || log.Contains("EINSTELLUNGEN VERSCHIEDEN") || log.Contains("NETZ-EINSTELLUNGEN"));
            if (!m.Success && !trouble) return;
            int f0 = m.Success ? int.Parse(m.Groups[2].Success ? m.Groups[2].Value : m.Groups[1].Value) : -1;
            string[] csvLines = Array.Empty<string>();
            try { if (logDir != null) csvLines = ReadShared(Path.Combine(logDir, "frames.csv")).Split('\n'); } catch { }
            var rows = new List<string>();
            if (csvLines.Length > 0)
            {
                rows.Add(csvLines[0].TrimEnd('\r'));
                if (m.Success)
                {
                    foreach (var l in csvLines.Skip(1))
                    {
                        var comma = l.IndexOf(',');
                        if (comma > 0 && int.TryParse(l[..comma], out var f) && f >= f0 - 600 && f <= f0 + 120) rows.Add(l.TrimEnd('\r'));
                    }
                }
                else rows.AddRange(csvLines.Skip(1).TakeLast(300).Select(l => l.TrimEnd('\r')));
            }
            var logLines = log.Split('\n').Select(l => l.TrimEnd('\r')).ToList();
            IEnumerable<string> excerpt;
            if (m.Success)
            {
                // Die 400 Zeilen vor der ersten Abweichung und die ersten Abweichungen selbst
                int first = logLines.FindIndex(l => l.Contains("DESYNC"));
                excerpt = logLines.Take(first < 0 ? logLines.Count : first).TakeLast(400)
                    .Concat(logLines.Where(l => l.Contains("DESYNC")).Take(5));
            }
            else excerpt = logLines.Count <= 400 ? logLines : logLines.Take(120).Append("[...]").Concat(logLines.TakeLast(280));
            string cfg = "", cpu = "";
            // Batman 2 hat seine eigene pcconfig.txt unter %APPDATA% (bisher kam hier versehentlich die von Batman 1 mit)
            var cfgPath = b2
                ? Path.Combine(Environment.GetFolderPath(Environment.SpecialFolder.ApplicationData),
                    "Warner Bros. Interactive Entertainment", "LEGO Batman 2", "pcconfig.txt")
                : Path.Combine(Path.GetDirectoryName(GameBridge.SaveDir)!, "pcconfig.txt");
            try { cfg = File.ReadAllText(cfgPath); } catch { }
            // Online liest das Spiel die gemeinsame Datei aus dem Log-Ordner (Grafik des Hosts, eigene Aufloesung)
            try { if (logDir != null) cfg += "\n--- netz_pcconfig.txt (im Spiel benutzt) ---\n" + ReadShared(Path.Combine(logDir, "netz_pcconfig.txt")); } catch { }
            // Die Bytes, auf die sich die Launcher geeinigt haben (Host-Datei, Kopie oder Standard; SHA-256 an der Schranke)
            string basis = "";
            try { if (logDir != null) basis = ReadShared(Path.Combine(logDir, "netz_basis.txt")); } catch { }
            try
            {
                cpu = Microsoft.Win32.Registry.GetValue(@"HKEY_LOCAL_MACHINE\HARDWARE\DESCRIPTION\System\CentralProcessor\0",
                    "ProcessorNameString", "") as string ?? "";
            }
            catch { }
            var doc = new JsonObject
            {
                ["frame"] = f0, ["dir"] = logDir != null ? Path.GetFileName(logDir) : "",
                ["log"] = string.Join("\n", excerpt), ["csv"] = string.Join("\n", rows),
                ["app"] = string.Join("\n", Log.TakeLast(80)), ["code"] = code,
                ["cfg"] = cfg, ["basis"] = basis,
                ["sys"] = $"{cpu.Trim()} | {Environment.ProcessorCount} Threads | {Environment.OSVersion}",
            };
            using var ms = new MemoryStream();
            using (var gz = new System.IO.Compression.GZipStream(ms, System.IO.Compression.CompressionLevel.SmallestSize))
                gz.Write(System.Text.Encoding.UTF8.GetBytes(doc.ToJsonString()));
            await _msg.SendAsync(peer.Id, pub, new JsonObject { ["t"] = "diag", ["d"] = Convert.ToBase64String(ms.ToArray()) });
            AddLog(m.Success ? $"Fehlerdaten (Desync ab Frame {f0}) an {peer.Name} geschickt"
                             : $"Sitzungsdaten (Abbruch) an {peer.Name} geschickt");
        }
        catch (Exception e) { AddLog("Fehlerdaten konnten nicht gesendet werden: " + e.Message); }
    }

    // Fehlerdaten des Partners: hoechstens 1 MB Base64 und 16 MB entpackt (sonst legt eine kleine Gzip-Bombe die App lahm)
    private const int DiagMaxBase64 = 1 << 20, DiagMaxUnpacked = 16 << 20;

    private void OnDiag(string fromName, JsonObject c)
    {
        try
        {
            var b64 = (string?)c["d"];
            if (b64 == null || b64.Length > DiagMaxBase64) { AddLog($"Fehlerdaten von {fromName} zu groß, verworfen"); return; }
            using var gz = new System.IO.Compression.GZipStream(new MemoryStream(Convert.FromBase64String(b64)),
                System.IO.Compression.CompressionMode.Decompress);
            using var outMs = new MemoryStream();
            var chunk = new byte[81920];
            int n;
            while ((n = gz.Read(chunk, 0, chunk.Length)) > 0)
            {
                if (outMs.Length + n > DiagMaxUnpacked) { AddLog($"Fehlerdaten von {fromName} entpackt zu groß, verworfen"); return; }
                outMs.Write(chunk, 0, n);
            }
            var doc = JsonNode.Parse(outMs.ToArray())!.AsObject();
            // Eindeutiger Ordner: zwei Meldungen in derselben Sekunde ueberschrieben sich frueher
            var stamp = $"{DateTime.Now:yyyyMMdd-HHmmss}-partner";
            var dir = Path.Combine(Profile.BaseDir, "logs", stamp);
            for (int i = 2; Directory.Exists(dir); i++) dir = Path.Combine(Profile.BaseDir, "logs", $"{stamp}-{i}");
            Directory.CreateDirectory(dir);
            File.WriteAllText(Path.Combine(dir, "partner_lbo.log"), (string?)doc["log"] ?? "");
            File.WriteAllText(Path.Combine(dir, "partner_frames.csv"), (string?)doc["csv"] ?? "");
            File.WriteAllText(Path.Combine(dir, "partner_pcconfig.txt"), (string?)doc["cfg"] ?? "");
            File.WriteAllText(Path.Combine(dir, "partner_netz_basis.txt"), (string?)doc["basis"] ?? "");
            File.WriteAllText(Path.Combine(dir, "partner_app.txt"), (string?)doc["app"] ?? "");
            int frame = (int?)doc["frame"] ?? -1;
            File.WriteAllText(Path.Combine(dir, "info.txt"),
                $"Von: {fromName}\n" + (frame >= 0 ? $"Erster Desync bei Frame: {frame}\n" : "Grund: Sitzung abgebrochen oder nicht gestartet\n") +
                $"Rueckgabe des Launchers: {(int?)doc["code"]}\nLog-Ordner beim Partner: {(string?)doc["dir"]}\n" +
                $"System des Partners: {(string?)doc["sys"]}\n");
            AddLog($"Fehlerdaten von {fromName} gespeichert: {dir}");
        }
        catch { }
    }

    private void OnLauncherLine(string line)
    {
        var logs = line.IndexOf("(Logs: ", StringComparison.Ordinal);
        if (logs >= 0 && line.EndsWith(")")) _logDir = line[(logs + 7)..^1].Trim();
        if (line.StartsWith("STATUS "))
        {
            if (_solo) return;  // Text fuer Solo steht schon
            SessionText = line[7..] switch
            {
                "verbinde" => "Verbinde …",
                "verbunden" => "Verbunden",
                "ping" => "Messe Ping …",
                "spielstand" => _isHost ? "Sende Spielstand …" : "Empfange Spielstand …",
                "spiel" => _isHost
                    ? (_sessionSlot >= 0 ? "Spiel läuft – euer Spielstand wird automatisch geladen" : "Spiel läuft – du bedienst die Menüs")
                    : "Spiel läuft – drücke START, sobald ihr im Spiel seid",
                "beendet" => "Spiel beendet",
                "steam" => "Warte auf Steam … (LEGO Batman 2 braucht ein laufendes, angemeldetes Steam)",
                var s => s,
            };
        }
        else if (line.StartsWith("ERROR ")) AddLog(line[6..]);
        else if (line.StartsWith("Ping:") || line.StartsWith("Verbunden") || line.StartsWith("Steam") || line.StartsWith("Warte auf Steam")
                 // Uebertragung und Einstellungen (Desync 23.10.2026: "Sende 0 Datei(en)" fehlte im Protokoll)
                 || line.StartsWith("Sende") || line.StartsWith("Empfange") || line.StartsWith("Einstellungen")
                 || line.StartsWith("Dateien") || line.StartsWith("Datei ") || line.StartsWith("Fehler") || line.StartsWith("  "))
            AddLog(line.Trim());
    }

    // --- Testhilfen -------------------------------------------------------------------
    private async Task AutoInviteAsync(string code)
    {
        // Freund hinzufuegen; danach erstellt TryAutoWorldAsync die Welt
        var id = Identity.ParseCode(code);
        if (id == null) return;
        while (!_msg.IsConnected) await Task.Delay(500);
        if (!Friends.Any(f => f.Id == id)) AddLog(await AddFriendAsync(code));
        await TryAutoWorldAsync();
    }

    private async Task TryAutoWorldAsync()
    {
        if (_opt.AutoWorld == null || _autoWorldDone || _opt.AutoInvite == null) return;
        var f = Friends.FirstOrDefault(x => x.Id == Identity.ParseCode(_opt.AutoInvite) && x.IsFriend);
        if (f == null) return;
        _autoWorldDone = true;
        var w = Worlds.FirstOrDefault(x => x.Name == _opt.AutoWorld);
        if (w == null)
        {
            var seed = Seeds().FirstOrDefault(s => s.Slot == _opt.Slot) ?? Seeds().First();
            await CreateWorldAsync(_opt.AutoWorld, f, seed);
        }
        TryAutoPlay();
    }

    private bool _autoPlayed;
    private void TryAutoPlay()
    {
        if (_opt.AutoB2 && !_autoPlayed && !SessionActive && _opt.AutoInvite != null)
        {
            // Test: LEGO Batman 2 mit dem Freund aus --auto-invite starten, sobald er online ist
            var f = Friends.FirstOrDefault(x => x.Id == Identity.ParseCode(_opt.AutoInvite) && x.IsFriend);
            if (f is { Presence: "on" }) { _autoPlayed = true; _ = PlayBatman2Async(f); }
            return;
        }
        if (!_opt.AutoPlay || _autoPlayed || SessionActive || _opt.AutoWorld == null) return;
        var w = Worlds.FirstOrDefault(x => x.Name == _opt.AutoWorld);
        if (w is not { CanPlayTogether: true }) return;
        _autoPlayed = true;
        _ = PlayTogetherAsync(w);
    }

    public async Task ShutdownAsync()
    {
        _sessionCts?.Cancel();
        await _msg.DisposeAsync();
    }
}

public sealed class AppOptions
{
    public string Profile { get; set; } = "standard";
    public bool Test { get; set; }
    public bool AutoAccept { get; set; }
    public string? AutoInvite { get; set; }
    public string? AutoWorld { get; set; }
    public bool AutoPlay { get; set; }
    public bool AutoB2 { get; set; }
    public string? Name { get; set; }
    public int MaxFrames { get; set; }
    public int Slot { get; set; } = -1;
    public string? Shot { get; set; }       // Test: Fenster als PNG speichern und beenden
    public string? ShotGame { get; set; }   // Test: dieses Spiel vor dem Bild auswaehlen
    public string? ShotView { get; set; }   // Test: "freunde" oder "details" vor dem Bild oeffnen

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
                case "--auto-world" when i + 1 < args.Length: o.AutoWorld = args[++i]; break;
                case "--auto-play": o.AutoPlay = true; break;
                case "--auto-b2": o.AutoB2 = true; break;
                case "--name" when i + 1 < args.Length: o.Name = args[++i]; break;
                case "--slot" when i + 1 < args.Length: o.Slot = int.Parse(args[++i]); break;
                case "--max-frames" when i + 1 < args.Length: o.MaxFrames = int.Parse(args[++i]); break;
                case "--shot" when i + 1 < args.Length: o.Shot = args[++i]; break;
                case "--shot-game" when i + 1 < args.Length: o.ShotGame = args[++i]; break;
                case "--shot-view" when i + 1 < args.Length: o.ShotView = args[++i]; break;
            }
        }
        return o;
    }
}
