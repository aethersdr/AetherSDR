<#
.SYNOPSIS
    Build an MSIX package from the staged Windows deploy directory.

.DESCRIPTION
    This script expects the same deploy directory used by the portable ZIP and
    Inno installer workflow: AetherSDR.exe, Qt DLLs/plugins from windeployqt,
    third-party runtime DLLs, and data/model files.

    Store identity values are deliberately parameters/env vars so the repo can
    build development packages before Partner Center assigns the final package
    Name and Publisher.
#>

[CmdletBinding()]
param(
    [string]$DeployDir = "deploy",
    [string]$PackageRoot = "msix-root",
    [string]$OutputDir = ".",

    [string]$PackageName = $env:AETHERSDR_MSIX_IDENTITY_NAME,
    [string]$Publisher = $env:AETHERSDR_MSIX_PUBLISHER,
    [string]$DisplayName = $(if ($env:AETHERSDR_MSIX_DISPLAY_NAME) { $env:AETHERSDR_MSIX_DISPLAY_NAME } else { "AetherSDR" }),
    [string]$PublisherDisplayName = $(if ($env:AETHERSDR_MSIX_PUBLISHER_DISPLAY_NAME) { $env:AETHERSDR_MSIX_PUBLISHER_DISPLAY_NAME } else { "AetherSDR" }),
    [string]$Description = $(if ($env:AETHERSDR_MSIX_DESCRIPTION) { $env:AETHERSDR_MSIX_DESCRIPTION } else { "Multi-platform SDR client for FlexRadio transceivers (6000/8600/Aurora)." }),
    [string]$Version = $env:AETHERSDR_MSIX_VERSION,
    [ValidateSet("x64", "x86", "arm", "arm64", "neutral")]
    [string]$Architecture = $(if ($env:AETHERSDR_MSIX_ARCHITECTURE) { $env:AETHERSDR_MSIX_ARCHITECTURE } else { "x64" }),
    [string]$MinVersion = $(if ($env:AETHERSDR_MSIX_MIN_VERSION) { $env:AETHERSDR_MSIX_MIN_VERSION } else { "10.0.19041.0" }),
    [string]$MaxVersionTested = $(if ($env:AETHERSDR_MSIX_MAX_VERSION_TESTED) { $env:AETHERSDR_MSIX_MAX_VERSION_TESTED } else { "10.0.26100.0" }),
    [string]$BackgroundColor = $(if ($env:AETHERSDR_MSIX_BACKGROUND_COLOR) { $env:AETHERSDR_MSIX_BACKGROUND_COLOR } else { "#202020" }),
    [string]$InstallerAccentColor = $(if ($env:AETHERSDR_MSIX_INSTALLER_ACCENT_COLOR) { $env:AETHERSDR_MSIX_INSTALLER_ACCENT_COLOR } else { "#202020" }),
    [string]$InstallerBackgroundColor = $(if ($env:AETHERSDR_MSIX_INSTALLER_BACKGROUND_COLOR) { $env:AETHERSDR_MSIX_INSTALLER_BACKGROUND_COLOR } else { "#202020" }),

    [string]$IconSource = "docs/assets/logo-circle.png",
    [string]$PdbPath = "build/AetherSDR.pdb",
    [switch]$CreateUpload,
    [switch]$ExcludeDfnrModel,

    [string]$CertificateFile = $env:AETHERSDR_MSIX_CERTIFICATE_FILE,
    [string]$CertificatePassword = $env:AETHERSDR_MSIX_CERTIFICATE_PASSWORD,
    [string]$TimestampUrl = $env:AETHERSDR_MSIX_TIMESTAMP_URL,
    [switch]$SkipSign
)

$ErrorActionPreference = "Stop"

function Resolve-InputPath {
    param(
        [Parameter(Mandatory = $true)]
        [string]$Path
    )

    return $ExecutionContext.SessionState.Path.GetUnresolvedProviderPathFromPSPath($Path)
}

