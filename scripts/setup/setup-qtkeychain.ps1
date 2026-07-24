<#
.SYNOPSIS
    Download and build qtkeychain for Windows x64.

.DESCRIPTION
    Downloads qtkeychain 0.16.0 source from GitHub, builds it with CMake + MSVC
    against the Qt6 installation pointed to by QT_ROOT_DIR (set by install-qt-action
    in CI, or by a local Qt installation), and installs it into third_party/qtkeychain/
    ready for CMake find_package(Qt6Keychain).

    Required for SmartLink credential persistence (saves the Auth0 refresh token
    in the Windows Credential Manager so users are not prompted on every launch).

.EXAMPLE
    .\setup-qtkeychain.ps1
#>

$ErrorActionPreference = "Stop"
. "$PSScriptRoot\_verify_sha256.ps1"

$QtKeychainVersion = "0.16.0"
$QtKeychainUrl     = "https://github.com/frankosterfeld/qtkeychain/archive/refs/tags/${QtKeychainVersion}.tar.gz"
# SHA256 of the GitHub source archive (#3665). Bump alongside the version.
$QtKeychainSha256  = "3be26ec4ae30eecf0c2ff7572ba83799791b157c76e15a05ef35f23dc25e4054"
$OutDir            = "$PSScriptRoot\..\..\third_party\qtkeychain"
$TarFile           = "$PSScriptRoot\..\..\third_party\qtkeychain-${QtKeychainVersion}.tar.gz"

# Normalize to absolute paths
$OutDir  = [System.IO.Path]::GetFullPath($OutDir)
$TarFile = [System.IO.Path]::GetFullPath($TarFile)

# ── Check if already set up ──────────────────────────────────────────────
if (Test-Path "$OutDir\lib\cmake\Qt6Keychain\Qt6KeychainConfig.cmake") {
    Write-Host "qtkeychain already set up in $OutDir" -ForegroundColor Green
    exit 0
}

# ── Locate Qt ────────────────────────────────────────────────────────────
# Resolve the Qt kit prefix from the first source that is set, most specific
# first: install-qt-action's QT_ROOT_DIR, then Qt6_DIR (three levels up), then
# an already-exported CMAKE_PREFIX_PATH (first entry of a ;-list), then qmake on
# PATH (<prefix>/bin/qmake -> <prefix>). This keeps a local build working
# whether or not the operator exported QT_ROOT_DIR.
$QtPrefix = $env:QT_ROOT_DIR
if (-not $QtPrefix -and $env:Qt6_DIR) {
    $QtPrefix = [System.IO.Path]::GetFullPath((Join-Path $env:Qt6_DIR "../../.."))
}
if (-not $QtPrefix -and $env:CMAKE_PREFIX_PATH) {
    $QtPrefix = ($env:CMAKE_PREFIX_PATH -split ';')[0]
}
if (-not $QtPrefix) {
    $qmake = Get-Command qmake -ErrorAction SilentlyContinue
    if ($qmake) {
        $QtPrefix = Split-Path -Parent (Split-Path -Parent $qmake.Source)
    }
}
if (-not $QtPrefix) {
    Write-Error "Qt not found. Set QT_ROOT_DIR, Qt6_DIR, or CMAKE_PREFIX_PATH, or put qmake on PATH before running this script."
    exit 1
}
Write-Host "Using Qt from: $QtPrefix" -ForegroundColor Cyan

# ── Create staging directory ─────────────────────────────────────────────
New-Item -ItemType Directory -Force -Path ([System.IO.Path]::GetFullPath("$OutDir\..\")) | Out-Null
New-Item -ItemType Directory -Force -Path $OutDir | Out-Null

# ── Download source ──────────────────────────────────────────────────────
if (-not (Test-Path $TarFile)) {
    Write-Host "Downloading qtkeychain ${QtKeychainVersion} source..." -ForegroundColor Cyan
    Invoke-WebRequest -Uri $QtKeychainUrl -OutFile $TarFile
    Confirm-Sha256 -Path $TarFile -Expected $QtKeychainSha256
}

# ── Extract ──────────────────────────────────────────────────────────────
Write-Host "Extracting..." -ForegroundColor Cyan
$TempDir = [System.IO.Path]::GetFullPath("$OutDir\..\qtkeychain-temp")
if (Test-Path $TempDir) { Remove-Item -Recurse -Force $TempDir }
New-Item -ItemType Directory -Force -Path $TempDir | Out-Null
tar -xzf $TarFile -C $TempDir
if ($LASTEXITCODE -ne 0) { Write-Error "tar extraction failed with exit code $LASTEXITCODE"; exit 1 }

$SrcDir = Get-ChildItem "$TempDir\qtkeychain-*" -Directory | Select-Object -First 1
if (-not $SrcDir) {
    Write-Error "Failed to locate extracted qtkeychain source directory in $TempDir"
    exit 1
}

# ── Build with CMake + MSVC ──────────────────────────────────────────────
Write-Host "Building qtkeychain ${QtKeychainVersion} from source..." -ForegroundColor Cyan

$BuildDir = Join-Path $SrcDir.FullName "build"
cmake -B $BuildDir -S $SrcDir.FullName -G "Ninja" `
    -DCMAKE_BUILD_TYPE=Release `
    -DBUILD_WITH_QT6=ON `
    -DBUILD_SHARED_LIBS=ON `
    -DBUILD_TRANSLATIONS=OFF `
    "-DCMAKE_PREFIX_PATH=$QtPrefix" `
    "-DCMAKE_INSTALL_PREFIX=$OutDir"

if ($LASTEXITCODE -ne 0) { Write-Error "CMake configure failed"; exit 1 }

cmake --build $BuildDir --config Release -j $env:NUMBER_OF_PROCESSORS
if ($LASTEXITCODE -ne 0) { Write-Error "CMake build failed"; exit 1 }

# ── Install to staging directory ─────────────────────────────────────────
# cmake --install places headers, lib, DLL, and cmake config files in the
# correct layout so find_package(Qt6Keychain) resolves against $OutDir.
cmake --install $BuildDir
if ($LASTEXITCODE -ne 0) { Write-Error "CMake install failed"; exit 1 }

# ── Cleanup ──────────────────────────────────────────────────────────────
Remove-Item -Recurse -Force $TempDir
Remove-Item -Force $TarFile

Write-Host "qtkeychain ready in $OutDir" -ForegroundColor Green
Write-Host "  cmake config: $OutDir\lib\cmake\Qt6Keychain\Qt6KeychainConfig.cmake"
if (Test-Path "$OutDir\bin\qt6keychain.dll") {
    Write-Host "  DLL:          $OutDir\bin\qt6keychain.dll"
}
