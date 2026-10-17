# Screenshot eines Fensters per PrintWindow, auch wenn es verdeckt ist: window_shot.ps1 <prozess-id> <ausgabe.png>
param([int]$procId, [string]$out)
Add-Type -AssemblyName System.Drawing
Add-Type @"
using System;
using System.Runtime.InteropServices;
public static class W {
    [StructLayout(LayoutKind.Sequential)] public struct RECT { public int L, T, R, B; }
    [DllImport("user32.dll")] public static extern bool GetWindowRect(IntPtr h, out RECT r);
    [DllImport("user32.dll")] public static extern bool PrintWindow(IntPtr h, IntPtr dc, uint flags);
}
"@
$h = (Get-Process -Id $procId).MainWindowHandle
$r = New-Object W+RECT
[W]::GetWindowRect($h, [ref]$r) | Out-Null
$bmp = New-Object System.Drawing.Bitmap ($r.R - $r.L), ($r.B - $r.T)
$g = [System.Drawing.Graphics]::FromImage($bmp)
$dc = $g.GetHdc()
[W]::PrintWindow($h, $dc, 2) | Out-Null
$g.ReleaseHdc($dc)
$bmp.Save($out, [System.Drawing.Imaging.ImageFormat]::Png)
