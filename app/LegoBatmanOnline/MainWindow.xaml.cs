using System.ComponentModel;
using System.Diagnostics;
using System.Windows;
using System.Windows.Input;
using System.Windows.Media.Animation;
using System.Windows.Media.Imaging;
using System.Windows.Threading;
using LegoBatmanOnline.Core;
using Microsoft.Win32;

namespace LegoBatmanOnline;

public partial class MainWindow : Window
{
    private readonly Lobby _lobby;
    private bool _closing;
    private bool _bgFront;   // false: BgA sichtbar, true: BgB sichtbar
    private List<GameItem> _visible = new();   // Kacheln der gewaehlten Kategorie
    private readonly DispatcherTimer _toastTimer = new() { Interval = TimeSpan.FromSeconds(6) };

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
        _lobby.PropertyChanged += (_, e) =>
        {
            if (e.PropertyName == nameof(Lobby.SelectedGame)) ShowBackground(_lobby.SelectedGame);
            if (e.PropertyName == nameof(Lobby.Name)) UpdateInitial();
            if (e.PropertyName == nameof(Lobby.Hint) && !string.IsNullOrWhiteSpace(_lobby.Hint)) ShowToast("LEGO ONLINE", _lobby.Hint, "!");
        };
        _lobby.SelectedGame = _lobby.Games.FirstOrDefault(g => g.Key == profile.LastGame) ?? _lobby.Games[1];
        ApplyFilter();
        UpdateInitial();
        StartClock();
        WatchFriends();
        _toastTimer.Tick += (_, _) => HideToast();
        Loaded += async (_, _) => await _lobby.StartAsync();
        if (opt.Shot != null) Loaded += async (_, _) => await ShotAsync(opt);
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

    // --- Launcher: Spielauswahl, Hintergrund, Uhr --------------------------------------------
    private void ShowBackground(GameItem? g)
    {
        if (g == null) return;
        BitmapImage img;
        try { img = new BitmapImage(new Uri(g.Background)); } catch { return; }
        var show = _bgFront ? BgA : BgB;
        var hide = _bgFront ? BgB : BgA;
        _bgFront = !_bgFront;
        show.Source = img;
        var ease = new CubicEase { EasingMode = EasingMode.EaseOut };
        show.BeginAnimation(OpacityProperty, new DoubleAnimation(0, 1, TimeSpan.FromMilliseconds(450)) { EasingFunction = ease });
        hide.BeginAnimation(OpacityProperty, new DoubleAnimation(0, TimeSpan.FromMilliseconds(450)) { EasingFunction = ease });
        // Titel kurz einblenden, wie beim Wechsel auf der Konsole
        TitleBlock.BeginAnimation(OpacityProperty, new DoubleAnimation(0.2, 1, TimeSpan.FromMilliseconds(320)));
        _lobby.RememberGame(g.Key);
    }

    private void UpdateInitial()
    {
        var n = _lobby.Name;
        Initial.Text = string.IsNullOrWhiteSpace(n) ? "?" : n.Trim()[..1].ToUpperInvariant();
    }

    private void StartClock()
    {
        Clock.Text = DateTime.Now.ToString("HH:mm");
        var t = new DispatcherTimer { Interval = TimeSpan.FromSeconds(5) };
        t.Tick += (_, _) => Clock.Text = DateTime.Now.ToString("HH:mm");
        t.Start();
    }

    // Kategorien: alle, nur spielbare, nur Platzhalter
    private void ApplyFilter()
    {
        if (TabAll == null || _lobby == null) return;
        _visible = _lobby.Games.Where(g => TabCoop.IsChecked == true ? g.Available
                                          : TabSoon.IsChecked == true ? !g.Available : true).ToList();
        TileList.ItemsSource = _visible;
        if (_visible.Count > 0 && (_lobby.SelectedGame == null || !_visible.Contains(_lobby.SelectedGame)))
            _lobby.SelectedGame = _visible[0];
    }

    private void Tab_Checked(object sender, RoutedEventArgs e) => ApplyFilter();