function Convert-ToMsixVersion {
    param(
        [Parameter(Mandatory = $true)]
        [string]$InputVersion
    )

    $parts = @($InputVersion.Split("."))
    if ($parts.Count -lt 2 -or $parts.Count -gt 4) {
        throw "MSIX version must have 2 to 4 numeric parts before normalization: $InputVersion"
    }

    while ($parts.Count -lt 4) {
        $parts += "0"
    }

    foreach ($part in $parts) {
        $number = 0
        if (-not [int]::TryParse($part, [ref]$number) -or $number -lt 0 -or $number -gt 65535) {
            throw "MSIX version component '$part' must be an integer from 0 through 65535."
        }
    }

    return ($parts -join ".")
}

function Get-ProjectVersion {
    $cmakePath = Resolve-InputPath "CMakeLists.txt"
    $match = Select-String -Path $cmakePath -Pattern "^\s*project\(AetherSDR\s+VERSION\s+([0-9]+(?:\.[0-9]+){1,3})" | Select-Object -First 1
    if (-not $match) {
        throw "Could not find project(AetherSDR VERSION ...) in CMakeLists.txt."
    }

    return $match.Matches[0].Groups[1].Value
}

function Find-WindowsSdkTool {
    param(
        [Parameter(Mandatory = $true)]
        [string]$ToolName
    )

    $command = Get-Command $ToolName -ErrorAction SilentlyContinue
    if ($command) {
        return $command.Source
    }

    $kitsRoot = Join-Path ${env:ProgramFiles(x86)} "Windows Kits\10\bin"
    if (Test-Path -LiteralPath $kitsRoot) {
        $candidates = Get-ChildItem -LiteralPath $kitsRoot -Directory -ErrorAction SilentlyContinue |
            Sort-Object Name -Descending |
            ForEach-Object { Join-Path $_.FullName "x64\$ToolName" } |
            Where-Object { Test-Path -LiteralPath $_ }

        if ($candidates) {
            return @($candidates)[0]
        }
    }

    $ackPath = Join-Path ${env:ProgramFiles(x86)} "Windows Kits\10\App Certification Kit\$ToolName"
    if (Test-Path -LiteralPath $ackPath) {
        return $ackPath
    }

    throw "Could not find $ToolName. Install the Windows SDK or add the tool to PATH."
}

function Escape-Xml {
    param(
        [AllowNull()]
        [string]$Value
    )

    if ($null -eq $Value) {
        return ""
    }

    return [System.Security.SecurityElement]::Escape($Value)
}

function New-LogoPng {
    param(
        [Parameter(Mandatory = $true)]
        [System.Drawing.Image]$Source,
        [Parameter(Mandatory = $true)]
        [string]$OutputPath,
        [Parameter(Mandatory = $true)]
        [int]$Width,
        [Parameter(Mandatory = $true)]
        [int]$Height,
        [double]$PaddingFraction = 0.02
    )

    $bitmap = New-Object System.Drawing.Bitmap $Width, $Height, ([System.Drawing.Imaging.PixelFormat]::Format32bppArgb)
    $graphics = [System.Drawing.Graphics]::FromImage($bitmap)
    try {
        $graphics.Clear([System.Drawing.Color]::Transparent)
        $graphics.CompositingQuality = [System.Drawing.Drawing2D.CompositingQuality]::HighQuality
        $graphics.InterpolationMode = [System.Drawing.Drawing2D.InterpolationMode]::HighQualityBicubic
        $graphics.SmoothingMode = [System.Drawing.Drawing2D.SmoothingMode]::HighQuality
        $graphics.PixelOffsetMode = [System.Drawing.Drawing2D.PixelOffsetMode]::HighQuality

        $availableWidth = [double]$Width * (1.0 - (2.0 * $PaddingFraction))
        $availableHeight = [double]$Height * (1.0 - (2.0 * $PaddingFraction))
        $scale = [Math]::Min($availableWidth / [double]$Source.Width, $availableHeight / [double]$Source.Height)
        $targetWidth = [Math]::Max(1, [int][Math]::Round([double]$Source.Width * $scale))
        $targetHeight = [Math]::Max(1, [int][Math]::Round([double]$Source.Height * $scale))
        $targetX = [int][Math]::Round(([double]$Width - $targetWidth) / 2.0)
        $targetY = [int][Math]::Round(([double]$Height - $targetHeight) / 2.0)

        $graphics.DrawImage($Source, $targetX, $targetY, $targetWidth, $targetHeight)
        $bitmap.Save($OutputPath, [System.Drawing.Imaging.ImageFormat]::Png)
    }
    finally {
        $graphics.Dispose()
        $bitmap.Dispose()
    }
}

