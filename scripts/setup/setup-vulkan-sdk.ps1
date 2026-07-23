<#
.SYNOPSIS
    Install the LunarG Vulkan SDK for the Windows whisper GPU (Vulkan) backend.

.DESCRIPTION
    AetherSDR's ASR GPU acceleration on Windows is whisper.cpp's Vulkan backend
    (ggml-vulkan), the same one the Linux AppImage ships. Building it needs three
    things from the toolchain:

      * glslc            — the shader compiler (SDK Bin/)
      * vulkan-1.lib     — the loader import library (SDK Lib/)
      * SPIRV-Headers    — ggml-vulkan does find_package(SPIRV-Headers CONFIG
                           REQUIRED); the SDK does not reliably ship that CMake
                           package, so we stage it ourselves into the SDK prefix.

    The runtime vulkan-1.dll (SDK Bin/) is bundled by the installer workflow so
    the exe launches even on machines without a Vulkan driver (the loader then
    reports zero devices and ggml falls back to CPU).

    Idempotent + cache-friendly: re-running with the SDK already present skips the
    download/install but still exports VULKAN_SDK. Keep the version in sync with
    the SDK the Linux/macOS GPU builds target when practical.

.NOTES
    The SDK installer is a large (~400 MB) code-signed LunarG executable with no
    vendor-published checksum sidecar, so — unlike the other pinned deps — it is
    version-pinned but not SHA-verified. The SPIRV-Headers tarball (tiny) is
    SHA-256 pinned as usual.

.EXAMPLE
    pwsh scripts/setup/setup-vulkan-sdk.ps1
#>

$ErrorActionPreference = "Stop"
. "$PSScriptRoot\_verify_sha256.ps1"

$Version    = "1.4.350.0"
$InstallDir = "C:\VulkanSDK\$Version"
$Installer  = "vulkansdk-windows-X64-$Version.exe"
$SdkUrl     = "https://sdk.lunarg.com/sdk/download/$Version/windows/$Installer"

# SPIRV-Headers matching the SDK tag (header-only; installs a CMake config so
# ggml-vulkan's find_package(SPIRV-Headers CONFIG REQUIRED) resolves).
$SpirvTag   = "vulkan-sdk-$Version"
$SpirvUrl   = "https://github.com/KhronosGroup/SPIRV-Headers/archive/refs/tags/$SpirvTag.tar.gz"
$SpirvSha   = "9905d9341f20388adb852c77dd982f2c4d539fd68e6c1f1bcebf034715f2d1d5"

function Export-VulkanEnv {
    param([string]$Dir)
    # Make VULKAN_SDK + its Bin/ visible to the rest of the job. FindVulkan uses
    # $ENV{VULKAN_SDK}; CMAKE_PREFIX_PATH (set in the configure step) points config
    # searches (SPIRV-Headers) at the same prefix.
    $env:VULKAN_SDK = $Dir
    if ($env:GITHUB_ENV) { "VULKAN_SDK=$Dir" | Out-File -FilePath $env:GITHUB_ENV -Append -Encoding utf8 }
    if ($env:GITHUB_PATH) { "$Dir\Bin" | Out-File -FilePath $env:GITHUB_PATH -Append -Encoding utf8 }
    Write-Host "VULKAN_SDK=$Dir" -ForegroundColor Green
}

# glslc + the staged SPIRV-Headers config together mean a previous (or cache-
# restored) install is complete — skip the heavy steps but always export env.
if ((Test-Path "$InstallDir\Bin\glslc.exe") -and
    (Test-Path "$InstallDir\share\cmake\SPIRV-Headers\SPIRV-HeadersConfig.cmake")) {
    Write-Host "Vulkan SDK $Version already staged in $InstallDir" -ForegroundColor Green
    Export-VulkanEnv $InstallDir
    exit 0
}

# --- Vulkan SDK ---------------------------------------------------------------
if (-not (Test-Path "$InstallDir\Bin\glslc.exe")) {
    Write-Host "Downloading Vulkan SDK $Version ..." -ForegroundColor Cyan
    if (-not (Test-Path $Installer)) {
        Invoke-WebRequest -Uri $SdkUrl -OutFile $Installer
    }
    Write-Host "Installing Vulkan SDK (silent) ..." -ForegroundColor Cyan
    # LunarG's Qt Installer Framework CLI: headless install of the default
    # components to $InstallDir.
    $p = Start-Process -FilePath ".\$Installer" -Wait -NoNewWindow -PassThru `
        -ArgumentList @("--root", $InstallDir, "--accept-licenses",
                        "--default-answer", "--confirm-command", "install")
    if ($p.ExitCode -ne 0) { Write-Error "Vulkan SDK installer exited $($p.ExitCode)"; exit 1 }
    if (-not (Test-Path "$InstallDir\Bin\glslc.exe")) {
        Write-Error "glslc.exe not found under $InstallDir after install"; exit 1
    }
} else {
    Write-Host "Vulkan SDK binaries already present in $InstallDir" -ForegroundColor Green
}

# --- SPIRV-Headers CMake package ---------------------------------------------
$SpirvTarball = "spirv-headers-$Version.tar.gz"
$SpirvExtract = "spirv-headers-extract"
Write-Host "Staging SPIRV-Headers $SpirvTag ..." -ForegroundColor Cyan
if (-not (Test-Path $SpirvTarball)) {
    Invoke-WebRequest -Uri $SpirvUrl -OutFile $SpirvTarball
}
Confirm-Sha256 -Path $SpirvTarball -Expected $SpirvSha

if (Test-Path $SpirvExtract) { Remove-Item -Recurse -Force $SpirvExtract }
New-Item -ItemType Directory -Force -Path $SpirvExtract | Out-Null
tar -xf $SpirvTarball -C $SpirvExtract
if ($LASTEXITCODE -ne 0) { Write-Error "SPIRV-Headers extraction failed"; exit 1 }
$SpirvSrc = Get-ChildItem $SpirvExtract -Directory |
    Where-Object { $_.Name -like "SPIRV-Headers-*" } | Select-Object -First 1
if (-not $SpirvSrc) { Write-Error "unexpected SPIRV-Headers archive layout"; exit 1 }

# Header-only: configure + install writes headers + SPIRV-HeadersConfig.cmake
# into the SDK prefix (nothing is compiled).
cmake -S "$($SpirvSrc.FullName)" -B "$SpirvExtract\build" -G "Ninja" `
    -DCMAKE_INSTALL_PREFIX="$InstallDir" | Out-Host
if ($LASTEXITCODE -ne 0) { Write-Error "SPIRV-Headers configure failed"; exit 1 }
cmake --install "$SpirvExtract\build" | Out-Host
if ($LASTEXITCODE -ne 0) { Write-Error "SPIRV-Headers install failed"; exit 1 }
Remove-Item -Recurse -Force $SpirvExtract

if (-not (Test-Path "$InstallDir\share\cmake\SPIRV-Headers\SPIRV-HeadersConfig.cmake")) {
    Write-Error "SPIRV-HeadersConfig.cmake not found under $InstallDir after install"; exit 1
}

Write-Host "Vulkan SDK $Version + SPIRV-Headers staged in $InstallDir" -ForegroundColor Green
Export-VulkanEnv $InstallDir
