using System.IO;
using System.Security.Cryptography;
using System.Text.Json;

namespace LegoBatmanOnline.Core;

public enum FriendState { Friend, RequestSent, RequestReceived }

public sealed class Friend
{
    public string Id { get; set; } = "";
    public string Name { get; set; } = "";
    public string PublicKey { get; set; } = "";   // Base64 SubjectPublicKeyInfo
    public FriendState State { get; set; }
}

/// <summary>
/// Lokale Daten dieser Installation: Name, privater Schluessel (per Windows-DPAPI geschuetzt),
/// Freundesliste und Einstellungen. Liegt unter %LOCALAPPDATA%\LegoBatmanOnline\app\[profil].
/// </summary>
public sealed class Profile
{
    public string Name { get; set; } = "";
    public string ProtectedKey { get; set; } = "";
    public List<Friend> Friends { get; set; } = new();
    public List<WorldInfo> Worlds { get; set; } = new();
    public string? GamePath { get; set; }
    public int LastSlot { get; set; }
    public string? LastGame { get; set; }   // zuletzt gewaehltes Spiel im Launcher
    public bool ShortcutsCreated { get; set; }
    private string _path = "";
    private static readonly JsonSerializerOptions Json = new() { WriteIndented = true };

    public static string BaseDir =>
        Path.Combine(Environment.GetFolderPath(Environment.SpecialFolder.LocalApplicationData), "LegoBatmanOnline");

    public static Profile Load(string profileName)
    {
        var dir = Path.Combine(BaseDir, "app", profileName);
        Directory.CreateDirectory(dir);
        var path = Path.Combine(dir, "profil.json");
        Profile p = File.Exists(path)
            ? JsonSerializer.Deserialize<Profile>(File.ReadAllText(path)) ?? new Profile()
            : new Profile();
        p._path = path;
        return p;
    }

    public void Save()
    {
        var tmp = _path + ".tmp";
        File.WriteAllText(tmp, JsonSerializer.Serialize(this, Json));
        File.Move(tmp, _path, true);
    }

    public Identity GetOrCreateIdentity()
    {
        if (!string.IsNullOrEmpty(ProtectedKey))
        {
            var raw = ProtectedData.Unprotect(Convert.FromBase64String(ProtectedKey), null, DataProtectionScope.CurrentUser);
            return Identity.FromPrivateKey(raw);
        }
        var id = Identity.Create();
        ProtectedKey = Convert.ToBase64String(
            ProtectedData.Protect(id.ExportPrivateKey(), null, DataProtectionScope.CurrentUser));
        Save();
        return id;
    }

    public Friend? Find(string id) => Friends.FirstOrDefault(f => f.Id == id);
}
