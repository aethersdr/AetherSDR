<#
.SYNOPSIS
    Install the exact Qt AetherSDR releases are built with, for a Windows
    source build.

.DESCRIPTION
    The Windows counterpart of setup-qt.sh. It installs the pinned Qt from
    cmake/qt-pin.env (the file every CI leg and release workflow is checked
    against) into a per-user cache shared by every checkout:

        %LOCALAPPDATA%\aethersdr\qt\<version>-<revision>\

    (override with AETHER_QT_CACHE), then builds qtkeychain against it with
    setup-qtkeychain.ps1. CMake looks there on its own (cmake/AetherQtPin.cmake),
    so afterwards a plain configure finds the pinned Qt with no -D flags.

    Two things differ from Linux and macOS, and both are why this script exists
    rather than a doc line:

    - aqtinstall: the newest PyPI release (3.3.0) cannot read the Windows
      repository layout Qt 6.11+ uses ("Failed to locate XML data"), so this
      installs aqt from the commit pinned as AQTINSTALL_GIT_REF - the same one
      CI and the release installer use.
    - extraction: py7zr fails at random on Windows Qt archives (aqtinstall #995),
      so 7-Zip is used when installed. Without it the script falls back to
      py7zr and says so.

    The checks run BEFORE the ~2 GB download: Visual Studio new enough to link
    Qt's static libraries (QT_MIN_VS), a usable Python, free disk space, and
    the Qt repository still serving the pinned package revision.

    Works in Windows PowerShell 5.1 and PowerShell 7. Requires git (for the
    pinned aqt) and, for qtkeychain, cmake, ninja and the MSVC environment
    (vcvars64.bat) - see docs/BUILDING.md.

.PARAMETER PrintPrefix
    Print the Qt prefix CMake will use, and exit.

.PARAMETER NoKeychain
    Skip the qtkeychain build (SmartLink credential persistence compiles out).

.EXAMPLE
    powershell -File scripts\setup\setup-qt.ps1
#>
param(
    [switch]$PrintPrefix,
    [switch]$NoKeychain
)

$ErrorActionPreference = "Stop"

function Fail([string]$Message) {
    Write-Host "ERROR: $Message" -ForegroundColor Red
    exit 1
}

$RepoRoot = [System.IO.Path]::GetFullPath("$PSScriptRoot\..\..")
$PinFile  = Join-Path $RepoRoot "cmake\qt-pin.env"
if (-not (Test-Path $PinFile)) { Fail "$PinFile is missing." }

# KEY=value lines, values optionally double-quoted - the same parse as
# cmake/AetherQtPin.cmake and tools/check_qt_pin.py.
$Pin = @{}
foreach ($line in Get-Content $PinFile) {
    if ($line -match '^([A-Z0-9_]+)="?([^"]*)"?$') { $Pin[$Matches[1]] = $Matches[2] }
}
foreach ($key in "QT_VERSION", "QT_PACKAGE_REVISION", "AQTINSTALL_GIT_REF",
                 "PY7ZR_VERSION", "QT_MODULES", "QT_MIN_VS") {
    if (-not $Pin[$key]) { Fail "cmake/qt-pin.env has no $key." }
}
$QtVersion = $Pin["QT_VERSION"]
$Revision  = $Pin["QT_PACKAGE_REVISION"]
$Modules   = $Pin["QT_MODULES"] -split '\s+'

# x64 only: AetherSDR ships no Windows ARM64 build, and Qt's ARM64 kit is a
# cross-compiled one that would need its own deployment story.
if ($env:PROCESSOR_ARCHITECTURE -ne "AMD64") {
    Fail "this script installs the x64 (msvc2022_64) kit; this machine is $env:PROCESSOR_ARCHITECTURE."
}
$AqtArch = "win64_msvc2022_64"
$KitDir  = "msvc2022_64"

$CacheRoot = $env:AETHER_QT_CACHE
if (-not $CacheRoot) { $CacheRoot = Join-Path $env:LOCALAPPDATA "aethersdr\qt" }
# Revision in the path for the reason setup-qt.sh gives: RC and final share a
# version string.
$InstallRoot  = Join-Path $CacheRoot "$QtVersion-$Revision"
$QtPrefix     = Join-Path $InstallRoot "$QtVersion\$KitDir"
$Stamp        = Join-Path $InstallRoot ".aether-qt-stamp"
$StampContent = "version=$QtVersion revision=$Revision arch=$AqtArch modules=$($Pin['QT_MODULES']) aqt=$($Pin['AQTINSTALL_GIT_REF'])"

if ($PrintPrefix) { Write-Output $QtPrefix; exit 0 }

function Build-Keychain {
    if ($NoKeychain) { return }
    Write-Host ""
    Write-Host "Building qtkeychain against Qt $QtVersion (skip with -NoKeychain)..." -ForegroundColor Cyan
    $saved = $env:QT_ROOT_DIR
    $env:QT_ROOT_DIR = $QtPrefix
    try {
        # Same PowerShell that is running this script (5.1 or 7), in its own
        # process so the child's `exit` cannot end this one.
        $hostExe = (Get-Process -Id $PID).Path
        & $hostExe -NoProfile -ExecutionPolicy Bypass -File (Join-Path $PSScriptRoot "setup-qtkeychain.ps1")
        if ($LASTEXITCODE -ne 0) { Fail "setup-qtkeychain.ps1 failed (exit $LASTEXITCODE)." }
    } finally {
        $env:QT_ROOT_DIR = $saved
    }
}

# -- Already installed? ----------------------------------------------------
$qmake = Join-Path $QtPrefix "bin\qmake.exe"
if ((Test-Path $Stamp) -and ((Get-Content $Stamp -Raw).Trim() -eq $StampContent) -and
    (Test-Path $qmake) -and ((& $qmake -query QT_VERSION) -eq $QtVersion)) {
    Write-Host "Qt $QtVersion ($Revision) already installed at $QtPrefix" -ForegroundColor Green
    Build-Keychain
    exit 0
}

# -- Preflight: refuse before downloading, with the way out ---------------
$vswhere = Join-Path ${env:ProgramFiles(x86)} "Microsoft Visual Studio\Installer\vswhere.exe"
if (-not (Test-Path $vswhere)) {
    Fail "Visual Studio not found. Install Visual Studio 2022 $($Pin['QT_MIN_VS'])+ (Build Tools
       is enough) with the 'Desktop development with C++' workload."
}
$vsVersion = & $vswhere -latest -products * `
    -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 `
    -property catalog_productDisplayVersion
