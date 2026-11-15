param(
    [string]$ExePath = "deploy\AetherSDR.exe"
)

$ErrorActionPreference = "Stop"
Set-StrictMode -Version Latest

$resolvedExe = (Resolve-Path -LiteralPath $ExePath).Path
$mt = Get-Command mt.exe -ErrorAction SilentlyContinue
if (-not $mt) {
    throw "mt.exe not found. Run from a Visual Studio or Windows SDK environment."
}

$manifestPath = Join-Path ([System.IO.Path]::GetTempPath()) ("aethersdr-manifest-" + [guid]::NewGuid() + ".xml")
try {
    & $mt.Source -nologo "-inputresource:$resolvedExe;#1" "-out:$manifestPath"
    if ($LASTEXITCODE -ne 0) {
        throw "mt.exe failed to extract the embedded manifest from $resolvedExe."
    }

    $manifest = Get-Content -LiteralPath $manifestPath -Raw
    if ($manifest -notmatch '<dpiAwareness\b[^>]*>[^<]*PerMonitorV2') {
        throw "AetherSDR.exe manifest does not declare PerMonitorV2 DPI awareness."
    }
    if ($manifest -notmatch '<dpiAware\b[^>]*>[^<]*true/pm') {
        throw "AetherSDR.exe manifest does not declare legacy per-monitor DPI awareness."
    }

    Write-Host "DPI manifest check passed for $resolvedExe"
}
finally {
    Remove-Item -LiteralPath $manifestPath -Force -ErrorAction SilentlyContinue
}
