using System.ComponentModel;
using System.Diagnostics;
using System.Windows;
using System.Windows.Input;
using LegoBatmanOnline.Core;
using Microsoft.Win32;

namespace LegoBatmanOnline;

public partial class MainWindow : Window
{
    private readonly Lobby _lobby;
    private bool _closing;

    public MainWindow(Profile profile, Identity me, AppOptions opt)
    {
        InitializeComponent();
        _lobby = new Lobby(profile, me, opt, Dispatcher);
        DataContext = _lobby;
        if (opt.Test) Title += $"  [{opt.Profile}]";
        if (_lobby.NeedsName)
        {
            SetupOverlay.Visibility = Visibility.Visible;
            Loaded += (_, _) => NameBox.Focus();
        }
        Loaded += async (_, _) => await _lobby.StartAsync();
        // Desync: Taskleistensymbol blinken lassen (das Spiel zeigt selbst eine Meldung)
        _lobby.AlertRequested += () =>
        {
            var info = new FLASHWINFO
            {
                cbSize = (uint)System.Runtime.InteropServices.Marshal.SizeOf<FLASHWINFO>(),
                hwnd = new System.Windows.Interop.WindowInteropHelper(this).Handle,
                dwFlags = 3 | 12,   // Titel und Taskleiste, bis das Fenster vorne ist
            };
            FlashWindowEx(ref info);
        };
    }

    [System.Runtime.InteropServices.StructLayout(System.Runtime.InteropServices.LayoutKind.Sequential)]
    private struct FLASHWINFO { public uint cbSize; public IntPtr hwnd; public uint dwFlags; public uint uCount; public uint dwTimeout; }

    [System.Runtime.InteropServices.DllImport("user32.dll")]
    private static extern bool FlashWindowEx(ref FLASHWINFO info);

    private void SaveName_Click(object sender, RoutedEventArgs e)
    {
        var name = Messenger.Clean(NameBox.Text);
        if (name.Length == 0) { NameBox.Focus(); return; }
        _lobby.Name = name;
        SetupOverlay.Visibility = Visibility.Collapsed;
    }

    private void NameBox_KeyDown(object sender, KeyEventArgs e)
    {
        if (e.Key == Key.Enter) SaveName_Click(sender, e);
    }

    private async void Copy_Click(object sender, RoutedEventArgs e)
    {
        try { Clipboard.SetText(_lobby.MyCode); } catch { return; }
        CopyButton.Content = "Kopiert ✓";
        await Task.Delay(1500);
        CopyButton.Content = "Kopieren";
    }

    // --- Freunde ---------------------------------------------------------------------
    private async void AddFriend_Click(object sender, RoutedEventArgs e)
    {
        var code = FriendCodeBox.Text;
        if (string.IsNullOrWhiteSpace(code)) return;
        AddResult.Text = "Suche …";
        AddResult.Text = await _lobby.AddFriendAsync(code);
        if (AddResult.Text.StartsWith("Anfrage")) FriendCodeBox.Clear();
    }

    private void FriendCodeBox_KeyDown(object sender, KeyEventArgs e)
    {
        if (e.Key == Key.Enter) AddFriend_Click(sender, e);
    }

    private static T ItemOf<T>(object sender) => (T)((FrameworkElement)sender).DataContext;

    private async void AcceptFriend_Click(object sender, RoutedEventArgs e) =>
        await _lobby.AcceptFriendAsync(ItemOf<FriendItem>(sender));

    private void RemoveFriend_Click(object sender, RoutedEventArgs e)
    {
        var f = ItemOf<FriendItem>(sender);
        if (MessageBox.Show($"{f.Name} aus der Freundesliste entfernen?", "LEGO Batman Online",
                MessageBoxButton.YesNo, MessageBoxImage.Question) == MessageBoxResult.Yes)
            _lobby.RemoveFriend(f);
    }