if (-not $vsVersion) {
    Fail "no Visual Studio install has the MSVC x64 tools. Add the 'Desktop development
       with C++' workload in the Visual Studio Installer."
}
$vsVersion = ($vsVersion | Select-Object -First 1).Trim()
# catalog_productDisplayVersion is e.g. "17.14.3"; compare numerically.
if ([version]($vsVersion -replace '[^0-9.].*$', '') -lt [version]$Pin["QT_MIN_VS"]) {
    Fail "Qt $QtVersion's static libraries need Visual Studio $($Pin['QT_MIN_VS']) or newer to link;
       this machine has $vsVersion. Update it in the Visual Studio Installer."
}
Write-Host "Visual Studio $vsVersion - OK (Qt $QtVersion needs $($Pin['QT_MIN_VS']))"

# The Microsoft Store's python.exe stub opens the Store instead of running, so
# probe for a real interpreter rather than trusting that one is on PATH.
$Python = $null
foreach ($candidate in @(@("py", "-3"), @("python"))) {
    $exe = $candidate[0]
    $pre = @($candidate | Select-Object -Skip 1)
    if (-not (Get-Command $exe -ErrorAction SilentlyContinue)) { continue }
    $out = & $exe @pre -c "import sys, venv; print(sys.version_info[0])" 2>$null
    if ($LASTEXITCODE -eq 0 -and $out -eq "3") { $Python = $candidate; break }
}
if (-not $Python) {
    Fail "no usable Python 3 found (needed for aqtinstall). Install it from python.org
       or with:  winget install Python.Python.3.12"
}
if (-not (Get-Command git -ErrorAction SilentlyContinue)) {
    Fail "git not found; the pinned aqtinstall is installed from its git repository."
}

New-Item -ItemType Directory -Force -Path $CacheRoot | Out-Null
$drive = New-Object System.IO.DriveInfo ([System.IO.Path]::GetPathRoot($CacheRoot))
if ($drive.AvailableFreeSpace -lt 3GB) {
    Fail "need ~3 GB free under $CacheRoot, have $([int]($drive.AvailableFreeSpace / 1MB)) MB.
       Point AETHER_QT_CACHE at a roomier drive."
}

# Qt 6.11+ keeps one Updates.xml per architecture on Windows - the layout PyPI
# aqt cannot read. A republished build behind the same version string must not
# install silently; see setup-qt.sh.
$verTag     = "qt6_" + ($QtVersion -replace '\.', '')
$pkg        = "qt.qt6." + ($QtVersion -replace '\.', '') + ".$AqtArch"
$updatesUrl = "https://download.qt.io/online/qtsdkrepository/windows_x86/desktop/$verTag/${verTag}_$KitDir/Updates.xml"
try {
    $xml = (Invoke-WebRequest -Uri $updatesUrl -UseBasicParsing -TimeoutSec 60).Content
} catch {
    Fail "could not read $updatesUrl (offline?)."
}
$served = $null
if ($xml -match "(?s)<Name>$([regex]::Escape($pkg))</Name>.*?<Version>([^<]+)</Version>") {
    $served = $Matches[1]
}
if (-not $served) { Fail "$pkg is not listed in $updatesUrl." }
if (-not $served.EndsWith("-$Revision")) {
    Fail "the Qt repository now serves $pkg $served, but cmake/qt-pin.env pins
       revision $Revision. Qt has republished $QtVersion; the pin needs a deliberate
       bump (and CI a re-run) before anyone builds against it."
}
Write-Host "Qt repository serves $served - matches the pin"

