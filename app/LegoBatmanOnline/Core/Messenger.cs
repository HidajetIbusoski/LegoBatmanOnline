using System.Buffers;
using System.Security.Cryptography;
using System.Text;
using System.Text.Json.Nodes;
using MQTTnet;
using MQTTnet.Protocol;

namespace LegoBatmanOnline.Core;

/// <summary>
/// Vermittlung ueber oeffentliche MQTT-Dienste ("Briefkasten").
///
/// Themen (alle unter lbobatman/v1/):
///   profile/[id]   oeffentlicher Schluessel + Name (retained), damit andere uns per Code finden
///   presence/[id]  "on"/"busy"/"off" (retained, Last Will = "off"); busy = spielt gerade
///   inbox/[id]     Nachrichten an diesen Spieler, Ende-zu-Ende verschluesselt (ECDH + AES-GCM)
///   world/[thema]  Spielstand einer gemeinsamen Welt (retained, mit dem Weltschluessel verschluesselt;
///                  das Thema ist ein Hash des Schluessels und fuer Fremde nicht erratbar)
///
/// Die App ist mit allen Diensten gleichzeitig verbunden und sendet ueber jeden. So finden sich zwei
/// Spieler auch dann, wenn einer der Dienste bei einem von ihnen gerade nicht erreichbar ist.
/// Doppelt ankommende Nachrichten werden ueber die Nonce aussortiert.
///
/// Die Dienste sehen nur Codes, Namen und verschluesselte Daten. IP-Adressen, Einladungen und
/// Freundschaftsanfragen sind verschluesselt und an den Absender gebunden.
/// </summary>
public sealed class Messenger : IAsyncDisposable
{
    private const string Prefix = "lbobatman/v1/";
    private static readonly (string Host, int Port)[] Brokers =
        { ("broker.hivemq.com", 1883), ("broker.emqx.io", 1883), ("test.mosquitto.org", 1883) };

    private readonly Identity _me;
    private readonly Func<string> _name;
    private readonly MqttClientFactory _factory = new();
    private readonly IMqttClient?[] _clients = new IMqttClient?[Brokers.Length];
    private readonly HashSet<string> _watched = new();
    private readonly HashSet<string> _worlds = new();
    private readonly Dictionary<string, string[]> _presence = new();
    private string _status = "on";
    private readonly Dictionary<string, TaskCompletionSource<(byte[] Pub, string Name)?>> _pendingProfiles = new();
    private readonly HashSet<string> _seen = new();
    private readonly Queue<string> _seenOrder = new();
    private readonly CancellationTokenSource _cts = new();
    private bool _wasConnected;

    public event Action<bool>? ConnectionChanged;
    public event Action<string, string>? PresenceChanged;                // id, "on" | "busy" | "off"
    public event Action<string, byte[], JsonObject>? MessageReceived;    // von id, oeffentlicher Schluessel, Inhalt
    public event Action<string, byte[]>? WorldReceived;                  // Thema, verschluesselter Spielstand
    public bool IsConnected => _clients.Any(c => c?.IsConnected == true);
    public string BrokerNames => string.Join(", ", Brokers.Where((_, i) => _clients[i]?.IsConnected == true).Select(b => b.Host));

    public Messenger(Identity me, Func<string> displayName)
    {
        _me = me;
        _name = displayName;
    }

    public Task StartAsync()
    {
        for (int i = 0; i < Brokers.Length; i++)
        {
            int b = i;
            _ = Task.Run(() => KeepConnectedAsync(b, _cts.Token));
        }
        return Task.CompletedTask;
    }