    // --- Welten ----------------------------------------------------------------------
    private void NewWorld_Click(object sender, RoutedEventArgs e)
    {
        var friends = _lobby.Friends.Where(f => f.IsFriend).ToList();
        if (friends.Count == 0)
        {
            _lobby.Hint = "Füge zuerst einen Freund hinzu – eine Welt teilst du immer mit einem Freund.";
            return;
        }
        WorldNameBox.Text = "";
        NewWorldError.Text = "";
        PartnerList.ItemsSource = friends;
        PartnerList.SelectedIndex = 0;
        SeedList.ItemsSource = _lobby.Seeds().ToList();
        SeedList.SelectedIndex = 0;
        NewWorldOverlay.Visibility = Visibility.Visible;
        WorldNameBox.Focus();
    }

    private void CancelNewWorld_Click(object sender, RoutedEventArgs e) => NewWorldOverlay.Visibility = Visibility.Collapsed;

    private async void CreateWorld_Click(object sender, RoutedEventArgs e)
    {
        if (PartnerList.SelectedItem is not FriendItem partner) { NewWorldError.Text = "Wähle einen Freund."; return; }
        if (SeedList.SelectedItem is not WorldSeed seed) { NewWorldError.Text = "Wähle einen Startpunkt."; return; }
        var err = await _lobby.CreateWorldAsync(WorldNameBox.Text, partner, seed);
        if (err.Length > 0) { NewWorldError.Text = err; return; }
        NewWorldOverlay.Visibility = Visibility.Collapsed;
    }

    private async void PlayTogether_Click(object sender, RoutedEventArgs e) =>
        await _lobby.PlayTogetherAsync(ItemOf<WorldItem>(sender));

    private async void PlaySolo_Click(object sender, RoutedEventArgs e) =>
        await _lobby.PlaySoloAsync(ItemOf<WorldItem>(sender));

    private void OpenBackups_Click(object sender, RoutedEventArgs e)
    {
        var dir = _lobby.BackupFolder(ItemOf<WorldItem>(sender));
        try { Process.Start(new ProcessStartInfo("explorer.exe", $"\"{dir}\"") { UseShellExecute = true }); } catch { }
    }

    private async void RemoveWorld_Click(object sender, RoutedEventArgs e)
    {
        var w = ItemOf<WorldItem>(sender);
        if (MessageBox.Show($"Welt „{w.Name}“ aus der Liste entfernen?\n\nDie Dateien bleiben auf deinem PC erhalten. " +
                            "Dein Freund behält die Welt.", "LEGO Batman Online",
                MessageBoxButton.YesNo, MessageBoxImage.Question) == MessageBoxResult.Yes)
            await _lobby.RemoveWorldAsync(w);
    }

    // --- Einladung und Sitzung ---------------------------------------------------------------
    private async void AcceptInvite_Click(object sender, RoutedEventArgs e) => await _lobby.AcceptInviteAsync();

    private async void Decline_Click(object sender, RoutedEventArgs e) => await _lobby.DeclineInviteAsync();

    private async void CancelSession_Click(object sender, RoutedEventArgs e) => await _lobby.CancelSessionAsync();

    private void PickGame_Click(object sender, RoutedEventArgs e)
    {
        var dlg = new OpenFileDialog { Filter = "LEGO Batman|LEGOBatman.exe", Title = "LEGOBatman.exe auswählen" };
        if (dlg.ShowDialog(this) == true) _lobby.SetGamePath(dlg.FileName);
    }

    protected override async void OnClosing(CancelEventArgs e)
    {
        base.OnClosing(e);
        if (_closing) return;
        if (_lobby.SessionActive &&
            MessageBox.Show("Es läuft gerade ein Spiel. Trotzdem schließen?\n\nDer Spielstand wird dann erst beim nächsten Start der App abgeglichen.",
                "LEGO Batman Online", MessageBoxButton.YesNo, MessageBoxImage.Warning) != MessageBoxResult.Yes)
        {
            e.Cancel = true;
            return;
        }
        // Erst abmelden (Status "offline"), dann wirklich schliessen
        e.Cancel = true;
        _closing = true;
        await _lobby.ShutdownAsync();
        Close();
    }
}
