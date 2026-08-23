<#
.SYNOPSIS
    Download and build PortAudio for Windows x64.

.DESCRIPTION
    Downloads PortAudio v19.7.0 source from GitHub, builds the static library
    with CMake + MSVC (WASAPI, WDM-KS, DirectSound, and MME host APIs), and
    places headers/lib in third_party/portaudio/ ready for CMake.

    Required for the callback-model CW sidetone sink (CwSidetonePortAudioSink)
    and its WASAPI host-API preference (#3193). Without it the Windows build
    silently falls back to the push-model QAudioSink sidetone path.

.EXAMPLE
    .\setup-portaudio.ps1
#>

$ErrorActionPreference = "Stop"
. "$PSScriptRoot\_verify_sha256.ps1"

$PaVersion = "19.7.0"
$PaUrl     = "https://github.com/PortAudio/portaudio/archive/refs/tags/v${PaVersion}.tar.gz"
# SHA256 of the GitHub source archive. Bump alongside the version.
$PaSha256  = "5af29ba58bbdbb7bbcefaaecc77ec8fc413f0db6f4c4e286c40c3e1b83174fa0"
$OutDir    = "third_party\portaudio"
$TarFile   = "third_party\portaudio-${PaVersion}.tar.gz"

# ── Check if already set up ──────────────────────────────────────────────
if (Test-Path "$OutDir\lib\portaudio_static_x64.lib") {
    Write-Host "PortAudio already set up in $OutDir" -ForegroundColor Green
    exit 0
}

# ── Create directories ───────────────────────────────────────────────────
New-Item -ItemType Directory -Force -Path "third_party" | Out-Null
New-Item -ItemType Directory -Force -Path $OutDir | Out-Null
New-Item -ItemType Directory -Force -Path "$OutDir\lib" | Out-Null
New-Item -ItemType Directory -Force -Path "$OutDir\include" | Out-Null

# ── Download source ─────────────────────────────────────────────────────
if (-not (Test-Path $TarFile)) {
    Write-Host "Downloading PortAudio ${PaVersion} source..." -ForegroundColor Cyan
    Invoke-WebRequest -Uri $PaUrl -OutFile $TarFile
    Confirm-Sha256 -Path $TarFile -Expected $PaSha256
}

# ── Extract ──────────────────────────────────────────────────────────────
Write-Host "Extracting..." -ForegroundColor Cyan
$tempDir = "third_party\portaudio-temp"
if (Test-Path $tempDir) { Remove-Item -Recurse -Force $tempDir }
New-Item -ItemType Directory -Force -Path $tempDir | Out-Null
tar -xzf $TarFile -C $tempDir 2>$null

$srcDir = Get-ChildItem "$tempDir\portaudio-*" -Directory | Select-Object -First 1
if (-not $srcDir) {
    Write-Error "Failed to locate extracted PortAudio source"
    exit 1
}

# ── Build with CMake + MSVC ──────────────────────────────────────────────
Write-Host "Building PortAudio from source with MSVC..." -ForegroundColor Cyan

$buildDir = "$($srcDir.FullName)\build"
# CMAKE_POLICY_VERSION_MINIMUM: v19.7.0's CMakeLists declares a
# cmake_minimum_required below 3.5, which CMake 4.x refuses outright — same
# situation and same fix as setup-hidapi.ps1. The flag MUST be quoted:
# PowerShell's native-argument tokenizer splits an unquoted -Dkey=3.5 at
# the dot, so CMake receives "3" and rejects it.
cmake -B $buildDir -S $srcDir.FullName -G "Ninja" `
    -DCMAKE_BUILD_TYPE=Release `
    -DPA_BUILD_SHARED=OFF `
    -DPA_BUILD_STATIC=ON `
    -DPA_BUILD_EXAMPLES=OFF `
    -DPA_BUILD_TESTS=OFF `
    "-DCMAKE_POLICY_VERSION_MINIMUM=3.5"

cmake --build $buildDir --config Release -j $env:NUMBER_OF_PROCESSORS

# ── Find and copy built artifacts ────────────────────────────────────────
$libFile = Get-ChildItem "$buildDir" -Recurse -Filter "portaudio_static_x64.lib" | Select-Object -First 1
if (-not $libFile) {
    Write-Error "Failed to build portaudio_static_x64.lib"
    exit 1
}

Copy-Item $libFile.FullName "$OutDir\lib\portaudio_static_x64.lib"
# Public header plus the pa_win_* host-API headers (WASAPI stream options etc.)
Copy-Item "$($srcDir.FullName)\include\*.h" "$OutDir\include\"

# ── Cleanup ──────────────────────────────────────────────────────────────
Remove-Item -Recurse -Force $tempDir
Remove-Item -Force $TarFile

Write-Host "PortAudio ready in $OutDir" -ForegroundColor Green
Write-Host "  Header: $OutDir\include\portaudio.h"
Write-Host "  Lib:    $OutDir\lib\portaudio_static_x64.lib"
