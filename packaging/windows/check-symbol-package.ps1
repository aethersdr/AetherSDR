<#
.SYNOPSIS
    Verify the Store symbol and upload archive topology.

.DESCRIPTION
    A single-architecture .appxsym must contain only flat PDB entries. The
    .msixupload must contain that exact .appxsym beside the .msix at its root.
#>

[CmdletBinding()]
param(
    [Parameter(Mandatory = $true)]
    [string]$AppxSymPath,
    [Parameter(Mandatory = $true)]
    [string]$MsixUploadPath,
    [string[]]$RequiredPdbs = @(
        "AetherSDR.pdb",
        "Qt6Core.pdb",
        "Qt6Gui.pdb",
        "Qt6Multimedia.pdb",
        "Qt6Network.pdb",
        "Qt6Widgets.pdb",
        "qwindows.pdb",
        "windowsmediaplugin.pdb"
    )
)

$ErrorActionPreference = "Stop"
Add-Type -AssemblyName System.IO.Compression.FileSystem

function Resolve-RequiredFile {
    param(
        [Parameter(Mandatory = $true)]
        [string]$Path,
        [Parameter(Mandatory = $true)]
        [string]$Description
    )

    $resolved = $ExecutionContext.SessionState.Path.GetUnresolvedProviderPathFromPSPath($Path)
    if (-not (Test-Path -LiteralPath $resolved -PathType Leaf)) {
        throw "$Description not found: $resolved"
    }
    return $resolved
}

function Get-ZipEntryNames {
    param(
        [Parameter(Mandatory = $true)]
        [string]$Path
    )

    $archive = [System.IO.Compression.ZipFile]::OpenRead($Path)
    try {
        return @($archive.Entries | ForEach-Object { $_.FullName.Replace("\", "/") })
    }
    finally {
        $archive.Dispose()
    }
}

function Get-EmbeddedEntrySha256 {
    param(
        [Parameter(Mandatory = $true)]
        [string]$ArchivePath,
        [Parameter(Mandatory = $true)]
        [string]$EntryName
    )

    $archive = [System.IO.Compression.ZipFile]::OpenRead($ArchivePath)
    try {
        $entry = $archive.GetEntry($EntryName)
        if (-not $entry) {
            throw "Archive $ArchivePath does not contain $EntryName."
        }
        $stream = $entry.Open()
        $sha256 = [System.Security.Cryptography.SHA256]::Create()
        try {
            return ([BitConverter]::ToString($sha256.ComputeHash($stream))).Replace("-", "").ToLowerInvariant()
        }
        finally {
            $sha256.Dispose()
            $stream.Dispose()
        }
    }
    finally {
        $archive.Dispose()
    }
}

$resolvedAppxSym = Resolve-RequiredFile -Path $AppxSymPath -Description ".appxsym"
$resolvedMsixUpload = Resolve-RequiredFile -Path $MsixUploadPath -Description ".msixupload"

$symbolEntries = @(Get-ZipEntryNames -Path $resolvedAppxSym)
if ($symbolEntries.Count -eq 0) {
    throw ".appxsym is empty: $resolvedAppxSym"
}

$invalidSymbolEntries = @($symbolEntries | Where-Object {
    $_.Contains("/") -or -not $_.EndsWith(".pdb", [System.StringComparison]::OrdinalIgnoreCase)
})
if ($invalidSymbolEntries.Count -gt 0) {
    throw ".appxsym entries must be flat PDB files at the archive root: $($invalidSymbolEntries -join ', ')"
}

foreach ($requiredPdb in $RequiredPdbs) {
    if ($symbolEntries -notcontains $requiredPdb) {
        throw ".appxsym is missing required symbol $requiredPdb. Entries: $($symbolEntries -join ', ')"
    }
}

$uploadEntries = @(Get-ZipEntryNames -Path $resolvedMsixUpload)
$nestedUploadEntries = @($uploadEntries | Where-Object { $_.Contains("/") })
if ($nestedUploadEntries.Count -gt 0) {
    throw ".msixupload entries must be at the archive root: $($nestedUploadEntries -join ', ')"
}

$msixEntries = @($uploadEntries | Where-Object { $_.EndsWith(".msix", [System.StringComparison]::OrdinalIgnoreCase) })
$appxSymEntries = @($uploadEntries | Where-Object { $_.EndsWith(".appxsym", [System.StringComparison]::OrdinalIgnoreCase) })
if ($msixEntries.Count -ne 1 -or $appxSymEntries.Count -ne 1 -or $uploadEntries.Count -ne 2) {
    throw ".msixupload must contain exactly one .msix and one .appxsym at its root. Entries: $($uploadEntries -join ', ')"
}

$standaloneHash = (Get-FileHash -LiteralPath $resolvedAppxSym -Algorithm SHA256).Hash.ToLowerInvariant()
$embeddedHash = Get-EmbeddedEntrySha256 -ArchivePath $resolvedMsixUpload -EntryName $appxSymEntries[0]
if ($standaloneHash -ne $embeddedHash) {
    throw "The .appxsym embedded in .msixupload does not match the verified standalone .appxsym."
}

Write-Host "Verified .appxsym: $($symbolEntries.Count) flat PDBs."
Write-Host "Verified .msixupload: one .msix plus the identical .appxsym at archive root."
