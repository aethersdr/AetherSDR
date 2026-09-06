# Resolve the Windows Installer workflow's publication policy without network
# access. Production uses the source release version; development packages use
# the workflow run counter. Microsoft's reserved fourth component stays zero.
[CmdletBinding()]
param(
    [Parameter(Mandatory = $true)][string]$EventName,
    [Parameter(Mandatory = $true)][string]$Ref,
    [Parameter(Mandatory = $true)][string]$Repository,
    [Parameter(Mandatory = $true)][long]$RunNumber,
    [bool]$RequestFlight = $false,
    [string]$ProductId,
    [string]$FlightId,
    [string]$ProjectFile = (Join-Path $PSScriptRoot "../../CMakeLists.txt")
)

$ErrorActionPreference = "Stop"

$upstream = $Repository -eq 'aethersdr/AetherSDR'
$release = $upstream -and $EventName -eq 'push' -and $Ref.StartsWith('refs/tags/v')
$flight = $upstream -and $EventName -eq 'workflow_dispatch' -and $RequestFlight
if ($RequestFlight -and -not $flight) {
    throw "Store flights require workflow_dispatch in aethersdr/AetherSDR."
}
if ($flight) {
    if ([string]::IsNullOrWhiteSpace($ProductId)) {
        throw "AETHERSDR_STORE_PRODUCT_ID is required for a flight."
    }
    if ([string]::IsNullOrWhiteSpace($FlightId)) {
        throw "AETHERSDR_STORE_FLIGHT_ID is required. Refusing to fall back to production."
    }
}

$match = Select-String -LiteralPath $ProjectFile -Pattern '^\s*project\(AetherSDR\s+VERSION\s+([0-9]+(?:\.[0-9]+){1,3})(?=\s|\))' | Select-Object -First 1
if (-not $match) {
    throw "Could not read the AetherSDR version from $ProjectFile."
}
$sourceVersion = [version]$match.Matches[0].Groups[1].Value
if ($sourceVersion.Major -lt 1 -or $sourceVersion.Major -gt 65535 -or $sourceVersion.Minor -gt 65535) {
    throw "The source version's first two MSIX components must fit in 16 bits, with a nonzero first component."
}

# Release tags must describe the same version as the application. Never let a
# flight's run counter or configured ID rewrite a production package version.
if ($release) {
    if ($sourceVersion.Build -lt 0 -or $sourceVersion.Build -gt 65535 -or $sourceVersion.Revision -gt 0) {
        throw "Production Store versions require a three-component source version (or a zero fourth component), with patch in 0..65535. Nonzero CalVer hotfix revisions need an explicit Store version policy."
    }
    $tagVersion = $Ref.Substring('refs/tags/v'.Length)
    if ($tagVersion -ne $match.Matches[0].Groups[1].Value) {
        throw "Release tag version '$tagVersion' does not match the source version. Bump CMakeLists.txt before tagging."
    }
    $msixVersion = "$($sourceVersion.Major).$($sourceVersion.Minor).$($sourceVersion.Build).0"
}
else {
    if ($RunNumber -lt 1 -or $RunNumber -gt 65535) {
        throw "RunNumber $RunNumber is outside the development MSIX component range 1..65535. See docs/WINDOWS-STORE-MSIX.md before changing or resetting flight numbering."
    }
    $msixVersion = "$($sourceVersion.Major).$($sourceVersion.Minor).$RunNumber.0"
}

[pscustomobject]@{
    releaseArtifacts = $release
    productionDraft = $release -and -not [string]::IsNullOrWhiteSpace($ProductId)
    publishFlight = $flight
    msixVersion = $msixVersion
}
