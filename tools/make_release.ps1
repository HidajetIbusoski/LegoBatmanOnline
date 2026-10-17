# Baut das Paket fuer Mitspieler: dist\LegoBatmanOnline.zip
#   LegoBatmanOnline.exe   die App (enthaelt Launcher und Mod-DLL, braucht kein installiertes .NET)
#   ANLEITUNG.txt
#   Extras\                Konsolen-Launcher fuer den Notfall (direkt per IP, z. B. ueber Radmin VPN)
$root = Split-Path -Parent $PSScriptRoot
& cmd /c "$root\build.bat"
if ($LASTEXITCODE -ne 0) { throw "Build fehlgeschlagen" }
$app = Join-Path $root "app\LegoBatmanOnline"
$pub = Join-Path $root "build\app"
& dotnet publish "$app\LegoBatmanOnline.csproj" -c Release -o $pub --nologo
if ($LASTEXITCODE -ne 0) { throw "App-Build fehlgeschlagen" }

$out = Join-Path $root "dist\LegoBatmanOnline"
Remove-Item -Recurse -Force (Join-Path $root "dist") -ErrorAction SilentlyContinue
New-Item -ItemType Directory -Force "$out\Extras" | Out-Null
Copy-Item "$pub\LegoBatmanOnline.exe" "$out\LegoBatmanOnline.exe"
Copy-Item "$root\build\lbo-launch.exe" "$out\Extras\LegoBatmanOnline-Konsole.exe"
Copy-Item "$root\build\lbo.dll" "$out\Extras\lbo.dll"
$text = Get-Content -Raw -Encoding UTF8 "$root\release\ANLEITUNG.txt"
[IO.File]::WriteAllText("$out\ANLEITUNG.txt", ($text -replace "`r?`n", "`r`n"), (New-Object Text.UTF8Encoding $true))
Compress-Archive -Path "$out\*" -DestinationPath (Join-Path $root "dist\LegoBatmanOnline.zip") -Force
Get-ChildItem (Join-Path $root "dist") -Recurse | Select-Object FullName, Length
