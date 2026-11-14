# One persistent secret opts production releases into the existing publisher.
[CmdletBinding()]
param(
    [string]$AutoPublish = $env:AETHERSDR_STORE_AUTO_PUBLISH,
    [string]$ProductId = $env:AETHERSDR_STORE_PRODUCT_ID,
    [string]$SearchDir = '.'
)

$ErrorActionPreference = 'Stop'
$publishArguments = @{
    ProductId = $ProductId
    SearchDir = $SearchDir
    UploadTimeoutSeconds = 300
}
if ($AutoPublish -ceq 'true') {
    $publishArguments.Commit = $true
    $publishArguments.CommitProduction = $true
}
& (Join-Path $PSScriptRoot 'publish-store.ps1') @publishArguments
