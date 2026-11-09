# Exercise the real production entry point with an in-process Store CLI;
# no credentials, subprocess Store client, sockets or submission requests.
[CmdletBinding()]
param([string]$RepositoryRoot = (Join-Path $PSScriptRoot '..'))
$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest
$entry = Join-Path $RepositoryRoot 'packaging/windows/invoke-store-production.ps1'
$scratch = Join-Path ([IO.Path]::GetTempPath()) ('aether-production-test-' + [guid]::NewGuid().ToString('N'))
New-Item -ItemType Directory -Path $scratch | Out-Null
New-Item -ItemType File -Path (Join-Path $scratch 'AetherSDR-26.11.2.0-Windows-x64.msixupload') | Out-Null
$script:assertions = 0
function Assert-True([bool]$Value, [string]$Message) {
    if (-not $Value) { throw $Message }
    $script:assertions++
}
function Assert-Throws([scriptblock]$Action, [string]$Pattern) {
    $caught = $null
    try { & $Action | Out-Null } catch { $caught = $_ }
    Assert-True ($null -ne $caught) "Expected failure matching $Pattern"
    Assert-True ($caught.ToString() -match $Pattern) "Wrong failure: $caught"
}
function Reset-Store {
    $global:aetherProduction = @{
        Calls = [Collections.Generic.List[object]]::new()
        Statuses = @('CommitStarted', 'PreProcessing', 'Certification', 'Published')
        Read = 0; Committed = $false; Fail = ''; BadJson = $false
        Version = '26.11.2.0'; FileStatus = 'Uploaded'; Id = 'NEW'; IngestedAt = 3
        PayloadPath = ''; Payload = $null; Sleeps = 0
    }
}
function global:Start-Sleep { param([int]$Seconds) $global:aetherProduction.Sleeps++ }
function global:msstore {
    $state = $global:aetherProduction
    $state.Calls.Add(@($args))
    $global:LASTEXITCODE = 0
    $command = if ($args[0] -eq 'submission') { "submission $($args[1])" } else { $args[0] }
    if ($state.Fail -eq $command) { $global:LASTEXITCODE = 9; return }
    switch ($command) {
        'publish' { return }
        'submission get' {
            if ($state.BadJson) { 'Private malformed response; do not print'; return }
            $status = 'PendingCommit'
            $version = '26.11.1.0'
            $fileStatus = 'PendingUpload'
            $id = 'NEW'
            if ($state.Committed) {
                $status = $state.Statuses[[Math]::Min($state.Read, $state.Statuses.Count - 1)]
                $state.Read++
                $version = $state.Version; $fileStatus = if ($state.Read -ge $state.IngestedAt) { $state.FileStatus } else { 'PendingUpload' }; $id = $state.Id
            }
            'Found Pending Submission.'
            [ordered]@{
                Id = $id; Status = $status; TargetPublishMode = 'Manual'
                FileUploadUrl = 'https://private.invalid/upload?sig=SECRET-SAS'
                Pricing = @{ PriceId = 'Free' }; Listings = @{ 'en-us' = @{ BaseListing = @{ ReleaseNotes = 'Keep me' } } }
                PackageDeliveryOptions = @{ PackageRollout = @{ IsPackageRollout = $true; PackageRolloutPercentage = 10 } }
                ApplicationPackages = @(@{ FileName = 'AetherSDR-26.11.2.0-Windows-x64.msixupload'; Version = $version; Architecture = 'x64'; FileStatus = $fileStatus })
            } | ConvertTo-Json -Depth 100
            return
        }
        'submission update' {
            $state.PayloadPath = $args[$args.IndexOf('--payload') + 1]
            $state.Payload = Get-Content -Raw -LiteralPath $state.PayloadPath | ConvertFrom-Json
            # Real update emits the submission JSON; the publisher must suppress it.
            $state.Payload | ConvertTo-Json -Depth 100
            return
        }
        'submission publish' { $state.Committed = $true; return }
        default { throw "Unexpected CLI command $command" }
    }
}
$savedFlight = $env:AETHERSDR_STORE_FLIGHT_ID
$savedSummary = $env:GITHUB_STEP_SUMMARY
$savedAutoPublish = $env:AETHERSDR_STORE_AUTO_PUBLISH
try {
    $env:AETHERSDR_STORE_FLIGHT_ID = 'DO-NOT-INHERIT'
    $env:GITHUB_STEP_SUMMARY = Join-Path $scratch 'summary.md'
    $inputs = @{ ProductId = 'PRODUCT'; ExpectedVersion = '26.11.2.0'; SearchDir = $scratch; IngestionTimeoutSeconds = 4; PollIntervalSeconds = 1 }
    foreach ($unarmed in @('', 'false', 'TRUE', 'yes')) {
        Reset-Store
        & $entry @inputs -AutoPublish $unarmed | Out-Null
        Assert-True ($global:aetherProduction.Calls.Count -eq 1) 'Unarmed release uploads only a draft'
        Assert-True ($global:aetherProduction.Calls[0] -contains '--noCommit') 'Unarmed release must not commit'
    }
    Reset-Store
    $env:AETHERSDR_STORE_AUTO_PUBLISH = 'true'
    $output = (& $entry @inputs 6>&1 | Out-String)
    $state = $global:aetherProduction
    Assert-True $state.Committed 'One secret must arm production commit'
    Assert-True ($state.Calls[0][0] -eq 'publish' -and $state.Calls[0] -contains '--noCommit') 'Upload precedes publication policy and commit'
    Assert-True ($state.Calls[0] -notcontains '--flightId') 'Production ignores the configured flight'
    Assert-True ($state.Payload.TargetPublishMode -ceq 'Immediate') 'Publication must be automatic'
    Assert-True (-not $state.Payload.PackageDeliveryOptions.PackageRollout.IsPackageRollout) 'Full publication must not inherit gradual rollout'
    Assert-True ($state.Payload.Pricing.PriceId -eq 'Free' -and $state.Payload.Listings.'en-us'.BaseListing.ReleaseNotes -eq 'Keep me') 'Changing publication mode preserves other metadata'
    Assert-True (-not (Test-Path -LiteralPath $state.PayloadPath)) 'Private update payload is removed'
    Assert-True ($state.Read -eq 3 -and $state.Sleeps -eq 2) 'Stop once Microsoft has ingested the expected package'
    Assert-True ($output -match 'Verified Store ingestion' -and $output -match 'Automatic certification and publication requested') 'Success reports ingestion and automatic publication request'
    Assert-True ($output -notmatch 'SECRET-SAS|private.invalid') 'CLI submission JSON stays out of public logs'
    Assert-True ((Get-Content -Raw $env:GITHUB_STEP_SUMMARY) -match '26.11.2.0.*\(Uploaded\)') 'Summary identifies the verified package'

    foreach ($failure in @('CommitFailed', 'PreProcessingFailed', 'CertificationFailed', 'ReleaseFailed', 'PublishFailed', 'Canceled')) {
        Reset-Store; $global:aetherProduction.Statuses = @($failure)
        Assert-Throws { & $entry @inputs -AutoPublish 'true' } 'failed:'
    }
    Reset-Store; $global:aetherProduction.Version = '26.11.1.0'
    Assert-Throws { & $entry @inputs -AutoPublish 'true' } 'Timed out'
    Reset-Store; $global:aetherProduction.FileStatus = 'PendingUpload'
    Assert-Throws { & $entry @inputs -AutoPublish 'true' } 'Timed out'
    Reset-Store; $global:aetherProduction.Id = 'OTHER'
    Assert-Throws { & $entry @inputs -AutoPublish 'true' } 'submission changed'
    Reset-Store; $global:aetherProduction.Statuses = @('Certification'); $global:aetherProduction.FileStatus = 'PendingUpload'
    Assert-Throws { & $entry @inputs -AutoPublish 'true' } 'Timed out'
    foreach ($command in @('publish', 'submission get', 'submission update', 'submission publish')) {
        Reset-Store; $global:aetherProduction.Fail = $command
        Assert-Throws { & $entry @inputs -AutoPublish 'true' } 'exit code 9'
        if ($command -ne 'submission publish') { Assert-True (-not $global:aetherProduction.Committed) 'CLI failure must propagate before commit' }
        if ($global:aetherProduction.PayloadPath) { Assert-True (-not (Test-Path $global:aetherProduction.PayloadPath)) 'Payload is removed on update failure too' }
    }
    Reset-Store; $global:aetherProduction.BadJson = $true
    Assert-Throws { & $entry @inputs -AutoPublish 'true' } 'not a valid submission'
    Reset-Store
    $inputs.SearchDir = Join-Path $scratch 'absent'
    Assert-Throws { & $entry @inputs -AutoPublish 'true' } 'without a package'
    Assert-True ($global:aetherProduction.Calls.Count -eq 0) 'No package must not report Store success'
    Write-Host "PASS: $script:assertions production publishing assertions; no network calls."
}
finally {
    $env:AETHERSDR_STORE_FLIGHT_ID = $savedFlight
    $env:GITHUB_STEP_SUMMARY = $savedSummary
    $env:AETHERSDR_STORE_AUTO_PUBLISH = $savedAutoPublish
    Remove-Item Function:\msstore, Function:\Start-Sleep
    Remove-Variable aetherProduction -Scope Global
    Remove-Item -LiteralPath $scratch -Recurse -Force
}
