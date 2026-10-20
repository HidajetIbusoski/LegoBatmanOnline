using System.Collections.ObjectModel;

namespace LegoBatmanOnline.Core;

// Ein Spiel im Launcher. Bilder liegen als Ressourcen unter Assets/Games/<Key>/ (bg.jpg 1920x1080,
// tile.jpg 320x320, logo.png). Available = Online-Koop ist fertig; die anderen sind Platzhalter.
public sealed class GameItem : Bindable
{
    public string Key { get; init; } = "";
    public string Title { get; init; } = "";
    public string Year { get; init; } = "";
    public string Description { get; init; } = "";
    public bool Available { get; init; }
    public string Background => $"pack://application:,,,/Assets/Games/{Key}/bg.jpg";
    public string Tile => $"pack://application:,,,/Assets/Games/{Key}/tile.jpg";
    public string Logo => $"pack://application:,,,/Assets/Games/{Key}/logo.png";
    public string StatusText => Available ? "ONLINE-KOOP BEREIT" : "IN ENTWICKLUNG";
    public string PlayText => Available ? "Zusammen spielen" : "Bald verfügbar";
    public string PlayHint => PlayText.ToUpperInvariant();

    private bool _selected;
    public bool Selected { get => _selected; set { _selected = value; Raise(); } }
}

public static class GameCatalog
{
    public static ObservableCollection<GameItem> Create() => new()
    {
        new GameItem
        {
            Key = "batman1", Title = "LEGO Batman: The Videogame", Year = "2008", Available = true,
            Description = "Batman und Robin gegen die Schurken von Arkham. Gemeinsame Welten: Ihr teilt einen " +
                          "Spielstand, jeder kann alleine weiterspielen, und seid ihr beide online, spielt ihr zusammen.",
        },
        new GameItem
        {
            Key = "batman2", Title = "LEGO Batman 2: DC Super Heroes", Year = "2012", Available = true,
            Description = "Batman, Robin und die Justice League gegen Lex Luthor und den Joker. Lade einen Freund ein, " +
                          "der Host wählt im Menü „Neues Spiel“, im Level steigt Spieler 2 mit Start ein.",
        },
        new GameItem
        {
            Key = "pirates", Title = "LEGO Pirates of the Caribbean", Year = "2011",
            Description = "Die vier Filme mit Jack Sparrow als LEGO-Abenteuer. Der Online-Koop ist in Arbeit.",
        },
        new GameItem
        {
            Key = "marvel", Title = "LEGO Marvel Super Heroes", Year = "2013",
            Description = "Iron Man, Hulk, Spider-Man und über 150 Helden in einem offenen New York. Der Online-Koop ist in Arbeit.",
        },
        new GameItem
        {
            Key = "hp", Title = "LEGO Harry Potter: Years 1-4", Year = "2010",
            Description = "Die ersten vier Jahre in Hogwarts, mit Zaubersprüchen und Tränken. Der Online-Koop ist in Arbeit.",
        },
    };
}
