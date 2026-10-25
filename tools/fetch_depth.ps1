# Downloads the optional depth module (ONNX Runtime with DirectML, DirectML, Depth Anything V2 Small)
# into the "depth" folder next to the project root and next to bin\Release\Muon.exe.
# Usage: powershell -ExecutionPolicy Bypass -File tools\fetch_depth.ps1
$ErrorActionPreference = 'Stop'
$ProgressPreference = 'SilentlyContinue'
$root = Split-Path -Parent $PSScriptRoot

$sources = @(
    @{ url = 'https://api.nuget.org/v3-flatcontainer/microsoft.ml.onnxruntime.directml/1.24.4/microsoft.ml.onnxruntime.directml.1.24.4.nupkg'
       files = @{ 'runtimes/win-x64/native/onnxruntime.dll' = 'onnxruntime.dll'
                  'runtimes/win-x64/native/onnxruntime_providers_shared.dll' = 'onnxruntime_providers_shared.dll' } },
    @{ url = 'https://api.nuget.org/v3-flatcontainer/microsoft.ai.directml/1.15.4/microsoft.ai.directml.1.15.4.nupkg'
       files = @{ 'bin/x64-win/DirectML.dll' = 'DirectML.dll' } },
    @{ url = 'https://huggingface.co/onnx-community/depth-anything-v2-small/resolve/main/onnx/model_fp16.onnx'
       out = 'depth_anything_v2_small_fp16.onnx' }
)
# pinned so a changed download is noticed
$hashes = @{
    'onnxruntime.dll' = 'e7eedec6a6f26dc39dc948276a75ef6d2bee3fff944d874ceed0bbd3b97bff40'
    'onnxruntime_providers_shared.dll' = '265c8daf29637cb259cac8be9f08f2cd45f3883f0f0e4949cbfddd5b4cbec3b6'
    'DirectML.dll' = '9c9e6d822561c6c41b90e6994b3e8857cf1d66dbfb1e0c4c799c7c89b4e92da1'
    'depth_anything_v2_small_fp16.onnx' = '2df6223f206b5164e21f664ace61dabeb9bb6a49b8b5a3e00510b4807d0f5b04'
}

$dest = "$root\depth"
$tmp = Join-Path ([IO.Path]::GetTempPath()) ("muon-depth-" + [Guid]::NewGuid().ToString('N'))
New-Item -ItemType Directory -Force $dest, $tmp | Out-Null
Add-Type -AssemblyName System.IO.Compression.FileSystem
try {
    foreach ($s in $sources) {
        if ($s.out) {
            if (Test-Path "$dest\$($s.out)") { continue }
            Write-Host "Downloading $($s.out)"
            Invoke-WebRequest $s.url -OutFile "$dest\$($s.out)"
            continue
        }
        $need = $s.files.Values | Where-Object { -not (Test-Path "$dest\$_") }
        if (-not $need) { continue }
        $pkg = "$tmp\pkg.zip"
        Write-Host "Downloading $($s.url.Split('/')[-1])"
        Invoke-WebRequest $s.url -OutFile $pkg
        $zip = [IO.Compression.ZipFile]::OpenRead($pkg)
        try {
            foreach ($k in $s.files.Keys) {
                $e = $zip.GetEntry($k)
                if (-not $e) { throw "$k not found in package" }
                [IO.Compression.ZipFileExtensions]::ExtractToFile($e, "$dest\$($s.files[$k])", $true)
            }
        } finally { $zip.Dispose() }
        Remove-Item $pkg
    }
    foreach ($f in $hashes.Keys) {
        $h = (Get-FileHash "$dest\$f" -Algorithm SHA256).Hash.ToLower()
        if ($h -ne $hashes[$f]) { Remove-Item "$dest\$f"; throw "$f has an unexpected SHA-256 ($h), removed" }
    }
} finally { Remove-Item -Recurse -Force $tmp -ErrorAction SilentlyContinue }

if (Test-Path "$root\bin\Release") {
    New-Item -ItemType Directory -Force "$root\bin\Release\depth" | Out-Null
    Copy-Item "$dest\*" "$root\bin\Release\depth" -Force
}
Write-Host "Depth module ready in $dest"
