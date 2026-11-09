# Exercise secret arming through the real publisher without Store requests.
[CmdletBinding()]
param([string]$RepositoryRoot = (Join-Path $PSScriptRoot '..'))
$ErrorActionPreference = 'Stop'
$entry = Join-Path $RepositoryRoot 'packaging/windows/invoke-store-production.ps1'
$scratch = Join-Path ([IO.Path]::GetTempPath()) ('aether-production-test-' + [guid]::NewGuid().ToString('N'))
New-Item -ItemType Directory -Path $scratch | Out-Null
New-Item -ItemType File -Path (Join-Path $scratch 'AetherSDR-test.msixupload') | Out-Null
$script:assertions = 0
function Assert-True([bool]$Value, [string]$Message) {
    if (-not $Value) { throw $Message }
    $script:assertions++
}
$global:aetherProductionCalls = [Collections.Generic.List[object]]::new()
$global:aetherProductionExitCode = 0
function global:msstore {
    $global:aetherProductionCalls.Add(@($args))
    $global:LASTEXITCODE = $global:aetherProductionExitCode
}
$savedAutoPublish = $env:AETHERSDR_STORE_AUTO_PUBLISH
$savedFlight = $env:AETHERSDR_STORE_FLIGHT_ID
try {
    $inputs = @{ ProductId = 'PRODUCT'; SearchDir = $scratch }
    foreach ($unarmed in @('', 'false', 'TRUE', 'yes')) {
        $global:aetherProductionCalls.Clear()
        & $entry @inputs -AutoPublish $unarmed | Out-Null
        Assert-True ($global:aetherProductionCalls.Count -eq 1) 'One direct upload call'
        Assert-True ($global:aetherProductionCalls[0] -contains '--noCommit') 'Unarmed retains draft mode'
    }
    $global:aetherProductionCalls.Clear()
    $env:AETHERSDR_STORE_AUTO_PUBLISH = 'true'
    $env:AETHERSDR_STORE_FLIGHT_ID = 'DO-NOT-INHERIT'
    & $entry @inputs | Out-Null
    Assert-True ($global:aetherProductionCalls.Count -eq 1) 'Armed release makes exactly one CLI call'
    $call = $global:aetherProductionCalls[0]
    Assert-True ($call[0] -eq 'publish') 'Upload and commit directly with publish'
    Assert-True ($call -notcontains '--noCommit') 'Armed release commits the uploaded package'
    Assert-True ($call -notcontains '--flightId') 'Production must not target a flight'
    Assert-True ($call[$call.IndexOf('-id') + 1] -eq 'PRODUCT') 'Publish the configured product'
    Assert-True ($call[$call.IndexOf('--uploadTimeout') + 1] -eq '300') 'Preserve the working upload timeout'
    $global:aetherProductionExitCode = 9
    $caught = $null
    try { & $entry @inputs | Out-Null } catch { $caught = $_ }
    Assert-True ($null -ne $caught -and $caught.ToString() -match 'exit code 9') 'Propagate Microsoft CLI failures'
    Write-Host "PASS: $script:assertions production publishing assertions; no network calls."
}
finally {
    $env:AETHERSDR_STORE_AUTO_PUBLISH = $savedAutoPublish
    $env:AETHERSDR_STORE_FLIGHT_ID = $savedFlight
    Remove-Item Function:\msstore
    Remove-Variable aetherProductionCalls, aetherProductionExitCode -Scope Global
    Remove-Item -LiteralPath $scratch -Recurse -Force
}
