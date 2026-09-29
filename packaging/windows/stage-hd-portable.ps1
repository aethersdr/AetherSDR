<#
.SYNOPSIS
    Stage a local, experimental digital FM portable payload without publishing it.
.DESCRIPTION
    Run with PowerShell 7 in the existing MSVC/Qt build environment after the
    application, daemon, and focused tests pass at the stated revision. This
    script does not build, download, install drivers, launch radios, or zip an
    untested payload. Use a new output directory for every attempt. Validate a
    clean extracted ZIP before handing it to another operator; see the guide.
#>
# ASCII-only so the file is also readable from older Windows consoles.
[CmdletBinding()]
param(
    [Parameter(Mandatory = $true)][string]$BuildDir,
    [Parameter(Mandatory = $true)][string]$OutputDir,
    [Parameter(Mandatory = $true)][ValidatePattern('^[0-9a-f]{40}$')][string]$Revision,
    [Parameter(Mandatory = $true)][string]$SourceArchive,
    [Parameter(Mandatory = $true)][ValidatePattern('^[0-9a-fA-F]{64}$')][string]$SourceSha256,
    [Parameter(Mandatory = $true)][string]$DependencyNoticesDir,
    [Parameter(Mandatory = $true)][string]$BuildInfo,
    [string]$QtDir = $env:QT_ROOT_DIR
)

$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest
if ($PSVersionTable.PSVersion.Major -lt 7) { throw 'Use PowerShell 7 (pwsh).' }
$repo = (Resolve-Path (Join-Path $PSScriptRoot '..\..')).Path
$build = (Resolve-Path -LiteralPath $BuildDir).Path
$output = $ExecutionContext.SessionState.Path.GetUnresolvedProviderPathFromPSPath($OutputDir)
if (Test-Path -LiteralPath $output) { throw "Output directory already exists: $output" }

function Require-File([string]$Path) {
    if (-not (Test-Path -LiteralPath $Path -PathType Leaf)) { throw "Missing file: $Path" }
}

foreach ($name in @('AetherSDR.exe', 'aetherd.exe', 'CMakeCache.txt')) {
    Require-File (Join-Path $build $name)
}
Require-File $SourceArchive
Require-File $BuildInfo
Require-File (Join-Path $QtDir 'bin\windeployqt.exe')
if (-not (Test-Path -LiteralPath $DependencyNoticesDir -PathType Container)) {
    throw 'DependencyNoticesDir must contain the verified external runtime licenses and provenance.'
}
$facts = Get-Content -LiteralPath $BuildInfo -Raw | ConvertFrom-Json
if ($facts.revision -ne $Revision) { throw 'BuildInfo revision does not match Revision.' }
if ($facts.platform -ne 'windows-x64' -or $facts.hdFm -ne $true -or $facts.rtl -ne $true) {
    throw 'BuildInfo must describe this Windows x64 digital FM + RTL build.'
}
foreach ($name in @('AetherSDR.exe', 'aetherd.exe')) {
    $expected = $facts.binarySha256.PSObject.Properties[$name]
    $actual = (Get-FileHash -LiteralPath (Join-Path $build $name) -Algorithm SHA256).Hash
    if (-not $expected -or $expected.Value -ne $actual) { throw "BuildInfo hash mismatch: $name" }
}
$cache = Get-Content -LiteralPath (Join-Path $build 'CMakeCache.txt') -Raw
foreach ($option in @('ENABLE_HD_FM', 'ENABLE_RTL')) {
    if ($cache -notmatch "(?m)^${option}:BOOL=ON\r?$") { throw "$option must be ON." }
}
if ((Get-FileHash -LiteralPath $SourceArchive -Algorithm SHA256).Hash -ne $SourceSha256) {
    throw 'Matching-source archive checksum mismatch.'
}
# A DFNR build may use an embedded model or a loose model beside the program.
# Copy only the known model names; an unrestricted archive glob could include
# recordings or build inputs. Refuse an enabled external-model runtime whose
# payload is absent rather than shipping an apparently available broken mode.
$modelNames = @('DeepFilterNet3_onnx.tar.gz', 'DeepFilterNet3_onnx.dfmodel')
$looseModels = @($modelNames | Where-Object {
    Test-Path -LiteralPath (Join-Path $build $_) -PathType Leaf
})
if ((Test-Path -LiteralPath (Join-Path $build 'deepfilter.dll') -PathType Leaf) -and
    $cache -match '(?m)^AETHER_EMBED_DFNR_MODEL:BOOL=OFF\r?$' -and
    $looseModels.Count -eq 0) {
    throw 'External DFNR model is missing from the build directory.'
}

