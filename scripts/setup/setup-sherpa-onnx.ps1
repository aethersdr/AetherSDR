<#
.SYNOPSIS
    Stage a prebuilt sherpa-onnx (C API) for Windows x64.

.DESCRIPTION
    Downloads k2-fsa's sherpa-onnx shared-library release (MD/Release, matching
    AetherSDR's dynamic-CRT build) and places the C-API import lib + DLLs under
    third_party/sherpa-onnx/ ready for CMake (which sets HAVE_SHERPA). Enables the
    optional "sherpa-onnx model..." Copy Assist backend. The bundle ships its own
    ONNX Runtime, which the app then shares.

    The "-lib" release ships binaries only; the C-API header is fetched from
    source at the matching tag. Keep the version in sync with
    setup-sherpa-onnx.sh (Linux/macOS).

.EXAMPLE
    .\setup-sherpa-onnx.ps1
#>

$ErrorActionPreference = "Stop"
. "$PSScriptRoot\_verify_sha256.ps1"

$Version = "1.13.4"
$Asset   = "sherpa-onnx-v${Version}-win-x64-shared-MD-Release-no-tts-lib"
$Url     = "https://github.com/k2-fsa/sherpa-onnx/releases/download/v${Version}/${Asset}.tar.bz2"
# SHA256 of the release tarball. Bump alongside the version.
$Sha256  = "dec41ab3944985cce39e596cb757732f1b275720d62f117fc5afe10f51c4bf7d"
$OutDir  = "third_party\sherpa-onnx"
$Tarball = "third_party\${Asset}.tar.bz2"

if (Test-Path "$OutDir\include\sherpa-onnx\c-api\c-api.h") {
    Write-Host "sherpa-onnx already staged in $OutDir" -ForegroundColor Green
    exit 0
}

New-Item -ItemType Directory -Force -Path "third_party" | Out-Null
New-Item -ItemType Directory -Force -Path "$OutDir\lib" | Out-Null
New-Item -ItemType Directory -Force -Path "$OutDir\include\sherpa-onnx\c-api" | Out-Null

if (-not (Test-Path $Tarball)) {
    Write-Host "Downloading ${Asset}.tar.bz2 ..." -ForegroundColor Cyan
    Invoke-WebRequest -Uri $Url -OutFile $Tarball
}
Confirm-Sha256 -Path $Tarball -Expected $Sha256

Write-Host "Extracting ..." -ForegroundColor Cyan
$tmp = "third_party\_sherpa_extract"
if (Test-Path $tmp) { Remove-Item -Recurse -Force $tmp }
New-Item -ItemType Directory -Force -Path $tmp | Out-Null
# Windows 10+ ships bsdtar, which auto-detects the bz2 compression.
tar -xf $Tarball -C $tmp
if ($LASTEXITCODE -ne 0) { Write-Error "tar extraction failed"; exit 1 }

$src = Get-ChildItem $tmp -Directory | Where-Object { $_.Name -like "sherpa-onnx-*" } | Select-Object -First 1
if (-not $src -or -not (Test-Path "$($src.FullName)\lib")) {
    Write-Error "unexpected archive layout under $tmp"
    exit 1
}
Copy-Item "$($src.FullName)\lib\*" "$OutDir\lib\" -Force
Remove-Item -Recurse -Force $tmp

Write-Host "Fetching C-API header ..." -ForegroundColor Cyan
Invoke-WebRequest `
    -Uri "https://raw.githubusercontent.com/k2-fsa/sherpa-onnx/v${Version}/sherpa-onnx/c-api/c-api.h" `
    -OutFile "$OutDir\include\sherpa-onnx\c-api\c-api.h"

Write-Host "sherpa-onnx $Version (win-x64) staged in $OutDir" -ForegroundColor Green
