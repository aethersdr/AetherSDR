<#
.SYNOPSIS
    Build the AetherSDR Windows installer locally, on a real Windows host.

.DESCRIPTION
    Mirrors .github/workflows/windows-installer.yml end-to-end so a native build
    produces the same artifact as CI — but on hardware with enough cores/RAM that
    the full ASR + GPU (whisper Vulkan) build finishes fast instead of hitting the
    GitHub runner's 6-hour timeout. (That timeout is a 4-core/16 GB free-runner
    resource limit, not a real build problem; a 32-core box with plenty of RAM
    builds it in well under an hour at full -j.)

    Stages the third_party deps via the same setup-*.ps1 scripts CI uses,
    configures with the release flags (ASR ONNX + sherpa + GPU Vulkan all
    REQUIRED — fail-loud if a runtime is missing), builds, deploys Qt + all
    runtime DLLs (ONNX Runtime, sherpa-onnx, vulkan-1), and optionally packages
    the Inno Setup installer.

.PREREQUISITES (install once)
    * Visual Studio 2022 (or Build Tools) with "Desktop development with C++".
      RUN THIS SCRIPT FROM the "x64 Native Tools Command Prompt for VS 2022"
      (then `pwsh`), so cl.exe/link.exe and the MSVC env are set.
    * Qt 6.8.x for msvc2022_64 + modules: qtmultimedia qtserialport qtwebsockets
      qtshadertools (Qt online installer or aqt). Pass its path via -QtDir.
    * CMake + Ninja on PATH.
    * (optional, for -Installer) Inno Setup 6 (ISCC.exe).
    The Vulkan SDK, ONNX Runtime, sherpa-onnx, opus, fftw, hidapi, deepfilter and
    qtkeychain are all staged automatically by the setup-*.ps1 scripts below.

.PARAMETER QtDir
    Path to the Qt msvc2022_64 kit, e.g. C:\Qt\6.8.2\msvc2022_64
    (must contain bin\windeployqt.exe). Defaults to $env:QT_ROOT_DIR.

.PARAMETER Jobs
    Build parallelism. Defaults to the CPU count. Do NOT copy CI's -j2 workaround
    here — that was only for the 4-core/16 GB runner.

.PARAMETER Installer
    Also build the Inno Setup installer (needs ISCC.exe). Otherwise stops after
    staging deploy\ (a runnable app tree you can smoke-test directly).

.PARAMETER UploadTag
    If set, upload the built installer (or deploy zip) to this GitHub release tag
    via `gh release upload` (e.g. a pre-release asset tag). Requires gh + auth.

.EXAMPLE
    pwsh scripts\build-windows-local.ps1 -QtDir C:\Qt\6.8.2\msvc2022_64
.EXAMPLE
    pwsh scripts\build-windows-local.ps1 -QtDir C:\Qt\6.8.2\msvc2022_64 -Installer
#>
param(
    [string]$QtDir = $env:QT_ROOT_DIR,
    [int]$Jobs = 0,
    [switch]$Installer,
    [string]$UploadTag
)

$ErrorActionPreference = "Stop"
if ($Jobs -le 0) { $Jobs = [int]$env:NUMBER_OF_PROCESSORS }
$repo = (Resolve-Path "$PSScriptRoot\..").Path
Set-Location $repo

function Assert-Tool($name, $hint) {
    if (-not (Get-Command $name -ErrorAction SilentlyContinue)) {
        throw "'$name' not found on PATH. $hint"
    }
}

Write-Host "== Prerequisite checks ==" -ForegroundColor Cyan
Assert-Tool cl    "Run from the 'x64 Native Tools Command Prompt for VS 2022' so MSVC is on PATH."
Assert-Tool cmake "Install CMake (bundled with VS, or standalone) and add it to PATH."
Assert-Tool ninja "Install Ninja (e.g. 'winget install Ninja-build.Ninja') and add it to PATH."
if (-not $QtDir -or -not (Test-Path "$QtDir\bin\windeployqt.exe")) {
    throw "Qt not found. Pass -QtDir C:\Qt\6.8.x\msvc2022_64 (must contain bin\windeployqt.exe)."
}
Write-Host "  MSVC + CMake + Ninja OK; Qt = $QtDir; jobs = $Jobs" -ForegroundColor Green