    private async Task KeepConnectedAsync(int b, CancellationToken ct)
    {
        int wait = 3000;
        var lastPresence = DateTime.UtcNow;
        while (!ct.IsCancellationRequested)
        {
            if (_clients[b]?.IsConnected != true)
            {
                try
                {
                    await ConnectAsync(b);
                    wait = 3000;
                    lastPresence = DateTime.UtcNow;
                }
                catch
                {
                    wait = Math.Min(wait * 2, 60000);
                }
                NotifyConnection();
            }
            else if (DateTime.UtcNow - lastPresence > TimeSpan.FromSeconds(30))
            {
                // Status regelmaessig erneuern: ein verspaeteter Last Will einer alten Verbindung
                // (Absturz, Neustart) wuerde uns sonst dauerhaft als offline zeigen
                lastPresence = DateTime.UtcNow;
                try { await PublishAsync(_clients[b]!, Prefix + "presence/" + _me.Id, Encoding.ASCII.GetBytes(_status), true); }
                catch { }
            }
            await Task.Delay(_clients[b]?.IsConnected == true ? 2000 : wait, ct).ContinueWith(_ => { });
        }
    }

    private void NotifyConnection()
    {
        bool now = IsConnected;
        if (now == _wasConnected) return;
        _wasConnected = now;
        ConnectionChanged?.Invoke(now);
    }

    private async Task ConnectAsync(int b)
    {
        _clients[b]?.Dispose();
        var client = _factory.CreateMqttClient();
        client.ApplicationMessageReceivedAsync += e => OnMessage(b, e);
        client.DisconnectedAsync += _ => { NotifyConnection(); return Task.CompletedTask; };
        var options = new MqttClientOptionsBuilder()
            .WithTcpServer(Brokers[b].Host, Brokers[b].Port)
            .WithClientId("lbo-" + _me.Id + "-" + RandomNumberGenerator.GetHexString(6))
            .WithCleanSession()
            .WithKeepAlivePeriod(TimeSpan.FromSeconds(20))
            .WithTimeout(TimeSpan.FromSeconds(8))
            .WithWillTopic(Prefix + "presence/" + _me.Id)
            .WithWillPayload(Encoding.ASCII.GetBytes("off"))
            .WithWillRetain(true)
            .WithWillQualityOfServiceLevel(MqttQualityOfServiceLevel.AtLeastOnce)
            .Build();
        await client.ConnectAsync(options);
        _clients[b] = client;
        await SubscribeAsync(client, Prefix + "inbox/" + _me.Id);
        string[] watched;
        lock (_watched) watched = _watched.ToArray();
        foreach (var id in watched) await SubscribeAsync(client, Prefix + "presence/" + id);
        string[] worlds;
        lock (_worlds) worlds = _worlds.ToArray();
        foreach (var t in worlds) await SubscribeAsync(client, Prefix + "world/" + t);
        await PublishAsync(client, Prefix + "profile/" + _me.Id, ProfilePayload(), true);
        await PublishAsync(client, Prefix + "presence/" + _me.Id, Encoding.ASCII.GetBytes(_status), true);
    }

    private IEnumerable<IMqttClient> Connected() => _clients.Where(c => c?.IsConnected == true).Select(c => c!);

    private Task SubscribeAsync(IMqttClient c, string topic) =>
        c.SubscribeAsync(_factory.CreateSubscribeOptionsBuilder()
            .WithTopicFilter(topic, MqttQualityOfServiceLevel.AtLeastOnce).Build());

    private static Task PublishAsync(IMqttClient c, string topic, byte[] payload, bool retain) =>
        c.PublishAsync(new MqttApplicationMessageBuilder()
            .WithTopic(topic).WithPayload(payload).WithRetainFlag(retain)
            .WithQualityOfServiceLevel(MqttQualityOfServiceLevel.AtLeastOnce).Build());

    /// <summary>Auf allen verbundenen Diensten ausfuehren; ein einzelner Ausfall ist kein Fehler.</summary>
    private async Task<bool> OnAllAsync(Func<IMqttClient, Task> action)
    {
        bool any = false;
        foreach (var c in Connected())
        {
            try { await action(c); any = true; } catch { }
        }
        return any;
    }

    private byte[] ProfilePayload() => Encoding.UTF8.GetBytes(
        new JsonObject { ["pub"] = Convert.ToBase64String(_me.PublicKey), ["name"] = _name() }.ToJsonString());

    public Task PublishProfileAsync() =>
        OnAllAsync(c => PublishAsync(c, Prefix + "profile/" + _me.Id, ProfilePayload(), true));

