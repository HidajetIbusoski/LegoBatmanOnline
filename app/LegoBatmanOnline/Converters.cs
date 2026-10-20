using System.Globalization;
using System.Windows;
using System.Windows.Data;

namespace LegoBatmanOnline;

// Sichtbar, wenn alle gebundenen Werte true sind (z. B. Freund + LEGO Batman 2 gewaehlt)
public sealed class AllTrueVisibilityConverter : IMultiValueConverter
{
    public object Convert(object[] values, Type targetType, object parameter, CultureInfo culture) =>
        values.All(v => v is true) ? Visibility.Visible : Visibility.Collapsed;

    public object[] ConvertBack(object value, Type[] targetTypes, object parameter, CultureInfo culture) =>
        throw new NotSupportedException();
}