    private void SwitchTab(int dir)
    {
        var tabs = new[] { TabAll, TabCoop, TabSoon };
        int i = Array.FindIndex(tabs, t => t.IsChecked == true);
        tabs[Math.Clamp(i + dir, 0, tabs.Length - 1)].IsChecked = true;
    }

    // Zahlen oben rechts (Anfragen, Freunde online) und Benachrichtigung bei neuen Anfragen
    private void WatchFriends()
    {
        void Hook(FriendItem f)
        {
            f.PropertyChanged += (_, e) =>
            {
                if (e.PropertyName == nameof(FriendItem.IsIncoming) && f.IsIncoming) ShowToast(f.Name.ToUpperInvariant(), "möchte dein Freund sein", f.Initial);
                if (e.PropertyName == nameof(FriendItem.Online) && f.Online && f.IsFriend) ShowToast(f.Name.ToUpperInvariant(), "ist jetzt online", f.Initial);
                UpdateCounts();
            };
        }
        foreach (var f in _lobby.Friends) Hook(f);
        _lobby.Friends.CollectionChanged += (_, e) =>
        {
            if (e.NewItems != null)
                foreach (FriendItem f in e.NewItems)
                {
                    Hook(f);
                    if (f.IsIncoming) ShowToast(f.Name.ToUpperInvariant(), "möchte dein Freund sein", f.Initial);
                }
            UpdateCounts();
        };
        UpdateCounts();
    }

    private void UpdateCounts()
    {
        BellCount.Text = _lobby.Friends.Count(f => f.IsIncoming).ToString();
        OnlineCount.Text = _lobby.Friends.Count(f => f.IsFriend && f.Online).ToString();
    }

    // Benachrichtigung oben in der Mitte, verschwindet nach 6 s
    private void ShowToast(string title, string text, string icon)
    {
        ToastTitle.Text = title;
        ToastText.Text = text;
        ToastIcon.Text = icon;
        Toast.IsHitTestVisible = true;
        var ease = new CubicEase { EasingMode = EasingMode.EaseOut };
        Toast.BeginAnimation(OpacityProperty, new DoubleAnimation(1, TimeSpan.FromMilliseconds(220)));
        ToastShift.BeginAnimation(System.Windows.Media.TranslateTransform.YProperty,
            new DoubleAnimation(-30, 0, TimeSpan.FromMilliseconds(260)) { EasingFunction = ease });
        _toastTimer.Stop();
        _toastTimer.Start();
    }

    private void HideToast()
    {
        _toastTimer.Stop();
        Toast.IsHitTestVisible = false;
        Toast.BeginAnimation(OpacityProperty, new DoubleAnimation(0, TimeSpan.FromMilliseconds(300)));
    }

    private void Toast_Click(object sender, MouseButtonEventArgs e) => HideToast();

    // Seitenleiste mit Freunden und Welten
    private void OpenFriends_Click(object sender, RoutedEventArgs e)
    {
        DetailsOverlay.Visibility = Visibility.Collapsed;
        Home.Visibility = Visibility.Visible;
        DrawerLayer.Visibility = Visibility.Visible;
        DrawerShift.BeginAnimation(System.Windows.Media.TranslateTransform.XProperty,
            new DoubleAnimation(440, 0, TimeSpan.FromMilliseconds(240)) { EasingFunction = new CubicEase { EasingMode = EasingMode.EaseOut } });
    }

    private void Details_Click(object sender, RoutedEventArgs e)
    {
        DrawerLayer.Visibility = Visibility.Collapsed;
        DetailsOverlay.Visibility = Visibility.Visible;
        Home.Visibility = Visibility.Hidden;
        DetailsOverlay.BeginAnimation(OpacityProperty, new DoubleAnimation(0, 1, TimeSpan.FromMilliseconds(200)));
    }

    private void CloseOverlays_Click(object sender, RoutedEventArgs e) => CloseOverlays();

