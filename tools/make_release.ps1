# Baut das Paket fuer Mitspieler: dist\LegoBatmanOnline.zip (Launcher, DLL, Anleitung)
$root = Split-Path -Parent $PSScriptRoot
& cmd /c "$root\build.bat"
if ($LASTEXITCODE -ne 0) { throw "Build fehlgeschlagen" }
$out = Join-Path $root "dist\LegoBatmanOnline"
Remove-Item -Recurse -Force (Join-Path $root "dist") -ErrorAction SilentlyContinue
New-Item -ItemType Directory -Force $out | Out-Null
Copy-Item "$root\build\lbo-launch.exe" "$out\LegoBatmanOnline.exe"
Copy-Item "$root\build\lbo.dll" "$out\lbo.dll"
$text = Get-Content -Raw -Encoding UTF8 "$root\release\ANLEITUNG.txt"
[IO.File]::WriteAllText("$out\ANLEITUNG.txt", ($text -replace "`r?`n", "`r`n"), (New-Object Text.UTF8Encoding $true))
Compress-Archive -Path "$out\*" -DestinationPath (Join-Path $root "dist\LegoBatmanOnline.zip") -Force
Get-ChildItem (Join-Path $root "dist") -Recurse | Select-Object FullName, Length
