# Asset manifest — the fifteen files, who makes them, who signs them

v26.9.3 and v26.9.4 carry exactly this set. Names substitute the version;
`X.Y.Z` is the bare version, `vX.Y.Z` the tag.

| # | asset | produced by | GPG `.asc` | Apple notarized | in `SHA256SUMS.txt` |
|---|---|---|---|---|---|
| 1 | `AetherSDR-vX.Y.Z-x86_64.AppImage` | AppImage (`build-appimage`, x86_64) | yes (2) | — | yes |
| 2 | `AetherSDR-vX.Y.Z-x86_64.AppImage.asc` | Sign Release Artifacts | — | — | — |
| 3 | `AetherSDR-vX.Y.Z-aarch64.AppImage` | AppImage (aarch64) | yes (4) | — | yes |
| 4 | `AetherSDR-vX.Y.Z-aarch64.AppImage.asc` | Sign Release Artifacts | — | — | — |
| 5 | `AetherSDR-vX.Y.Z-macOS-apple-silicon.dmg` | macOS DMG (`build-dmg`, apple-silicon) | no | yes — "Sign DMG" + "Notarize DMG" steps | no |
| 6 | `AetherSDR-vX.Y.Z-macOS-intel.dmg` | macOS DMG (intel) | no | yes — same steps | no |
| 7 | `AetherSDR-vX.Y.Z-Windows-x64-setup.exe` | Windows Installer (`build-windows`) | yes (8) | — | yes |
| 8 | `AetherSDR-vX.Y.Z-Windows-x64-setup.exe.asc` | Sign Release Artifacts | — | — | — |
| 9 | `AetherSDR-vX.Y.Z-Windows-x64-portable.zip` | Windows Installer | yes (10) | — | yes |
| 10 | `AetherSDR-vX.Y.Z-Windows-x64-portable.zip.asc` | Sign Release Artifacts | — | — | — |
| 11 | `AetherSDR-vX.Y.Z-source.tar.gz` | Sign Release Artifacts (`git archive` of the **tag**) | yes (12) | — | yes |
| 12 | `AetherSDR-vX.Y.Z-source.tar.gz.asc` | Sign Release Artifacts | — | — | — |
| 13 | `SHA256SUMS.txt` | Sign Release Artifacts (`sha256sum *` over 1, 3, 7, 9, 11) | yes (14) | — | — |
| 14 | `SHA256SUMS.txt.asc` | Sign Release Artifacts | — | — | — |
| 15 | `AetherSDR-X.Y.Z.0-Windows-x64.msixupload` | Windows Installer ("Create MSIX package") | no | — | no |

**Hotfix delta** (nonzero fourth component): row 15 is absent —
`get-store-build-plan.ps1` returns `storeEligible = false`, the MSIX step is
skipped, nothing is staged. Fourteen assets. Everything else is identical.
v26.7.4.1 predates that rule and carries a `26.7.4.1` `.msixupload` that the
Store cannot take; it also carries no `.asc` at all, because the signing
trigger could not fire and nobody dispatched it.

**Deliberately absent** from `SHA256SUMS.txt`: both DMGs (Apple-signed; the
signing job neither waits for nor downloads them) and the `.msixupload` (a
maintainer-only Partner Center package). A `SHA256SUMS.txt` with more or
fewer than five lines was written over a partial set — the wait step timed
out or the workflow changed.

**Also absent since #5600**: `com.aethersdr.radio.streamDeckPlugin` and
`streamcontroller-aethersdr.zip` (+ `.asc`), which every release through
v26.9.2 carried. Their presence on a new release is a `WARN`.

## Signature timing

The four CI-built binaries (rows 1, 3, 7, 9) are attached by their build
workflows before the signing job runs, so each `.asc` is minutes newer than
its binary; a `.asc` older than its binary means the binary was re-attached
(a re-run) after signing and the signature covers other bytes. The tarball
and `SHA256SUMS.txt` (rows 11, 13) are produced by the signing job and go up
in the same `gh release upload` as their `.asc`, so those timestamps tie
within a second, in either order — v26.9.4's tarball is stamped one second
*after* its signature. Both signing runs upload with `--clobber`, so the
second run's timestamps overwrite the first's when it is not skipped.

## Timing envelope — the last five tags

Minutes from the tag push to the asset's `created_at` (v26.8.4, v26.9.1,
v26.9.2, v26.9.3, v26.9.4):

| asset | min | max | note |
|---|---|---|---|
| aarch64 AppImage | 13 | 16 | |
| x86_64 AppImage | 15 | 22 | |
| Apple Silicon DMG | 15 | 19 | |
| Windows setup / ZIP / `.msixupload` | 27 | 40 | all three in one upload |
| Intel DMG | 30 | 42 | v26.7.3: 2 h 1 min; v26.7.4: 1 h 42 min; v26.7.4.1: attempt 2, 39 h later |
| `.asc` set, `SHA256SUMS.txt`, tarball | 28 | 41 | ~40–60 s after the last signable attached |

The signing run triggered by AppImage starts within seconds of that
workflow's completion and spends its time in "Wait for every signable
artifact to be attached" until the Windows upload lands (up to 30 min, then
it fails). The run triggered by Windows Installer is `skipped` when that
workflow concluded `failure` — which, since the Store step went live, is the
usual shape of a fully signed release with a red Windows badge.

## Failure record

| tag | what failed | how it was resolved |
|---|---|---|
| v26.7.2 | macOS DMG job failed; no signing trigger | both DMGs uploaded by the maintainer (`ten9876`) 7 h later; never signed |
| v26.7.3 | — | signed by `workflow_dispatch` 4.5 h after the tag |
| v26.7.4 | AppImage failed on the first two tag pushes | tag pushed three times in one hour; third build green; signed by dispatch |
| v26.7.4.1 | Intel DMG failed | attempt 2 succeeded 39 h later; never signed; `26.7.4.1` msixupload |
| v26.8.1 | Windows red at Store step (portal-created submission) | signed by dispatch 2.5 h after the tag |
| v26.8.2 | signing trigger could not fire (#5029) | unsigned 7 days; signed by dispatch on 2026-08-16 |
| v26.8.3 | release created by CI (bare, `github-actions[bot]`) | title and body pasted in; signed by dispatch 1 h later |
| v26.8.4 | Windows red at Store step (`Azure blob: 0%`) | assets attached; signing fired on `workflow_run` for the first time |
| v26.9.1 | Windows red at Store step (`Azure blob: 0%`); tag off-main, tree missed 3 files | #5325 repaired `main`; tag left in place by maintainer decision |
| v26.9.2 | `.msixupload` versioned `26.9.205.0`; tag off-main | #5467; the asset was left as attached |
| v26.9.3 | — | clean fifteen; Store draft staged |
| v26.9.4 | Windows red at Store step (portal-created submission) | assets and signatures complete; Store upload by hand |
