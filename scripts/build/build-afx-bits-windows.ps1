#!/usr/bin/env pwsh
<#
.SYNOPSIS
    Assemble the Windows AFX-bits pack (.zip) for the BNR feature.

.DESCRIPTION
    Produces afx-bits-2.1.0-windows-x86_64-sm_<arch>.zip with the exact layout
    NvidiaAfxFilter (Windows path) expects:

        bin/
          NVAudioEffects.dll              (AFX core)
          <denoiser feature DLL>          (from NGC nvidia/maxine via SDK script)
          cublas64_12.dll cublasLt64_12.dll cufft64_11.dll nvrtc64_120_0.dll
          nvinfer_10.dll
          libcrypto-3-x64.dll
        features/denoiser/models/sm_<arch>/denoiser_48k.trtpkg
        licenses/
        NOTICE.txt

    The script flattens the SDK's tree into bin/ so LOAD_WITH_ALTERED_SEARCH_PATH
    resolves the core's transitive deps from the pack itself — no host CUDA
    install required at runtime.

.PARAMETER SdkDir
    Path to the extracted Maxine Audio Effects SDK (the directory whose
    immediate children are bin/, features/, include/, lib/, ...).

.PARAMETER Arch
    Target SM architecture (sm_89 = ada / RTX 40-series).

.PARAMETER OutDir
    Where the staged tree and final .zip are written.

.PARAMETER NgcApiKey
    Optional NGC API key. If absent, falls back to env var NGC_API_KEY.

.EXAMPLE
    pwsh scripts/build/build-afx-bits-windows.ps1 `
        -SdkDir 'C:\nvidia\maxine-afx-sdk-2.1.0' `
        -Arch sm_89
#>

[CmdletBinding()]
param(
    [Parameter(Mandatory=$true)]
    [string]$SdkDir,

    [Parameter()]
    [string]$Arch = 'sm_89',

    [Parameter()]
    [string]$OutDir = "$(Join-Path $PSScriptRoot '../../build/afx-pack')",

    [Parameter()]
    [string]$NgcApiKey = $env:NGC_API_KEY,

    [Parameter()]
    [string]$Version = '2.1.0'
)

$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest

function Log($msg) { Write-Host "[afx-pack] $msg" -ForegroundColor Cyan }
function Die($msg) { Write-Error "[afx-pack] $msg"; exit 1 }

# ── Resolve inputs ──────────────────────────────────────────────────────────
$SdkDir = (Resolve-Path -LiteralPath $SdkDir).Path
if (-not (Test-Path -LiteralPath $SdkDir -PathType Container)) {
    Die "SDK directory not found: $SdkDir"
}

# Map sm_XX → SDK arch tag used by download_features.ps1.
$archTag = switch ($Arch) {
    'sm_75' { 'turing' }
    'sm_86' { 'ampere' }
    'sm_89' { 'ada' }
    'sm_90' { 'hopper' }
    default { Die "Unsupported arch: $Arch (expected sm_75/86/89/90)" }
}
Log "Target arch: $Arch (NGC tag: $archTag)"

# ── Locate SDK pieces ───────────────────────────────────────────────────────
$sdkBin = Join-Path $SdkDir 'bin'
$sdkExtCuda = Join-Path $sdkBin 'external/cuda/bin'
$sdkExtTrt  = Join-Path $sdkBin 'external/nvtrt/bin'
$sdkExtSsl  = Join-Path $sdkBin 'external/openssl/bin'
$dlScript   = Join-Path $SdkDir 'features/download_features.ps1'

foreach ($p in @($sdkBin, $sdkExtCuda, $sdkExtTrt, $sdkExtSsl, $dlScript)) {
    if (-not (Test-Path -LiteralPath $p)) { Die "Missing in SDK: $p" }
}

$core = Join-Path $sdkBin 'NVAudioEffects.dll'
if (-not (Test-Path -LiteralPath $core)) { Die "Missing core DLL: $core" }

# ── Run the SDK's feature downloader (fetches feature DLL + model from NGC) ─
if (-not $NgcApiKey) {
    Die "NGC API key not provided. Pass -NgcApiKey or set NGC_API_KEY env var."
}
$env:NGC_API_KEY = $NgcApiKey

Log "Running SDK download_features.ps1 (denoiser-48k / $archTag)"
Push-Location $SdkDir
try {
    # The SDK script accepts -features and -arch flags. Exact flag names vary
    # between SDK versions — adjust here if 2.1.0 differs from earlier ones.
    pwsh -NoProfile -File $dlScript -features 'denoiser-48k' -arch $archTag
    if ($LASTEXITCODE -ne 0) { Die "download_features.ps1 failed with $LASTEXITCODE" }
} finally {
    Pop-Location
}

# ── Stage the pack tree ─────────────────────────────────────────────────────
$packName = "afx-bits-$Version-windows-x86_64-$Arch"
$stage    = Join-Path $OutDir $packName
if (Test-Path -LiteralPath $stage) { Remove-Item -Recurse -Force $stage }
New-Item -ItemType Directory -Force -Path $stage | Out-Null
New-Item -ItemType Directory -Force -Path (Join-Path $stage 'bin') | Out-Null
New-Item -ItemType Directory -Force -Path (Join-Path $stage "features/denoiser/models/$Arch") | Out-Null
New-Item -ItemType Directory -Force -Path (Join-Path $stage 'licenses') | Out-Null

# Core + flattened siblings.
Copy-Item -LiteralPath $core -Destination (Join-Path $stage 'bin') -Force
Log "Copied NVAudioEffects.dll"

