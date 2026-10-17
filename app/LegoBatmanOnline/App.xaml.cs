using System.IO;
using System.Windows;
using LegoBatmanOnline.Core;

namespace LegoBatmanOnline;

public partial class App : Application
{
    private Mutex? _single;

    protected override void OnStartup(StartupEventArgs e)
    {
        base.OnStartup(e);
        DispatcherUnhandledException += (_, ex) =>
        {
            WriteCrash(ex.Exception);
            MessageBox.Show("Unerwarteter Fehler:\n" + ex.Exception.Message, "LEGO Batman Online",
                MessageBoxButton.OK, MessageBoxImage.Error);
            ex.Handled = true;
        };

        var opt = AppOptions.Parse(e.Args);
        // Pro Profil nur eine App, sonst wuerden zwei Fenster denselben Freundescode benutzen
        _single = new Mutex(true, "LegoBatmanOnline-App-" + opt.Profile, out bool first);
        if (!first)
        {
            MessageBox.Show("LEGO Batman Online läuft bereits.", "LEGO Batman Online");
            Shutdown();
            return;
        }

        var profile = Profile.Load(opt.Profile);
        if (opt.Name != null) { profile.Name = Messenger.Clean(opt.Name); profile.Save(); }
        var me = profile.GetOrCreateIdentity();
        var win = new MainWindow(profile, me, opt);
        MainWindow = win;
        win.Show();
    }

    private static void WriteCrash(Exception ex)
    {
        try
        {
            Directory.CreateDirectory(Profile.BaseDir);
            File.AppendAllText(Path.Combine(Profile.BaseDir, "app-fehler.txt"), $"{DateTime.Now:s}\n{ex}\n\n");
        }
        catch { }
    }
}