# ---------------------------------------------------------------------------
# 1. third_party deps — the exact scripts CI runs (each is idempotent/cached).
# ---------------------------------------------------------------------------
Write-Host "== Staging third_party deps ==" -ForegroundColor Cyan
foreach ($s in @(
        "setup-opus.ps1", "setup-fftw.ps1", "setup-hidapi.ps1",
        "setup-deepfilter.ps1", "setup-qtkeychain.ps1", "setup-onnxruntime.ps1",
        "setup-sherpa-onnx.ps1", "setup-vulkan-sdk.ps1")) {
    Write-Host "  -> $s" -ForegroundColor DarkCyan
    & "$repo\scripts\setup\$s"
    if ($LASTEXITCODE -ne 0) { throw "$s failed" }
}
# setup-vulkan-sdk.ps1 exports VULKAN_SDK via $GITHUB_ENV in CI; locally, derive it.
if (-not $env:VULKAN_SDK) {
    $sdk = Get-ChildItem "C:\VulkanSDK" -Directory -ErrorAction SilentlyContinue |
        Sort-Object Name -Descending | Select-Object -First 1
    if (-not $sdk) { throw "VULKAN_SDK not set and no C:\VulkanSDK\* found" }
    $env:VULKAN_SDK = $sdk.FullName
}
Write-Host "  VULKAN_SDK = $env:VULKAN_SDK" -ForegroundColor Green

