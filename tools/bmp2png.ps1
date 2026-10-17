# Wandelt alle BMPs in einem Ordner in verkleinerte PNGs um: bmp2png.ps1 <ordner> [breite]
param([string]$dir, [int]$width = 960)
Add-Type -AssemblyName System.Drawing
Get-ChildItem $dir -Filter *.bmp | ForEach-Object {
  $img = [System.Drawing.Image]::FromFile($_.FullName)
  $h = [int]($img.Height * $width / $img.Width)
  $bmp = New-Object System.Drawing.Bitmap $width, $h
  $g = [System.Drawing.Graphics]::FromImage($bmp)
  $g.DrawImage($img, 0, 0, $width, $h)
  $out = [IO.Path]::ChangeExtension($_.FullName, '.png')
  $bmp.Save($out, [System.Drawing.Imaging.ImageFormat]::Png)
  $g.Dispose(); $bmp.Dispose(); $img.Dispose()
  Remove-Item $_.FullName
}