New-Item -ItemType Directory -Path $output | Out-Null
foreach ($name in @('AetherSDR.exe', 'aetherd.exe')) {
    Copy-Item -LiteralPath (Join-Path $build $name) -Destination $output
}
foreach ($name in (@('aether-dv-waveform.exe', 'AetherDV.cfg') + $looseModels)) {
    $path = Join-Path $build $name
    if (Test-Path -LiteralPath $path -PathType Leaf) { Copy-Item -LiteralPath $path -Destination $output }
}
# CMake stages enabled non-Qt DLLs beside the executable. Never copy build/
# recursively: that also copies profiles, fixtures, credentials and large PDBs.
Get-ChildItem -LiteralPath $build -File -Filter '*.dll' |
    ForEach-Object { Copy-Item -LiteralPath $_.FullName -Destination $output }
foreach ($name in @('AetherSDR.exe', 'aetherd.exe')) {
    & (Join-Path $QtDir 'bin\windeployqt.exe') (Join-Path $output $name) `
        --release --no-translations --no-system-d3d-compiler --no-compiler-runtime
    if ($LASTEXITCODE -ne 0) { throw "windeployqt failed for $name." }
}
# Qt's dependency lookup can find another qtkeychain build in its shared bin
# directory. Retain the exact task-built dependency that the app was linked to.
$keychain = Join-Path $build 'qt6keychain.dll'
if (Test-Path -LiteralPath $keychain -PathType Leaf) {
    $deployedKeychain = Join-Path $output 'qt6keychain.dll'
    Copy-Item -LiteralPath $keychain -Destination $deployedKeychain -Force
    if ((Get-FileHash -LiteralPath $keychain -Algorithm SHA256).Hash -ne
        (Get-FileHash -LiteralPath $deployedKeychain -Algorithm SHA256).Hash) {
        throw 'Task-built qtkeychain deployment hash mismatch.'
    }
}
& (Join-Path $PSScriptRoot 'stage-msvc-runtime.ps1') -OutputDir $output
& (Join-Path $PSScriptRoot 'check-deploy-dependencies.ps1') -DeployDir $output
# The shared audit accepts a matching basename anywhere in the payload. A
# plugin in another subdirectory is not on this process's DLL search path.
$images = @(Get-ChildItem -LiteralPath $output -Recurse -File |
    Where-Object { $_.Extension -in '.dll', '.exe' })
$shipped = @{}
foreach ($file in $images) { $shipped[$file.Name] = $true }
foreach ($file in $images) {
    $imports = & dumpbin.exe /nologo /dependents $file.FullName
    if ($LASTEXITCODE -ne 0) { throw "Cannot inspect imports for $($file.Name)." }
    foreach ($line in $imports) {
        $name = "$line".Trim()
        if ($name -notmatch '^[^\s]+\.dll$' -or -not $shipped.ContainsKey($name)) { continue }
        if (-not (Test-Path -LiteralPath (Join-Path $output $name)) -and
            -not (Test-Path -LiteralPath (Join-Path $file.DirectoryName $name))) {
            throw "$($file.Name) imports $name from an unreachable package subdirectory."
        }
    }
}
foreach ($name in @('Qt6Widgets.dll', 'platforms\qwindows.dll', 'vcruntime140.dll')) {
    Require-File (Join-Path $output $name)
}
if (-not ((Test-Path (Join-Path $output 'rtlsdr.dll')) -or
          (Test-Path (Join-Path $output 'librtlsdr.dll')))) {
    throw 'The RTL runtime DLL is absent.'
}

$licenses = Join-Path $output 'licenses'
New-Item -ItemType Directory -Path $licenses | Out-Null
Copy-Item -LiteralPath (Join-Path $repo 'LICENSE') -Destination (Join-Path $licenses 'AetherSDR-GPL-3.0.txt')
Copy-Item -LiteralPath (Join-Path $repo 'THIRD_PARTY_LICENSES') -Destination (Join-Path $licenses 'THIRD_PARTY_LICENSES.txt')
foreach ($vendor in @('nrsc5', 'faad_hdc')) {
    $origin = Join-Path $repo "third_party\$vendor"
    $target = Join-Path $licenses $vendor
    New-Item -ItemType Directory -Path $target | Out-Null
    foreach ($name in @('README.aethersdr.md', 'SOURCE-MANIFEST.json', 'COPYING.GPL-3.0', 'LICENSE-SOURCE.json')) {
        $path = Join-Path $origin $name
        if (Test-Path -LiteralPath $path -PathType Leaf) { Copy-Item -LiteralPath $path -Destination $target }
    }
    foreach ($name in @('LICENSE', 'COPYING', 'README', 'AUTHORS')) {
        $path = Join-Path $origin "upstream\$name"
        if (Test-Path -LiteralPath $path -PathType Leaf) { Copy-Item -LiteralPath $path -Destination $target }
    }
}
Copy-Item -LiteralPath $DependencyNoticesDir -Destination (Join-Path $licenses 'runtime-dependencies') -Recurse
New-Item -ItemType Directory -Path (Join-Path $output 'source') | Out-Null
Copy-Item -LiteralPath $SourceArchive -Destination (Join-Path $output 'source')
Copy-Item -LiteralPath $BuildInfo -Destination (Join-Path $output 'BUILD-INFO.json')
$readme = Get-Content -LiteralPath (Join-Path $PSScriptRoot 'README-HD-FM.txt.in') -Raw
$readme.Replace('@REVISION@', $Revision).Replace('@SOURCE_ARCHIVE@', (Split-Path $SourceArchive -Leaf)) |
    Set-Content -LiteralPath (Join-Path $output 'README-HD-FM.txt') -Encoding utf8NoBOM
@'
@echo off
setlocal
cd /d "%~dp0"
set "AETHER_SETTINGS_DIR=%~dp0profile"
set "AETHER_AUTOMATION="
set "AETHER_AUTOMATION_ALLOW_TX="
set "AETHER_AUTOMATION_NO_TX=1"
"%~dp0AetherSDR.exe" --config set AutoConnectToLastRadio False
if errorlevel 1 (
  echo Could not prepare isolated settings. Extract the whole ZIP to a writable folder.
  pause
  exit /b 1
)
start "AetherSDR Digital test" /wait "%~dp0AetherSDR.exe"
'@ | Set-Content -LiteralPath (Join-Path $output 'Start-AetherSDR-HD.cmd') -Encoding ascii

# Store only package-relative paths, hashes and sizes. Test evidence, cache
# paths, host usernames and raw recordings belong outside the friend payload.
$inventory = @(Get-ChildItem -LiteralPath $output -Recurse -File | Sort-Object FullName | ForEach-Object {
    [ordered]@{
        path = [IO.Path]::GetRelativePath($output, $_.FullName).Replace('\', '/')
        bytes = $_.Length
        sha256 = (Get-FileHash -LiteralPath $_.FullName -Algorithm SHA256).Hash.ToLowerInvariant()
    }
})
$inventory | ConvertTo-Json -Depth 4 | Set-Content -LiteralPath (Join-Path $output 'FILES-SHA256.json') -Encoding utf8NoBOM
Write-Host "HD-PORTABLE-STAGED: $output"
Write-Host 'Staging is not startup qualification. Follow the clean-extraction checks before delivery.'