foreach ($srcDir in @($sdkExtCuda, $sdkExtTrt, $sdkExtSsl)) {
    Get-ChildItem -LiteralPath $srcDir -Filter '*.dll' -File | ForEach-Object {
        Copy-Item -LiteralPath $_.FullName -Destination (Join-Path $stage 'bin') -Force
        Log "Copied $($_.Name) from $((Split-Path $srcDir -Parent | Split-Path -Leaf))"
    }
}

# Feature DLL — download_features.ps1 drops it under <sdk>/bin (the SDK already
# expects features alongside the core). Glob for the denoiser feature.
$featureDll = Get-ChildItem -LiteralPath $sdkBin -Filter '*denoiser*.dll' -File -ErrorAction SilentlyContinue |
              Where-Object { $_.Name -ne 'NVAudioEffects.dll' } |
              Select-Object -First 1
if (-not $featureDll) {
    Die "Feature DLL (*denoiser*.dll) not found under $sdkBin after download. Inspect download_features.ps1 output."
}
Copy-Item -LiteralPath $featureDll.FullName -Destination (Join-Path $stage 'bin') -Force
Log "Copied feature DLL: $($featureDll.Name)"

# Model — the SDK drops it under features/<...>/models/<arch>. Locate the .trtpkg
# (or whatever NGC names it) and rename to the canonical filename the app expects.
$modelSearchRoots = @(
    Join-Path $SdkDir 'features'
    Join-Path $SdkDir 'models'
)
$modelFile = $null
foreach ($root in $modelSearchRoots) {
    if (-not (Test-Path -LiteralPath $root)) { continue }
    $modelFile = Get-ChildItem -LiteralPath $root -Recurse -File `
        -Include '*denoiser*48k*.trtpkg', '*denoiser*48k*.bin', '*denoiser_48k*' `
        -ErrorAction SilentlyContinue |
        Where-Object { $_.FullName -match '(?i)\\(' + [regex]::Escape($archTag) + '|' + [regex]::Escape($Arch) + ')\\' } |
        Select-Object -First 1
    if ($modelFile) { break }
    # Fallback: any denoiser_48k* under this root.
    $modelFile = Get-ChildItem -LiteralPath $root -Recurse -File `
        -Include '*denoiser*48k*.trtpkg', '*denoiser*48k*.bin', '*denoiser_48k*' `
        -ErrorAction SilentlyContinue | Select-Object -First 1
    if ($modelFile) { break }
}
if (-not $modelFile) {
    Die "Denoiser model not found under SDK after download. Inspect features/ tree manually."
}
Copy-Item -LiteralPath $modelFile.FullName `
          -Destination (Join-Path $stage "features/denoiser/models/$Arch/denoiser_48k.trtpkg") -Force
Log "Copied model: $($modelFile.Name) → features/denoiser/models/$Arch/denoiser_48k.trtpkg"

# ── Licenses + NOTICE ───────────────────────────────────────────────────────
$licenseGlobs = @('*license*', '*LICENSE*', '*SLA*', '*EULA*')
$licenseFiles = @()
foreach ($g in $licenseGlobs) {
    $licenseFiles += Get-ChildItem -LiteralPath $SdkDir -Recurse -File -Include $g -ErrorAction SilentlyContinue
}
$licenseFiles = $licenseFiles | Sort-Object FullName -Unique
foreach ($f in $licenseFiles) {
    Copy-Item -LiteralPath $f.FullName -Destination (Join-Path $stage 'licenses') -Force
}
Log "Copied $($licenseFiles.Count) license file(s)"

@"
AetherSDR — NVIDIA AFX runtime pack
====================================

This archive bundles the NVIDIA Maxine Audio Effects SDK runtime (v$Version)
and a per-GPU-arch denoiser model so AetherSDR's BNR (background noise
removal) can load the GPU denoiser in-process.

Contents are governed by NVIDIA's Software License Agreement and the
Maxine SDK product terms (see licenses/ for the originals).

Generated by scripts/build/build-afx-bits-windows.ps1 from Maxine AFX SDK $Version.
Target arch: $Arch
"@ | Set-Content -LiteralPath (Join-Path $stage 'NOTICE.txt') -Encoding UTF8

# ── Zip + sha256 ────────────────────────────────────────────────────────────
$zipPath = Join-Path $OutDir "$packName.zip"
if (Test-Path -LiteralPath $zipPath) { Remove-Item -Force $zipPath }
Log "Compressing → $zipPath"
Compress-Archive -Path (Join-Path $stage '*') -DestinationPath $zipPath -CompressionLevel Optimal -Force

$hash = (Get-FileHash -LiteralPath $zipPath -Algorithm SHA256).Hash.ToLower()
$size = (Get-Item -LiteralPath $zipPath).Length

Log "=============================================="
Log "Pack:   $zipPath"
Log "Size:   $([math]::Round($size / 1MB, 1)) MB"
Log "SHA256: $hash"
Log "=============================================="
Log "Next steps:"
Log "  1. Extract this zip into %LOCALAPPDATA%\AetherSDR\AetherSDR\nvidia-afx\current\"
Log "     (or set AETHER_NVAFX_DIR to its extracted location)"
Log "  2. Launch AetherSDR, AetherDSP → BNR → accept license, confirm Active"
Log "  3. If it works: paste this sha256 into kWinTarballSha in"
Log "     src/core/NvidiaAfxPack.cpp, commit + push (updates PR #3902)"
Log "  4. gh release upload afx-bits-$Version $zipPath"
