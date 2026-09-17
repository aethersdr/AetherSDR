<#
.SYNOPSIS
    Download and build PortAudio for Windows x64.

.DESCRIPTION
    Downloads PortAudio v19.7.0 source from GitHub, builds the static library
    with CMake + MSVC, and places headers/lib in third_party/portaudio/ ready
    for CMake. WASAPI is requested EXPLICITLY (-DPA_USE_WASAPI=ON) because the
    sidetone sink's whole reason to exist is #3193's WASAPI preference — a
    future PortAudio bump must not be able to drop it silently. WDM-KS /
    DirectSound / MME come from upstream's Windows defaults.

    Required for the callback-model CW sidetone sink (CwSidetonePortAudioSink)
    and its WASAPI host-API preference (#3193). Without it the Windows build
    silently falls back to the push-model QAudioSink sidetone path.

.EXAMPLE
    .\setup-portaudio.ps1
#>

$ErrorActionPreference = "Stop"
. "$PSScriptRoot\_verify_sha256.ps1"

$PaVersion = "19.7.0"
$PaUrl     = "https://github.com/PortAudio/portaudio/archive/refs/tags/v${PaVersion}.tar.gz"
# SHA256 of the GitHub source archive. Bump alongside the version.
$PaSha256  = "5af29ba58bbdbb7bbcefaaecc77ec8fc413f0db6f4c4e286c40c3e1b83174fa0"
$OutDir    = "third_party\portaudio"
$TarFile   = "third_party\portaudio-${PaVersion}.tar.gz"

# ── Check if already set up ──────────────────────────────────────────────
# The stamp is part of the test on purpose. A lib built before the #5713
# patch below is indistinguishable from a patched one by presence alone —
# CMake only checks that the header and lib exist — so testing the lib alone
# would leave a pre-patch build in place forever and silently, which is the
# opposite of the fail-loud property the patch block is built around. The one
# person guaranteed to hit that is whoever is reproducing #5713.
$PatchStamp = "$OutDir\.patched-5713"
if ((Test-Path "$OutDir\lib\portaudio_static_x64.lib") -and (Test-Path $PatchStamp)) {
    Write-Host "PortAudio already set up in $OutDir" -ForegroundColor Green
    exit 0
}

# ── Create directories ───────────────────────────────────────────────────
New-Item -ItemType Directory -Force -Path "third_party" | Out-Null
New-Item -ItemType Directory -Force -Path $OutDir | Out-Null
New-Item -ItemType Directory -Force -Path "$OutDir\lib" | Out-Null
New-Item -ItemType Directory -Force -Path "$OutDir\include" | Out-Null

# ── Download source ─────────────────────────────────────────────────────
if (-not (Test-Path $TarFile)) {
    Write-Host "Downloading PortAudio ${PaVersion} source..." -ForegroundColor Cyan
    Invoke-WebRequest -Uri $PaUrl -OutFile $TarFile
}
# Verify OUTSIDE the download guard. This script deletes the tarball on
# success, so a tarball that survives to a later run is by definition from a
# run that failed partway — possibly mid-download. Verifying only what we just
# fetched would consume that one unchecked.
Confirm-Sha256 -Path $TarFile -Expected $PaSha256

# ── Extract ──────────────────────────────────────────────────────────────
Write-Host "Extracting..." -ForegroundColor Cyan
$tempDir = "third_party\portaudio-temp"
if (Test-Path $tempDir) { Remove-Item -Recurse -Force $tempDir }
New-Item -ItemType Directory -Force -Path $tempDir | Out-Null
tar -xzf $TarFile -C $tempDir 2>$null

$srcDir = Get-ChildItem "$tempDir\portaudio-*" -Directory | Select-Object -First 1
if (-not $srcDir) {
    Write-Error "Failed to locate extracted PortAudio source"
    exit 1
}

# ── Patch: free the allocation, not the pointer's address (#5713) ─────────
# PortAudio v19.7.0 (and master as of 2026-09-16) frees the wrong pointer in
# three places. Each is the same one-token slip: the function takes a
# KSMULTIPLE_ITEM** out-param, allocates into *ksMultipleItem, then on its
# error branch hands ksMultipleItem — the caller's stack slot — to
# PaUtil_FreeMemory, which is GlobalFree on Windows. The heap manager gets a
# stack address and the process dies with 0xc0000374 STATUS_HEAP_CORRUPTION
# inside Pa_Initialize(). The real allocation is leaked on the way past.
#
#   src/os/win/pa_win_wdmks_utils.c   WdmGetPinPropertyMulti   (:157)
#       Reached from the DirectSound and MME per-device channel-count queries
#       (pa_win_ds.c:920/:1082, pa_win_wmme.c:663/:796), all four compiled in
#       by PA_USE_WDMKS_DEVICE_INFO, which defaults ON.
#   src/hostapi/wdmks/pa_win_wdmks.c  WdmGetPinPropertyMulti   (:902)
#   src/hostapi/wdmks/pa_win_wdmks.c  WdmGetPropertyMulti      (:951)
#       Reached from PinNew() during WDM-KS pin enumeration — PA_USE_WDMKS,
#       also ON by default — so this file ships in the same static lib.
#
# The trigger is a kernel-streaming driver whose second IOCTL_KS_PROPERTY
# reply disagrees with its own size query. Every FlexRadio DAX 2.0.3 endpoint
# does exactly that (pin 0, KSPROPERTY_PIN_DATARANGES: size query answers
# 96 bytes / ERROR_MORE_DATA, data query returns 0 bytes / ERROR_MORE_DATA),
# and those endpoints exist only while DAX.exe is running — which is why the
# #5713 connect crash needs DAX and nothing else. WdmSyncIoctl() swallows
# ERROR_MORE_DATA only when outBufferCount == 0, i.e. on the size query, so
# the data query's non-zero buffer turns the same reply into a hard error and
# takes the bad branch. Measured on a FLEX-8400: 3/3 crashes unpatched, clean
# with these lines fixed.
#
# Considered and rejected: -DPA_USE_WDMKS_DEVICE_INFO=OFF. It would compile
# out all four call sites into the utils copy and is immune to upstream
# reformatting — but it does not touch pa_win_wdmks.c's own two copies, which
# PA_USE_WDMKS keeps in the build, and it costs the DirectSound and MME
# reported channel counts for every device. Patching fixes the defect instead
# of routing around one of its two entry points, and it survives the day
# someone adds a caller.
#
# Reported upstream as PortAudio/portaudio#1176. When PortAudio ships the fix,
# delete this block rather than leaving the guard to stop a build on the first
# version that carries it.
#
# Replacement is exact-match with an expected hit count per file, so a
# PortAudio bump cannot silently drop a patch — it stops the build instead.
$paPatches = @(
    @{
        File     = "src\os\win\pa_win_wdmks_utils.c"
        Expected = 1
        Sites    = "WdmGetPinPropertyMulti (DirectSound/MME device-info query)"
        Bad      = "        PaUtil_FreeMemory( ksMultipleItem );`n        return paUnanticipatedHostError;"
        Good     = "        PaUtil_FreeMemory( *ksMultipleItem );   /* AetherSDR #5713: was ksMultipleItem (the caller's stack slot) */`n        *ksMultipleItem = NULL;`n        return paUnanticipatedHostError;"
    },
    @{
        File     = "src\hostapi\wdmks\pa_win_wdmks.c"
        Expected = 2
        Sites    = "WdmGetPinPropertyMulti + WdmGetPropertyMulti (WDM-KS pin enumeration)"
        Bad      = "        PaUtil_FreeMemory( ksMultipleItem );`n    }`n`n    return result;`n}"
        Good     = "        PaUtil_FreeMemory( *ksMultipleItem );   /* AetherSDR #5713: was ksMultipleItem (the caller's stack slot) */`n        *ksMultipleItem = NULL;`n    }`n`n    return result;`n}"
    }
)

foreach ($paPatch in $paPatches) {
    $paFile = Join-Path $srcDir.FullName $paPatch.File
    $paText = (Get-Content -Raw -Encoding UTF8 $paFile) -replace "`r`n", "`n"
    $paHits = ([regex]::Matches($paText, [regex]::Escape($paPatch.Bad))).Count
    if ($paHits -ne $paPatch.Expected) {
        Write-Error "$($paPatch.File): expected exactly $($paPatch.Expected) ksMultipleItem error-branch free(s) to patch, found $paHits - PortAudio source changed, re-check the #5713 patch"
        exit 1
    }
    [IO.File]::WriteAllText($paFile, $paText.Replace($paPatch.Bad, $paPatch.Good), (New-Object Text.UTF8Encoding $false))
    Write-Host "Applied #5713 fix to $($paPatch.File) - $paHits site(s): $($paPatch.Sites)" -ForegroundColor Yellow
}

# ── Build with CMake + MSVC ──────────────────────────────────────────────
Write-Host "Building PortAudio from source with MSVC..." -ForegroundColor Cyan

$buildDir = "$($srcDir.FullName)\build"
# CMAKE_POLICY_VERSION_MINIMUM: v19.7.0's CMakeLists declares a
# cmake_minimum_required below 3.5, which CMake 4.x refuses outright — same
# situation and same fix as setup-hidapi.ps1. The flag MUST be quoted:
# PowerShell's native-argument tokenizer splits an unquoted -Dkey=3.5 at
# the dot, so CMake receives "3" and rejects it.
cmake -B $buildDir -S $srcDir.FullName -G "Ninja" `
    -DCMAKE_BUILD_TYPE=Release `
    -DPA_BUILD_SHARED=OFF `
    -DPA_BUILD_STATIC=ON `
    -DPA_USE_WASAPI=ON `
    -DPA_BUILD_EXAMPLES=OFF `
    -DPA_BUILD_TESTS=OFF `
    "-DCMAKE_POLICY_VERSION_MINIMUM=3.5"

cmake --build $buildDir --config Release -j $env:NUMBER_OF_PROCESSORS

# ── Find and copy built artifacts ────────────────────────────────────────
$libFile = Get-ChildItem "$buildDir" -Recurse -Filter "portaudio_static_x64.lib" | Select-Object -First 1
if (-not $libFile) {
    Write-Error "Failed to build portaudio_static_x64.lib"
    exit 1
}

Copy-Item $libFile.FullName "$OutDir\lib\portaudio_static_x64.lib"
# Public header plus the pa_win_* host-API headers (WASAPI stream options etc.)
Copy-Item "$($srcDir.FullName)\include\*.h" "$OutDir\include\"

# Stamp the output so the early-exit guard above can tell a patched lib from a
# pre-#5713 one. Written only after the lib is in place, so an interrupted run
# re-patches and rebuilds rather than claiming a patch it never applied.
Set-Content -Path $PatchStamp -Value "#5713 ksMultipleItem free - pa_win_wdmks_utils.c, pa_win_wdmks.c (x2)" -Encoding UTF8

# ── Cleanup ──────────────────────────────────────────────────────────────
Remove-Item -Recurse -Force $tempDir
Remove-Item -Force $TarFile

Write-Host "PortAudio ready in $OutDir" -ForegroundColor Green
Write-Host "  Header: $OutDir\include\portaudio.h"
Write-Host "  Lib:    $OutDir\lib\portaudio_static_x64.lib"