function New-AppInstallerUx {
    param(
        [Parameter(Mandatory = $true)]
        [string]$OutputPath,
        [Parameter(Mandatory = $true)]
        [string]$AccentColor,
        [Parameter(Mandatory = $true)]
        [string]$BackgroundColor
    )

    $xml = @"
<?xml version="1.0" encoding="utf-8"?>
<AppInstallerUX
  xmlns="http://schemas.microsoft.com/msix/appinstallerux"
  xmlns:ux="http://schemas.microsoft.com/msix/appinstallerux"
  xmlns:ux2="http://schemas.microsoft.com/msix/appinstallerux/2"
  IgnorableNamespaces="ux ux2"
  Version="1.0.0">
  <UX
    AccentColor="$(Escape-Xml $AccentColor)"
    FontFamily="Segoe UI"
    AllowUserInteraction="true"
    BackgroundColor="$(Escape-Xml $BackgroundColor)"
    AppNameInTitle="true"
    HyperLinkFontSize="12">
    <Icon HorizontalAlignment="right" Logo="Assets\AppInstallerLogo.png" TopMargin="64" />
    <Buttons HorizontalAlignment="right" IsSecondaryButtonAccent="true" />
    <LaunchWhenReady HorizontalAlignment="left" />
    <AppInformation Mode="normal" />
  </UX>
</AppInstallerUX>
"@

    Set-Content -LiteralPath $OutputPath -Value $xml -Encoding UTF8
}

function New-AppxSym {
    param(
        [Parameter(Mandatory = $true)]
        [string]$InputPdbPath,
        [Parameter(Mandatory = $true)]
        [string]$OutputPath
    )

    if (-not (Test-Path -LiteralPath $InputPdbPath)) {
        Write-Host "No PDB found at $InputPdbPath; skipping .appxsym creation."
        return $null
    }

    $stageDir = Join-Path ([System.IO.Path]::GetDirectoryName($OutputPath)) "appxsym-stage"
    if (Test-Path -LiteralPath $stageDir) {
        Remove-Item -LiteralPath $stageDir -Recurse -Force
    }
    New-Item -ItemType Directory -Path $stageDir -Force | Out-Null

    Copy-Item -LiteralPath $InputPdbPath -Destination (Join-Path $stageDir ([System.IO.Path]::GetFileName($InputPdbPath))) -Force

    $zipPath = "$OutputPath.zip"
    if (Test-Path -LiteralPath $zipPath) {
        Remove-Item -LiteralPath $zipPath -Force
    }
    if (Test-Path -LiteralPath $OutputPath) {
        Remove-Item -LiteralPath $OutputPath -Force
    }

    Compress-Archive -Path (Join-Path $stageDir "*") -DestinationPath $zipPath -Force
    Move-Item -LiteralPath $zipPath -Destination $OutputPath -Force
    Remove-Item -LiteralPath $stageDir -Recurse -Force

    return $OutputPath
}

