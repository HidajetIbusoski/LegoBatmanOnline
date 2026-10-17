using System.Diagnostics;
using System.IO;
using System.Reflection;
using System.Security.Cryptography;
using System.Text.RegularExpressions;
using Microsoft.Win32;

namespace LegoBatmanOnline.Core;

public sealed record SaveSlot(int Index, DateTime Saved)
{
    public string Title => $"Spiel {Index + 1}";
    public string SavedText => $"gespeichert am {Saved:dd.MM.yyyy} um {Saved:HH:mm}";
}

public sealed record Endpoint(int Port, string Public, string Local, int Key, bool SymmetricNat);

/// <summary>
/// Verbindung zu Spiel und Mod: Spiel finden, Spielstaende auflisten, Mod-Dateien bereitstellen
/// und den Launcher (lbo-launch.exe) fuer STUN und Verbindungsaufbau aufrufen.
/// </summary>
public static class GameBridge
{
    public static string SaveDir => Path.Combine(
        Environment.GetFolderPath(Environment.SpecialFolder.LocalApplicationData),
        "Warner Bros. Interactive Entertainment", "LEGO Batman", "Saved Games");

    /// <summary>Sucht LEGOBatman.exe in allen Steam-Bibliotheken.</summary>
    public static string? FindGame()
    {
        var steam = Registry.GetValue(@"HKEY_CURRENT_USER\Software\Valve\Steam", "SteamPath", null) as string;
        if (steam == null) return null;
        steam = steam.Replace('/', '\\');
        var libs = new List<string> { steam };
        var vdf = Path.Combine(steam, "steamapps", "libraryfolders.vdf");
        if (File.Exists(vdf))
            foreach (Match m in Regex.Matches(File.ReadAllText(vdf), "\"path\"\\s+\"([^\"]+)\""))
                libs.Add(m.Groups[1].Value.Replace(@"\\", @"\"));
        foreach (var lib in libs)
        {
            var exe = Path.Combine(lib, "steamapps", "common", "Lego Batman", "LEGOBatman.exe");
            if (File.Exists(exe)) return exe;
        }
        return null;
    }

    public static List<SaveSlot> ListSaves()
    {
        var list = new List<SaveSlot>();
        if (!Directory.Exists(SaveDir)) return list;
        for (int i = 0; i < 6; i++)
        {
            var f = Path.Combine(SaveDir, $"SaveGame{i}.LEGO Batman_SavedGame");
            if (File.Exists(f)) list.Add(new SaveSlot(i, File.GetLastWriteTime(f)));
        }
        return list;
    }

    /// <summary>
    /// Entpackt lbo-launch.exe und lbo.dll (in die App eingebettet) in einen versionierten Ordner.
    /// Ein laufendes Spiel sperrt seine DLL; durch den Versionsordner stoert das spaetere Updates nicht.
    /// </summary>
    public static string PrepareBinaries()
    {
        var asm = Assembly.GetExecutingAssembly();
        byte[] Read(string name)
        {
            using var s = asm.GetManifestResourceStream(name) ?? throw new InvalidOperationException(name + " fehlt in der App");
            using var ms = new MemoryStream();
            s.CopyTo(ms);
            return ms.ToArray();
        }
        var exe = Read("lbo-launch.exe");
        var dll = Read("lbo.dll");
        var ver = Convert.ToHexString(SHA256.HashData(exe.Concat(dll).ToArray()))[..12];
        var dir = Path.Combine(Profile.BaseDir, "bin", ver);
        Directory.CreateDirectory(dir);
        var exePath = Path.Combine(dir, "lbo-launch.exe");
        if (!File.Exists(exePath)) File.WriteAllBytes(exePath, exe);
        var dllPath = Path.Combine(dir, "lbo.dll");
        if (!File.Exists(dllPath)) File.WriteAllBytes(dllPath, dll);
        return exePath;
    }

    /// <summary>Belegt einen UDP-Port und ermittelt per STUN die oeffentliche Adresse.</summary>
    public static async Task<Endpoint?> StunAsync(string launcher)
    {
        var psi = new ProcessStartInfo(launcher, "--stun")
        {
            UseShellExecute = false, RedirectStandardOutput = true, CreateNoWindow = true,
        };
        using var p = Process.Start(psi)!;
        var output = await p.StandardOutput.ReadToEndAsync();
        await p.WaitForExitAsync();
        var m = Regex.Match(output, @"STUN port=(\d+) public=(\S+) local=(\S+) symmetric=(\d) key=(\d+)");
        if (!m.Success) return null;
        return new Endpoint(int.Parse(m.Groups[1].Value), m.Groups[2].Value, m.Groups[3].Value,
            int.Parse(m.Groups[5].Value), m.Groups[4].Value == "1");
    }

    /// <summary>
    /// Startet Verbindungsaufbau und Spiel. Jede Ausgabezeile des Launchers geht an onLine
    /// (z. B. "STATUS verbunden", "ERROR ..."). Kehrt zurueck, wenn das Spiel beendet ist.
    /// </summary>
    public static Task<int> ConnectAndPlayAsync(string launcher, bool host, Endpoint me, Endpoint peer,
        int slot, string? saveDir, Action<string> onLine, IDictionary<string, string>? extraEnv, CancellationToken ct)
    {
        var psi = new ProcessStartInfo(launcher);
        foreach (var a in new[] { "--connect", host ? "host" : "join", "--port", me.Port.ToString(), "--key", me.Key.ToString(),
                     "--peer-key", peer.Key.ToString(), "--peer-public", peer.Public, "--peer-local", peer.Local })
            psi.ArgumentList.Add(a);
        if (slot >= 0) { psi.ArgumentList.Add("--slot"); psi.ArgumentList.Add(slot.ToString()); }
        if (saveDir != null) { psi.ArgumentList.Add("--savedir"); psi.ArgumentList.Add(saveDir); }
        return RunAsync(psi, onLine, extraEnv, ct);
    }

    /// <summary>Alleine spielen: Spiel ohne Netz, Spielstaende aus saveDir (gemeinsame Welt).</summary>
    public static Task<int> PlaySoloAsync(string launcher, string saveDir, Action<string> onLine,
        IDictionary<string, string>? extraEnv, CancellationToken ct)
    {
        var psi = new ProcessStartInfo(launcher);
        psi.ArgumentList.Add("--solo");
        psi.ArgumentList.Add("--savedir");
        psi.ArgumentList.Add(saveDir);
        return RunAsync(psi, onLine, extraEnv, ct);
    }

    private static async Task<int> RunAsync(ProcessStartInfo psi, Action<string> onLine,
        IDictionary<string, string>? extraEnv, CancellationToken ct)
    {
        psi.UseShellExecute = false;
        psi.RedirectStandardOutput = true;
        psi.RedirectStandardError = true;
        psi.CreateNoWindow = true;
        if (extraEnv != null) foreach (var kv in extraEnv) psi.Environment[kv.Key] = kv.Value;
        using var p = Process.Start(psi)!;
        p.OutputDataReceived += (_, e) => { if (e.Data != null) onLine(e.Data); };
        p.ErrorDataReceived += (_, e) => { if (e.Data != null) onLine(e.Data); };
        p.BeginOutputReadLine();
        p.BeginErrorReadLine();
        using (ct.Register(() => { try { if (!p.HasExited) p.Kill(true); } catch { } }))
            await p.WaitForExitAsync();
        return p.ExitCode;
    }

    /// <summary>Verknuepfungen auf Desktop und im Startmenue (einmalig bei der Einrichtung).</summary>
    public static void CreateShortcuts()
    {
        var exe = Environment.ProcessPath!;
        foreach (var folder in new[] { Environment.SpecialFolder.DesktopDirectory, Environment.SpecialFolder.Programs })
        {
            var lnk = Path.Combine(Environment.GetFolderPath(folder), "LEGO Batman Online.lnk");
            var shellType = Type.GetTypeFromProgID("WScript.Shell");
            if (shellType == null) return;
            dynamic shell = Activator.CreateInstance(shellType)!;
            dynamic sc = shell.CreateShortcut(lnk);
            sc.TargetPath = exe;
            sc.WorkingDirectory = Path.GetDirectoryName(exe);
            sc.Description = "LEGO Batman Online-Koop";
            sc.Save();
        }
    }
}
