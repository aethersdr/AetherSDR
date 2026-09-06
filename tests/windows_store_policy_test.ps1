# Socket-free behavioral tests. The real publisher calls an in-process CLI
# substitute, never msstore.exe, authentication, or a Partner Center endpoint.
[CmdletBinding()]
param([string]$RepositoryRoot = (Join-Path $PSScriptRoot '..'))
$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest
$planScript = Join-Path $RepositoryRoot 'packaging/windows/get-store-build-plan.ps1'
$publishScript = Join-Path $RepositoryRoot 'packaging/windows/publish-store.ps1'
$packageScript = Join-Path $RepositoryRoot 'packaging/windows/create-msix.ps1'
$scratch = Join-Path ([IO.Path]::GetTempPath()) ('aether-store-test-' + [guid]::NewGuid().ToString('N'))
New-Item -ItemType Directory -Path $scratch | Out-Null
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
$global:aetherStoreCalls = [System.Collections.Generic.List[object]]::new()
$global:aetherStoreExitCode = 0
function global:msstore {
    $global:aetherStoreCalls.Add(@($args))
    $global:LASTEXITCODE = $global:aetherStoreExitCode
}
$savedFlight = $env:AETHERSDR_STORE_FLIGHT_ID
try {
    $project = Join-Path $scratch 'CMakeLists.txt'
    Set-Content -LiteralPath $project -Value 'project(AetherSDR VERSION 26.9.1 LANGUAGES CXX)'
    $inputs = @{ EventName = 'workflow_dispatch'; Ref = 'refs/heads/main'; Repository = 'aethersdr/AetherSDR'; RunNumber = 203; ProjectFile = $project; ProductId = 'PRODUCT'; FlightId = 'FLIGHT' }
    foreach ($eventName in @('push', 'workflow_dispatch', 'pull_request')) {
        foreach ($ref in @('refs/heads/main', 'refs/tags/v26.9.1')) {
            foreach ($requested in @($false, $true)) {
                $inputs.EventName = $eventName; $inputs.Ref = $ref
                if ($requested -and $eventName -ne 'workflow_dispatch') {
                    Assert-Throws { & $planScript @inputs -RequestFlight $requested } 'require workflow_dispatch'
                    continue
                }
                $plan = & $planScript @inputs -RequestFlight $requested
                $releaseExpected = $eventName -eq 'push' -and $ref -eq 'refs/tags/v26.9.1'
                Assert-True ($plan.releaseArtifacts -eq $releaseExpected) "Release routing: $eventName $ref $requested"
                Assert-True ($plan.productionDraft -eq $releaseExpected) "Production routing: $eventName $ref $requested"
                Assert-True ($plan.publishFlight -eq ($eventName -eq 'workflow_dispatch' -and $requested)) 'Flight routing'
                Assert-True ($plan.msixVersion -eq '26.9.203.0') 'Store version must reserve revision zero'
            }
        }
    }
    $inputs.EventName = 'workflow_dispatch'; $inputs.Ref = 'refs/tags/v26.9.1'
    foreach ($blank in @('', ' ')) {
        $inputs.FlightId = $blank
        Assert-Throws { & $planScript @inputs -RequestFlight $true } 'FLIGHT_ID is required'
    }
    $inputs.FlightId = 'FLIGHT'; $inputs.ProductId = ' '
    Assert-Throws { & $planScript @inputs -RequestFlight $true } 'PRODUCT_ID is required'
    $inputs.EventName = 'push'
    Assert-True (-not (& $planScript @inputs).productionDraft) 'Unset product must keep production dormant'
    $inputs.ProductId = 'PRODUCT'; $inputs.Repository = 'example/fork'
    Assert-True (-not (& $planScript @inputs).releaseArtifacts) 'Fork must not publish a release'
    Assert-True (-not (& $planScript @inputs).productionDraft) 'Fork must not stage production'
    $inputs.EventName = 'workflow_dispatch'
    Assert-Throws { & $planScript @inputs -RequestFlight $true } 'require workflow_dispatch'
    $inputs.Repository = 'aethersdr/AetherSDR'
    $flightVersion = [version](& $planScript @inputs -RequestFlight $true).msixVersion
    $inputs.RunNumber = 204; $inputs.EventName = 'push'
    Set-Content -LiteralPath $project -Value 'project(AetherSDR VERSION 26.9.2.1 LANGUAGES CXX)'
    $productionVersion = [version](& $planScript @inputs).msixVersion
    Assert-True ($productionVersion -gt $flightVersion) 'Next production must outrank a flight'
    Assert-True ($productionVersion.Revision -eq 0) 'CalVer hotfix must not occupy Store revision'
    foreach ($valid in @(1, 65535)) {
        $inputs.RunNumber = $valid
        Assert-True (([version](& $planScript @inputs).msixVersion).Build -eq $valid) 'Run number boundary'
    }
    foreach ($invalid in @(0, -1, 65536)) {
        $inputs.RunNumber = $invalid
        Assert-Throws { & $planScript @inputs } 'MSIX component range 1\.\.65535'
    }
    $inputs.RunNumber = 204
    Set-Content -LiteralPath $project -Value 'project(AetherSDR VERSION invalid)'
    Assert-Throws { & $planScript @inputs } 'Could not read'

    New-Item -ItemType File -Path (Join-Path $scratch 'AetherSDR-test.msixupload') | Out-Null
    $publishInputs = @{ ProductId = 'PRODUCT'; SearchDir = $scratch }
    $env:AETHERSDR_STORE_FLIGHT_ID = 'MUST-NOT-BE-INHERITED'
    & $publishScript @publishInputs
    $call = $global:aetherStoreCalls[0]
    Assert-True ($call -contains '--noCommit') 'Production must default to draft'
    Assert-True ($call -notcontains '--flightId') 'Production must not inherit a flight'
    Assert-True ($call -notcontains '--verbose') 'Verbose authentication diagnostics must stay disabled'
    Assert-True ($call[$call.IndexOf('--uploadTimeout') + 1] -eq '300') 'Explicit upload timeout'
    & $publishScript @publishInputs -FlightId 'FLIGHT' -Commit
    $call = $global:aetherStoreCalls[1]
    Assert-True ($call -notcontains '--noCommit') 'Explicit flight commit must certify'
    Assert-True ($call[$call.IndexOf('--flightId') + 1] -eq 'FLIGHT') 'Flight ID must reach CLI'
    & $publishScript @publishInputs -FlightId 'FLIGHT'
    Assert-True ($global:aetherStoreCalls[2] -contains '--noCommit') 'Flight draft remains possible'
    & $publishScript @publishInputs -Commit -CommitProduction
    Assert-True ($global:aetherStoreCalls[3] -notcontains '--noCommit') 'Explicit production override must work'
    foreach ($blank in @('', ' ')) {
        Assert-Throws { & $publishScript @publishInputs -FlightId $blank -Commit } 'certify PRODUCTION'
    }
    Assert-Throws { & $publishScript @publishInputs -CommitProduction } 'requires -Commit'
    Assert-Throws { & $publishScript @publishInputs -Commit -CommitProduction -FlightId 'FLIGHT' } 'must not be combined'
    Assert-Throws { & $publishScript @publishInputs -UploadTimeoutSeconds 99 } 'UploadTimeoutSeconds'
    Assert-True ($global:aetherStoreCalls.Count -eq 4) 'Refusals must occur before any CLI invocation'
    $global:aetherStoreExitCode = 9
    Assert-Throws { & $publishScript @publishInputs } 'exit code 9'
    $global:aetherStoreExitCode = 0
    New-Item -ItemType File -Path (Join-Path $scratch 'AetherSDR-extra.msixupload') | Out-Null
    Assert-Throws { & $publishScript @publishInputs } 'Expected exactly one'
    Assert-True ($global:aetherStoreCalls.Count -eq 5) 'Ambiguous upload must not call CLI'

    # Fail on Store-invalid versions before touching deploy files or SDK tools.
    Assert-Throws { & $packageScript -Version '26.9.1.203' -CreateUpload -DeployDir (Join-Path $scratch 'absent') } 'fourth version component'
    Write-Host "PASS: $script:assertions Store policy assertions; no network calls."
}
finally {
    $env:AETHERSDR_STORE_FLIGHT_ID = $savedFlight
    Remove-Item Function:\msstore
    Remove-Variable aetherStoreCalls, aetherStoreExitCode -Scope Global
    Remove-Item -LiteralPath $scratch -Recurse -Force
}
