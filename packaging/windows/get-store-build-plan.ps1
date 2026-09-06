# Resolve the Windows Installer workflow's publication policy without network
# access. Both Store channels share the workflow run counter so production can
# supersede flights while Microsoft's reserved fourth component stays zero.
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

# This step runs on every Windows Installer run, so a bare range-validation
# failure here would take the whole workflow down — portable ZIP and Inno
# installer included — with a message that says nothing about why. Name the
# cause and the fix instead: MSIX version components are 16-bit, so the shared
# workflow run counter cannot exceed 65535.
if ($RunNumber -lt 1 -or $RunNumber -gt 65535) {
    throw ("RunNumber $RunNumber is outside the MSIX component range 1..65535. " +
        "The Windows Installer run counter has passed Microsoft's 16-bit package " +
        "version limit; see the Version discipline section of " +
        "docs/WINDOWS-STORE-MSIX.md before changing or resetting the counter.")
}

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

[pscustomobject]@{
    releaseArtifacts = $release
    productionDraft = $release -and -not [string]::IsNullOrWhiteSpace($ProductId)
    publishFlight = $flight
    msixVersion = "$($sourceVersion.Major).$($sourceVersion.Minor).$RunNumber.0"
}