# ---------------------------------------------------------------------------
# 2. Configure — same flags as windows-installer.yml (minus the empty vcpkg
#    toolchain vars; zlib is vendored). CMAKE_PREFIX_PATH points CMake at Qt and
#    the Vulkan SDK (glslc + SPIRV-Headers) so REQUIRE_ASR_GPU can succeed.
# ---------------------------------------------------------------------------
Write-Host "== Configure ==" -ForegroundColor Cyan
cmake -B build -G "Ninja" `
    -DCMAKE_BUILD_TYPE=RelWithDebInfo `
    -DMQTT_TLS=OFF -DREQUIRE_KEYCHAIN=ON -DREQUIRE_SERIALPORT=ON `
    -DREQUIRE_ASR_ONNX=ON -DREQUIRE_ASR_SHERPA=ON -DREQUIRE_ASR_GPU=ON `
    -DCMAKE_PREFIX_PATH="$QtDir;$env:VULKAN_SDK" `
    -DENABLE_NVIDIA_AFX=ON
if ($LASTEXITCODE -ne 0) { throw "configure failed" }

# ---------------------------------------------------------------------------
# 3. Build. Opus first (ExternalProject ordering), then everything at full -j.
#    No ggml-vulkan isolation / -j2 needed here — that was a CI-runner workaround.
# ---------------------------------------------------------------------------
Write-Host "== Build (opus, then full -j$Jobs) ==" -ForegroundColor Cyan
cmake --build build --target build_opus -j $Jobs
if ($LASTEXITCODE -ne 0) { throw "opus build failed" }
cmake --build build -j $Jobs
if ($LASTEXITCODE -ne 0) { throw "build failed" }

# ---------------------------------------------------------------------------
# 4. Deploy — mirror the workflow's "Deploy Qt" step.
# ---------------------------------------------------------------------------
Write-Host "== Deploy ==" -ForegroundColor Cyan
$deploy = "$repo\deploy"
if (Test-Path $deploy) { Remove-Item -Recurse -Force $deploy }
New-Item -ItemType Directory -Force -Path $deploy | Out-Null
Copy-Item build\AetherSDR.exe $deploy\ -Force
foreach ($f in @("aether-dv-waveform.exe", "AetherDV.cfg")) {
    if (-not (Test-Path "build\$f")) { throw "expected build\$f is missing" }
    Copy-Item "build\$f" $deploy\ -Force
}
# windeployqt needs qt6keychain.dll in the Qt bin dir to resolve it as a module.
if (Test-Path "third_party\qtkeychain\bin\qt6keychain.dll") {
    Copy-Item third_party\qtkeychain\bin\qt6keychain.dll "$QtDir\bin\" -Force
}
& "$QtDir\bin\windeployqt.exe" "$deploy\AetherSDR.exe" --release --no-translations --no-system-d3d-compiler
foreach ($dll in @(
        "third_party\fftw3\bin\libfftw3-3.dll", "third_party\fftw3\bin\libfftw3f-3.dll",
        "third_party\hidapi\bin\hidapi.dll",
        "third_party\deepfilter\lib\windows-x86_64\deepfilter.dll",
        "third_party\qtkeychain\bin\qt6keychain.dll")) {
    if (Test-Path $dll) { Copy-Item $dll $deploy\ -Force }
}
# ASR runtime DLLs: CMake POST_BUILD stages ONNX Runtime + sherpa-onnx next to
# the exe (build\ root); ship them alongside it.
Get-ChildItem build\*.dll -ErrorAction SilentlyContinue | ForEach-Object {
    Write-Host "  bundling ASR runtime DLL: $($_.Name)" -ForegroundColor DarkCyan
    Copy-Item $_.FullName $deploy\ -Force
}
# Vulkan loader (whisper GPU): the exe hard-links it, so it must ship. Its
# location varies by SDK layout (runtime\x64 on newer SDKs, Bin on older) with a
# driver copy in System32 — take the first that exists.
$vkCandidates = @(
    (Join-Path $env:VULKAN_SDK "runtime\x64\vulkan-1.dll"),
    (Join-Path $env:VULKAN_SDK "Bin\vulkan-1.dll"),
    "$env:SystemRoot\System32\vulkan-1.dll"
)
$vkDll = $vkCandidates | Where-Object { Test-Path $_ } | Select-Object -First 1
if (-not $vkDll) { throw "vulkan-1.dll not found in: $($vkCandidates -join '; ')" }
Copy-Item $vkDll $deploy\ -Force
Write-Host "  deploy\ staged ($((Get-ChildItem $deploy).Count) items)" -ForegroundColor Green

# Quick sanity: confirm the GPU + ASR runtimes actually landed in the payload.
foreach ($need in @("vulkan-1.dll", "onnxruntime.dll")) {
    if (-not (Test-Path "$deploy\$need")) {
        Write-Warning "expected $need not present in deploy\ — check the build config"
    }
}

# ---------------------------------------------------------------------------
# 5. (optional) Installer — Inno Setup, mirroring the workflow's packaging.
# ---------------------------------------------------------------------------
if ($Installer) {
    Assert-Tool iscc "Install Inno Setup 6 (provides ISCC.exe) to build the installer."
    Write-Host "== Staging MSVC runtime + building installer ==" -ForegroundColor Cyan
    & powershell -NoProfile -ExecutionPolicy Bypass -File packaging\windows\stage-msvc-runtime.ps1 -OutputDir installer-runtime
    Copy-Item installer-runtime\*.dll $deploy\ -Force
    $version = (Get-Content CMakeLists.txt | Select-String -Pattern 'project\(AetherSDR VERSION (\d+\.\d+\.\d+)' | ForEach-Object { $_.Matches[0].Groups[1].Value } | Select-Object -First 1)
    if (-not $version) { $version = "0.0.0-local" }
    $runtimeDir = (Resolve-Path "installer-runtime").Path
    & iscc "/DAPP_VERSION=$version" "/DVC_RUNTIME_DIR=$runtimeDir" packaging\windows\installer.iss
    Write-Host "  installer built (see packaging\windows\Output or the .iss OutputDir)" -ForegroundColor Green
}

# ---------------------------------------------------------------------------
# 6. (optional) Upload to a GitHub release tag.
# ---------------------------------------------------------------------------
if ($UploadTag) {
    Assert-Tool gh "Install GitHub CLI + `gh auth login` to upload."
    $artifact = Get-ChildItem -Recurse -Include "*Setup*.exe", "AetherSDR*.exe" packaging\windows 2>$null |
        Select-Object -First 1
    if (-not $artifact) {
        Write-Host "  no installer found; zipping deploy\ instead" -ForegroundColor Yellow
        $artifact = "$repo\AetherSDR-windows-x64.zip"
        Compress-Archive -Path "$deploy\*" -DestinationPath $artifact -Force
    }
    Write-Host "  uploading $artifact to release $UploadTag (as prerelease asset)" -ForegroundColor Cyan
    gh release upload $UploadTag $artifact --clobber
}

Write-Host "`nDONE. GPU (Vulkan) + ONNX + sherpa build complete." -ForegroundColor Green
Write-Host "Smoke-test: launch deploy\AetherSDR.exe, enable Copy Assist, pick a GPU device," -ForegroundColor Green
Write-Host "and confirm transcription runs on the Vulkan backend." -ForegroundColor Green
