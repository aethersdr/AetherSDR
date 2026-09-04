<#
.SYNOPSIS
    Stage application and deployed Qt PDBs for Store and local debugging.

.DESCRIPTION
    Run this immediately after windeployqt --pdb. The script verifies that
    every deployed Qt DLL/plugin has the same-named PDB beside it, copies all
    deployed PDBs plus AetherSDR.pdb into one flat, architecture-specific
    symbol directory, and removes PDBs from the runtime deploy tree.

    Microsoft Store .appxsym files contain the PDBs for one architecture at
    the archive root. create-msix.ps1 consumes this directory without adding
    another path level.
#>

[CmdletBinding()]
param(
    [string]$DeployDir = "deploy",
    [string]$ApplicationPdb = "build/AetherSDR.pdb",
    [string]$OutputDir = "symbols",
    [string]$QtRootDir = $env:QT_ROOT_DIR
)

$ErrorActionPreference = "Stop"

function Resolve-InputPath {
    param(
        [Parameter(Mandatory = $true)]
        [string]$Path
    )

    return $ExecutionContext.SessionState.Path.GetUnresolvedProviderPathFromPSPath($Path)
}

function Get-RelativeDeployPath {
    param(
        [Parameter(Mandatory = $true)]
        [string]$Root,
        [Parameter(Mandatory = $true)]
        [string]$Path
    )

    $prefix = $Root.TrimEnd([System.IO.Path]::DirectorySeparatorChar, [System.IO.Path]::AltDirectorySeparatorChar)
    return $Path.Substring($prefix.Length).TrimStart([System.IO.Path]::DirectorySeparatorChar, [System.IO.Path]::AltDirectorySeparatorChar).Replace("\", "/")
}

$resolvedDeployDir = Resolve-InputPath $DeployDir
$resolvedApplicationPdb = Resolve-InputPath $ApplicationPdb
$resolvedOutputDir = Resolve-InputPath $OutputDir
$resolvedQtRootDir = $(if ([string]::IsNullOrWhiteSpace($QtRootDir)) { $null } else { Resolve-InputPath $QtRootDir })

if (-not (Test-Path -LiteralPath $resolvedDeployDir -PathType Container)) {
    throw "Deploy directory not found: $resolvedDeployDir"
}
if (-not (Test-Path -LiteralPath $resolvedApplicationPdb -PathType Leaf)) {
    throw "Application PDB not found: $resolvedApplicationPdb"
}
if (-not $resolvedQtRootDir -or -not (Test-Path -LiteralPath $resolvedQtRootDir -PathType Container)) {
    throw "QtRootDir is required and must identify the exact Qt installation used by windeployqt."
}

# Identify Qt binaries by mapping each deployed relative path back to the exact
# Qt installation used by windeployqt. This covers new plugin directories
# automatically instead of maintaining a fragile directory allow-list.
$qtBinaries = @()
foreach ($binary in Get-ChildItem -LiteralPath $resolvedDeployDir -Recurse -File -Filter "*.dll") {
    $relativePath = Get-RelativeDeployPath -Root $resolvedDeployDir -Path $binary.FullName
    if ($binary.Name -ieq "qt6keychain.dll") {
        continue # injected into Qt/bin only so windeployqt can scan it; not a Qt SDK binary
    }

    if ($relativePath.Contains("/")) {
        $sourcePath = Join-Path (Join-Path $resolvedQtRootDir "plugins") $relativePath.Replace("/", [System.IO.Path]::DirectorySeparatorChar)
    }
    else {
        # Qt's Multimedia module also installs third-party FFmpeg DLLs in
        # Qt/bin without corresponding Qt PDBs. Root libraries owned by Qt
        # carry the Qt6 prefix; leave other bundled dependencies out of this
        # Qt-symbol completeness check.
        if ($binary.Name -notlike "Qt6*.dll") {
            continue
        }
        $sourcePath = Join-Path (Join-Path $resolvedQtRootDir "bin") $binary.Name
    }

    if (Test-Path -LiteralPath $sourcePath -PathType Leaf) {
        $qtBinaries += [pscustomobject]@{
            Deployed = $binary
            Source = Get-Item -LiteralPath $sourcePath
            RelativePath = $relativePath
        }
    }
}
$qtBinaries = @($qtBinaries | Sort-Object RelativePath)

if ($qtBinaries.Count -eq 0) {
    throw "No deployed Qt DLLs were found under $resolvedDeployDir. Run windeployqt before staging symbols."
}

$missingPdbs = @()
foreach ($binary in $qtBinaries) {
    $sourcePdbPath = [System.IO.Path]::ChangeExtension($binary.Source.FullName, ".pdb")
    $deployedPdbPath = [System.IO.Path]::ChangeExtension($binary.Deployed.FullName, ".pdb")
    if (-not (Test-Path -LiteralPath $sourcePdbPath -PathType Leaf)) {
        $missingPdbs += "$($binary.RelativePath) (missing from Qt installation)"
        continue
    }
    if (-not (Test-Path -LiteralPath $deployedPdbPath -PathType Leaf)) {
        $missingPdbs += "$($binary.RelativePath) (not deployed)"
        continue
    }

    $sourceHash = (Get-FileHash -LiteralPath $sourcePdbPath -Algorithm SHA256).Hash
    $deployedHash = (Get-FileHash -LiteralPath $deployedPdbPath -Algorithm SHA256).Hash
    if ($sourceHash -ne $deployedHash) {
        throw "Deployed PDB does not match the exact Qt-installation PDB for $($binary.RelativePath)."
    }
}

if ($missingPdbs.Count -gt 0) {
    throw ("windeployqt did not deploy matching PDBs for these Qt binaries: " + ($missingPdbs -join ", ") +
        ". Install the Qt debug_info and <module>.debug_information archives and run windeployqt --pdb.")
}

$deployPdbs = @(Get-ChildItem -LiteralPath $resolvedDeployDir -Recurse -File -Filter "*.pdb" | Sort-Object FullName)
$allPdbs = @((Get-Item -LiteralPath $resolvedApplicationPdb)) + $deployPdbs
$duplicateNames = @($allPdbs | Group-Object Name | Where-Object { $_.Count -gt 1 })
if ($duplicateNames.Count -gt 0) {
    $details = $duplicateNames | ForEach-Object {
        "$($_.Name): $(($_.Group.FullName) -join ', ')"
    }
    throw "PDB names must be unique when flattened into one .appxsym: $($details -join '; ')"
}

if (Test-Path -LiteralPath $resolvedOutputDir) {
    Remove-Item -LiteralPath $resolvedOutputDir -Recurse -Force
}
New-Item -ItemType Directory -Path $resolvedOutputDir -Force | Out-Null

$manifestEntries = @()
foreach ($pdb in $allPdbs) {
    $destination = Join-Path $resolvedOutputDir $pdb.Name
    Copy-Item -LiteralPath $pdb.FullName -Destination $destination -Force

    $sourceBinary = $null
    if ($pdb.FullName -eq $resolvedApplicationPdb) {
        $candidate = Join-Path $resolvedDeployDir "AetherSDR.exe"
        if (Test-Path -LiteralPath $candidate -PathType Leaf) {
            $sourceBinary = $candidate
        }
    }
    else {
        $candidate = [System.IO.Path]::ChangeExtension($pdb.FullName, ".dll")
        if (Test-Path -LiteralPath $candidate -PathType Leaf) {
            $sourceBinary = $candidate
        }
    }

    $manifestEntries += [ordered]@{
        pdb = $pdb.Name
        pdbSha256 = (Get-FileHash -LiteralPath $destination -Algorithm SHA256).Hash.ToLowerInvariant()
        binary = $(if ($sourceBinary) { Get-RelativeDeployPath -Root $resolvedDeployDir -Path $sourceBinary } else { $null })
        binarySha256 = $(if ($sourceBinary) { (Get-FileHash -LiteralPath $sourceBinary -Algorithm SHA256).Hash.ToLowerInvariant() } else { $null })
    }
}

$manifestPath = Join-Path $resolvedOutputDir "symbols-manifest.json"
$manifestEntries | ConvertTo-Json -Depth 4 | Set-Content -LiteralPath $manifestPath -Encoding UTF8

# The portable ZIP, Inno installer, and MSIX all consume deploy/. Symbols are
# Store/debugging metadata and must not ship in those runtime payloads.
foreach ($pdb in $deployPdbs) {
    Remove-Item -LiteralPath $pdb.FullName -Force
}

$remainingPdbs = @(Get-ChildItem -LiteralPath $resolvedDeployDir -Recurse -File -Filter "*.pdb")
if ($remainingPdbs.Count -gt 0) {
    throw "PDB files remain in the runtime deploy tree: $(($remainingPdbs.FullName) -join ', ')"
}

Write-Host "Staged $($allPdbs.Count) PDBs in $resolvedOutputDir ($($qtBinaries.Count) Qt binaries verified)."
Write-Host "Symbol manifest: $manifestPath"