function New-MsixUpload {
    param(
        [Parameter(Mandatory = $true)]
        [string]$MsixPath,
        [string]$AppxSymPath,
        [Parameter(Mandatory = $true)]
        [string]$OutputPath
    )

    $stageDir = Join-Path ([System.IO.Path]::GetDirectoryName($OutputPath)) "msixupload-stage"
    if (Test-Path -LiteralPath $stageDir) {
        Remove-Item -LiteralPath $stageDir -Recurse -Force
    }
    New-Item -ItemType Directory -Path $stageDir -Force | Out-Null

    Copy-Item -LiteralPath $MsixPath -Destination $stageDir -Force
    if ($AppxSymPath -and (Test-Path -LiteralPath $AppxSymPath)) {
        Copy-Item -LiteralPath $AppxSymPath -Destination $stageDir -Force
    }

    $zipPath = "$OutputPath.zip"
    if (Test-Path -LiteralPath $zipPath) {
        Remove-Item -LiteralPath $zipPath -Force
    }
    if (Test-Path -LiteralPath $OutputPath) {
        Remove-Item -LiteralPath $OutputPath -Force
    }

    Compress-Archive -Path (Join-Path $stageDir "*") -DestinationPath $zipPath -Force
    Move-Item -LiteralPath $zipPath -Destination $OutputPath -Force
    Remove-Item -LiteralPath $stageDir -Recurse -Force
}

if ([string]::IsNullOrWhiteSpace($PackageName)) {
    $PackageName = "AetherSDR.AetherSDR"
    Write-Warning "AETHERSDR_MSIX_IDENTITY_NAME is not set; using development identity '$PackageName'."
}

if ([string]::IsNullOrWhiteSpace($Publisher)) {
    $Publisher = "CN=AetherSDR Development"
    Write-Warning "AETHERSDR_MSIX_PUBLISHER is not set; using development publisher '$Publisher'. Store builds must use the Partner Center Publisher value."
}

if ([string]::IsNullOrWhiteSpace($Version)) {
    $Version = Get-ProjectVersion
}
$msixVersion = Convert-ToMsixVersion $Version

$resolvedDeployDir = Resolve-InputPath $DeployDir
$resolvedPackageRoot = Resolve-InputPath $PackageRoot
$resolvedOutputDir = Resolve-InputPath $OutputDir
$resolvedIconSource = Resolve-InputPath $IconSource
$resolvedPdbPath = Resolve-InputPath $PdbPath

if (-not (Test-Path -LiteralPath $resolvedDeployDir)) {
    throw "Deploy directory not found: $resolvedDeployDir"
}

$exePath = Join-Path $resolvedDeployDir "AetherSDR.exe"
if (-not (Test-Path -LiteralPath $exePath)) {
    throw "AetherSDR.exe not found in deploy directory: $resolvedDeployDir"
}

if (-not (Test-Path -LiteralPath $resolvedIconSource)) {
    throw "Icon source not found: $resolvedIconSource"
}

New-Item -ItemType Directory -Path $resolvedOutputDir -Force | Out-Null

if (Test-Path -LiteralPath $resolvedPackageRoot) {
    Remove-Item -LiteralPath $resolvedPackageRoot -Recurse -Force
}
New-Item -ItemType Directory -Path $resolvedPackageRoot -Force | Out-Null

Copy-Item -Path (Join-Path $resolvedDeployDir "*") -Destination $resolvedPackageRoot -Recurse -Force

# windeployqt may copy vc_redist.x64.exe for traditional installers. MSIX
# packages carry the app-local CRT DLLs instead, so keep the redistributable
# installer executable out of the package payload.
Get-ChildItem -LiteralPath $resolvedPackageRoot -File -Filter "vc_redist*.exe" -ErrorAction SilentlyContinue |
    Remove-Item -Force