    /// <summary>Online-Status eines Freundes verfolgen.</summary>
    public async Task WatchAsync(string id)
    {
        lock (_watched) if (!_watched.Add(id)) return;
        await OnAllAsync(c => SubscribeAsync(c, Prefix + "presence/" + id));
    }

    /// <summary>Eigenen Status setzen: spielt gerade (busy) oder nur online.</summary>
    public Task SetBusyAsync(bool busy)
    {
        _status = busy ? "busy" : "on";
        return OnAllAsync(c => PublishAsync(c, Prefix + "presence/" + _me.Id, Encoding.ASCII.GetBytes(_status), true));
    }

    /// <summary>Spielstand einer Welt verfolgen (retained, kommt nach dem Abonnieren sofort).</summary>
    public async Task WatchWorldAsync(string topic)
    {
        lock (_worlds) if (!_worlds.Add(topic)) return;
        await OnAllAsync(c => SubscribeAsync(c, Prefix + "world/" + topic));
    }

    public async Task UnwatchWorldAsync(string topic)
    {
        lock (_worlds) _worlds.Remove(topic);
        await OnAllAsync(c => c.UnsubscribeAsync(Prefix + "world/" + topic));
    }

    /// <summary>Spielstand einer Welt bei allen Diensten hinterlegen. true, wenn mindestens einer ihn hat.</summary>
    public Task<bool> PublishWorldAsync(string topic, byte[] payload) =>
        OnAllAsync(c => PublishAsync(c, Prefix + "world/" + topic, payload, true));

    /// <summary>Oeffentlichen Schluessel und Namen zu einem Code holen (aus dem retained Profil).</summary>
    public async Task<(byte[] Pub, string Name)?> LookupAsync(string id, TimeSpan timeout)
    {
        if (!IsConnected) return null;
        var tcs = new TaskCompletionSource<(byte[] Pub, string Name)?>(TaskCreationOptions.RunContinuationsAsynchronously);
        lock (_pendingProfiles) _pendingProfiles[id] = tcs;
        var topic = Prefix + "profile/" + id;
        await OnAllAsync(c => SubscribeAsync(c, topic));
        var done = await Task.WhenAny(tcs.Task, Task.Delay(timeout));
        lock (_pendingProfiles) _pendingProfiles.Remove(id);
        await OnAllAsync(c => c.UnsubscribeAsync(topic));
        return done == tcs.Task ? tcs.Task.Result : null;
    }

    /// <summary>Verschluesselte Nachricht an einen Spieler senden (ueber alle verbundenen Dienste).</summary>
    public async Task<bool> SendAsync(string toId, byte[] toPub, JsonObject content)
    {
        content["ts"] = DateTimeOffset.UtcNow.ToUnixTimeSeconds();
        content["name"] = _name();
        var plain = Encoding.UTF8.GetBytes(content.ToJsonString());
        var nonce = RandomNumberGenerator.GetBytes(12);
        var cipher = new byte[plain.Length];
        var tag = new byte[16];
        using (var aes = new AesGcm(_me.SharedKey(toPub), 16))
            aes.Encrypt(nonce, plain, cipher, tag, Encoding.ASCII.GetBytes(_me.Id + "|" + toId));
        var env = new JsonObject
        {
            ["from"] = _me.Id,
            ["pub"] = Convert.ToBase64String(_me.PublicKey),
            ["n"] = Convert.ToBase64String(nonce),
            ["c"] = Convert.ToBase64String(cipher.Concat(tag).ToArray()),
        };
        var payload = Encoding.UTF8.GetBytes(env.ToJsonString());
        return await OnAllAsync(c => PublishAsync(c, Prefix + "inbox/" + toId, payload, false));
    }

