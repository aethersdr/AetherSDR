<#
.SYNOPSIS
    Embedded-signature checks shared by the Windows signing steps (#6288).

.DESCRIPTION
    Dot-source this file. Get-AuthenticodeSignature is not used to decide
    whether a file is signed: when a file's hash is also listed in a system
    catalog (dxil.dll is), it reports the catalog signature rather than the
    one embedded in the file, which is the only signature a ZIP or installer
    carries to another machine. SignTool without /a checks the embedded
    signature alone.
#>

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
