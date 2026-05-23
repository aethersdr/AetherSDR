<#
.SYNOPSIS
    Download and prepare FFTW3 prebuilt libraries for Windows x64.

.DESCRIPTION
    Downloads the official FFTW 3.3.5 prebuilt 64-bit DLLs from fftw.org,
    extracts them, and generates MSVC import libraries (.lib) using lib.exe.
    The result is placed in third_party/fftw3/ ready for CMake.

    Requires Visual Studio 2022 (for lib.exe).

.EXAMPLE
    .\setup-fftw.ps1
#>

$ErrorActionPreference = "Stop"

$FftwUrl   = "https://fftw.org/pub/fftw/fftw-3.3.5-dll64.zip"
$OutDir    = "third_party\fftw3"
$ZipFile   = "third_party\fftw3-dll64.zip"
$VsVars    = "C:\Program Files\Microsoft Visual Studio\2022\Community\VC\Auxiliary\Build\vcvars64.bat"
# Also try Enterprise (GitHub CI) and BuildTools (winget)
if (-not (Test-Path $VsVars)) {
    $VsVars = "C:\Program Files\Microsoft Visual Studio\2022\Enterprise\VC\Auxiliary\Build\vcvars64.bat"
}
if (-not (Test-Path $VsVars)) {
    $VsVars = "C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\VC\Auxiliary\Build\vcvars64.bat"
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

# ── Import MSVC environment and generate .lib ────────────────────────────
Write-Host "Generating MSVC import library..." -ForegroundColor Cyan

if (-not (Test-Path $VsVars)) {
    Write-Error "Visual Studio 2022 not found at: $VsVars"
    exit 1
}

# Import MSVC env
$envVars = & cmd.exe /c "`"$VsVars`" >nul 2>&1 && set" 2>&1
foreach ($line in $envVars) {
    if ($line -match "^([^=]+)=(.*)$") {
        [System.Environment]::SetEnvironmentVariable($matches[1], $matches[2], "Process")
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
