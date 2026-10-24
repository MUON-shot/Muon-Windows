# Builds the release zip: dist\Muon-<version>-win64.zip
# Usage: powershell -ExecutionPolicy Bypass -File tools\package.ps1   (after build.bat)
$ErrorActionPreference = 'Stop'
$root = Split-Path -Parent $PSScriptRoot
$ver = (Select-String -Path "$root\src\version.h" -Pattern 'MUON_VERSION_STR "([^"]+)"').Matches[0].Groups[1].Value
$exe = "$root\bin\Release\Muon.exe"
if (-not (Test-Path $exe)) { throw "Muon.exe not found, run build.bat first" }

$stage = "$root\dist\Muon-$ver"
Remove-Item -Recurse -Force $stage -ErrorAction SilentlyContinue
New-Item -ItemType Directory -Force "$stage\licenses" | Out-Null
Copy-Item $exe $stage
Copy-Item "$root\LICENSE" "$stage\LICENSE.txt"
Copy-Item "$root\THIRD_PARTY_NOTICES.md" $stage
Copy-Item "$root\licenses\*" "$stage\licenses"
Copy-Item "$root\docs\README-release.txt" "$stage\README.txt"

$zip = "$root\dist\Muon-$ver-win64.zip"
Remove-Item $zip -ErrorAction SilentlyContinue
Compress-Archive -Path "$stage\*" -DestinationPath $zip
$hash = (Get-FileHash $zip -Algorithm SHA256).Hash.ToLower()
"$hash  Muon-$ver-win64.zip" | Out-File -Encoding ascii "$root\dist\Muon-$ver-win64.zip.sha256"
Write-Host "Created $zip"
Write-Host "SHA-256 $hash"
