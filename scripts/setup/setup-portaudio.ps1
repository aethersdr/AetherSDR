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

# ── The #5713 patch set ───────────────────────────────────────────────────
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
# takes the bad branch.
#
# Evidence, and its limits. The FLEX-8400 bench matrix (3/3 crashes unpatched,
# clean start patched) was measured against the pa_win_wdmks_utils.c site —
# the one an unpatched build reaches first, which is why it is the one that
# could be observed at all. The two pa_win_wdmks.c sites are fixed on source
# analysis plus a dry run against the pinned tarball; they have never been
# seen to fire, because the DirectSound query kills the process before WDM-KS
# host-API init is reached. Do not read this block as three bench-verified
# lines.
#
# *ksMultipleItem = NULL is load-bearing at the pa_win_wdmks.c sites rather
# than defensive: PinNew() declares KSMULTIPLE_ITEM* item = NULL (:1332) and
# frees item again at its error: label (:2023), so freeing the real allocation
# without clearing the out-param would convert the crash into a double free.
# PaUtil_FreeMemory null-checks, so the second free is then a no-op.
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
# The anchor is the bare statement, deliberately: it is already unique per
# file (1 in the utils copy, 2 in the host-API copy), so pinning the lines
# around it would buy no disambiguation while tripling the surface a cosmetic
# upstream reindent can break. The per-file Expected count does the
# disambiguating, and a mismatch stops the build rather than silently
# dropping a patch.
$paBad  = "        PaUtil_FreeMemory( ksMultipleItem );"
$paGood = "        PaUtil_FreeMemory( *ksMultipleItem );   /* AetherSDR #5713: was ksMultipleItem (the caller's stack slot) */`n        *ksMultipleItem = NULL;"
$paPatches = @(
    @{ File = "src\os\win\pa_win_wdmks_utils.c";  Expected = 1; Sites = "WdmGetPinPropertyMulti (DirectSound/MME device-info query)" }
    @{ File = "src\hostapi\wdmks\pa_win_wdmks.c"; Expected = 2; Sites = "WdmGetPinPropertyMulti + WdmGetPropertyMulti (WDM-KS pin enumeration)" }
)

# The stamp records WHICH patch set built the lib, not merely that one did.
# Presence alone would let a $PaVersion bump, or a later change to $paPatches,
# reuse a lib built for a different source tree while the fail-loud guard
# below never runs - the exact silent-drop this block exists to prevent.
$paSigSource = "portaudio=$PaVersion`n$paBad`n$paGood`n" +
    (($paPatches | ForEach-Object { "$($_.File)=$($_.Expected)" }) -join "`n")
$paSigHash = [BitConverter]::ToString(
    [Security.Cryptography.SHA256]::Create().ComputeHash(
        [Text.Encoding]::UTF8.GetBytes($paSigSource))).Replace("-", "").Substring(0, 16).ToLower()
$PatchSignature = "portaudio=$PaVersion patchset=5713 sig=$paSigHash"

# ── Check if already set up ──────────────────────────────────────────────
# The stamp is part of the test on purpose, and it is compared by CONTENT. A
# lib built before this patch - or for another PortAudio version, or another
# patch set - is otherwise indistinguishable from a current one by presence
# alone, and CMakeLists.txt only tests that the header and lib EXIST. The one
# person guaranteed to be bitten by a silently-reused stale lib is whoever is
# reproducing #5713.
$PatchStamp = "$OutDir\.patched-5713"
$stampCurrent = (Test-Path $PatchStamp) -and
                ((Get-Content -Raw -Encoding UTF8 $PatchStamp).Trim() -eq $PatchSignature)
if ((Test-Path "$OutDir\lib\portaudio_static_x64.lib") -and $stampCurrent) {
    Write-Host "PortAudio already set up in $OutDir ($PatchSignature)" -ForegroundColor Green
    exit 0
}

# Past this point we are rebuilding, so clear the old artefacts NOW rather
# than overwriting them at the end. Every step below can fail - download,
# checksum, the patch guard, cl.exe - and CMake accepts any lib+header pair
# that merely exists. Leaving them in place would turn an aborted run into a
# green build silently linking the unpatched, crashing PortAudio.
if (Test-Path $PatchStamp) { Remove-Item -Force $PatchStamp }
if (Test-Path "$OutDir\lib\portaudio_static_x64.lib") {
    Write-Host "Removing stale PortAudio lib (no current #5713 patch stamp)" -ForegroundColor Yellow
    Remove-Item -Force "$OutDir\lib\portaudio_static_x64.lib"
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

# ── Apply the #5713 patch set ─────────────────────────────────────────────
# Latin-1 round-trips every byte value, so a source file carrying a stray
# non-UTF-8 byte (accented contributor names turn up in PortAudio) survives
# the read/write instead of being rewritten with U+FFFD. Both target files are
# ASCII in 19.7.0; this is about the next bump, not this one.
$paEnc = [Text.Encoding]::GetEncoding(28591)
foreach ($paPatch in $paPatches) {
    $paFile = Join-Path $srcDir.FullName $paPatch.File
    $paText = $paEnc.GetString([IO.File]::ReadAllBytes($paFile)) -replace "`r`n", "`n"
    $paHits = ([regex]::Matches($paText, [regex]::Escape($paBad))).Count
    if ($paHits -ne $paPatch.Expected) {
        Write-Error "$($paPatch.File): expected exactly $($paPatch.Expected) ksMultipleItem error-branch free(s) to patch, found $paHits - PortAudio source changed, re-check the #5713 patch"
        exit 1
    }
    [IO.File]::WriteAllBytes($paFile, $paEnc.GetBytes($paText.Replace($paBad, $paGood)))
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
Set-Content -Path $PatchStamp -Value $PatchSignature -Encoding UTF8

# ── Cleanup ──────────────────────────────────────────────────────────────
Remove-Item -Recurse -Force $tempDir
Remove-Item -Force $TarFile

Write-Host "PortAudio ready in $OutDir" -ForegroundColor Green
Write-Host "  Header: $OutDir\include\portaudio.h"
Write-Host "  Lib:    $OutDir\lib\portaudio_static_x64.lib"
