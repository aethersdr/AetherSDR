# Only an exact opt-in arms production commit. The secret is read inside the
# release environment, never exported as a job output or passed as shell text.
[CmdletBinding()]
param(
    [string]$AutoPublish = $env:AETHERSDR_STORE_AUTO_PUBLISH,
    [string]$ProductId = $env:AETHERSDR_STORE_PRODUCT_ID,
    [Parameter(Mandatory = $true)][string]$ExpectedVersion,
    [string]$SearchDir = '.',
    [ValidateRange(1, 1800)][int]$IngestionTimeoutSeconds = 1800,
    [ValidateRange(1, 60)][int]$PollIntervalSeconds = 60
)

$ErrorActionPreference = 'Stop'
$publishArguments = @{
    ProductId = $ProductId
    SearchDir = $SearchDir
    ExpectedVersion = $ExpectedVersion
    UploadTimeoutSeconds = 300
    IngestionTimeoutSeconds = $IngestionTimeoutSeconds
    PollIntervalSeconds = $PollIntervalSeconds
}
if ($AutoPublish -ceq 'true') {
    $publishArguments.Commit = $true
    $publishArguments.CommitProduction = $true
    Write-Host 'Production Store automatic publication is armed.'
}
else {
    Write-Host 'Production Store automatic publication is disarmed; uploading a draft only.'
}
& (Join-Path $PSScriptRoot 'publish-store.ps1') @publishArguments
