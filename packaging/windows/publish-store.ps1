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
    reviews the pending submission in Partner Center and clicks "Submit to
    Store" to begin certification. Pass -Commit to send a flight straight to
    certification; production also requires -CommitProduction explicitly.
    Pass -FlightId to target an existing Partner Center package flight. The
    package identity and ProductId remain those of the production app; the
    flight controls the restricted audience.

    Authentication is expected to already be configured via a prior
    `msstore reconfigure` call (tenant/seller/client id + client secret), which
    the workflow does from GitHub secrets.

    Behavior on a missing package is deliberate: the MSIX build step in CI is
    `continue-on-error`, so a flaky package build leaves no `.msixupload`. In
    that case this script warns and exits 0 rather than turning an
    already-completed release red — the missing-package failure is already
    surfaced by the MSIX step's own annotation. A genuine publish API failure
    still exits non-zero so it is visible.

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
    to 300. This is always passed explicitly because msstore CLI v0.4.0 and
    v0.4.1 incorrectly use zero when --uploadTimeout is omitted.

.PARAMETER Commit
    Send the submission straight to certification instead of staging a draft.
    Drops the `--noCommit` safety gate. Production also requires -CommitProduction.

.PARAMETER CommitProduction
    Explicit second opt-in for -Commit without a flight. Cannot be used with a
    flight or without -Commit; ordinary tag builds pass neither switch.
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
    [switch]$CommitProduction
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

$uploads = @(Get-ChildItem -LiteralPath $SearchDir -Filter $UploadGlob -File -ErrorAction SilentlyContinue |
    Sort-Object LastWriteTime -Descending)

if ($uploads.Count -eq 0) {
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
