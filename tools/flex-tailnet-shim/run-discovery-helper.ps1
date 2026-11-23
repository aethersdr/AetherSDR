# Runs discovery_helper.py so SmartSDR, DAX and CAT for Windows can list
# a radio on the tailnet. Close this window to stop it.
#
# The radio's tailnet name defaults to flex-radio. Set the TAILNET_SHIM
# environment variable to change it (setx TAILNET_SHIM my-radio), or pass
# --shim my-radio. Other arguments go to the helper.
#
# Uses Python from python.org. Under the Microsoft Store Python the helper
# connected but its broadcasts never reached DAX or CAT.
$shim = if ($env:TAILNET_SHIM) { $env:TAILNET_SHIM } else { 'flex-radio' }
$roots = @("$env:ProgramFiles", "$env:LOCALAPPDATA\Programs\Python")
$py = Get-ChildItem -Path $roots -Directory -Filter 'Python3*' -ErrorAction SilentlyContinue |
    Where-Object { Test-Path (Join-Path $_.FullName 'python.exe') } |
    Sort-Object { [int]($_.Name -replace '\D', '') } |
    Select-Object -Last 1
if (-not $py) {
    Write-Host 'Python was not found. Install it from python.org, not the Microsoft Store.'
    Read-Host 'Press Enter to close'
    exit 1
}
$Host.UI.RawUI.WindowTitle = 'Discovery helper - close this window to stop it'
& (Join-Path $py.FullName 'python.exe') (Join-Path $PSScriptRoot 'discovery_helper.py') --shim $shim @args
Write-Host ''
Write-Host 'The discovery helper has stopped.'
Read-Host 'Press Enter to close'
