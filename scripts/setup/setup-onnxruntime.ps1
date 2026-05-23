<#
.SYNOPSIS
    Download ONNX Runtime prebuilt binaries for Windows x64.

.DESCRIPTION
    Downloads the official ONNX Runtime 1.18.x prebuilt release from GitHub
    and places headers / import library / DLL in third_party/onnxruntime/
    ready for CMake (find_library / find_path).

    Required only for the S_History_v2-AI CNN signal classifier.
    AetherSDR compiles and runs without it — ONNX is an optional enhancement.

.EXAMPLE
    .\setup-onnxruntime.ps1
#>

$ErrorActionPreference = "Stop"

$OrtVersion = "1.18.1"
$OrtUrl     = "https://github.com/microsoft/onnxruntime/releases/download/v${OrtVersion}/onnxruntime-win-x64-${OrtVersion}.zip"
$OutDir     = "third_party\onnxruntime"
$ZipFile    = "third_party\onnxruntime-win-x64-${OrtVersion}.zip"

# ── Check if already set up ──────────────────────────────────────────────
if (Test-Path "$OutDir\lib\onnxruntime.lib") {
    Write-Host "ONNX Runtime already set up in $OutDir" -ForegroundColor Green
    exit 0
}

# ── Create directories ───────────────────────────────────────────────────
New-Item -ItemType Directory -Force -Path "third_party" | Out-Null
New-Item -ItemType Directory -Force -Path $OutDir | Out-Null
New-Item -ItemType Directory -Force -Path "$OutDir\include" | Out-Null
New-Item -ItemType Directory -Force -Path "$OutDir\lib" | Out-Null
New-Item -ItemType Directory -Force -Path "$OutDir\bin" | Out-Null

# ── Download ─────────────────────────────────────────────────────────────
if (-not (Test-Path $ZipFile)) {
    Write-Host "Downloading ONNX Runtime ${OrtVersion} prebuilt package..." -ForegroundColor Cyan
    Invoke-WebRequest -Uri $OrtUrl -OutFile $ZipFile
}

# ── Extract ──────────────────────────────────────────────────────────────
Write-Host "Extracting..." -ForegroundColor Cyan
$tempDir = "third_party\onnxruntime-temp"
if (Test-Path $tempDir) { Remove-Item -Recurse -Force $tempDir }
Expand-Archive -Path $ZipFile -DestinationPath $tempDir -Force

$srcDir = Get-ChildItem "$tempDir\onnxruntime-win-x64-*" -Directory | Select-Object -First 1
if (-not $srcDir) {
    Write-Error "Could not find extracted onnxruntime directory in $tempDir"
    exit 1
}

# ── Copy headers ─────────────────────────────────────────────────────────
# ONNX Runtime packages keep headers directly in include/
Copy-Item "$($srcDir.FullName)\include\*" "$OutDir\include\" -Recurse -Force

# ── Copy lib / DLL ───────────────────────────────────────────────────────
# The prebuilt package puts .lib and .dll together in lib/
$libFile = Get-ChildItem "$($srcDir.FullName)\lib" -Filter "onnxruntime.lib" | Select-Object -First 1
$dllFiles = Get-ChildItem "$($srcDir.FullName)\lib" -Filter "*.dll"

if (-not $libFile) {
    Write-Error "onnxruntime.lib not found in extracted package"
    exit 1
}

Copy-Item $libFile.FullName "$OutDir\lib\onnxruntime.lib"
foreach ($dll in $dllFiles) {
    Copy-Item $dll.FullName "$OutDir\bin\$($dll.Name)"
}

# ── Cleanup ──────────────────────────────────────────────────────────────
Remove-Item -Recurse -Force $tempDir
Remove-Item -Force $ZipFile

Write-Host "ONNX Runtime ${OrtVersion} ready in $OutDir" -ForegroundColor Green
Write-Host "  Headers: $OutDir\include\"
Write-Host "  Lib:     $OutDir\lib\onnxruntime.lib"
Write-Host "  DLLs:    $OutDir\bin\"
Write-Host ""
Write-Host "Re-run CMake to pick up ONNX Runtime:" -ForegroundColor Yellow
Write-Host "  cmake -B build -G Ninja -DCMAKE_BUILD_TYPE=RelWithDebInfo"
