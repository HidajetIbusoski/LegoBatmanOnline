using System.IO;
using System.IO.Compression;
using System.Security.Cryptography;
using System.Text;
using System.Text.Json.Nodes;

namespace LegoBatmanOnline.Core;

/// <summary>Eine gemeinsame Welt: ein benannter Spielstand, den zwei Freunde teilen (im Profil gespeichert).</summary>
public sealed class WorldInfo
{
    public string Id { get; set; } = "";
    public string Name { get; set; } = "";
    public string Key { get; set; } = "";        // Base64, 32 Byte, nur die beiden Mitglieder kennen ihn
    public string Partner { get; set; } = "";    // Freundes-ID
    public long VersionTs { get; set; }           // Unix-ms des lokalen Stands (0 = noch keiner)
    public string VersionSha { get; set; } = "";
    public string VersionBy { get; set; } = "";   // Name dessen, der zuletzt gespielt hat
}

public sealed record WorldVersion(long Ts, string Sha, string By, string Name, Dictionary<string, byte[]> Files,
    Dictionary<string, long> Times);

/// <summary>
/// Dateien und Abgleich einer Welt. Ordner: %LOCALAPPDATA%\LegoBatmanOnline\welten\[profil]\[id]\
///   spielstand\    die SaveGame-Dateien; das Spiel liest und speichert hier (LBO_SAVEDIR)
///   sicherungen\   aeltere Staende, die beim Uebernehmen eines neueren ersetzt wurden
/// </summary>
public static class WorldStore
{
    private const int MaxBackups = 20;

    public static string Dir(string profile, string id) => Path.Combine(Profile.BaseDir, "welten", profile, id);
    public static string SavesDir(string profile, string id) => Path.Combine(Dir(profile, id), "spielstand");
    public static string BackupDir(string profile, string id) => Path.Combine(Dir(profile, id), "sicherungen");

    public static WorldInfo Create(string name, string partner) => new()
    {
        Id = RandomNumberGenerator.GetHexString(32, true),
        Name = name,
        Key = Convert.ToBase64String(RandomNumberGenerator.GetBytes(32)),
        Partner = partner,
    };

    /// <summary>Thema beim Vermittler: Hash des Schluessels, damit Fremde es weder kennen noch beschreiben.</summary>
    public static string Topic(WorldInfo w) =>
        Convert.ToHexString(SHA256.HashData(Encoding.ASCII.GetBytes("thema|" + w.Key)))[..32].ToLowerInvariant();

    public static string[] SaveFiles(string dir) =>
        Directory.Exists(dir)
            ? Directory.GetFiles(dir, "SaveGame*").OrderBy(f => f, StringComparer.OrdinalIgnoreCase).ToArray()
            : Array.Empty<string>();

    /// <summary>Pruefsumme ueber Namen und Inhalt aller Spielstand-Dateien ("" = keine).</summary>
    public static string Hash(string dir)
    {
        var files = SaveFiles(dir);
        if (files.Length == 0) return "";
        using var sha = IncrementalHash.CreateHash(HashAlgorithmName.SHA256);
        foreach (var f in files)
        {
            sha.AppendData(Encoding.UTF8.GetBytes(Path.GetFileName(f).ToLowerInvariant() + "\n"));
            sha.AppendData(File.ReadAllBytes(f));
        }
        return Convert.ToHexString(sha.GetHashAndReset())[..32];
    }

    public static DateTime LastWrite(string dir) =>
        SaveFiles(dir).Select(File.GetLastWriteTimeUtc).DefaultIfEmpty(DateTime.MinValue).Max();

    /// <summary>Spielstand mit der juengsten Aenderung (fuers automatische Laden), oder -1.</summary>
    public static int NewestSlot(string dir)
    {
        var best = SaveFiles(dir)
            .Select(f => (File: f, Slot: SlotOf(f)))
            .Where(x => x.Slot >= 0)
            .OrderByDescending(x => File.GetLastWriteTimeUtc(x.File))
            .FirstOrDefault();
        return best.File == null ? -1 : best.Slot;
    }

    private static int SlotOf(string file)
    {
        var name = Path.GetFileName(file);
        return name.Length > 8 && char.IsDigit(name[8]) ? name[8] - '0' : -1;
    }


