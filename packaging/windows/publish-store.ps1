<#
.SYNOPSIS
    Upload an AetherSDR MSIX build to Microsoft Store production or a flight.

.DESCRIPTION
    Wraps the Microsoft Store Developer CLI (`msstore`) publish step that the
    Windows Installer workflow runs on a `v*` tag push. It locates the
    `.msixupload` produced by create-msix.ps1 and hands it to `msstore publish`
    for the given Store product Id.

    By default it stages a DRAFT submission (`--noCommit`): the package is
    uploaded to Partner Center but certification is NOT started. A maintainer
    can submit that draft manually. The production workflow's arming secret
    passes -Commit -CommitProduction for automatic certification/publication.
    Pass -Commit with -FlightId to submit a restricted flight.
    Pass -FlightId to target an existing Partner Center package flight. The
    package identity and ProductId remain those of the production app; the
    flight controls the restricted audience.

    Authentication is expected to already be configured via a prior
    `msstore reconfigure` call (tenant/seller/client id + client secret), which
    the workflow does from GitHub secrets.

    Behavior on a missing package is deliberate: the MSIX build step in CI is
    `continue-on-error`, so a flaky package build leaves no `.msixupload`. In
    that case a draft request warns and exits 0. A committed submission fails
    when its package is missing. Store CLI and ingestion errors also fail.

.PARAMETER ProductId
    The 12-character Microsoft Store product Id for the AetherSDR listing.
    Defaults to $env:AETHERSDR_STORE_PRODUCT_ID.

.PARAMETER UploadGlob
    Glob used to find the upload package. Defaults to the create-msix.ps1
    naming convention "AetherSDR-*.msixupload".

.PARAMETER FlightId
    Optional Partner Center package flight Id. It must be passed explicitly;
    the production path never inherits a flight from the environment.

.PARAMETER SearchDir
    Directory to search for the upload package. Defaults to the current
    directory (where the workflow runs create-msix.ps1 with -OutputDir .).

.PARAMETER UploadTimeoutSeconds
    Network timeout, in seconds, for each Azure blob upload request. Defaults
    to 300. The CLI sets no StorageTransferOptions, so a .msixupload under
    256 MiB is sent as a single PUT and this timeout has to cover the entire
    transfer rather than one chunk. At ~200 MB the CLI's own 100 s default
    would require a sustained ~2 MB/s for the whole upload, so this is passed
    explicitly as a deliberate value.

.PARAMETER Commit
    Send the submission straight to certification instead of staging a draft.
    Drops the `--noCommit` safety gate. Production also requires -CommitProduction.

.PARAMETER CommitProduction
    Explicit second opt-in for -Commit without a flight. Cannot be used with a
    flight or without -Commit. Production uploads, commits and waits for the
    expected package to be ingested; Microsoft then publishes it automatically.

.PARAMETER ExpectedVersion
    Required with -CommitProduction: the package version from the build plan.

.PARAMETER IngestionTimeoutSeconds
    Maximum polling window for production package ingestion (30 minutes).
#>

[CmdletBinding()]
param(
    [string]$ProductId = $env:AETHERSDR_STORE_PRODUCT_ID,
    [string]$FlightId,
    [string]$UploadGlob = "AetherSDR-*.msixupload",
    [string]$SearchDir = ".",
    [ValidateRange(100, 100000)]
    [long]$UploadTimeoutSeconds = 300,
    [switch]$Commit,
    [switch]$CommitProduction,
    [string]$ExpectedVersion,
    [ValidateRange(1, 1800)]
    [int]$IngestionTimeoutSeconds = 1800,
    [ValidateRange(1, 60)]
    [int]$PollIntervalSeconds = 60
)

$ErrorActionPreference = "Stop"

if ([string]::IsNullOrWhiteSpace($ProductId)) {
    throw "ProductId is required. Set AETHERSDR_STORE_PRODUCT_ID or pass -ProductId."
}

# Validate intent before looking for artifacts or calling the Store CLI. In
# particular, a dropped/blank flight argument must never commit production.
$hasFlight = -not [string]::IsNullOrWhiteSpace($FlightId)
if ($CommitProduction -and (-not $Commit -or $hasFlight)) {
    throw "-CommitProduction requires -Commit and must not be combined with -FlightId."
}
if ($Commit -and -not $hasFlight -and -not $CommitProduction) {
    throw "-Commit without -FlightId would certify PRODUCTION. Pass -CommitProduction to explicitly authorize it."
}
if ($CommitProduction -and $ExpectedVersion -notmatch '^\d+\.\d+\.\d+\.0$') {
    throw "-CommitProduction requires ExpectedVersion in Store format YY.M.PATCH.0."
}

$uploads = @(Get-ChildItem -LiteralPath $SearchDir -Filter $UploadGlob -File -ErrorAction SilentlyContinue |
    Sort-Object LastWriteTime -Descending)

if ($uploads.Count -eq 0) {
    if ($Commit) {
        throw "No '$UploadGlob' found in '$SearchDir'; refusing to report a committed Store publication without a package."
    }
    Write-Warning ("No '$UploadGlob' found in '$SearchDir'; nothing to publish. " +
        "The MSIX packaging step likely did not produce an upload package " +
        "(it is continue-on-error). Skipping Store submission.")
    exit 0
}

if ($uploads.Count -gt 1) {
    $names = ($uploads | ForEach-Object { $_.Name }) -join ", "
    throw "Expected exactly one '$UploadGlob' but found $($uploads.Count): $names. " +
        "Refusing to guess which package to submit."
}

$upload = $uploads[0].FullName
if ($CommitProduction) {
    & (Join-Path $PSScriptRoot 'publish-store-production.ps1') `
        -ProductId $ProductId -UploadPath $upload -ExpectedVersion $ExpectedVersion `
        -UploadTimeoutSeconds $UploadTimeoutSeconds `
        -IngestionTimeoutSeconds $IngestionTimeoutSeconds -PollIntervalSeconds $PollIntervalSeconds
    return
}
Write-Host "Store product Id : $ProductId"
if (-not [string]::IsNullOrWhiteSpace($FlightId)) {
    Write-Host "Store flight Id  : $FlightId"
}
Write-Host "Upload package   : $upload"
Write-Host "Upload timeout   : $UploadTimeoutSeconds seconds"

$publishArgs = @(
    "publish",
    $upload,
    "-id",
    $ProductId,
    "--uploadTimeout",
    $UploadTimeoutSeconds.ToString([System.Globalization.CultureInfo]::InvariantCulture)
)
if (-not [string]::IsNullOrWhiteSpace($FlightId)) {
    $publishArgs += @("--flightId", $FlightId)
}
if (-not $Commit) {
    # --noCommit (-nc) uploads the package but keeps the submission in DRAFT
    # state; a maintainer commits it from Partner Center. This is the safety
    # gate that keeps CI from pushing to the live channel on its own.
    $publishArgs += "--noCommit"
    Write-Host "Mode             : DRAFT (--noCommit; a maintainer submits from Partner Center)"
}
else {
    if (-not [string]::IsNullOrWhiteSpace($FlightId)) {
        Write-Host "Mode             : COMMIT (restricted flight sent to certification)"
    }
    else {
        Write-Host "Mode             : COMMIT (production submission sent to certification)"
    }
}

Write-Host "Running: msstore $($publishArgs -join ' ')"
& msstore @publishArgs
if ($LASTEXITCODE -ne 0) {
    throw "msstore publish failed with exit code $LASTEXITCODE."
}

if ($Commit) {
    Write-Host "Microsoft Store submission committed successfully."
}
else {
    Write-Host "Microsoft Store submission staged successfully."
}
