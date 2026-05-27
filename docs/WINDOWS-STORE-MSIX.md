# Windows Store MSIX Packaging

This document tracks the command-line MSIX path for AetherSDR. The goal is to
reuse the existing Windows `deploy` directory, then add the package identity,
manifest, visual assets, signing, and Store upload wrapper around it.

## Current Build Path

1. Build `AetherSDR.exe` with MSVC.
2. Run `windeployqt` into `deploy`.
3. Copy third-party DLLs and the MSVC runtime DLLs into `deploy`.
4. Run `packaging/windows/create-msix.ps1`.

The script creates `msix-root/`, writes `AppxManifest.xml`, generates package
icons from `docs/assets/logo-circle.png`, adds App Installer UX metadata, runs
`makeappx.exe`, optionally omits the DFNR model archive for Store readiness,
optionally signs the MSIX with `signtool.exe`, and optionally creates a
`.msixupload` archive for Partner Center.

Development package:

```powershell
powershell -NoProfile -ExecutionPolicy Bypass -Command ". 'C:\Users\patj\Documents\AetherSDR\scripts\enter-msvc.ps1' -Arch x64; & '.\packaging\windows\create-msix.ps1' -DeployDir deploy -OutputDir . -CreateUpload -SkipSign"
```

Store identity package:

```powershell
$env:AETHERSDR_MSIX_IDENTITY_NAME = "PartnerCenter.Assigned.Name"
$env:AETHERSDR_MSIX_PUBLISHER = "CN=Partner-Center-Assigned-Publisher"
$env:AETHERSDR_MSIX_PUBLISHER_DISPLAY_NAME = "AetherSDR"
powershell -NoProfile -ExecutionPolicy Bypass -Command ". 'C:\Users\patj\Documents\AetherSDR\scripts\enter-msvc.ps1' -Arch x64; & '.\packaging\windows\create-msix.ps1' -DeployDir deploy -OutputDir . -CreateUpload -ExcludeDfnrModel"
```

## Manifest Values

Values we can automate:

- `Identity.Version`: read from `project(AetherSDR VERSION ...)` and normalized
  to four MSIX components, such as `26.5.3.0`.
- `Identity.ProcessorArchitecture`: `x64` for the current Windows build.
- `Application.Executable`: `AetherSDR.exe`.
- `TargetDeviceFamily`: `Windows.Desktop`, currently Windows 10 build 19041+
  because the manifest uses `uap10:RuntimeBehavior`.
- Visual assets: generated from the existing AetherSDR logo.

Values that must come from Partner Center for the Store package:

- `Identity.Name`: assigned after reserving the product name.
- `Identity.Publisher`: assigned by Partner Center and tied to the developer
  account/certificate identity.

Values that need maintainer choice:

- Public Store/product name: likely `AetherSDR`, but confirm before reserving.
- `PublisherDisplayName`: `AetherSDR`.
- Short manifest description:
  `Multi-platform SDR client for FlexRadio transceivers (6000/8600/Aurora).`
- Capability disclosure comfort:
  - `runFullTrust`: required for a packaged classic desktop app.
  - `internetClient`: needed for SmartLink, release metadata, propagation data,
    and other internet-backed features.
  - `privateNetworkClientServer`: needed for LAN radio/peripheral TCP and UDP.
  - `microphone`: recommended because AetherSDR captures PC mic audio for TX.

## Automation Plan

Already automatable:

- Local MSIX creation from the existing Windows deploy folder.
- CI artifact creation after the current `windeployqt` deployment step.
- Store identity injection from GitHub repository variables.
- Signing from GitHub secrets when a development/test PFX exists.
- `.msixupload` creation for Partner Center.

Not fully automatable until account setup:

- Reserving the Store product name.
- Copying the Partner Center package identity values into GitHub variables.
- Final Store submission unless Partner Center API credentials are created and
  stored as secrets.

## Known WACK Follow-Ups

The Windows App Certification Kit currently gives useful Store-readiness
signals, but some findings need follow-up before final submission:

- `Blocked executables`: AetherSDR shells out to PowerShell for Windows support
  bundle ZIP creation. Replace that path with in-process ZIP creation.
- `Archive files usage`: Store MSIX builds pass `-ExcludeDfnrModel` so the
  package does not include `DeepFilterNet3_onnx.tar.gz`. The archive contains
  `enc.onnx`, `erb_dec.onnx`, `df_dec.onnx`, and `config.ini`, but the current
  DeepFilter C API expects the tar.gz path. Check whether `df_create(nullptr,
  ...)` can use the embedded default model before restoring DFNR in Store
  packages.
