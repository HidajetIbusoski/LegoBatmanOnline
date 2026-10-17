# Speichert einen verkleinerten Screenshot des gesamten Desktops: desktop.ps1 <ausgabe.png>
param([string]$out)
Add-Type -AssemblyName System.Drawing, System.Windows.Forms
$b = [System.Windows.Forms.SystemInformation]::VirtualScreen
$bmp = New-Object System.Drawing.Bitmap $b.Width, $b.Height
$g = [System.Drawing.Graphics]::FromImage($bmp)
$g.CopyFromScreen($b.Left, $b.Top, 0, 0, $bmp.Size)
$w = 1280; $h = [int]($b.Height * $w / $b.Width)
$small = New-Object System.Drawing.Bitmap $bmp, $w, $h
$small.Save($out, [System.Drawing.Imaging.ImageFormat]::Png)
