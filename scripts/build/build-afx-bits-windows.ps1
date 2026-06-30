#!/usr/bin/env pwsh
<#
.SYNOPSIS
    Assemble the Windows AFX-bits pack (.zip) for the BNR feature.

.DESCRIPTION
    Produces afx-bits-2.1.0-windows-x86_64-sm_<arch>.zip with the exact layout
    NvidiaAfxFilter (Windows path) expects:

        bin/
          NVAudioEffects.dll              (AFX core, from SDK)
          nvafxdenoiser.dll               (denoiser feature DLL, from NGC)
          cublas64_12.dll cublasLt64_12.dll cufft64_11.dll nvrtc64_120_0.dll   (from SDK)
          nvinfer_10.dll                  (TensorRT, from SDK)
          libcrypto-3-x64.dll             (OpenSSL, from SDK)
        features/denoiser/models/sm_<arch>/denoiser_48k.trtpkg  (from NGC)
        licenses/                         (NVIDIA SLA + product terms, from SDK)
        NOTICE.txt

    The script flattens the SDK tree into bin/ so LOAD_WITH_ALTERED_SEARCH_PATH
    resolves the core's transitive deps from the pack itself — no host CUDA
    install required at runtime.

    The SDK's own features/download_features.ps1 is broken against a personal
    NGC API key (it queries /org/nvidia/team/maxine/... which only returns
    files to org members). We bypass it and use `ngc registry model
    download-version` directly, which works on personal keys.

.PARAMETER SdkDir
    Path to the extracted Maxine Audio Effects SDK (the directory whose
    immediate children are bin/, features/, include/, lib/, license/, ...).

.PARAMETER Arch
    Target SM architecture. Currently the app's pack is per-arch and the manifest
    pins one zip per arch — pick the one matching the test GPU. Valid: sm_75,
    sm_86, sm_89, sm_100.

.PARAMETER OutDir
    Where the staged tree and final .zip are written.

.PARAMETER NgcExe
    Path to ngc.exe. Defaults to the standard installer location.

.EXAMPLE
    pwsh scripts/build/build-afx-bits-windows.ps1 `
        -SdkDir 'C:\nvidia\maxine-afx-sdk-2.1.0\2026-03-11_NVIDIA_AFX_SDK_Win_v2.1.0.9' `
        -Arch sm_89
#>