    /// <summary>Verpacken (gzip) und mit dem Weltschluessel verschluesseln (AES-GCM, AAD = Welt-ID).</summary>
    public static byte[] Pack(WorldInfo w, string dir)
    {
        var files = new JsonObject();
        var times = new JsonObject();
        foreach (var f in SaveFiles(dir))
        {
            files[Path.GetFileName(f)] = Convert.ToBase64String(File.ReadAllBytes(f));
            times[Path.GetFileName(f)] = new DateTimeOffset(File.GetLastWriteTimeUtc(f)).ToUnixTimeMilliseconds();
        }
        var doc = new JsonObject
        {
            ["ts"] = w.VersionTs, ["sha"] = w.VersionSha, ["by"] = w.VersionBy, ["name"] = w.Name,
            ["files"] = files, ["times"] = times,
        };
        byte[] plain;
        using (var ms = new MemoryStream())
        {
            using (var gz = new GZipStream(ms, CompressionLevel.SmallestSize))
                gz.Write(Encoding.UTF8.GetBytes(doc.ToJsonString()));
            plain = ms.ToArray();
        }
        var nonce = RandomNumberGenerator.GetBytes(12);
        var cipher = new byte[plain.Length];
        var tag = new byte[16];
        using (var aes = new AesGcm(Convert.FromBase64String(w.Key), 16))
            aes.Encrypt(nonce, plain, cipher, tag, Encoding.ASCII.GetBytes("welt|" + w.Id));
        var outp = new byte[1 + 12 + cipher.Length + 16];
        outp[0] = 1;
        nonce.CopyTo(outp, 1);
        cipher.CopyTo(outp, 13);
        tag.CopyTo(outp, 13 + cipher.Length);
        return outp;
    }

    public static WorldVersion? Unpack(WorldInfo w, byte[] data)
    {
        try
        {
            if (data.Length < 1 + 12 + 16 || data[0] != 1) return null;
            var nonce = data.AsSpan(1, 12);
            var cipher = data.AsSpan(13, data.Length - 13 - 16);
            var tag = data.AsSpan(data.Length - 16);
            var plain = new byte[cipher.Length];
            using (var aes = new AesGcm(Convert.FromBase64String(w.Key), 16))
                aes.Decrypt(nonce, cipher, tag, plain, Encoding.ASCII.GetBytes("welt|" + w.Id));
            using var gz = new GZipStream(new MemoryStream(plain), CompressionMode.Decompress);
            using var outMs = new MemoryStream();
            gz.CopyTo(outMs);
            var doc = JsonNode.Parse(outMs.ToArray())!.AsObject();
            var files = new Dictionary<string, byte[]>();
            foreach (var kv in doc["files"]!.AsObject())
            {
                var name = Path.GetFileName(kv.Key);
                // nur Spielstand-Dateien, keine Pfade
                if (!name.StartsWith("SaveGame", StringComparison.OrdinalIgnoreCase) || name != kv.Key) return null;
                files[name] = Convert.FromBase64String((string)kv.Value!);
            }
            var times = new Dictionary<string, long>();
            if (doc["times"] is JsonObject t)
                foreach (var kv in t) times[kv.Key] = (long)kv.Value!;
            return new WorldVersion((long)doc["ts"]!, (string)doc["sha"]!, Messenger.Clean((string?)doc["by"]),
                Messenger.Clean((string?)doc["name"]), files, times);
        }
        catch { return null; }
    }

    /// <summary>Aktuellen Stand in sicherungen\[zeit] kopieren (aelteste werden ab 20 geloescht).</summary>
    public static void Backup(string profile, string id, string reason)
    {
        var src = SaveFiles(SavesDir(profile, id));
        if (src.Length == 0) return;
        var root = BackupDir(profile, id);
        var dst = Path.Combine(root, $"{DateTime.Now:yyyy-MM-dd_HH-mm-ss}_{reason}");
        Directory.CreateDirectory(dst);
        foreach (var f in src) File.Copy(f, Path.Combine(dst, Path.GetFileName(f)), true);
        foreach (var old in Directory.GetDirectories(root).OrderByDescending(d => d).Skip(MaxBackups))
            try { Directory.Delete(old, true); } catch { }
    }

    /// <summary>Spielstand-Dateien durch die uebergebenen ersetzen.</summary>
    public static void Replace(string profile, string id, WorldVersion v)
    {
        var dir = SavesDir(profile, id);
        Directory.CreateDirectory(dir);
        foreach (var f in SaveFiles(dir)) File.Delete(f);
        foreach (var kv in v.Files)
        {
            var p = Path.Combine(dir, kv.Key);
            File.WriteAllBytes(p, kv.Value);
            var ms = v.Times.TryGetValue(kv.Key, out var t) ? t : v.Ts;
            File.SetLastWriteTimeUtc(p, DateTimeOffset.FromUnixTimeMilliseconds(ms).UtcDateTime);
        }
    }
}
