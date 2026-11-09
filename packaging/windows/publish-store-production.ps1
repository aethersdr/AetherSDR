# msstore v0.4.2 uploads to blob storage before commit and polls only through
# CommitStarted. Verify ingestion here; Microsoft owns certification and
# Immediate publication.
[CmdletBinding()]
param(
    [Parameter(Mandatory = $true)][string]$ProductId,
    [Parameter(Mandatory = $true)][string]$UploadPath,
    [Parameter(Mandatory = $true)][ValidatePattern('^\d+\.\d+\.\d+\.0$')][string]$ExpectedVersion,
    [long]$UploadTimeoutSeconds = 300,
    [ValidateRange(1, 1800)][int]$IngestionTimeoutSeconds = 1800,
    [ValidateRange(1, 60)][int]$PollIntervalSeconds = 60
)

$ErrorActionPreference = 'Stop'
function Get-StoreSubmission {
    $output = (& msstore submission get $ProductId | Out-String).Trim()
    if ($LASTEXITCODE -ne 0) { throw "msstore submission get failed with exit code $LASTEXITCODE." }
    # The CLI writes a human preamble before its JSON. Never echo the payload:
    # it contains the SAS fileUploadUrl and private listing/certification data.
    $json = [regex]::Match($output, '(?s)\{.*\}\s*$')
    try {
        if (-not $json.Success) { throw 'Missing JSON' }
        $submission = $json.Value | ConvertFrom-Json
        if ([string]::IsNullOrWhiteSpace($submission.Id) -or
            [string]::IsNullOrWhiteSpace($submission.Status)) { throw 'Missing identity/status' }
    }
    catch { throw 'Store submission response is not a valid submission with an ID and status.' }
    return $submission
}

function Test-ExpectedPackage($Submission) {
    return @($Submission.ApplicationPackages | Where-Object {
        $_.Version -ceq $ExpectedVersion -and $_.Architecture -eq 'x64' -and $_.FileStatus -eq 'Uploaded'
    }).Count -gt 0
}

function Wait-StoreIngestion([string]$SubmissionId) {
    $attempts = [int][Math]::Ceiling($IngestionTimeoutSeconds / $PollIntervalSeconds)
    for ($attempt = 0; $attempt -le $attempts; $attempt++) {
        $current = Get-StoreSubmission
        if ($current.Id -cne $SubmissionId) { throw 'Store submission changed while verifying ingestion; refusing to verify another submission.' }
        if ($current.Status -match 'Failed$' -or $current.Status -eq 'Canceled') {
            throw "Microsoft Store submission $SubmissionId failed: $($current.Status). See Partner Center for Microsoft's report."
        }
        Write-Host "Store submission $SubmissionId : $($current.Status)"
        if (Test-ExpectedPackage $current) {
            Write-Host "Verified Store ingestion: submission $SubmissionId, x64 version $ExpectedVersion (Uploaded)."
            Write-Host 'Automatic certification and publication requested; Microsoft handles the remaining submission process.'
            if ($env:GITHUB_STEP_SUMMARY) {
                @("### Microsoft Store submitted", "", "Submission: $SubmissionId", "", "Verified package: x64 $ExpectedVersion (Uploaded)", "", "Status at verification: $($current.Status); publication mode: Immediate", "", 'Microsoft handles certification and automatic publication. No manual Store submission or release is required.') | Add-Content -LiteralPath $env:GITHUB_STEP_SUMMARY
            }
            return
        }
        if ($attempt -lt $attempts) { Start-Sleep -Seconds $PollIntervalSeconds }
    }
    throw "Timed out after $IngestionTimeoutSeconds seconds waiting for Store submission $SubmissionId to ingest version $ExpectedVersion. Microsoft may still be processing the upload; do not upload again manually."
}

# Upload first, then set publication policy on the newly created draft. The
# CLI recreates the draft from the last published submission during publish.
& msstore publish $UploadPath -id $ProductId --uploadTimeout $UploadTimeoutSeconds.ToString([Globalization.CultureInfo]::InvariantCulture) --noCommit
if ($LASTEXITCODE -ne 0) { throw "msstore publish failed with exit code $LASTEXITCODE." }
$draft = Get-StoreSubmission
$submissionId = $draft.Id
$draft.TargetPublishMode = 'Immediate'
if ($null -ne $draft.PackageDeliveryOptions -and $null -ne $draft.PackageDeliveryOptions.PackageRollout) {
    $draft.PackageDeliveryOptions.PackageRollout.IsPackageRollout = $false
}
$payloadPath = Join-Path ([IO.Path]::GetTempPath()) ('aether-store-' + [guid]::NewGuid().ToString('N') + '.json')
try {
    $draft | ConvertTo-Json -Depth 100 | Set-Content -LiteralPath $payloadPath -Encoding utf8
    & msstore submission update $ProductId --payload $payloadPath | Out-Null
    if ($LASTEXITCODE -ne 0) { throw "msstore submission update failed with exit code $LASTEXITCODE." }
}
finally { Remove-Item -LiteralPath $payloadPath -Force -ErrorAction SilentlyContinue }
& msstore submission publish $ProductId
if ($LASTEXITCODE -ne 0) { throw "msstore submission publish failed with exit code $LASTEXITCODE." }
Wait-StoreIngestion $submissionId