[CmdletBinding()]
param(
    [Parameter(Mandatory=$true)]
    [string]$SdkDir,

    [Parameter()]
    [ValidateSet('sm_75','sm_86','sm_89','sm_100')]
    [string]$Arch = 'sm_89',

    [Parameter()]
    [string]$OutDir = "$(Join-Path $PSScriptRoot '../../build/afx-pack')",

    [Parameter()]
    [string]$NgcExe = 'C:\Program Files\NVIDIA Corporation\NGCCLI\amd64\ngc.exe',

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
if (-not (Test-Path -LiteralPath $NgcExe)) {
    Die "ngc.exe not found at $NgcExe — install NGC CLI and run 'ngc config set' first."
}

# Map sm_XX → NGC's GPU architecture tag (used in model version IDs).
$archTag = switch ($Arch) {
    'sm_75'  { 'turing' }
    'sm_86'  { 'ampere' }
    'sm_89'  { 'ada' }
    'sm_100' { 'blackwell' }
}
Log "Target arch: $Arch (NGC tag: $archTag)"

# ── Locate SDK pieces ───────────────────────────────────────────────────────
$sdkBin = Join-Path $SdkDir 'bin'
$sdkExtCuda = Join-Path $sdkBin 'external/cuda/bin'
$sdkExtTrt  = Join-Path $sdkBin 'external/nvtrt/bin'
$sdkExtSsl  = Join-Path $sdkBin 'external/openssl/bin'
$sdkLicense = Join-Path $SdkDir 'license'
foreach ($p in @($sdkBin, $sdkExtCuda, $sdkExtTrt, $sdkExtSsl)) {
    if (-not (Test-Path -LiteralPath $p)) { Die "Missing in SDK: $p" }
}
$core = Join-Path $sdkBin 'NVAudioEffects.dll'
if (-not (Test-Path -LiteralPath $core)) { Die "Missing core DLL: $core" }

# ── Fetch feature DLL + per-arch model from NGC ─────────────────────────────
$ngcWorkDir = Join-Path $OutDir 'ngc-cache'
New-Item -ItemType Directory -Force -Path $ngcWorkDir | Out-Null

Log "Downloading feature DLL (afx_win_denoiser:$Version-dynamic-library)"
& $NgcExe registry model download-version "nvidia/maxine/afx_win_denoiser:$Version-dynamic-library" `
    --dest $ngcWorkDir --format_type json | Out-Null
if ($LASTEXITCODE -ne 0) { Die "ngc download (dynamic-library) failed" }

$featureDir = Join-Path $ngcWorkDir "afx_win_denoiser_v$Version-dynamic-library"
$featureDll = Join-Path $featureDir 'nvafxdenoiser.dll'
if (-not (Test-Path -LiteralPath $featureDll)) {
    Die "Feature DLL not found at expected path: $featureDll"
}

Log "Downloading per-arch model (afx_win_denoiser:$Version-48k-$archTag)"
& $NgcExe registry model download-version "nvidia/maxine/afx_win_denoiser:$Version-48k-$archTag" `
    --dest $ngcWorkDir --format_type json | Out-Null
if ($LASTEXITCODE -ne 0) { Die "ngc download (48k-$archTag) failed" }

$modelDir = Join-Path $ngcWorkDir "afx_win_denoiser_v$Version-48k-$archTag"
# The package ships two variants — denoiser_48k.trtpkg (canonical) and
# denoiser_v2_48k.trtpkg (newer). The app's findModel() looks for the canonical
# name; ship that one.
$modelFile = Join-Path $modelDir 'denoiser_48k.trtpkg'
if (-not (Test-Path -LiteralPath $modelFile)) {
    Die "Model file not found at expected path: $modelFile"
}

# ── Stage the pack tree ─────────────────────────────────────────────────────
$packName = "afx-bits-$Version-windows-x86_64-$Arch"
$stage    = Join-Path $OutDir $packName
if (Test-Path -LiteralPath $stage) { Remove-Item -Recurse -Force $stage }
New-Item -ItemType Directory -Force -Path $stage | Out-Null
New-Item -ItemType Directory -Force -Path (Join-Path $stage 'bin') | Out-Null
New-Item -ItemType Directory -Force -Path (Join-Path $stage "features/denoiser/models/$Arch") | Out-Null
New-Item -ItemType Directory -Force -Path (Join-Path $stage 'licenses') | Out-Null

# SLIM pack — only the AFX-specific binaries and OpenSSL. CUDA libs
# (cublas, cublasLt, cufft, nvrtc, cuda-runtime) and TensorRT (nvinfer)
# come from PyPI wheels at install time, driven by the manifest in
# NvidiaAfxPack.cpp. That gets the pack zip from ~1 GB down to ~38 MB so
# it can ship as a GitHub release asset.
Copy-Item -LiteralPath $core -Destination (Join-Path $stage 'bin') -Force
Log "Copied NVAudioEffects.dll"

# OpenSSL — small (~5 MB), no clean PyPI source, ship it.
Get-ChildItem -LiteralPath $sdkExtSsl -Filter '*.dll' -File | ForEach-Object {
    Copy-Item -LiteralPath $_.FullName -Destination (Join-Path $stage 'bin') -Force
    Log "Copied $($_.Name) (from external/openssl)"
}

Copy-Item -LiteralPath $featureDll -Destination (Join-Path $stage 'bin') -Force
Log "Copied nvafxdenoiser.dll"

Copy-Item -LiteralPath $modelFile `
          -Destination (Join-Path $stage "features/denoiser/models/$Arch/denoiser_48k.trtpkg") -Force
Log "Copied denoiser_48k.trtpkg → features/denoiser/models/$Arch/"

# ── Licenses + NOTICE ───────────────────────────────────────────────────────
if (Test-Path -LiteralPath $sdkLicense) {
    Get-ChildItem -LiteralPath $sdkLicense -File | ForEach-Object {
        Copy-Item -LiteralPath $_.FullName -Destination (Join-Path $stage 'licenses') -Force
        Log "Copied license: $($_.Name)"
    }
}
$thirdParty = Join-Path $sdkBin 'external/ThirdPartyLicenses.txt'
if (Test-Path -LiteralPath $thirdParty) {
    Copy-Item -LiteralPath $thirdParty -Destination (Join-Path $stage 'licenses') -Force
    Log "Copied ThirdPartyLicenses.txt"
}

@"
AetherSDR — NVIDIA AFX runtime pack
====================================

This archive bundles the NVIDIA Maxine Audio Effects SDK runtime (v$Version)
and the per-GPU-arch denoiser model so AetherSDR's BNR (background noise
removal) can load the GPU denoiser in-process.

Contents are governed by NVIDIA's Software License Agreement and the
Maxine SDK product terms — see licenses/ for the originals.

Generated by scripts/build/build-afx-bits-windows.ps1
SDK:        Maxine AFX SDK $Version (Windows)
Target arch: $Arch ($archTag)
Feature:    afx_win_denoiser $Version-dynamic-library + $Version-48k-$archTag
"@ | Set-Content -LiteralPath (Join-Path $stage 'NOTICE.txt') -Encoding UTF8

# ── Zip + sha256 (AFX zip — small, ~33 MB) ──────────────────────────────────
$zipPath = Join-Path $OutDir "$packName.zip"
if (Test-Path -LiteralPath $zipPath) { Remove-Item -Force $zipPath }
Log "Compressing → $zipPath"
Compress-Archive -Path (Join-Path $stage '*') -DestinationPath $zipPath -CompressionLevel Optimal -Force

$hash = (Get-FileHash -LiteralPath $zipPath -Algorithm SHA256).Hash.ToLower()
$size = (Get-Item -LiteralPath $zipPath).Length

# ── TensorRT runtime zip (separate component) ───────────────────────────────
# pypi.nvidia.com hosts tensorrt-cu12-libs as a 1.6 GB wheel that includes the
# builder, plugins and ONNX parser — Maxine AFX uses a pre-baked .trtpkg engine
# and only needs the runtime (nvinfer_<ver>.dll, ~420 MB raw / ~220 MB zip).
# Ship that one DLL alone so the user-side download matches Linux footprint.
$trtVersion = '10.9.0.34'
$trtStage = Join-Path $OutDir "afx-bits-$Version-windows-x86_64-tensorrt-$trtVersion"
if (Test-Path -LiteralPath $trtStage) { Remove-Item -Recurse -Force $trtStage }
New-Item -ItemType Directory -Force -Path (Join-Path $trtStage 'bin') | Out-Null

# Pick the core inference runtime specifically (nvinfer_<ver>.dll) — not the
# plugin/ONNX-parser DLLs that may also live here. Grabbing 'first *.dll' could
# stage the wrong one if the SDK ever ships more than nvinfer in nvtrt/bin.
$trtSrc = Get-ChildItem -LiteralPath $sdkExtTrt -Filter 'nvinfer*.dll' -File |
          Where-Object { $_.Name -match '^nvinfer_\d+\.dll$' } |
          Select-Object -First 1
if (-not $trtSrc) { Die "core nvinfer_<ver>.dll not found under $sdkExtTrt" }
Copy-Item -LiteralPath $trtSrc.FullName -Destination (Join-Path $trtStage 'bin') -Force
Log "Staged $($trtSrc.Name) for TRT-only pack"

$trtZipPath = Join-Path $OutDir "afx-bits-$Version-windows-x86_64-tensorrt-$trtVersion.zip"
if (Test-Path -LiteralPath $trtZipPath) { Remove-Item -Force $trtZipPath }
Log "Compressing → $trtZipPath"
Compress-Archive -Path (Join-Path $trtStage '*') -DestinationPath $trtZipPath -CompressionLevel Optimal -Force
$trtHash = (Get-FileHash -LiteralPath $trtZipPath -Algorithm SHA256).Hash.ToLower()
$trtSize = (Get-Item -LiteralPath $trtZipPath).Length

Log "=============================================="
Log "AFX pack:    $zipPath"
Log "  Size:    $([math]::Round($size / 1MB, 1)) MB"
Log "  SHA256:  $hash"
Log "----------------------------------------------"
Log "TRT pack:    $trtZipPath"
Log "  Size:    $([math]::Round($trtSize / 1MB, 1)) MB"
Log "  SHA256:  $trtHash"
Log "=============================================="
Log "Next steps:"
Log "  1. Extract the AFX zip into %LOCALAPPDATA%\AetherSDR\AetherSDR\nvidia-afx\current\"
Log "     (or set AETHER_NVAFX_DIR to its extracted location)"
Log "  2. Launch AetherSDR, AetherDSP → BNR → accept license, confirm Active"
Log "  3. If it works, pin BOTH shas in src/core/NvidiaAfxPack.cpp:"
Log "       kWinTarballSha  = $hash"
Log "       kWinTensorrtSha = $trtHash"
Log "     (and kWinTensorrtVer = $trtVersion), then commit + push (updates PR #3902)"
Log "  4. Publish BOTH assets:"
Log "       gh release upload afx-bits-$Version `"$zipPath`""
Log "       gh release upload afx-bits-$Version `"$trtZipPath`""