- `DPIAwarenessValidation`: AetherSDR.exe now embeds a PerMonitorV2 desktop
  app manifest, and the Windows installer workflow verifies the deployed
  executable before MSIX packaging. WACK 10.0.26100.7705 reports
  `DPIAwarenessValidation` as passing on the generated MSIX.
- Qt and vendor DLLs may still report process-launch imports or short blocked
  string matches. Treat those separately from app-owned launch behavior.

## GitHub Variables

The Windows installer workflow reads these optional repository variables before
building the MSIX artifact:

- `AETHERSDR_MSIX_IDENTITY_NAME`
- `AETHERSDR_MSIX_PUBLISHER`
- `AETHERSDR_MSIX_DISPLAY_NAME`
- `AETHERSDR_MSIX_PUBLISHER_DISPLAY_NAME`
- `AETHERSDR_MSIX_DESCRIPTION`
- `AETHERSDR_MSIX_BACKGROUND_COLOR`
- `AETHERSDR_MSIX_INSTALLER_ACCENT_COLOR`
- `AETHERSDR_MSIX_INSTALLER_BACKGROUND_COLOR`

If the identity variables are unset, CI still builds a development package using
`AetherSDR.AetherSDR` and `CN=AetherSDR Development`. That package is useful for
manifest/package validation, but it is not the final Store identity.

## Local Sideload Signing

Windows requires MSIX packages to be signed with a certificate that is trusted
on the machine installing the package. Unsigned packages, or packages signed by
an untrusted self-signed certificate, fail with errors such as `0x800B010A`.

For local development, create a certificate whose subject exactly matches the
development manifest publisher:

```powershell
$cert = New-SelfSignedCertificate `
  -Type Custom `
  -Subject "CN=AetherSDR Development" `
  -FriendlyName "AetherSDR MSIX Development" `
  -KeyUsage DigitalSignature `
  -CertStoreLocation "Cert:\CurrentUser\My" `
  -TextExtension @("2.5.29.37={text}1.3.6.1.5.5.7.3.3", "2.5.29.19={text}")

$password = Read-Host -AsSecureString "PFX password"
New-Item -ItemType Directory -Force -Path packaging\windows\certs | Out-Null
Export-PfxCertificate -Cert $cert -FilePath packaging\windows\certs\aethersdr-msix-dev.pfx -Password $password
Export-Certificate -Cert $cert -FilePath packaging\windows\certs\aethersdr-msix-dev.cer
```

Trust the certificate on the test machine, then rebuild the package without
`-SkipSign`:

```powershell
Import-Certificate -FilePath packaging\windows\certs\aethersdr-msix-dev.cer -CertStoreLocation Cert:\LocalMachine\TrustedPeople
$env:AETHERSDR_MSIX_CERTIFICATE_FILE = "packaging\windows\certs\aethersdr-msix-dev.pfx"
$env:AETHERSDR_MSIX_CERTIFICATE_PASSWORD = "<the PFX password>"
powershell -NoProfile -ExecutionPolicy Bypass -Command ". 'C:\Users\patj\Documents\AetherSDR\scripts\enter-msvc.ps1' -Arch x64; & '.\packaging\windows\create-msix.ps1' -DeployDir deploy -OutputDir . -CreateUpload"
```

Production Store packages should use Partner Center identity values. Packages
distributed through the Microsoft Store are signed by the Store during
submission, so this local self-signed certificate is only for sideload testing.

## Notes From Microsoft Docs

- [Manual MSIX packaging](https://learn.microsoft.com/en-us/windows/msix/desktop/desktop-to-uwp-manual-conversion)
  is manifest plus package components plus `MakeAppx.exe`.
- [MakeAppx.exe](https://learn.microsoft.com/en-us/windows/msix/package/create-app-package-with-makeappx-tool)
  creates `.msix` packages, but does not create `.msixupload` files for
  Partner Center; those are normally produced by Visual Studio or assembled
  manually.
- [Custom App Installer UX](https://learn.microsoft.com/en-us/windows/msix/app-installer/how-to-create-custom-app-installer-ux)
  uses `Msix.AppInstaller.Data/MSIXAppInstallerData.xml` under the package root.
- [Package identity](https://learn.microsoft.com/en-us/windows/apps/desktop/modernize/package-identity-overview)
  consists of name, version, architecture, resource ID, and publisher.
- [MSIX signing](https://learn.microsoft.com/en-us/windows/msix/package/sign-msix-package-guide)
  requires the package certificate subject to match the manifest publisher; the
  Store signs submitted packages for Store distribution.
- [Desktop full-trust packages](https://learn.microsoft.com/en-us/windows/apps/desktop/modernize/desktop-to-uwp-distribute)
  require Store approval for the `runFullTrust` restricted capability.
- The Store signs published packages with a trusted certificate, but local
  sideload testing still needs a package signed by a certificate trusted on the
  test machine.
