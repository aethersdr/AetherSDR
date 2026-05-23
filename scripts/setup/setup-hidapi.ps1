<#
.SYNOPSIS
    Download and build hidapi for Windows x64.

.DESCRIPTION
    Downloads hidapi 0.14.0 source from GitHub, builds it with CMake + MSVC,
    and places headers/lib/dll in third_party/hidapi/ ready for CMake.

    Required for USB HID device support (Stream Deck, Icom RC-28,
    Griffin PowerMate, Contour Shuttle, etc.).

.EXAMPLE
    .\setup-hidapi.ps1
#>

$ErrorActionPreference = "Stop"

$HidapiVersion = "0.14.0"
$HidapiUrl     = "https://github.com/libusb/hidapi/archive/refs/tags/hidapi-${HidapiVersion}.tar.gz"
$OutDir        = "third_party\hidapi"
$TarFile       = "third_party\hidapi-${HidapiVersion}.tar.gz"

# ── Check if already set up ──────────────────────────────────────────────
if (Test-Path "$OutDir\lib\hidapi.lib") {
    Write-Host "hidapi already set up in $OutDir" -ForegroundColor Green
    exit 0
}

# ── Create directories ───────────────────────────────────────────────────
New-Item -ItemType Directory -Force -Path "third_party" | Out-Null
New-Item -ItemType Directory -Force -Path $OutDir | Out-Null
New-Item -ItemType Directory -Force -Path "$OutDir\lib" | Out-Null
New-Item -ItemType Directory -Force -Path "$OutDir\include\hidapi" | Out-Null
New-Item -ItemType Directory -Force -Path "$OutDir\bin" | Out-Null

# ── Download source ─────────────────────────────────────────────────────
if (-not (Test-Path $TarFile)) {
    Write-Host "Downloading hidapi ${HidapiVersion} source..." -ForegroundColor Cyan
    Invoke-WebRequest -Uri $HidapiUrl -OutFile $TarFile
}

# ── Extract ──────────────────────────────────────────────────────────────
Write-Host "Extracting..." -ForegroundColor Cyan
$tempDir = "third_party\hidapi-temp"
if (Test-Path $tempDir) { Remove-Item -Recurse -Force $tempDir }
New-Item -ItemType Directory -Force -Path $tempDir | Out-Null
tar -xzf $TarFile -C $tempDir 2>$null

$srcDir = Get-ChildItem "$tempDir\hidapi-hidapi-*" -Directory | Select-Object -First 1
if (-not $srcDir) {
    $srcDir = Get-ChildItem "$tempDir\hidapi-*" -Directory | Select-Object -First 1
}

# ── Copy headers ─────────────────────────────────────────────────────────
Copy-Item "$($srcDir.FullName)\hidapi\hidapi.h" "$OutDir\include\hidapi\"

# ── Build with CMake + MSVC ──────────────────────────────────────────────
Write-Host "Building hidapi from source with MSVC..." -ForegroundColor Cyan

$buildDir = "$($srcDir.FullName)\build"
cmake -B $buildDir -S $srcDir.FullName -G "Ninja" `
    -DCMAKE_BUILD_TYPE=Release `
    -DBUILD_SHARED_LIBS=ON

cmake --build $buildDir --config Release -j $env:NUMBER_OF_PROCESSORS

# ── Find and copy built artifacts ────────────────────────────────────────
# hidapi builds hidapi.dll + hidapi.lib (import lib) on Windows
$dllFile = Get-ChildItem "$buildDir" -Recurse -Filter "hidapi.dll" | Select-Object -First 1
$libFile = Get-ChildItem "$buildDir" -Recurse -Filter "hidapi.lib" | Select-Object -First 1

if (-not $libFile) {
    Write-Error "Failed to build hidapi.lib"
    exit 1
}

Copy-Item $libFile.FullName "$OutDir\lib\hidapi.lib"
if ($dllFile) {
    Copy-Item $dllFile.FullName "$OutDir\bin\hidapi.dll"
}

# ── Cleanup ──────────────────────────────────────────────────────────────
Remove-Item -Recurse -Force $tempDir
Remove-Item -Force $TarFile

Write-Host "hidapi ready in $OutDir" -ForegroundColor Green
Write-Host "  Header: $OutDir\include\hidapi\hidapi.h"
Write-Host "  Lib:    $OutDir\lib\hidapi.lib"
if ($dllFile) {
    Write-Host "  DLL:    $OutDir\bin\hidapi.dll"
}
