<#
.SYNOPSIS
    Stage a Microsoft Store submission for an AetherSDR MSIX build.

.DESCRIPTION
    Wraps the Microsoft Store Developer CLI (`msstore`) publish step that the
    Windows Installer workflow runs on a `v*` tag push. It locates the
    `.msixupload` produced by create-msix.ps1 and hands it to `msstore publish`
    for the given Store product Id.

    By default it stages a DRAFT submission (`--noCommit`): the package is
    uploaded to Partner Center but certification is NOT started. A maintainer
    reviews the pending submission in Partner Center and clicks "Submit to
    Store" to begin certification. Pass -Commit to skip the draft gate and send
    the submission straight to certification (each tag goes live on approval).

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

.PARAMETER SearchDir
    Directory to search for the upload package. Defaults to the current
    directory (where the workflow runs create-msix.ps1 with -OutputDir .).

.PARAMETER Commit
    Send the submission straight to certification instead of staging a draft.
    Drops the `--noCommit` safety gate.
#>

[CmdletBinding()]
param(
    [string]$ProductId = $env:AETHERSDR_STORE_PRODUCT_ID,
    [string]$UploadGlob = "AetherSDR-*.msixupload",
    [string]$SearchDir = ".",
    [switch]$Commit
)

$ErrorActionPreference = "Stop"

if ([string]::IsNullOrWhiteSpace($ProductId)) {
    throw "ProductId is required. Set AETHERSDR_STORE_PRODUCT_ID or pass -ProductId."
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
Write-Host "Upload package   : $upload"

$publishArgs = @("publish", $upload, "-id", $ProductId)
if (-not $Commit) {
    # --noCommit (-nc) uploads the package but keeps the submission in DRAFT
    # state; a maintainer commits it from Partner Center. This is the safety
    # gate that keeps CI from pushing to the live channel on its own.
    $publishArgs += "--noCommit"
    Write-Host "Mode             : DRAFT (--noCommit; a maintainer submits from Partner Center)"
}
else {
    Write-Host "Mode             : COMMIT (submission sent to certification)"
}

Write-Host "Running: msstore $($publishArgs -join ' ')"
& msstore @publishArgs
if ($LASTEXITCODE -ne 0) {
    throw "msstore publish failed with exit code $LASTEXITCODE."
}

Write-Host "Microsoft Store submission staged successfully."
