<#
.SYNOPSIS
    Download and prepare libopus prebuilt libraries for Windows x64.

.DESCRIPTION
    Downloads the official Opus 1.5.2 release source, extracts the headers,
    and downloads a prebuilt x64 DLL+lib from the vcpkg export.
    The result is placed in third_party/opus/ ready for CMake.

    Required for SmartLink compressed audio (independent of RADE).

.EXAMPLE
    .\setup-opus.ps1
#>

$ErrorActionPreference = "Stop"

# Opus 1.5.2 source.  Primary mirror is GitHub Releases (Microsoft CDN,
# essentially never down); xiph.org is the official upstream and serves
# the same tarball but has been intermittently unreachable from GitHub-
# hosted Windows runners.  Try GitHub first, fall back to xiph, retry
# each with backoff before failing the build.
$OpusVersion = "1.5.2"
$OpusSrcUrls = @(
    "https://github.com/xiph/opus/releases/download/v${OpusVersion}/opus-${OpusVersion}.tar.gz",
    "https://downloads.xiph.org/releases/opus/opus-${OpusVersion}.tar.gz"
)
$OutDir      = "third_party\opus"
$TarFile     = "third_party\opus-${OpusVersion}.tar.gz"
$VsVars      = "C:\Program Files\Microsoft Visual Studio\2022\Community\VC\Auxiliary\Build\vcvars64.bat"
# Also try BuildTools edition (installed via winget)
if (-not (Test-Path $VsVars)) {
    $VsVars = "C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\VC\Auxiliary\Build\vcvars64.bat"
}

# ── Check if already set up ──────────────────────────────────────────────
if (Test-Path "$OutDir\lib\opus.lib") {
    Write-Host "Opus already set up in $OutDir" -ForegroundColor Green
    exit 0
}

# ── Create directories ───────────────────────────────────────────────────
New-Item -ItemType Directory -Force -Path "third_party" | Out-Null
New-Item -ItemType Directory -Force -Path $OutDir | Out-Null
New-Item -ItemType Directory -Force -Path "$OutDir\lib" | Out-Null
New-Item -ItemType Directory -Force -Path "$OutDir\include\opus" | Out-Null
New-Item -ItemType Directory -Force -Path "$OutDir\bin" | Out-Null

# ── Download source for headers ──────────────────────────────────────────
if (-not (Test-Path $TarFile)) {
    $downloaded = $false
    foreach ($url in $OpusSrcUrls) {
        for ($attempt = 1; $attempt -le 3; $attempt++) {
            Write-Host "Downloading Opus ${OpusVersion} from $url (attempt $attempt/3)..." -ForegroundColor Cyan
            try {
                Invoke-WebRequest -Uri $url -OutFile $TarFile -TimeoutSec 60
                $downloaded = $true
                break
            } catch {
                Write-Warning "Download failed: $_"
                if ($attempt -lt 3) {
                    Start-Sleep -Seconds (5 * $attempt)
                }
            }
        }
        if ($downloaded) { break }
    }
    if (-not $downloaded) {
        Write-Error "All Opus download attempts failed (tried $($OpusSrcUrls.Count) mirrors x 3 retries)"
        exit 1
    }
}

# ── Extract headers ──────────────────────────────────────────────────────
Write-Host "Extracting headers..." -ForegroundColor Cyan
$tempDir = "third_party\opus-temp"
if (Test-Path $tempDir) { Remove-Item -Recurse -Force $tempDir }
New-Item -ItemType Directory -Force -Path $tempDir | Out-Null
tar -xzf $TarFile -C $tempDir 2>$null

$srcDir = Get-ChildItem "$tempDir\opus-*" -Directory | Select-Object -First 1
Copy-Item "$($srcDir.FullName)\include\*.h" "$OutDir\include\opus\"

# ── Build static library from source using CMake + MSVC ──────────────────
Write-Host "Building Opus from source with MSVC..." -ForegroundColor Cyan

# Import MSVC env if not already set (CI uses ilammy/msvc-dev-cmd)
if (-not (Get-Command cl.exe -ErrorAction SilentlyContinue)) {
    if (-not (Test-Path $VsVars)) {
        # Try Enterprise/Professional editions
        $VsVars = $VsVars -replace "Community", "Enterprise"
        if (-not (Test-Path $VsVars)) {
            $VsVars = $VsVars -replace "Enterprise", "Professional"
        }
    }
    if (-not (Test-Path $VsVars)) {
        Write-Error "Visual Studio 2022 not found"
        exit 1
    }
    $envVars = & cmd.exe /c "`"$VsVars`" >nul 2>&1 && set" 2>&1
    foreach ($line in $envVars) {
        if ($line -match "^([^=]+)=(.*)$") {
            [System.Environment]::SetEnvironmentVariable($matches[1], $matches[2], "Process")
        }
    }
}

$buildDir = "$($srcDir.FullName)\build"
cmake -B $buildDir -S $srcDir.FullName -G "Ninja" `
    -DCMAKE_BUILD_TYPE=Release `
    -DBUILD_SHARED_LIBS=OFF `
    -DOPUS_BUILD_TESTING=OFF `
    -DOPUS_BUILD_PROGRAMS=OFF `
    -DOPUS_INSTALL_PKG_CONFIG_MODULE=OFF `
    -DOPUS_INSTALL_CMAKE_CONFIG_MODULE=OFF

cmake --build $buildDir --config Release -j $env:NUMBER_OF_PROCESSORS

# Find the built static library
$libFile = Get-ChildItem "$buildDir" -Recurse -Filter "opus.lib" | Select-Object -First 1
if (-not $libFile) {
    $libFile = Get-ChildItem "$buildDir" -Recurse -Filter "libopus.a" | Select-Object -First 1
}
if (-not $libFile) {
    Write-Error "Failed to build opus.lib"
    exit 1
}

Copy-Item $libFile.FullName "$OutDir\lib\opus.lib"

# ── Cleanup ──────────────────────────────────────────────────────────────
Remove-Item -Recurse -Force $tempDir
Remove-Item -Force $TarFile

Write-Host "Opus ready in $OutDir" -ForegroundColor Green
Write-Host "  Headers: $OutDir\include\opus\opus.h"
Write-Host "  Lib:     $OutDir\lib\opus.lib"