if ($ExcludeDfnrModel) {
    foreach ($filter in @("DeepFilterNet3_onnx.tar.gz", "DeepFilterNet3_onnx.dfmodel")) {
        $dfnrModels = Get-ChildItem -LiteralPath $resolvedPackageRoot -Recurse -File -Filter $filter -ErrorAction SilentlyContinue
        foreach ($model in $dfnrModels) {
            Write-Host "Excluding loose DFNR model payload from MSIX package: $($model.FullName)"
            Remove-Item -LiteralPath $model.FullName -Force
        }
    }
}

$assetsDir = Join-Path $resolvedPackageRoot "Assets"
New-Item -ItemType Directory -Path $assetsDir -Force | Out-Null

Add-Type -AssemblyName System.Drawing
$sourceImage = [System.Drawing.Image]::FromFile($resolvedIconSource)
try {
    New-LogoPng -Source $sourceImage -OutputPath (Join-Path $assetsDir "StoreLogo.png") -Width 50 -Height 50 -PaddingFraction 0.02
    New-LogoPng -Source $sourceImage -OutputPath (Join-Path $assetsDir "AppInstallerLogo.png") -Width 256 -Height 256 -PaddingFraction 0.0
    New-LogoPng -Source $sourceImage -OutputPath (Join-Path $assetsDir "Square44x44Logo.png") -Width 44 -Height 44
    New-LogoPng -Source $sourceImage -OutputPath (Join-Path $assetsDir "Square71x71Logo.png") -Width 71 -Height 71
    New-LogoPng -Source $sourceImage -OutputPath (Join-Path $assetsDir "Square150x150Logo.png") -Width 150 -Height 150
    New-LogoPng -Source $sourceImage -OutputPath (Join-Path $assetsDir "Square310x310Logo.png") -Width 310 -Height 310
    New-LogoPng -Source $sourceImage -OutputPath (Join-Path $assetsDir "Wide310x150Logo.png") -Width 310 -Height 150
    foreach ($targetSize in @(16, 24, 32, 48, 64, 256)) {
        New-LogoPng -Source $sourceImage -OutputPath (Join-Path $assetsDir "Square44x44Logo.targetsize-$targetSize.png") -Width $targetSize -Height $targetSize -PaddingFraction 0.0
        New-LogoPng -Source $sourceImage -OutputPath (Join-Path $assetsDir "Square44x44Logo.targetsize-$targetSize`_altform-unplated.png") -Width $targetSize -Height $targetSize -PaddingFraction 0.0
    }
}
finally {
    $sourceImage.Dispose()
}

