<#
.SYNOPSIS
    Download and prepare FFTW3 prebuilt libraries for Windows x64.

.DESCRIPTION
    Downloads the official FFTW 3.3.5 prebuilt 64-bit DLLs from fftw.org,
    extracts them, and generates MSVC import libraries (.lib) using lib.exe.
    The result is placed in third_party/fftw3/ ready for CMake.

    Requires the MSVC toolchain (lib.exe). On CI, the environment is already
    configured by ilammy/msvc-dev-cmd, so lib.exe is on PATH. Otherwise the
    MSVC environment is imported from vcvars64.bat, located via vswhere
    (version/edition-agnostic — works with VS 2022, 2026, and newer).

.EXAMPLE
    .\setup-fftw.ps1
#>

$ErrorActionPreference = "Stop"

$FftwUrl   = "https://fftw.org/pub/fftw/fftw-3.3.5-dll64.zip"
$OutDir    = "third_party\fftw3"
$ZipFile   = "third_party\fftw3-dll64.zip"
# Locate vcvars64.bat in a version/edition-agnostic way. vswhere lives at a
# stable path and finds any installed VS (2022, 2026, …); fall back to known
# hardcoded locations for environments without it.  Only used when lib.exe is
# not already on PATH (see below) — on CI msvc-dev-cmd has already set it up.
$VsVars  = $null
$vswhere = "${env:ProgramFiles(x86)}\Microsoft Visual Studio\Installer\vswhere.exe"
if (Test-Path $vswhere) {
    $vsInstall = & $vswhere -latest -products * `
        -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 `
        -property installationPath 2>$null | Select-Object -First 1
    if ($vsInstall) {
        $candidate = Join-Path $vsInstall "VC\Auxiliary\Build\vcvars64.bat"
        if (Test-Path $candidate) { $VsVars = $candidate }
    }
}
if (-not $VsVars) {
    foreach ($p in @(
        "C:\Program Files\Microsoft Visual Studio\2022\Community\VC\Auxiliary\Build\vcvars64.bat",
        "C:\Program Files\Microsoft Visual Studio\2022\Enterprise\VC\Auxiliary\Build\vcvars64.bat",
        "C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\VC\Auxiliary\Build\vcvars64.bat"
    )) {
        if (Test-Path $p) { $VsVars = $p; break }
    }
}

# ── Check if already set up ──────────────────────────────────────────────
if ((Test-Path "$OutDir\lib\fftw3.lib") -and (Test-Path "$OutDir\lib\fftw3f.lib")) {
    Write-Host "FFTW3 already set up in $OutDir (double + float)" -ForegroundColor Green
    exit 0
}

# ── Create directories ───────────────────────────────────────────────────
New-Item -ItemType Directory -Force -Path "third_party" | Out-Null
New-Item -ItemType Directory -Force -Path $OutDir | Out-Null
New-Item -ItemType Directory -Force -Path "$OutDir\lib" | Out-Null
New-Item -ItemType Directory -Force -Path "$OutDir\include" | Out-Null
New-Item -ItemType Directory -Force -Path "$OutDir\bin" | Out-Null

# ── Download ─────────────────────────────────────────────────────────────
if (-not (Test-Path $ZipFile)) {
    Write-Host "Downloading FFTW3 prebuilt DLLs..." -ForegroundColor Cyan
    Invoke-WebRequest -Uri $FftwUrl -OutFile $ZipFile
}

# ── Extract ──────────────────────────────────────────────────────────────
Write-Host "Extracting..." -ForegroundColor Cyan
$tempDir = "third_party\fftw3-temp"
if (Test-Path $tempDir) { Remove-Item -Recurse -Force $tempDir }
Expand-Archive -Path $ZipFile -DestinationPath $tempDir -Force

# ── Copy files ───────────────────────────────────────────────────────────
Copy-Item "$tempDir\fftw3.h" "$OutDir\include\"
Copy-Item "$tempDir\libfftw3-3.dll" "$OutDir\bin\"
Copy-Item "$tempDir\libfftw3-3.def" "$OutDir\lib\"
# Float precision (fftw3f) — needed by libspecbleach (NR4)
Copy-Item "$tempDir\libfftw3f-3.dll" "$OutDir\bin\" -ErrorAction SilentlyContinue
Copy-Item "$tempDir\libfftw3f-3.def" "$OutDir\lib\" -ErrorAction SilentlyContinue

# ── Ensure the MSVC environment, then generate .lib ──────────────────────
Write-Host "Generating MSVC import library..." -ForegroundColor Cyan

# If lib.exe is already on PATH (CI: ilammy/msvc-dev-cmd ran earlier), use it
# directly. Otherwise import the MSVC env from the vcvars64.bat located above.
if (-not (Get-Command lib.exe -ErrorAction SilentlyContinue)) {
    if (-not $VsVars) {
        Write-Error "MSVC toolchain not found: lib.exe is not on PATH and no Visual Studio installation was located (checked vswhere and known VS2022 paths). Install the VC++ build tools or run from a Developer prompt."
        exit 1
    }
    Write-Host "Importing MSVC environment from $VsVars" -ForegroundColor Cyan
    $envVars = & cmd.exe /c "`"$VsVars`" >nul 2>&1 && set" 2>&1
    foreach ($line in $envVars) {
        if ($line -match "^([^=]+)=(.*)$") {
            [System.Environment]::SetEnvironmentVariable($matches[1], $matches[2], "Process")
        }
    }
}

# Generate .lib from .def (double and float precision)
Push-Location "$OutDir\lib"
& lib.exe /machine:x64 /def:libfftw3-3.def /out:fftw3.lib 2>&1 | Out-Null
if (Test-Path "libfftw3f-3.def") {
    & lib.exe /machine:x64 /def:libfftw3f-3.def /out:fftw3f.lib 2>&1 | Out-Null
}
Pop-Location

if (-not (Test-Path "$OutDir\lib\fftw3.lib")) {
    Write-Error "Failed to generate fftw3.lib"
    exit 1
}

# ── Cleanup ──────────────────────────────────────────────────────────────
Remove-Item -Recurse -Force $tempDir
Remove-Item -Force $ZipFile

Write-Host "FFTW3 ready in $OutDir" -ForegroundColor Green
Write-Host "  Header: $OutDir\include\fftw3.h"
Write-Host "  Lib:    $OutDir\lib\fftw3.lib"
Write-Host "  DLL:    $OutDir\bin\libfftw3-3.dll"
