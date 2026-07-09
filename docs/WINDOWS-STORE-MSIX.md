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
`makeappx.exe`, optionally omits loose DFNR model payloads for Store readiness,
optionally signs the MSIX with `signtool.exe`, and optionally creates a
`.msixupload` archive for Partner Center.

Windows DFNR builds embed the DeepFilterNet model payload in Qt resources by
default (`AETHER_EMBED_DFNR_MODEL=ON`). At runtime, AetherSDR extracts that
payload to writable app-local data as `DeepFilterNet3_onnx.dfmodel` because the
DeepFilter C API requires a filesystem path. This keeps DFNR available in Store
MSIX builds without packaging a loose `DeepFilterNet3_onnx.tar.gz` file.

Development package:

```powershell
powershell -NoProfile -ExecutionPolicy Bypass -Command ". 'C:\Users\patj\Documents\AetherSDR\scripts\enter-msvc.ps1' -Arch x64; & '.\packaging\windows\create-msix.ps1' -DeployDir deploy -OutputDir . -CreateUpload -SkipSign"
```

Store identity package:

```powershell
powershell -NoProfile -ExecutionPolicy Bypass -Command ". 'C:\Users\patj\Documents\AetherSDR\scripts\enter-msvc.ps1' -Arch x64; . '.\packaging\windows\store-identity.ps1'; & '.\packaging\windows\create-msix.ps1' -DeployDir deploy -OutputDir . -CreateUpload -SkipSign -ExcludeDfnrModel"
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

Partner Center Store package values:

- `Identity.Name`: `AetherSDR.AetherSDR`
- `Identity.Publisher`: `CN=E03F94A2-AEAB-46D2-8BF1-6419C305CC44`
- `PublisherDisplayName`: `AetherSDR`

These values are in `packaging/windows/store-identity.ps1`. The file only sets
variables that are currently unset, so CI repository variables or local shell
environment variables can still override them for testing.

Values that need maintainer choice:

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
- Store identity injection from `packaging/windows/store-identity.ps1`, with
  optional GitHub repository variable overrides.
- Signing from GitHub secrets when a development/test PFX exists.
- `.msixupload` creation for Partner Center, with bundled `.appxsym` debug
  symbols for crash decoding (see below).

Not fully automatable until account setup:

- Final Store submission unless Partner Center API credentials are created and
  stored as secrets.

## Debug Symbols (.appxsym / PDB)

Microsoft Store crash reports in Partner Center only resolve to a symbolized
stack trace if the submitted `.msixupload` carries matching debug symbols.
There is no separate "upload symbols" step in Partner Center — the symbols
travel *inside* the upload package as a `.appxsym` file (a zip of the PDB),
which Partner Center auto-ingests on submission.

The Windows installer workflow produces this automatically:

1. **Configure** builds with `-DCMAKE_BUILD_TYPE=RelWithDebInfo`, not
   `Release`. Plain MSVC `Release` does not emit `/Zi`/`/DEBUG`, so
   `build/AetherSDR.pdb` would never exist. `RelWithDebInfo` keeps the same
   `-O2` optimization level (only the inliner threshold, `/Ob1` vs. `/Ob2`,
   differs) while producing a full PDB.
2. `create-msix.ps1 -CreateUpload -RequirePdb` zips `build/AetherSDR.pdb` into
   `<package>.appxsym` (`New-AppxSym`) and bundles it alongside the `.msix`
   into `<package>.msixupload` (`New-MsixUpload`). `-RequirePdb` makes the
   step fail the build if no PDB was found, instead of silently shipping a
   symbol-less upload package — CI passes it; local dev builds can omit it.
3. CI uploads `build/AetherSDR.pdb` as its own **Windows-Symbols** artifact
   (90-day retention) and the standalone `.appxsym` alongside the other
   Windows-Installer artifacts, so a maintainer can pull matching symbols for
   local WinDbg/Visual Studio debugging without unzipping a `.msixupload`.

Submitting the `.msixupload` to Partner Center (manually today, or via the
automated draft-staging flow below) is the only "publish" step needed —
Partner Center decodes future crash stacks against the bundled `.appxsym`
automatically, with no further action required.

### Symbolizing a crash dump (macOS / Linux)

You don't need Windows or WinDbg to symbolize a reporter's crash. The
[`rust-minidump`](https://github.com/rust-minidump/rust-minidump) toolchain
works cross-platform against the same PDB:

1. **Get the reporter's dump.** A Windows Error Reporting minidump lives in
   `%LOCALAPPDATA%\CrashDumps\AetherSDR.exe.<pid>.dmp` (enable full dumps via
   the `HKLM\...\Windows Error Reporting\LocalDumps` key, `DumpType=2`). Store
   Partner Center TSVs are unsymbolized — always request the `.dmp`.
2. **Get the matching PDB** — it must be the *exact* build that crashed or the
   debug-id won't match. Pull the `Windows-Symbols` artifact from the
   `windows-installer.yml` CI run for that tag/branch:
   `gh run download <run-id> -R aethersdr/AetherSDR -n Windows-Symbols`.
3. **Convert + walk.** [`dump_syms`](https://github.com/mozilla/dump_syms)
   turns the PDB into a Breakpad `.sym`; `minidump-stackwalk` walks the dump
   and matches modules by debug-id:
   ```sh
   dump_syms --store ./symbols AetherSDR.pdb          # → symbols/AetherSDR.pdb/<id>/AetherSDR.sym
   minidump-stackwalk --human --symbols-path ./symbols \
     --symbols-url https://msdl.microsoft.com/download/symbols \
     AetherSDR.exe.<pid>.dmp
   ```
   Our frames symbolize from the `.sym`; the `--symbols-url` resolves Windows
   system DLLs. Third-party GPU/driver frames (e.g. Intel `igd10iumd64.dll`)
   stay as `module+offset` — usually enough, since the value is in *our*
   caller frames leading into the driver. If the crashing thread won't unwind
   past a driver frame (no CFI), fall back to a manual stack scan: parse the
   dump's `Memory64ListStream`, find the region containing the thread's `rsp`,
   and scan 8-byte-aligned values for pointers into `AetherSDR.exe` (resolve
   offsets against the `.sym` `FUNC` table).

## Known WACK Follow-Ups

The Windows App Certification Kit currently gives useful Store-readiness
signals, but some findings need follow-up before final submission:

- `Blocked executables`: AetherSDR shells out to PowerShell for Windows support
  bundle ZIP creation. Replace that path with in-process ZIP creation.
- `Archive files usage`: Windows DFNR builds embed the model payload in Qt
  resources and do not deploy a loose `DeepFilterNet3_onnx.tar.gz` file. The
  `-ExcludeDfnrModel` switch remains as a packaging safety net for older deploy
  directories or custom builds with loose DFNR payloads.
- `DPIAwarenessValidation`: AetherSDR.exe now embeds a PerMonitorV2 desktop
  app manifest, and the Windows installer workflow verifies the deployed
  executable before MSIX packaging. WACK 10.0.26100.7705 reports
  `DPIAwarenessValidation` as passing on the generated MSIX.
- Qt and vendor DLLs may still report process-launch imports or short blocked
  string matches. Treat those separately from app-owned launch behavior.

## GitHub Variables

The Windows installer workflow sources `packaging/windows/store-identity.ps1`
before building the MSIX artifact. These optional repository variables override
the checked-in defaults when set:

- `AETHERSDR_MSIX_IDENTITY_NAME`
- `AETHERSDR_MSIX_PUBLISHER`
- `AETHERSDR_MSIX_DISPLAY_NAME`
- `AETHERSDR_MSIX_PUBLISHER_DISPLAY_NAME`
- `AETHERSDR_MSIX_DESCRIPTION`
- `AETHERSDR_MSIX_BACKGROUND_COLOR`
- `AETHERSDR_MSIX_INSTALLER_ACCENT_COLOR`
- `AETHERSDR_MSIX_INSTALLER_BACKGROUND_COLOR`

If the identity variables are unset, CI now uses the checked-in Partner Center
identity defaults from `store-identity.ps1`.

## Automated Store Submission (weekly release cycle) — WIRED

> Status: **wired, dormant until credentials are set.** The `Windows Installer`
> workflow now stages a **draft** Store submission on every `v*` tag push, but
> the step is a no-op until the maintainer completes the one-time Entra /
> Partner Center setup below and sets the `AETHERSDR_STORE_PRODUCT_ID`
> repository variable. Until then the workflow behaves exactly as before
> (builds the `.msixupload`, attaches it to the release).

On a `v*` tag push the workflow:

1. Builds `AetherSDR.exe`, runs `windeployqt`, packages the MSIX, and creates
   the `.msixupload` (existing steps).
2. `microsoft/microsoft-store-apppublisher@v1.1` puts the `msstore` CLI on PATH.
3. `msstore reconfigure` authenticates from the four GitHub secrets.
4. `packaging/windows/publish-store.ps1` finds the `.msixupload` and runs
   `msstore publish <pkg>.msixupload -id <ProductId> --noCommit` — staging a
   **draft**. `--noCommit` is the safety gate that keeps it out of
   certification. A maintainer reviews the pending submission in Partner Center
   and clicks **Submit to Store** to start certification. **CI never publishes
   to the live channel on its own.**

Guard rails (all three must pass before Partner Center is touched):

- The step runs only on a `refs/tags/` ref — never on PRs or branch
  `workflow_dispatch` runs.
- The step is skipped unless the `AETHERSDR_STORE_PRODUCT_ID` **repository
  variable** is set — so the feature is dormant until you opt in.
- Forks cannot read the repo secrets, so the `reconfigure` step is a no-op
  there even on a tag.

If the MSIX packaging step (which is `continue-on-error`) produced no
`.msixupload`, `publish-store.ps1` warns and exits 0 rather than turning an
otherwise-successful release red.

### One-time setup (maintainer, outside the repo)

The `msstore` GitHub Actions path is currently supported for **free products
only**, which AetherSDR is. The app must already be published and live in the
Store — done via the manual `.msixupload`. **TL;DR:** create an Entra app
registration, give it the **Manager** role in Partner Center, then store four
secrets + one variable in GitHub.

0. **Individual accounts: create a free Entra tenant first.** An individual
   (personal-MSA) Partner Center account has no Entra/Azure AD tenant by
   default, but the submission API needs one. Partner Center → gear →
   Account settings → **Tenants** → **Create Microsoft Entra ID**. It's free,
   needs no paid Azure subscription, and the account owner already has the
   **Manager** role required to do it. Company accounts can skip this.

1. **Microsoft Entra (Azure AD) app registration** — this is the "service
   account" the CI uses to authenticate.
   - Register an app in Entra ID (`entra.microsoft.com` → **App registrations**
     → New registration) in the tenant from step 0. Single-tenant is fine; no
     redirect URI is needed for the client-credentials flow.
   - In Partner Center → Account settings → User management → *Microsoft Entra
     applications*, add that app and assign it the **Manager** role. (This is
     the step that actually authorizes the app to submit; the Entra
     registration alone is not enough.)
   - In the app registration → **Certificates & secrets** → New client secret,
     create a secret and copy the **value** immediately (it is shown once).

2. **Collect the four credential values + the product Id:**

   | What | Where to find it | Goes into GitHub as |
   |---|---|---|
   | **Tenant ID** | Entra admin center → Overview → Tenant ID | secret `AZURE_AD_TENANT_ID` |
   | **Client ID** | The app registration's *Application (client) ID* | secret `AZURE_AD_APPLICATION_CLIENT_ID` |
   | **Client Secret** | The secret *value* from step 1 | secret `AZURE_AD_APPLICATION_SECRET` |
   | **Seller ID** | Partner Center → Account settings → Identifiers (a.k.a. Publisher/Seller ID) | secret `SELLER_ID` |
   | **Store product ID** | 12-char ID from the Partner Center product URL, or `msstore apps list` after a local `msstore reconfigure` | **variable** `AETHERSDR_STORE_PRODUCT_ID` |

3. **GitHub repo configuration** (Settings → Secrets and variables → Actions):
   - Secrets (the four credentials above): `AZURE_AD_TENANT_ID`,
     `AZURE_AD_APPLICATION_CLIENT_ID`, `AZURE_AD_APPLICATION_SECRET`,
     `SELLER_ID`.
   - Variable: `AETHERSDR_STORE_PRODUCT_ID` (the Store product ID). **Leaving
     this unset keeps the whole Store-submission step disabled** — set it last,
     once the four secrets are in place, to switch the automation on.

4. **First run.** Cut a release (or re-tag) so a `v*` tag pushes. Watch the
   *Stage Microsoft Store submission (draft)* step in the **Windows Installer**
   workflow, then confirm a new **draft** submission appears in Partner Center.
   Click **Submit to Store** there to start certification for the first
   automated build; later you can promote to fully automatic (see below).

### Version discipline

Each Store submission must carry a higher `Identity.Version` than the live one.
The MSIX version is derived from `project(AetherSDR VERSION ...)` in
`CMakeLists.txt` and normalized to four parts. Within a single month, weekly
releases must bump the CalVer **patch** (and the 4th hotfix component if
needed), or Partner Center will reject the package as not newer.

### Promoting to fully automatic later

To move from draft to auto-publish, drop `--noCommit` (each tag goes straight to
certification), or publish to a **flight/insider ring** first with
`-f <flightId>` and promote manually. Keep the draft gate until the weekly
cadence has proven stable.

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
