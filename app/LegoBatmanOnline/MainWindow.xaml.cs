using System.ComponentModel;
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
    }

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

    private static FriendItem ItemOf(object sender) => (FriendItem)((FrameworkElement)sender).DataContext;

    private async void AcceptFriend_Click(object sender, RoutedEventArgs e) => await _lobby.AcceptFriendAsync(ItemOf(sender));

    private async void Invite_Click(object sender, RoutedEventArgs e) => await _lobby.InviteAsync(ItemOf(sender));

    private void RemoveFriend_Click(object sender, RoutedEventArgs e)
    {
        var f = ItemOf(sender);
        if (MessageBox.Show($"{f.Name} aus der Freundesliste entfernen?", "LEGO Batman Online",
                MessageBoxButton.YesNo, MessageBoxImage.Question) == MessageBoxResult.Yes)
            _lobby.RemoveFriend(f);
    }

    private async void AcceptInvite_Click(object sender, RoutedEventArgs e) => await _lobby.AcceptInviteAsync();

    private async void Decline_Click(object sender, RoutedEventArgs e) => await _lobby.DeclineInviteAsync();

    private async void CancelSession_Click(object sender, RoutedEventArgs e) => await _lobby.CancelSessionAsync();

    private void RefreshSaves_Click(object sender, RoutedEventArgs e) => _lobby.RefreshSaves();

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
            MessageBox.Show("Es läuft gerade ein Online-Spiel. Trotzdem schließen?", "LEGO Batman Online",
                MessageBoxButton.YesNo, MessageBoxImage.Warning) != MessageBoxResult.Yes)
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
