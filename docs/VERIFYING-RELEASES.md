# Verifying AetherSDR Releases

## Signing Overview

| Platform | Signing Method |
|----------|---------------|
| Linux AppImage | Listed in signed `SHA256SUMS.txt` + GPG detached signature (`.asc`) |
| Windows .exe | Authenticode + listed in signed `SHA256SUMS.txt` + GPG detached signature (`.asc`) |
| Windows .zip | Authenticode (every exe/dll inside) + listed in signed `SHA256SUMS.txt` + GPG detached signature (`.asc`) |
| Source archive | Listed in signed `SHA256SUMS.txt` + GPG detached signature (`.asc`) |
| macOS DMG | Apple codesign + notarization |
| macOS .pkg | Apple codesign + notarization |
| PDF manual | Listed in `SHA256SUMS.txt`, whose signature `SHA256SUMS.txt.asc` covers it |

Each release page also carries `SHA256SUMS.txt`, covering the Linux, Windows
and source artifacts, the macOS app tarballs and the PDF manual, and its GPG
signature `SHA256SUMS.txt.asc`. That one signature covers every file listed,
so checking it is the recommended way to verify those files.
Releases through v26.11.2 instead sign the manual with its own
`AetherSDR-Manual-vX.Y.Z.pdf.asc`.

From v26.11.4, the per-file `.asc` signatures are not on the release page.
They are at `https://updates.aethersdr.com/releases/vX.Y.Z/`, under the same
file names. Releases through v26.11.3 keep them on the release page. They
move there a short while after a release is published, so a release from
the last few hours may still have them on its page.

## Verify the PDF Manual

```bash
gpg --verify SHA256SUMS.txt.asc SHA256SUMS.txt
sha256sum -c --ignore-missing SHA256SUMS.txt
```

The `AetherSDR-Manual-vX.Y.Z.pdf` line must read `OK`.

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

### Option 1: Verify the checksums, then check the file (recommended)

Download `SHA256SUMS.txt` and `SHA256SUMS.txt.asc` from the release page into
the folder that holds your download, then:

```bash
gpg --verify SHA256SUMS.txt.asc SHA256SUMS.txt
sha256sum -c --ignore-missing SHA256SUMS.txt
```

Your download's line must read `OK`.

### Option 2: Verify the artifact's own signature

```bash
curl -O https://updates.aethersdr.com/releases/vX.Y.Z/AetherSDR-vX.Y.Z-x86_64.AppImage.asc
gpg --verify AetherSDR-vX.Y.Z-x86_64.AppImage.asc AetherSDR-vX.Y.Z-x86_64.AppImage
```

For a release through v26.11.3, download the `.asc` from its release page
instead.

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

The setup `.exe` and `.zip` are also GPG-signed. Their `.asc` signatures are
at `https://updates.aethersdr.com/releases/vX.Y.Z/` from v26.11.4, and on the
release page through v26.11.3:

```powershell
# Install Gpg4win from https://gpg4win.org/
gpg --import RELEASE-SIGNING-KEY.pub.asc
curl.exe -O https://updates.aethersdr.com/releases/vX.Y.Z/AetherSDR-vX.Y.Z-Windows-x64-setup.exe.asc
gpg --verify AetherSDR-vX.Y.Z-Windows-x64-setup.exe.asc AetherSDR-vX.Y.Z-Windows-x64-setup.exe
```

## Signing dry run (maintainers)

To prove a change to Windows signing without cutting a release, dispatch the
Windows Installer workflow on a `signtest-*` tag. Pushing a `v*` tag would
start every release workflow instead.

```sh
gh api -X POST repos/aethersdr/AetherSDR/environments/windows-signing/deployment-branch-policies \
  -f name='signtest-*' -f type=tag          # note the returned id
git tag -m "signing dry run" signtest-<n> <commit> && git push origin refs/tags/signtest-<n>
gh workflow run windows-installer.yml --ref signtest-<n>
```

When the run finishes, delete the tag and the rule. While the rule exists,
any `signtest-*` tag can sign.

```sh
git push origin :refs/tags/signtest-<n>
gh api -X DELETE repos/aethersdr/AetherSDR/environments/windows-signing/deployment-branch-policies/<id>
```

## Commit Signing

All commits on `main` must be GPG-signed by their author. GitHub displays
a green "Verified" badge on signed commits. Contributors should set up
commit signing with their personal GPG key — see
[DEVELOPER-GUIDE.md](DEVELOPER-GUIDE.md#commit-signing) for setup instructions.