    private Task OnMessage(int broker, MqttApplicationMessageReceivedEventArgs e)
    {
        try
        {
            var topic = e.ApplicationMessage.Topic;
            var payload = e.ApplicationMessage.Payload.ToArray();
            if (topic.StartsWith(Prefix + "presence/"))
            {
                var id = topic[(Prefix.Length + 9)..];
                var state = Encoding.ASCII.GetString(payload) switch { "on" => "on", "busy" => "busy", _ => "off" };
                string before, after;
                lock (_presence)
                {
                    if (!_presence.TryGetValue(id, out var states))
                        _presence[id] = states = Enumerable.Repeat("off", Brokers.Length).ToArray();
                    before = Combine(states);
                    states[broker] = state;
                    after = Combine(states);
                }
                if (before != after || after == "off") PresenceChanged?.Invoke(id, after);
            }
            else if (topic.StartsWith(Prefix + "world/"))
            {
                if (payload.Length > 0) WorldReceived?.Invoke(topic[(Prefix.Length + 6)..], payload);
            }
            else if (topic.StartsWith(Prefix + "profile/"))
            {
                var id = topic[(Prefix.Length + 8)..];
                var doc = JsonNode.Parse(payload)!.AsObject();
                var pub = Convert.FromBase64String((string)doc["pub"]!);
                if (Identity.IdFromPublicKey(pub) != id) return Task.CompletedTask;  // gefaelscht
                TaskCompletionSource<(byte[] Pub, string Name)?>? tcs;
                lock (_pendingProfiles) _pendingProfiles.TryGetValue(id, out tcs);
                tcs?.TrySetResult((pub, Clean((string?)doc["name"])));
            }
            else if (topic == Prefix + "inbox/" + _me.Id)
            {
                HandleInbox(payload);
            }
        }
        catch { /* fremde oder kaputte Nachrichten ignorieren */ }
        return Task.CompletedTask;
    }

    private void HandleInbox(byte[] payload)
    {
        var env = JsonNode.Parse(payload)!.AsObject();
        var from = (string)env["from"]!;
        var pub = Convert.FromBase64String((string)env["pub"]!);
        if (Identity.IdFromPublicKey(pub) != from) return;  // Absender passt nicht zum Schluessel
        var nonceText = (string)env["n"]!;
        var nonce = Convert.FromBase64String(nonceText);
        var blob = Convert.FromBase64String((string)env["c"]!);
        if (blob.Length < 16) return;
        var cipher = blob[..^16];
        var tag = blob[^16..];
        var plain = new byte[cipher.Length];
        using (var aes = new AesGcm(_me.SharedKey(pub), 16))
            aes.Decrypt(nonce, cipher, tag, plain, Encoding.ASCII.GetBytes(from + "|" + _me.Id));
        // Erst nach erfolgreicher Pruefung als gesehen merken, sonst koennte ein Fremder Nonces blockieren
        lock (_seen)
        {
            if (!_seen.Add(nonceText)) return;  // schon ueber einen anderen Dienst angekommen
            _seenOrder.Enqueue(nonceText);
            while (_seenOrder.Count > 2000) _seen.Remove(_seenOrder.Dequeue());
        }
        var content = JsonNode.Parse(plain)!.AsObject();
        var ts = (long?)content["ts"] ?? 0;
        if (Math.Abs(DateTimeOffset.UtcNow.ToUnixTimeSeconds() - ts) > 600) return;  // zu alt (Wiederholung)
        content["name"] = Clean((string?)content["name"]);
        MessageReceived?.Invoke(from, pub, content);
    }

    /// <summary>Status ueber alle Dienste: einer meldet "busy" → busy, sonst einer "on" → on.</summary>
    private static string Combine(string[] states) =>
        states.Contains("busy") ? "busy" : states.Contains("on") ? "on" : "off";

    public static string Clean(string? name)
    {
        name = new string((name ?? "").Where(c => !char.IsControl(c)).ToArray()).Trim();
        return name.Length > 24 ? name[..24] : name;
    }

    public async ValueTask DisposeAsync()
    {
        _cts.Cancel();
        await OnAllAsync(async c =>
        {
            await PublishAsync(c, Prefix + "presence/" + _me.Id, Encoding.ASCII.GetBytes("off"), true);
            await c.DisconnectAsync();
        });
        foreach (var c in _clients) c?.Dispose();
    }
}
