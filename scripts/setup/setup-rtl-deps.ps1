<# Pinned x64 MSVC RTL-SDR Blog build. Run from a VS developer environment. #>
param([string]$OutDir = "third_party\rtl-deps")
$ErrorActionPreference = 'Stop'
. "$PSScriptRoot\_verify_sha256.ps1"
$pins = Get-Content "$PSScriptRoot\rtl-dependencies.json" -Raw | ConvertFrom-Json
$prefix = [IO.Path]::GetFullPath($OutDir)
$work = Join-Path (Get-Location) '.rtl-deps-build'
$sources = Join-Path $prefix 'share\aethersdr-rtl-sources'
New-Item -ItemType Directory -Force $work,$sources,"$prefix\bin","$prefix\lib","$prefix\include" | Out-Null
function Check-Native([string]$step) { if ($LASTEXITCODE -ne 0) { throw "$step failed: $LASTEXITCODE" } }
foreach ($name in @('libusb','rtl','pthreads')) {
    $pin = $pins.$name
    $archive = Join-Path $sources $pin.archive
    if (-not (Test-Path $archive)) { Invoke-WebRequest $pin.url -OutFile $archive }
    Confirm-Sha256 $archive $pin.sha256
    tar -xf $archive -C $work
    Check-Native "extract $name"
}
$usb = Join-Path $work $pins.libusb.directory
$rtl = Join-Path $work $pins.rtl.directory
$pthreads = Join-Path $work $pins.pthreads.directory
msbuild "$usb\msvc\libusb_dll.vcxproj" /m:4 /p:Configuration=Release /p:Platform=x64 /p:PlatformToolset=v143
Check-Native 'libusb build'
Push-Location $pthreads
try {
    $savedCl = $env:CL
    $env:CL = "$savedCl /FS /D_TIMESPEC_DEFINED"
    nmake /nologo VC
    Check-Native 'pthreads build'
} finally { $env:CL = $savedCl; Pop-Location }
$usbOutput = "$usb\build\v143\x64\Release\dll"
cmake -S $rtl -B "$work\rtl-build" -G Ninja -DCMAKE_BUILD_TYPE=Release `
    -DCMAKE_DISABLE_FIND_PACKAGE_PkgConfig=ON -DCMAKE_DISABLE_FIND_PACKAGE_Git=ON `
    "-DLIBUSB_LIBRARIES=$usbOutput/libusb-1.0.lib" "-DLIBUSB_INCLUDE_DIRS=$usb/libusb" `
    "-DTHREADS_PTHREADS_LIBRARY=$pthreads/pthreadVC2.lib" "-DTHREADS_PTHREADS_INCLUDE_DIR=$pthreads" `
    -DCMAKE_C_FLAGS=/D_TIMESPEC_DEFINED
Check-Native 'RTL configure'
cmake --build "$work\rtl-build" --target rtlsdr -j4
Check-Native 'RTL build'
Copy-Item "$work\rtl-build\src\rtlsdr.dll","$usbOutput\libusb-1.0.dll" "$prefix\bin\"
Copy-Item "$work\rtl-build\src\rtlsdr.lib" "$prefix\lib\"
Copy-Item "$rtl\include\rtl-sdr.h","$rtl\include\rtl-sdr_export.h" "$prefix\include\"
Copy-Item "$rtl\COPYING" "$sources\RTL-SDR-COPYING"
Copy-Item "$usb\COPYING" "$sources\libusb-COPYING"
Copy-Item "$PSScriptRoot\rtl-dependencies.json","$PSScriptRoot\setup-rtl-deps.ps1","$PSScriptRoot\_verify_sha256.ps1" "$sources\"
Write-Host "RTL libraries and corresponding sources ready at $prefix"