    private bool CloseOverlays()
    {
        bool any = DrawerLayer.Visibility == Visibility.Visible || DetailsOverlay.Visibility == Visibility.Visible ||
                   LogPanel.Visibility == Visibility.Visible;
        DrawerLayer.Visibility = Visibility.Collapsed;
        DetailsOverlay.Visibility = Visibility.Collapsed;
        LogPanel.Visibility = Visibility.Collapsed;
        Home.Visibility = Visibility.Visible;
        return any;
    }

    private void Log_Click(object sender, RoutedEventArgs e) =>
        LogPanel.Visibility = LogPanel.Visibility == Visibility.Visible ? Visibility.Collapsed : Visibility.Visible;

    private void Tile_Click(object sender, MouseButtonEventArgs e)
    {
        if (((FrameworkElement)sender).DataContext is GameItem g)
        {
            if (_lobby.SelectedGame == g) Play_Click(sender, e);
            else _lobby.SelectedGame = g;
        }
    }

    private void Window_KeyDown(object sender, KeyEventArgs e)
    {
        if (e.OriginalSource is System.Windows.Controls.TextBox) return;
        if (SetupOverlay.Visibility == Visibility.Visible || _lobby.HasIncoming || _lobby.SessionActive) return;
        if (NewWorldOverlay.Visibility == Visibility.Visible || PickFriendOverlay.Visibility == Visibility.Visible)
        {
            if (e.Key == Key.Escape) { NewWorldOverlay.Visibility = PickFriendOverlay.Visibility = Visibility.Collapsed; e.Handled = true; }
            return;
        }
        if (e.Key == Key.Escape) { e.Handled = CloseOverlays(); return; }
        if (DrawerLayer.Visibility == Visibility.Visible) return;
        int i = _lobby.SelectedGame == null ? 0 : Math.Max(0, _visible.IndexOf(_lobby.SelectedGame));
        e.Handled = true;
        switch (e.Key)
        {
            case Key.Left when _visible.Count > 0: _lobby.SelectedGame = _visible[Math.Max(0, i - 1)]; break;
            case Key.Right when _visible.Count > 0: _lobby.SelectedGame = _visible[Math.Min(_visible.Count - 1, i + 1)]; break;
            case Key.Q: SwitchTab(-1); break;
            case Key.E: SwitchTab(1); break;
            case Key.F:
            case Key.W: OpenFriends_Click(sender, e); break;
            case Key.D:
                if (DetailsOverlay.Visibility == Visibility.Visible) CloseOverlays(); else Details_Click(sender, e);
                break;
            case Key.P: Log_Click(sender, e); break;
            case Key.Enter: Play_Click(sender, e); break;
            default: e.Handled = false; break;
        }
    }

    private void Play_Click(object sender, RoutedEventArgs e)
    {
        var g = _lobby.SelectedGame;
        if (g == null || !g.Available || _lobby.SessionActive) return;
        CloseOverlays();
        if (g.Key == "batman1")
        {
            // Erste Welt, deren Partner online ist; sonst eine neue Welt anlegen
            var w = _lobby.Worlds.FirstOrDefault(x => x.CanPlayTogether);
            if (w != null) _ = _lobby.PlayTogetherAsync(w);
            else if (_lobby.Worlds.Count == 0) NewWorld_Click(sender, e);
            else _lobby.Hint = "Gerade ist niemand aus deinen Welten online. Du kannst alleine weiterspielen.";
            return;
        }
        if (g.Key == "batman2")
        {
            var friends = _lobby.Friends.Where(f => f.IsFriend).OrderByDescending(f => f.Online).ToList();
            if (friends.Count == 0)
            {
                _lobby.Hint = "Füge zuerst einen Freund über seinen Code hinzu.";
                OpenFriends_Click(sender, e);
                FriendCodeBox.Focus();
                return;
            }
            CloseOverlays();
            PickFriendList.ItemsSource = friends;
            PickFriendList.SelectedItem = friends.FirstOrDefault(f => f.Online);
            PickFriendError.Text = "";
            PickFriendOverlay.Visibility = Visibility.Visible;
            PickFriendList.Focus();
        }
    }

