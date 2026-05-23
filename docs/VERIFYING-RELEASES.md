# Verifying MasterSDR Releases

## Signing Overview

| Platform | Signing Method |
|----------|---------------|
| Linux AppImage | GPG detached signature (`.asc`) |
| Windows .exe | GPG detached signature (`.asc`) |
| Windows .zip | GPG detached signature (`.asc`) |
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
curl -sSL https://raw.githubusercontent.com/MasterSDR/MasterSDR/main/docs/RELEASE-SIGNING-KEY.pub.asc | gpg --import
```

Or from keys.openpgp.org:

```bash
gpg --keyserver keys.openpgp.org --recv-keys <KEY_ID>
```

## Verify a Linux Download

### Option 1: Verify the artifact directly

```bash
gpg --verify MasterSDR-v1.0.0-x86_64.AppImage.asc MasterSDR-v1.0.0-x86_64.AppImage
```

### Option 2: Verify checksums first, then check the file

```bash
gpg --verify SHA256SUMS.txt.asc SHA256SUMS.txt
sha256sum -c SHA256SUMS.txt
```

Expected output should show **"Good signature from MasterSDR Release Signing"**.

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

The Windows `.exe` installer and `.zip` portable build are GPG-signed.
Windows SmartScreen may still show a warning because the binaries are
not Authenticode-signed. This is expected for open-source projects
without an EV code signing certificate. To verify the download:

```powershell
# Install Gpg4win from https://gpg4win.org/
gpg --import RELEASE-SIGNING-KEY.pub.asc
gpg --verify MasterSDR-Setup-vX.Y.Z.exe.asc MasterSDR-Setup-vX.Y.Z.exe
```

## Commit Signing

All commits on `main` must be GPG-signed by their author. GitHub displays
a green "Verified" badge on signed commits. Contributors should set up
commit signing with their personal GPG key — see
[CONTRIBUTING.md](../CONTRIBUTING.md#commit-signing) for setup instructions.
