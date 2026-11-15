<#
.SYNOPSIS
    Stage app-local MSVC runtime DLLs for the Windows installer.

.DESCRIPTION
    Finds the x64 Microsoft.VC*.CRT redist directory from the active Visual
    Studio/MSVC environment and copies its DLLs to a staging directory for
    Inno Setup. Prefer VCToolsRedistDir because GitHub Actions initializes it
    through ilammy/msvc-dev-cmd before packaging.

    The OpenMP runtime (vcomp140.dll) ships in a SEPARATE redist directory,
    Microsoft.VC<nnn>.OpenMP, sitting next to the CRT one -- it is not in
    Microsoft.VC*.CRT. ggml is compiled with /openmp (GGML_OPENMP defaults ON)
    and its static libs carry /DEFAULTLIB:"VCOMP", so AetherSDR.exe imports
    vcomp140.dll directly. Staging only the CRT left that one import to be
    satisfied by whatever machine-wide redistributable happened to be present,
    and any other installer repairing/downgrading/removing it produced
    "VCOMP140.DLL was not found" on startup -- unfixable by reinstalling
    AetherSDR, because the file was never in our payload (#4781). Both
    directories are staged here, version-matched, so the payload is
    self-contained.
#>

# Keep this file ASCII-only. CI runs it with `powershell` (Windows PowerShell
# 5.1), which decodes a BOM-less .ps1 as the ANSI codepage: a UTF-8 em-dash
# becomes the three characters `a"` in CP1252, and PowerShell honours that curly
# quote as a string delimiter. Inside a double-quoted string that ends the string
# early and the whole file fails to PARSE -- not a cosmetic mojibake, a script
# that never runs. The sibling scripts under scripts/setup/ do contain non-ASCII
# and are fine only because the workflow invokes those with `pwsh` (UTF-8).

[CmdletBinding()]
param(
    [string]$OutputDir = "installer-runtime"
)

$ErrorActionPreference = "Stop"

function Get-MsvcRuntimeVersion {
    param([System.IO.DirectoryInfo]$Directory)

    if ($Directory.FullName -match "\\MSVC\\([^\\]+)\\x64\\Microsoft\.VC[^\\]+\.CRT$") {
        $versionText = $Matches[1]
        $parsed = $null
        if ([System.Version]::TryParse($versionText, [ref]$parsed)) {
            return $parsed
        }
    }

    return [System.Version]::new(0, 0)
}

function Get-CrtDirsFromRedistRoot {
    param([string]$RedistRoot)

    if ([string]::IsNullOrWhiteSpace($RedistRoot)) {
        return @()
    }

    $x64Root = Join-Path $RedistRoot "x64"
    if (-not (Test-Path -LiteralPath $x64Root)) {
        return @()
    }

    return @(Get-ChildItem -LiteralPath $x64Root -Directory -Filter "Microsoft.VC*.CRT" -ErrorAction SilentlyContinue)
}

$candidateDirs = @()

if ($env:VCToolsRedistDir) {
    $candidateDirs += Get-CrtDirsFromRedistRoot -RedistRoot $env:VCToolsRedistDir
}

if (-not $candidateDirs) {
    $vsRoots = @(
        "${env:ProgramFiles}\Microsoft Visual Studio",
        "${env:ProgramFiles(x86)}\Microsoft Visual Studio"
    ) | Where-Object { $_ -and (Test-Path -LiteralPath $_) }

    foreach ($root in $vsRoots) {
        $candidateDirs += Get-ChildItem -LiteralPath $root -Recurse -Directory -Filter "Microsoft.VC*.CRT" -ErrorAction SilentlyContinue |
            Where-Object { $_.FullName -match "\\VC\\Redist\\MSVC\\[^\\]+\\x64\\Microsoft\.VC[^\\]+\.CRT$" }
    }
}

$runtimeDir = $candidateDirs |
    Sort-Object @{ Expression = { Get-MsvcRuntimeVersion -Directory $_ }; Descending = $true }, FullName -Descending |
    Select-Object -First 1

if (-not $runtimeDir) {
    throw "Could not find an x64 Microsoft.VC*.CRT runtime directory. Make sure the MSVC redist components are installed."
}

# Take the OpenMP redist as a SIBLING of the chosen CRT directory, deriving its
# name from that directory instead of globbing independently: the CRT search
# above sorts descending to take the newest, so an independent search with its
# own ordering could silently pick a different toolset the moment the redist
# layout stops being version-scoped. Deriving the name makes the version match
# an invariant rather than a coincidence -- Microsoft.VC143.CRT pairs with
# Microsoft.VC143.OpenMP, and nothing else.
$redistArchRoot = Split-Path -Parent $runtimeDir.FullName
$openMpDirName = $runtimeDir.Name -replace '\.CRT$', '.OpenMP'
$expectedOpenMpDir = Join-Path $redistArchRoot $openMpDirName
$openMpDir = Get-ChildItem -LiteralPath $redistArchRoot -Directory -Filter $openMpDirName -ErrorAction SilentlyContinue |
    Select-Object -First 1

if (-not $openMpDir) {
    # Hard failure, not a warning. Silently omitting this DLL is precisely the
    # bug in #4781, and a broken installer is far more expensive than a red CI
    # run. The OpenMP redist installs with the "MSVC v143 - VS 2022 C++ x64/x86
    # build tools" component that already provides the CRT redist next to it.
    throw "Could not find the OpenMP runtime directory $expectedOpenMpDir (sibling of the chosen CRT redist $($runtimeDir.FullName)). " +
          "AetherSDR.exe imports vcomp140.dll (ggml is built with /openmp), so it must ship app-local. " +
          "Install the MSVC v143 C++ build tools redist component and retry."
}

$resolvedOutputDir = $ExecutionContext.SessionState.Path.GetUnresolvedProviderPathFromPSPath($OutputDir)
New-Item -ItemType Directory -Force -Path $resolvedOutputDir | Out-Null

$sourceDirs = @($runtimeDir, $openMpDir)
foreach ($sourceDir in $sourceDirs) {
    $runtimeDlls = Get-ChildItem -LiteralPath $sourceDir.FullName -File -Filter "*.dll"
    if (-not $runtimeDlls) {
        throw "No runtime DLLs found in $($sourceDir.FullName)."
    }

    foreach ($dll in $runtimeDlls) {
        Copy-Item -LiteralPath $dll.FullName -Destination $resolvedOutputDir -Force
    }

    Write-Host "Staged MSVC runtime from $($sourceDir.FullName)"
}

# Guard the one file this whole script exists to guarantee: a future refactor
# that loses it again must fail here, not in a user's startup dialog.
if (-not (Test-Path -LiteralPath (Join-Path $resolvedOutputDir "vcomp140.dll"))) {
    throw "vcomp140.dll was not staged from $($openMpDir.FullName) -- the payload would fail to start on machines without a system-wide VC++ redistributable."
}

Get-ChildItem -LiteralPath $resolvedOutputDir -File -Filter "*.dll" |
    Sort-Object Name |
    ForEach-Object { Write-Host "  $($_.Name)" }