    private async void PickFriendOk_Click(object sender, RoutedEventArgs e)
    {
        if (PickFriendList.SelectedItem is not FriendItem f) { PickFriendError.Text = "Wähle einen Freund."; return; }
        if (!f.Online) { PickFriendError.Text = $"{f.Name} ist gerade offline."; return; }
        PickFriendOverlay.Visibility = Visibility.Collapsed;
        await _lobby.PlayBatman2Async(f);
    }

    private void PickFriendCancel_Click(object sender, RoutedEventArgs e) => PickFriendOverlay.Visibility = Visibility.Collapsed;

    // Test (--shot Datei [--shot-game Schluessel]): Fenster selbst rendern, als PNG speichern, beenden
    private async Task ShotAsync(AppOptions opt)
    {
        if (opt.ShotGame != null && _lobby.Games.FirstOrDefault(g => g.Key == opt.ShotGame) is { } g) _lobby.SelectedGame = g;
        if (opt.ShotView == "freunde") OpenFriends_Click(this, new RoutedEventArgs());
        if (opt.ShotView == "details") Details_Click(this, new RoutedEventArgs());
        await Task.Delay(2500);
        var root = (FrameworkElement)Content;
        var dpi = System.Windows.Media.VisualTreeHelper.GetDpi(this);
        var bmp = new RenderTargetBitmap((int)(root.ActualWidth * dpi.DpiScaleX), (int)(root.ActualHeight * dpi.DpiScaleY),
                                         96 * dpi.DpiScaleX, 96 * dpi.DpiScaleY, System.Windows.Media.PixelFormats.Pbgra32);
        bmp.Render(root);
        var enc = new PngBitmapEncoder();
        enc.Frames.Add(BitmapFrame.Create(bmp));
        using (var f = System.IO.File.Create(opt.Shot!)) enc.Save(f);
        _closing = true;
        Application.Current.Shutdown();
    }

    // --- Profil --------------------------------------------------------------------------------
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
        ShowToast("FREUNDESCODE", $"{_lobby.MyCode} kopiert – schick ihn deinem Freund.", "✓");
        CodeText.Text = "Kopiert ✓";
        await Task.Delay(1500);
        CodeText.SetBinding(System.Windows.Controls.TextBlock.TextProperty, nameof(Lobby.MyCode));
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
        if (MessageBox.Show($"{f.Name} aus der Freundesliste entfernen?", "LEGO Online",
                MessageBoxButton.YesNo, MessageBoxImage.Question) == MessageBoxResult.Yes)
            _lobby.RemoveFriend(f);
    }

    // --- Welten (LEGO Batman 1) -----------------------------------------------------------
    private void NewWorld_Click(object sender, RoutedEventArgs e)
    {
        var friends = _lobby.Friends.Where(f => f.IsFriend).ToList();
        if (friends.Count == 0)
        {
            _lobby.Hint = "Füge zuerst einen Freund hinzu – eine Welt teilst du immer mit einem Freund.";
            return;
        }
        CloseOverlays();
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

    private async void PlayBatman2_Click(object sender, RoutedEventArgs e) =>
        await _lobby.PlayBatman2Async(ItemOf<FriendItem>(sender));

    private void OpenBackups_Click(object sender, RoutedEventArgs e)
    {
        var dir = _lobby.BackupFolder(ItemOf<WorldItem>(sender));
        try { Process.Start(new ProcessStartInfo("explorer.exe", $"\"{dir}\"") { UseShellExecute = true }); } catch { }
    }

    private async void RemoveWorld_Click(object sender, RoutedEventArgs e)
    {
        var w = ItemOf<WorldItem>(sender);
        if (MessageBox.Show($"Welt „{w.Name}“ aus der Liste entfernen?\n\nDie Dateien bleiben auf deinem PC erhalten. " +
                            "Dein Freund behält die Welt.", "LEGO Online",
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
                "LEGO Online", MessageBoxButton.YesNo, MessageBoxImage.Warning) != MessageBoxResult.Yes)
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