# -- Install ---------------------------------------------------------------
$Venv = Join-Path $CacheRoot "aqt-venv-$($Pin['AQTINSTALL_GIT_REF'].Substring(0, 12))-py7zr$($Pin['PY7ZR_VERSION'])"
$Aqt  = Join-Path $Venv "Scripts\aqt.exe"
if (-not (Test-Path (Join-Path $Venv ".complete"))) {
    if (Test-Path $Venv) { Remove-Item -Recurse -Force $Venv }
    $pyExe = $Python[0]
    $pyPre = @($Python | Select-Object -Skip 1)
    & $pyExe @pyPre -m venv $Venv
    if ($LASTEXITCODE -ne 0) { Fail "python -m venv failed (exit $LASTEXITCODE)." }
    & (Join-Path $Venv "Scripts\pip.exe") install -q `
        "git+https://github.com/miurahr/aqtinstall.git@$($Pin['AQTINSTALL_GIT_REF'])" `
        "py7zr==$($Pin['PY7ZR_VERSION'])"
    if ($LASTEXITCODE -ne 0) { Fail "installing aqtinstall failed (exit $LASTEXITCODE)." }
    New-Item -ItemType File -Force -Path (Join-Path $Venv ".complete") | Out-Null
}

$extract = @()
$sevenZip = Get-Command 7z.exe -ErrorAction SilentlyContinue
if (-not $sevenZip -and (Test-Path "$env:ProgramFiles\7-Zip\7z.exe")) {
    $sevenZip = Get-Item "$env:ProgramFiles\7-Zip\7z.exe"
}
if ($sevenZip) {
    $sevenZipPath = if ($sevenZip.Source) { $sevenZip.Source } else { $sevenZip.FullName }
    $extract = @("--external", $sevenZipPath)
    Write-Host "Extracting with 7-Zip ($sevenZipPath)"
} else {
    Write-Host "WARNING: 7-Zip not found, so aqt will extract with py7zr, which fails at random" -ForegroundColor Yellow
    Write-Host "         on Windows Qt archives (aqtinstall #995). If it does, install 7-Zip" -ForegroundColor Yellow
    Write-Host "         (winget install 7zip.7zip) and re-run." -ForegroundColor Yellow
}

# Staged, then renamed into place, so an interrupted install never leaves a
# half-populated kit for a later configure to trust.
$Staging = Join-Path $CacheRoot ".staging-$QtVersion-$Revision-$PID"
if (Test-Path $Staging) { Remove-Item -Recurse -Force $Staging }
# Parallel 7-Zip workers race to create the shared output root on a fresh
# install (seen on CI); create it up front, as ci.yml does.
$StagedPrefix = Join-Path $Staging "$QtVersion\$KitDir"
New-Item -ItemType Directory -Force -Path $StagedPrefix | Out-Null

try {
    Write-Host "Installing Qt $QtVersion $AqtArch ($($Pin['QT_MODULES'])) - about 2 GB..." -ForegroundColor Cyan
    $aqtArgs = @("install-qt", "windows", "desktop", $QtVersion, $AqtArch, "-m") + $Modules +
               $extract + @("--outputdir", $Staging)
    & $Aqt @aqtArgs
    if ($LASTEXITCODE -ne 0) { Fail "aqt install-qt failed (exit $LASTEXITCODE)." }

    $stagedQmake = Join-Path $StagedPrefix "bin\qmake.exe"
    if (-not (Test-Path $stagedQmake)) { Fail "aqt finished but left no qmake at $StagedPrefix." }
    $got = & $stagedQmake -query QT_VERSION
    if ($got -ne $QtVersion) { Fail "aqt installed Qt $got, expected $QtVersion." }
    # qmake only proves qtbase landed; a partial install can still exit 0.
    $moduleConfig = @{ qtmultimedia = "Multimedia"; qtwebsockets = "WebSockets";
                       qtserialport = "SerialPort"; qtshadertools = "ShaderTools" }
    foreach ($m in $Modules) {
        if (-not $moduleConfig.ContainsKey($m)) { continue }
        $name = $moduleConfig[$m]
        if (-not (Test-Path (Join-Path $StagedPrefix "lib\cmake\Qt6$name\Qt6${name}Config.cmake"))) {
            Fail "aqt did not install Qt6$name ($m)."
        }
    }

    if (Test-Path $InstallRoot) { Remove-Item -Recurse -Force $InstallRoot }
    Move-Item -Path $Staging -Destination $InstallRoot
} finally {
    if (Test-Path $Staging) { Remove-Item -Recurse -Force $Staging -ErrorAction SilentlyContinue }
}
Set-Content -Path $Stamp -Value $StampContent -NoNewline
Write-Host "Qt $QtVersion installed at $QtPrefix" -ForegroundColor Green

Build-Keychain

Write-Host ""
Write-Host "Done. From a vcvars64 prompt, CMake finds this Qt automatically:"
Write-Host "    cmake -B build -G Ninja -DCMAKE_BUILD_TYPE=RelWithDebInfo"
Write-Host "(-DAETHER_USE_PINNED_QT=OFF to build against another Qt instead.)"
