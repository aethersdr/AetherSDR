#!/usr/bin/env pwsh
<#
.SYNOPSIS
    Package the whisper.cpp + ggml-vulkan static libs from an already-completed
    Windows build into a release asset, so CI (GitHub's stock 4-core windows-latest
    runners, capped at 6 h / step-timeout 4 h) can consume the prebuilt libs
    instead of compiling whisper/ggml/ggml-vulkan from source — which today
    exceeds the timeout on those runners.

.DESCRIPTION
    Mirrors the design of scripts/build/build-afx-bits-windows.ps1: we host a
    small pinned release asset on our GitHub release, the app's CMake config
    detects it and skips the (expensive) source build.

    What ships in the zip (paths preserved so CMake can find them next to their
    source counterparts under third_party/whisper.cpp/):

        libs/
          ggml-base.lib             (~8 MB, from ggml/src/)
          ggml.lib                  (~1 MB, from ggml/src/)
          ggml-cpu.lib              (~5 MB, from ggml/src/)
          ggml-vulkan.lib           (~62 MB, from ggml/src/ggml-vulkan/)
          whisper.lib               (~10 MB, from src/)
        NOTICE.txt

    Total ~90 MB uncompressed → ~30 MB compressed.

    All are Release / MD / /std:c++20, built with MSVC 14.44 against Vulkan SDK
    1.4.350.0. Consumers must match those ABI knobs; if they don't, add a new
    per-config asset (e.g. ...-Debug-...) rather than mixing configs.

.PARAMETER BuildDir
    Root of a completed AetherSDR build tree (contains third_party/whisper.cpp/
    subdirs). Defaults to <repo>/build.

.PARAMETER OutDir
    Where the staged tree and final .zip are written.

.PARAMETER Version
    whisper.cpp upstream version tag — used only for the zip filename and the
    NOTICE.txt banner. Must match third_party/whisper.cpp/AETHER_VENDORING.md.

.EXAMPLE
    pwsh scripts/build/build-whisper-vulkan-windows.ps1
#>

[CmdletBinding()]
param(
    [string]$BuildDir = "$(Join-Path $PSScriptRoot '../../build')",
    [string]$OutDir   = "$(Join-Path $PSScriptRoot '../../build/whisper-gpu-pack')",
    [string]$Version  = '1.9.1'
)

$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest

function Log($msg) { Write-Host "[whisper-gpu] $msg" -ForegroundColor Cyan }
function Die($msg) { Write-Error "[whisper-gpu] $msg"; exit 1 }

# ── Locate the .lib artifacts in the build tree ─────────────────────────────
$whisperRoot = Join-Path $BuildDir 'third_party/whisper.cpp'
if (-not (Test-Path -LiteralPath $whisperRoot)) {
    Die "whisper build tree not found: $whisperRoot (run a full local build first — see scripts/build-windows-local.ps1)"
}

$libs = @{
    'ggml-base'   = Join-Path $whisperRoot 'ggml/src/ggml-base.lib'
    'ggml'        = Join-Path $whisperRoot 'ggml/src/ggml.lib'
    'ggml-cpu'    = Join-Path $whisperRoot 'ggml/src/ggml-cpu.lib'
    'ggml-vulkan' = Join-Path $whisperRoot 'ggml/src/ggml-vulkan/ggml-vulkan.lib'
    'whisper'    = Join-Path $whisperRoot 'src/whisper.lib'
}
foreach ($name in $libs.Keys) {
    if (-not (Test-Path -LiteralPath $libs[$name])) { Die "Missing: $($libs[$name])" }
}

# ── Stage ───────────────────────────────────────────────────────────────────
$packName = "whisper-gpu-$Version-windows-x86_64"
$stage    = Join-Path $OutDir $packName
if (Test-Path -LiteralPath $stage) { Remove-Item -Recurse -Force $stage }
New-Item -ItemType Directory -Force -Path (Join-Path $stage 'libs') | Out-Null

foreach ($name in $libs.Keys) {
    Copy-Item -LiteralPath $libs[$name] -Destination (Join-Path $stage 'libs') -Force
    $size = (Get-Item -LiteralPath $libs[$name]).Length
    Log "Staged $name.lib ($([math]::Round($size / 1MB, 1)) MB)"
}

# whisper.cpp commit hash + Vulkan SDK version for provenance.
$commitFile = Join-Path $PSScriptRoot '../../third_party/whisper.cpp/COMMIT'
$commit = if (Test-Path -LiteralPath $commitFile) { (Get-Content -LiteralPath $commitFile -Raw).Trim() } else { 'unknown' }
$vkSdk = if ($env:VULKAN_SDK) { Split-Path $env:VULKAN_SDK -Leaf } else { 'unknown' }

@"
AetherSDR — prebuilt whisper.cpp + ggml-vulkan static libs (Windows x64)
========================================================================

Contents are static libraries, built with MSVC 14.44 (VS 2022 17.14) in
Release mode with the /MD runtime and -std:c++20, against:

  whisper.cpp:  v$Version (commit $commit)
  Vulkan SDK:   $vkSdk

Provided as a CI-time cache so GitHub-hosted windows-latest runners
(4 core / 16 GB) can build AetherSDR without recompiling ggml-vulkan
from source — a step that otherwise exceeds the 4-hour step timeout on
that hardware.

The application's own CMake config finds these next to the vendored
whisper.cpp source tree in third_party/whisper.cpp/. Consumers must
match the compiler + runtime + language standard listed above; if you
bump any of them, cut a new asset rather than mixing configurations.
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
Log "Upload as a pre-release asset:"
Log "  gh release create whisper-gpu-$Version --prerelease --repo aethersdr/AetherSDR --title 'whisper-gpu $Version (CI cache)' <notes>"
Log "  gh release upload whisper-gpu-$Version '$zipPath' --repo aethersdr/AetherSDR"