$appInstallerUxDir = Join-Path $resolvedPackageRoot "Msix.AppInstaller.Data"
New-Item -ItemType Directory -Path $appInstallerUxDir -Force | Out-Null
New-AppInstallerUx `
    -OutputPath (Join-Path $appInstallerUxDir "MSIXAppInstallerData.xml") `
    -AccentColor $InstallerAccentColor `
    -BackgroundColor $InstallerBackgroundColor

$manifestPath = Join-Path $resolvedPackageRoot "AppxManifest.xml"
$manifest = @"
<?xml version="1.0" encoding="utf-8"?>
<Package
  xmlns="http://schemas.microsoft.com/appx/manifest/foundation/windows10"
  xmlns:uap="http://schemas.microsoft.com/appx/manifest/uap/windows10"
  xmlns:uap10="http://schemas.microsoft.com/appx/manifest/uap/windows10/10"
  xmlns:rescap="http://schemas.microsoft.com/appx/manifest/foundation/windows10/restrictedcapabilities"
  IgnorableNamespaces="uap uap10 rescap">
  <Identity
    Name="$(Escape-Xml $PackageName)"
    Publisher="$(Escape-Xml $Publisher)"
    Version="$(Escape-Xml $msixVersion)"
    ProcessorArchitecture="$(Escape-Xml $Architecture)" />
  <Properties>
    <DisplayName>$(Escape-Xml $DisplayName)</DisplayName>
    <PublisherDisplayName>$(Escape-Xml $PublisherDisplayName)</PublisherDisplayName>
    <Logo>Assets\StoreLogo.png</Logo>
  </Properties>
  <Resources>
    <Resource Language="en-us" />
  </Resources>
  <Dependencies>
    <TargetDeviceFamily Name="Windows.Desktop" MinVersion="$(Escape-Xml $MinVersion)" MaxVersionTested="$(Escape-Xml $MaxVersionTested)" />
  </Dependencies>
  <Applications>
    <Application
      Id="AetherSDR"
      Executable="AetherSDR.exe"
      uap10:RuntimeBehavior="packagedClassicApp"
      uap10:TrustLevel="mediumIL">
      <uap:VisualElements
        DisplayName="$(Escape-Xml $DisplayName)"
        Description="$(Escape-Xml $Description)"
        BackgroundColor="$(Escape-Xml $BackgroundColor)"
        Square150x150Logo="Assets\Square150x150Logo.png"
        Square44x44Logo="Assets\Square44x44Logo.png">
        <uap:DefaultTile
          Square71x71Logo="Assets\Square71x71Logo.png"
          Wide310x150Logo="Assets\Wide310x150Logo.png"
          Square310x310Logo="Assets\Square310x310Logo.png" />
      </uap:VisualElements>
    </Application>
  </Applications>
  <Capabilities>
    <Capability Name="internetClient" />
    <Capability Name="privateNetworkClientServer" />
    <rescap:Capability Name="runFullTrust" />
    <DeviceCapability Name="microphone" />
  </Capabilities>
</Package>
"@

Set-Content -LiteralPath $manifestPath -Value $manifest -Encoding UTF8

$makeAppx = Find-WindowsSdkTool "makeappx.exe"
$packageBaseName = "AetherSDR-$msixVersion-Windows-$Architecture"
$msixPath = Join-Path $resolvedOutputDir "$packageBaseName.msix"
if (Test-Path -LiteralPath $msixPath) {
    Remove-Item -LiteralPath $msixPath -Force
}

Write-Host "Packing MSIX: $msixPath"
& $makeAppx pack /v /h SHA256 /d $resolvedPackageRoot /p $msixPath /o
if ($LASTEXITCODE -ne 0) {
    throw "makeappx failed with exit code $LASTEXITCODE."
}

if (-not $SkipSign -and -not [string]::IsNullOrWhiteSpace($CertificateFile)) {
    $signtool = Find-WindowsSdkTool "signtool.exe"
    $resolvedCertificateFile = Resolve-InputPath $CertificateFile
    $signArgs = @("sign", "/fd", "SHA256", "/f", $resolvedCertificateFile)
    if (-not [string]::IsNullOrWhiteSpace($CertificatePassword)) {
        $signArgs += @("/p", $CertificatePassword)
    }
    if (-not [string]::IsNullOrWhiteSpace($TimestampUrl)) {
        $signArgs += @("/tr", $TimestampUrl, "/td", "SHA256")
    }
    $signArgs += $msixPath

    Write-Host "Signing MSIX with $resolvedCertificateFile"
    & $signtool @signArgs
    if ($LASTEXITCODE -ne 0) {
        throw "signtool failed with exit code $LASTEXITCODE."
    }
}
elseif ($SkipSign) {
    Write-Host "Skipping MSIX signing because -SkipSign was specified."
}
else {
    Write-Warning "No signing certificate was provided; created unsigned MSIX."
}

$appxSymPath = $null
if ($CreateUpload) {
    $appxSymPath = New-AppxSym -InputPdbPath $resolvedPdbPath -OutputPath (Join-Path $resolvedOutputDir "$packageBaseName.appxsym")
    $uploadPath = Join-Path $resolvedOutputDir "$packageBaseName.msixupload"
    New-MsixUpload -MsixPath $msixPath -AppxSymPath $appxSymPath -OutputPath $uploadPath
    Write-Host "Created Store upload package: $uploadPath"
}

Write-Host "Created MSIX package: $msixPath"
