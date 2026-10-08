# Verifying AetherSDR Releases

## Signing Overview

| Platform | Signing Method |
|----------|---------------|
| Linux AppImage | GPG detached signature (`.asc`) |
| Windows .exe | Authenticode + GPG detached signature (`.asc`) |
| Windows .zip | Authenticode (every exe/dll inside) + GPG detached signature (`.asc`) |
| Source archive | GPG detached signature (`.asc`) |
| macOS DMG | Apple codesign + notarization |
| macOS .pkg | Apple codesign + notarization |

Each release also includes a GPG-signed `SHA256SUMS.txt` covering all
Linux and source artifacts.

## GPG Key Fingerprint

    B765 6E6B CB2E 022B 79F0  F97B 5578 D10E 3D59 18F3

## Import the Public Key

From the repository:

```bash
curl -sSL https://raw.githubusercontent.com/aethersdr/AetherSDR/main/docs/RELEASE-SIGNING-KEY.pub.asc | gpg --import
```

Or from keys.openpgp.org:

```bash
gpg --keyserver keys.openpgp.org --recv-keys <KEY_ID>
```

## Verify a Linux Download

### Option 1: Verify the artifact directly

```bash
gpg --verify AetherSDR-v1.0.0-x86_64.AppImage.asc AetherSDR-v1.0.0-x86_64.AppImage
```

### Option 2: Verify checksums first, then check the file

```bash
gpg --verify SHA256SUMS.txt.asc SHA256SUMS.txt
sha256sum -c SHA256SUMS.txt
```

Expected output should show **"Good signature from AetherSDR Release Signing"**.

GPG is typically pre-installed on Linux. If not:

```bash
# Arch
sudo pacman -S gnupg

# Debian/Ubuntu
sudo apt install gnupg
```

## macOS Users

The DMG and .pkg are Apple notarized. macOS Gatekeeper verifies the Apple
signature automatically — no manual steps required.

## Windows Users

The Windows setup `.exe`, its uninstaller, and every `.exe` and `.dll` in
the setup and the portable `.zip` are Authenticode-signed with Azure Artifact
Signing. The publisher is **Jeremy Fielder**. Files that already carry a vendor
signature, such as the Microsoft Visual C++ runtime, keep it.

Windows checks the signature automatically. To check it yourself, open the
file's **Properties → Digital Signatures**, or run:

```powershell
Get-AuthenticodeSignature .\AetherSDR-vX.Y.Z-Windows-x64-setup.exe | Format-List Status, SignerCertificate, TimeStamperCertificate
```

`Status` should be `Valid`. SmartScreen can still warn about a new release
until it builds download reputation; signing every release with the same
identity is what lets that reputation carry over.

The setup `.exe` and `.zip` are also GPG-signed:

```powershell
# Install Gpg4win from https://gpg4win.org/
gpg --import RELEASE-SIGNING-KEY.pub.asc
gpg --verify AetherSDR-vX.Y.Z-Windows-x64-setup.exe.asc AetherSDR-vX.Y.Z-Windows-x64-setup.exe
```

## Commit Signing

All commits on `main` must be GPG-signed by their author. GitHub displays
a green "Verified" badge on signed commits. Contributors should set up
commit signing with their personal GPG key — see
[DEVELOPER-GUIDE.md](DEVELOPER-GUIDE.md#commit-signing) for setup instructions.
