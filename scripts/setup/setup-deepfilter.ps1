<#
.SYNOPSIS
    Build DeepFilterNet3 libdf library for Windows x64.

.DESCRIPTION
    Clones the DeepFilterNet repository, builds the C API library (DLL) using
    Cargo with the MSVC target, and copies the output to third_party/deepfilter/
    ready for CMake. Also generates a MinGW import library if dlltool is available.

    Requires: Rust toolchain (cargo), git, Visual Studio 2022 (for MSVC target).

.EXAMPLE
    .\setup-deepfilter.ps1
#>

$ErrorActionPreference = "Stop"

$DfnrCommit = "d375b2d8309e0935d165700c91da9de862a99c31"
$DfnrRepo   = "https://github.com/Rikorose/DeepFilterNet.git"
$OutDir     = "third_party\deepfilter"
$LibDir     = "$OutDir\lib\windows-x86_64"
$ModelName  = "DeepFilterNet3_onnx.tar.gz"
$RustTarget = "x86_64-pc-windows-msvc"

# ── Check if already set up ─────────────────────────────────────────────
if ((Test-Path "$LibDir\deepfilter.dll") -and (Test-Path "$OutDir\models\$ModelName")) {
    Write-Host "DeepFilterNet3 already set up in $LibDir" -ForegroundColor Green
    exit 0
}

# ── Try downloading pre-built binary ────────────────────────────────────
$ReleaseRepo = if ($env:DFNR_RELEASE_REPO) { $env:DFNR_RELEASE_REPO } else { "ten9876/AetherSDR" }
$ReleaseTag  = "dfnr-libs"
$Platform    = "windows-x86_64"
$Tarball     = "libdeepfilter-$Platform.tar.gz"
$DownloadUrl = "https://github.com/$ReleaseRepo/releases/download/$ReleaseTag/$Tarball"

Write-Host "Trying pre-built binary from $DownloadUrl ..." -ForegroundColor Cyan
try {
    $TarPath = Join-Path $env:TEMP $Tarball
    Invoke-WebRequest -Uri $DownloadUrl -OutFile $TarPath -UseBasicParsing -ErrorAction Stop
    if (-not (Test-Path $OutDir)) { New-Item -ItemType Directory -Force -Path $OutDir | Out-Null }
    tar xzf $TarPath -C $OutDir
    Remove-Item $TarPath -ErrorAction SilentlyContinue
    if (Test-Path "$LibDir\deepfilter.dll") {
        Write-Host "DeepFilterNet3 ready (pre-built) in $LibDir" -ForegroundColor Green
        exit 0
    }
    Write-Host "Download succeeded but DLL not found — falling back to source build" -ForegroundColor Yellow
} catch {
    Write-Host "Pre-built binary not available — falling back to source build" -ForegroundColor Yellow
}

# ── Check prerequisites ─────────────────────────────────────────────────
if (-not (Get-Command cargo -ErrorAction SilentlyContinue)) {
    Write-Error "Rust toolchain not found. Install from https://rustup.rs"
    exit 1
}

# Ensure MSVC target is installed
& rustup target add $RustTarget 2>&1 | Out-Null

if (-not (Get-Command cargo-cbuild -ErrorAction SilentlyContinue)) {
    Write-Host "Installing cargo-c..." -ForegroundColor Cyan
    & cargo install cargo-c
}

# ── Clone and build ─────────────────────────────────────────────────────
$TempDir = Join-Path $env:TEMP "deepfilter-build-$(Get-Random)"
try {
    Write-Host "Cloning DeepFilterNet at $DfnrCommit..." -ForegroundColor Cyan
    & git clone --depth 50 $DfnrRepo "$TempDir\DeepFilterNet"
    Push-Location "$TempDir\DeepFilterNet"
    & git checkout $DfnrCommit

    Write-Host "Building libdf for Windows x64 (MSVC)..." -ForegroundColor Cyan
    & cargo cbuild --release -p deep_filter --features capi --target $RustTarget
    if ($LASTEXITCODE -ne 0) { throw "cargo cbuild failed" }
    Pop-Location

    # ── Locate outputs ───────────────────────────────────────────────────
    $BuildDir = "$TempDir\DeepFilterNet\target\$RustTarget\release"

    if (-not (Test-Path "$BuildDir\deepfilter.dll")) {
        throw "Build succeeded but deepfilter.dll not found in $BuildDir"
    }

    # ── Copy outputs ─────────────────────────────────────────────────────
    New-Item -ItemType Directory -Force -Path $LibDir | Out-Null
    New-Item -ItemType Directory -Force -Path "$OutDir\include" | Out-Null
    New-Item -ItemType Directory -Force -Path "$OutDir\models" | Out-Null

    # DLL
    Copy-Item "$BuildDir\deepfilter.dll" "$LibDir\"
    Write-Host "  DLL: $LibDir\deepfilter.dll" -ForegroundColor Green

    # MSVC import library
    if (Test-Path "$BuildDir\deepfilter.dll.lib") {
        Copy-Item "$BuildDir\deepfilter.dll.lib" "$LibDir\"
        Write-Host "  Import lib (MSVC): $LibDir\deepfilter.dll.lib" -ForegroundColor Green
    }
    # Also copy static lib for MSVC linking
    if (Test-Path "$BuildDir\deepfilter.lib") {
        Copy-Item "$BuildDir\deepfilter.lib" "$LibDir\"
    }

    # Generate MinGW import library if dlltool is available
    if (Get-Command dlltool -ErrorAction SilentlyContinue) {
        Push-Location $LibDir
        & gendef deepfilter.dll 2>&1 | Out-Null
        & dlltool -d deepfilter.def -l libdeepfilter.dll.a -D deepfilter.dll 2>&1 | Out-Null
        Write-Host "  Import lib (MinGW): $LibDir\libdeepfilter.dll.a" -ForegroundColor Green
        Pop-Location
    }

    # Header
    $Header = "$BuildDir\include\deep_filter\deep_filter.h"
    if (Test-Path $Header) {
        Copy-Item $Header "$OutDir\include\"
    }

    # Model
    $ModelSrc = "$TempDir\DeepFilterNet\models\$ModelName"
    if (Test-Path $ModelSrc) {
        Copy-Item $ModelSrc "$OutDir\models\"
        Write-Host "  Model: $OutDir\models\$ModelName" -ForegroundColor Green
    }

    # Commit hash
    $DfnrCommit | Out-File -Encoding ascii -NoNewline "$OutDir\COMMIT"

    Write-Host "DeepFilterNet3 ready in $LibDir" -ForegroundColor Green
}
finally {
    if (Test-Path $TempDir) {
        Remove-Item -Recurse -Force $TempDir -ErrorAction SilentlyContinue
    }
}
