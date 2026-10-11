<#
.SYNOPSIS
    Helpers shared by the Windows signing steps (#6288).

.DESCRIPTION
    Dot-source this file. Get-AuthenticodeSignature is not used to decide
    whether a file is signed: when a file's hash is also listed in a system
    catalog (dxil.dll is), it reports the catalog signature rather than the
    one embedded in the file, which is the only signature a ZIP or installer
    carries to another machine. SignTool without /a checks the embedded
    signature alone.
#>

# The files the release ships and the gate verifies: every exe/dll in the
# payload, plus the MSVC runtime the setup packs from installer-runtime\
# (VC_RUNTIME_DIR). The sign step and the gate both call this, so they cannot
# disagree about what "the payload" is.
function Get-AuthenticodeTargets {
    @(Get-ChildItem -Path deploy -Recurse -File -Include *.exe, *.dll) +
    @(Get-ChildItem -Path installer-runtime -File -Filter *.dll)
}

# Returns @{ Valid = <bool>; Timestamped = <bool> } for the file's EMBEDDED
# signature under the default Authenticode policy.
function Get-EmbeddedSignatureState {
    param(
        [Parameter(Mandatory = $true)][string]$SignTool,
        [Parameter(Mandatory = $true)][string]$Path
    )
    $out = & $SignTool verify /pa /v $Path 2>&1 | Out-String
    $valid = $LASTEXITCODE -eq 0
    @{
        Valid = $valid
        Timestamped = $valid -and ($out -match 'The signature is timestamped:')
    }
}

# Runs one signtool sign over $Files, retrying a failed attempt. A
# timestamp-server blip must not fail a multi-hour tag build, and re-signing
# an already signed file replaces its signature, so retrying the whole list
# is safe. Does nothing for an empty list.
function Invoke-AuthenticodeSign {
    param(
        [Parameter(Mandatory = $true)][string]$SignTool,
        [Parameter(Mandatory = $true)][string[]]$Arguments,
        [string[]]$Files = @()
    )
    if ($Files.Count -eq 0) { return }
    for ($attempt = 1; $attempt -le 3; $attempt++) {
        & $SignTool @Arguments @Files
        if ($LASTEXITCODE -eq 0) { return }
        if ($attempt -eq 3) { throw "signtool sign failed ($LASTEXITCODE) after 3 attempts" }
        Write-Host "::warning::signtool sign failed ($LASTEXITCODE); retrying in 30 s"
        Start-Sleep -Seconds 30
    }
}
