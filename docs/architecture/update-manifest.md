# In-App Update Manifest — Schema 1

**Status:** RFC #5304 Phase 2. The CI side writes and publishes this; the
app's `UpdateStager` (`src/core/UpdateStager.{h,cpp}`) reads and applies it
into a staging copy. Swapping the staged tree in is the platform phases'.

Each release publishes one signed manifest that names, for each updatable
payload, every file of the new tree and the patch bundles that turn each of
the last four releases' trees into it. `tools/build_update_manifest.py`
writes it and `.github/workflows/update-manifest.yml` signs and publishes it.

## Where it lives

| Object | Location |
|---|---|
| Manifest, newest release | `https://updates.aethersdr.com/latest/manifest.json` |
| Its signature | `https://updates.aethersdr.com/latest/manifest.json.sig` |
| Manifest, any release | `https://updates.aethersdr.com/releases/<tag>/manifest.json` (and `.sig`) |
| Patch bundles, full payloads | GitHub release assets of the new release |

The R2 bucket behind `updates.aethersdr.com` is written only by the release
workflow's `publish` job (`tools/publish_update_manifest.py`), with a token
scoped to that bucket.

A release counts as **published** only when everything a client reads is
served and verifies: `releases/<tag>/` holds the manifest and a signature that
checks against the committed key, and, when the tag is GitHub's latest
release, `latest/` holds a verifying pair naming that release or a newer one.
A run that finds anything less publishes again, which repairs a partial
upload. `latest/` is promoted only over an older release, re-read just before
the write, and the publish job runs one at a time across tags, so an older
release's run can never roll it back.

Bundles and full payloads stay on GitHub Releases, so every URL the manifest
names is under `https://github.com/aethersdr/AetherSDR/releases/download/<tag>/`,
inside the prefix the app's download allowlist already enforces
(`UpdateChecker::isTrustedDownloadUrl`). The publish job refuses to sign a
manifest that names anything else, or another release.

**Telemetry boundary.** RFC #5304 asked that no project-owned endpoint see
clients. The update check still goes only to `api.github.com`; this host is
the one exception, ruled in Phase 2. `updates.aethersdr.com` sees a client's
IP address when, and only when, the client fetches a manifest to install an
update. Nothing else is sent: the request carries no identifier, and the
client sends nothing back.

## Signature

`manifest.json.sig` is the raw 64-byte Ed25519 signature over the exact bytes
of `manifest.json` (no canonicalisation: verify the bytes as downloaded,
then parse). The app embeds the public key from
`packaging/update-signing.pub` and refuses a manifest that does not verify.
`key_id` is the first 16 hex digits of the SHA-256 of the raw 32-byte public
key; it lets a client tell a rotated key apart, never chooses which key to
trust.

A valid signature proves the manifest came from the release workflow. It
does not prove it is the newest one. Whoever can serve bytes at this host
could **replay** an older signed manifest (rolling clients back) or **freeze**
`latest/` on an old one (holding clients on an old build forever, signature
valid). The client must:

- refuse a manifest whose `version` is not newer than the running build;
- refuse one whose `generated_at` is older than the newest it has accepted;
- require `tag` to equal the `tag_name` of GitHub's latest release, which the
  update check already fetches. A `latest/` that disagrees with GitHub is
  stale or frozen, and the client offers the release page instead.

## Top level

```json
{
 "schema": 1,
 "product": "AetherSDR",
 "tag": "v26.10.4",
 "version": "26.10.4",
 "generated_at": "2026-11-01T18:04:11Z",
 "key_id": "3fa1c2…",
 "payloads": [ … ]
}
```

A client refuses any `schema` it does not know. Versions compare as four
numbers, `YY.M.patch.hotfix`, with an absent hotfix counting as 0 (a hotfix
release is `"version": "26.7.4.1"`).

## Payload

```json
{
 "platform": "windows",
 "arch": "x86_64",
 "full": {"asset": "AetherSDR-v26.10.4-Windows-x64-portable.zip",
          "url": "https://github.com/…/v26.10.4/AetherSDR-v26.10.4-Windows-x64-portable.zip",
          "sha256": "…", "size": 157145703},
 "files": [ {"path": "AetherSDR.exe", "type": "file", "size": 151234567,
             "sha256": "…", "executable": false},
            {"path": "AppRun", "type": "symlink", "target": "usr/bin/AetherSDR"} ],
 "patches": [ … ]
}
```

- `platform` / `arch`: `windows` or `linux`; `x86_64` or `arm64`, as
  `QSysInfo::buildCpuArchitecture()` names them. macOS updates through Sparkle
  (Phase 4) and is not listed.
- **Windows** is the tree inside the portable zip, which is the `deploy\` tree
  `setup.exe` installs. **Linux** is the tree inside the AppImage's squashfs,
  which `AetherSDR --install` installs (see
  [The Linux install](#the-linux-install-phase-5)).
- `full`: the whole payload, the fallback when no patch applies or anything
  fails to verify. Its `sha256` and `size` are as binding as a bundle's: release
  assets can be replaced by anyone with write access, and the signed manifest
  is what pins them. For Linux, `full` is the release's `.tar.zst` (see
  [The full release in-app (Linux)](#the-full-release-in-app-linux)). A
  manifest whose Linux `full` is still the AppImage, a container the app
  cannot unpack, sends a Linux client that cannot patch to the browser.
- `files`: the complete new tree. Paths use `/`, are relative and never contain
  `..`. Directories are implied. A symlink's `target` is relative and resolves
  inside the tree: the builder refuses any other link, and the client must too.

## Patch

```json
{
 "from": "26.10.3",
 "bundle": {"asset": "AetherSDR-v26.10.4-Windows-x64-from-v26.10.3.update.tar",
            "url": "…", "sha256": "…", "size": 5755368},
 "unchanged": 1206,
 "ops": [
  {"op": "patch", "path": "AetherSDR.exe", "from_sha256": "…", "member": "000000.zst",
   "window_log": 28, "sha256": "…", "size": 151234567, "executable": false},
  {"op": "add", "path": "plugins/new.dll", "member": "000001.zst",
   "window_log": 13, "sha256": "…", "size": 5000, "executable": false},
  {"op": "remove", "path": "old.dll"},
  {"op": "symlink", "path": "AppRun", "target": "usr/bin/AetherSDR"}
 ]
}
```

The bundle is a plain tar of zstd frames, one per `patch` or `add` op, named
by `member`. Its bytes depend only on its contents.

Ops are applied **in order**. Every `remove` comes first; after the removals,
remove each directory they left empty (deepest first). That is what lets a
path change kind between releases: a file that becomes a directory, a
directory that becomes a file, a directory that becomes a symlink.

- `patch`: decompress `member` with the current file as the zstd dictionary
  (`--patch-from`), with the decoder's window limit raised to `window_log`.
- `add`: decompress `member` on its own, with the same window limit. A changed
  file is shipped this way when that is smaller than its delta.
- `remove`: delete the file.
- `symlink`: create or replace the link. Never write through a link: a path
  with a symlink component is refused.

A file in `files` with no op is unchanged from the `from` release.

## Applying (the contract the app-side PR implements)

1. Verify the signature, then `schema`, `version` and `generated_at`, as above.
2. Pick the payload for this build's platform and arch, and the patch whose
   `from` is the running version. With none, use `full`.
3. Download the bundle (or `full`), check its `sha256` and `size`, and unpack
   it into a staging copy of the install. Never patch the running install in
   place (Principle XIV). Resolve every write against the staging root as a
   boundary.
4. Decompress each member with the decoder's output capped at the op's
   `size`, and its window limit at `window_log` (at most 31). Output beyond
   `size` is a hard fail. The window limit bounds the decoder's memory when
   it streams; a frame small enough to decode in one pass allocates no window
   at all, so the output cap is the check that always applies.
5. For each `patch` op, check the current file against `from_sha256` before
   using it. A mismatch is a hard fail, not a best effort (Principle VII).
6. After all ops, check every entry in `files` against the staged tree. Any
   mismatch abandons the staged tree and falls back to `full` (below).
7. Files in the install that are not in `files` are left alone. That covers
   the installer's own `unins000.*` on Windows.
8. Swap on quit, keeping the previous tree for rollback.

### The full release in-app (Windows)

When no patch applies, or the patch fails anywhere (download, hash, a
drifted install, a result that does not match), a request with
`fullFallback` stages `full` instead of sending the user to the browser.
That needs `full` to be a `.zip` of at most 512 MiB here; Linux's `.tar.zst` is below.

- Its `sha256` and `size` are checked like a bundle's.
- The zip must hold exactly the files in `files`: no others, none missing,
  and no more bytes in all than `files` adds up to. Entry names written
  with `\` (as `Compress-Archive` writes them) are read as `/`. Folder
  entries are allowed; a link is not, since a zip cannot carry one.
- The staged tree is those files plus the installer's own top-level
  `unins*` files, copied from the install, so `setup.exe`'s uninstaller
  still works after the swap. Nothing else comes from the install.
- Every file is then checked against `files`, as in step 6.

If the full release fails too, the browser fallback is offered, once.

The Windows zip is unpacked in memory: the download and its unpacked
files are held at once, on a worker thread. For v26.10.3 that measured
548 MiB peak (a 157 MB zip, 344 MB of files). The caps above are hard
ceilings, so this is footprint, not safety.

### The full release in-app (Linux)

The client stages `full` in-app when its name ends in `.tar.zst` (and in
`.zip`, Windows above); anything else, the AppImage, goes to the browser.
The tarball is the payload tree itself, built from `files`, so it is the
same tree the patches produce:

- A zstd stream (one frame or several) of a POSIX ustar archive, with a
  window of at most 2^27 (zstd's default levels use 2^23 and less, `--long`
  2^27). Its `sha256` and `size` are checked first, so nothing unsigned is
  ever decompressed.
- Exactly the entries in `files`: each file (type `0`) once, with the size
  `files` gives, and each link (type `2`) once, to its signed `target`.
  Folder entries (type `5`) are allowed only for folders that hold one of
  them, so the AppImage's empty folders are left out. Names are relative,
  `/`-separated, and may use ustar's `prefix` field. Hard links, devices,
  GNU long names, pax headers, anything after the end-of-archive blocks
  except zeros, and a missing end are all refused.
- File modes in the archive are ignored: each file's executable bit comes
  from `files`.
- It is unpacked as it is decompressed, straight into the staging folder,
  so only the download is held in memory (134 MB for v26.10.4 x86_64, from
  a 348 MB tree). Every file is then checked against `files`, as in step 6.
  Nothing comes from the install.

### What stays staged

The staging root holds at most the one staged release waiting to be
swapped in (`<stagingRoot>/<version>`), with its record beside it, never
in it:

| File | What |
|---|---|
| `<version>.json` | `version`, the `from` version it was staged for, and the `install` it was staged from. No tag: the release's tag is `v<version>`, which the signed manifest binds |
| `<version>.manifest.json`, `.sig` | The signed manifest and signature that vouch for every file |

The record is written after the tree is in place, so a tree without a
complete record is never used. Each attempt works in its own `.work-*`
folder, removed on failure or cancel.

At startup `UpdateStager::stagedRelease` keeps the staged release only if
it is still the one to install: staged for the running version and this
install, GitHub's latest (when that is known yet), and not the release
that was rolled back. GitHub's latest is compared with `v<version>`: the
record is unsigned and keeps no tag of its own, and `revalidate` binds the
version to the signed manifest. Everything else staging wrote is removed: other versions,
half-written records, and work folders a crashed run left. The staging
root is the helper's state folder too, so only staging's own names are
swept (`.work-*`, `<version>`, its three record files, and a record
`QSaveFile` left half written as `<record>.XXXXXX`); the helper's
journal, result, repair plan, log and copy are left alone.

Before the swap, `revalidate` checks the stored signature with the
built-in key and every file in `files` against the stored manifest, so a
release file changed or removed on disk since it was staged is never
swapped in. A file added since is not caught: the patch path keeps the
install's own files (`unins*`, anything a portable user put there), so the
tree is never exactly `files`. Both trees are the user's own, at the same
level, so no boundary is crossed.

### Testing an update before there is a release

A build configured with `AETHER_UPDATE_DEV_OVERRIDES` (`BUILD-OPTIONS.md`;
`windows-installer.yml`'s `dev_overrides` dispatch input builds one, never
on a tag and never for the Store flight) honors four environment variables, each logged when used:

| Variable | Effect |
|---|---|
| `AETHER_UPDATE_MANIFEST_URL` | Fetch the manifest (and `.sig`) from here |
| `AETHER_UPDATE_TRUST_KEYS` | Also trust these Ed25519 keys (64 hex digits each, comma-separated) |
| `AETHER_UPDATE_DOWNLOAD_BASE` | Fetch release files from `<base>/<tag>/<asset>`. The URLs the manifest names are still checked as signed. |
| `AETHER_UPDATE_PRETEND_VERSION` | Run as this version, so the build's own release is an update. It is removed from the environment at startup, so the self-test and a restarted app are the real version. |

The freeze and replay checks are unchanged.

The release workflow applies every bundle to the real earlier release, by
this contract, before anything is signed, so a bundle that does not rebuild
its release never ships. The tests add a reference applier written from this
document. The client PR owns the negative cases and must test each: a bad signature, a replayed older manifest, a source
file that fails `from_sha256`, a corrupted bundle or member, a result that
fails its `sha256` or exceeds its `size`, a `latest/` that disagrees with
GitHub, and paths or symlink targets that escape the install (absolute, `..`,
a write through a link, or a link pointing outside it).

## Swapping it in (Windows, Phase 3)

`AetherSDRUpdater.exe` (`src/updater/`) does the swap after AetherSDR
exits. It is built with the static MSVC runtime and no Qt, and the app
copies it into the state folder before starting it
(`UpdateInstaller::launchHelper`, called by `UpdateController` below),
because it renames the folder it was installed in. Its
command line is versioned (`--protocol 1`), and anything else is refused.

In-place updates are per-user. `launchHelper` refuses while AetherSDR runs
elevated across a UAC boundary (the split token's full half): the helper
would run from a user-writable folder at the elevated level, and a
swapped-in tree would keep that folder's permissions inside Program
Files. With UAC off, or as the built-in Administrator, there is no lower
level to escape from, and it runs. A per-machine install run normally fails the
helper's writable-parent check and is left alone (`NotPossible`); either
way the answer is to run the installer.

1. It waits for AetherSDR to exit, up to five minutes, and never kills it.
2. It checks first: the install and the staged release each hold
   `AetherSDR.exe`, the install's parent folder is writable, and no
   `<install>.new` it did not create is in the way.
3. It moves the staged release to `<install>.new`. Across volumes it copies
   instead, so the swap is a same-volume rename.
4. It renames `<install>` to `<install>.previous`, then `.new` to `<install>`.
   A brief lock is retried for up to 30 s; one that persists is undone.
   Only one `.previous` is kept.
5. It runs `AetherSDR.exe --update-selftest <marker>` from the new tree.
   The new version starts Qt, writes its version to the marker and exits,
   with no window, no settings and no radio. A missing or wrong marker, or
   no exit within 60 s, restores `.previous` and keeps the failed release as
   `<install>.failed`.
6. Restart to update then starts the app; a swap on quit does not.

A journal (`swap-journal.txt`) records each step before it is taken, and
a step is taken only once its record reached the disk: it is flushed
(`FlushFileBuffers`) and moved into place with `MOVEFILE_WRITE_THROUGH`, so
a power cut leaves the old record or the new one whole. If a record can't
be written, the step is skipped and the swap undone, so the last record on
disk always matches the folders. A helper that dies mid-swap is repaired by
the next run, but only after AetherSDR has exited, never while it runs. The
install is restored from `.previous`, and a release that was in place but
never passed its self-test (the journal at `swap-2`, `selftest` or
`rollback`) is rolled back. The journal is kept while a repair is still
owed: after a failed rollback, or when the app never exited. Windows has
no atomic folder swap; between step 4's two renames nothing runnable is at
`<install>` for a moment, so nothing could start AetherSDR to start the
helper again. Before the first rename the helper therefore arms a repair:
`repair-plan.txt` in the state folder (the install and the version) and an
`HKCU\Software\Microsoft\Windows\CurrentVersion\RunOnce` entry that runs
`AetherSDRUpdater.exe --protocol 1 --repair --state-dir <dir>` at the next
logon. If the repair cannot be armed (or its command line would pass
RunOnce's 260 characters), nothing is renamed.

The repair runs `recover()` alone. Windows deletes a RunOnce value before
running it, so the repair arms itself again first, and one cut short runs
at the following logon. While an `AetherSDR.exe` runs from the install it
renames nothing and waits for the next logon. It records `SwapUndone`,
never `RolledBack`, since the release was never tested, and only when it
moved a folder. One with nothing owed only disarms. The helper disarms it
whenever it removes the journal.

The journal names the file the swap left at `<install>`: the volume serial
and NTFS file ID of the new release's `AetherSDR.exe`, which a rename keeps
and a file written again does not. `recover()` rolls back only that tree,
so a release reinstalled after the Broken message (below) is never
replaced by the stale `.previous`. The outcome goes to `swap-result.txt`, which the app reads once
at its next start and checks before believing (a known outcome and a
plain version, or nothing is logged or kept). A rolled-back release is
remembered as `Updates.rolledBackVersion`, which `UpdateController` uses to
never stage or offer it again.

### The self-test is a permanent contract

The helper that runs is the old install's, and it tests every later
release. So `AetherSDR.exe --update-selftest <marker>` is frozen: one
marker path argument, read from the Unicode command line, and a marker
holding exactly the version (`26.10.4`, no newline), written before any
window, settings write or radio. A release may add a new self-test beside
it, never rename, drop or change this one: an old helper would otherwise
start the full app, stop it after 60 s and roll back a good release.

If a rollback rename itself fails, the helper shows a message naming the
three folders and the log (`updater.log`), rather than failing silently.

## The Linux install (Phase 5)

There is no helper on Linux. Each release has its own folder, and a link
names the one to run:

```
~/.local/share/AetherSDR/install/<version>/   the payload tree (AppRun, usr/...)
~/.local/share/AetherSDR/install/current      -> <version>
~/.local/bin/aethersdr                         -> .../install/current/AppRun
~/.local/share/applications/AetherSDR.desktop  Exec/TryExec .../install/current/AppRun
```

(`~/.local/share` is `$XDG_DATA_HOME`.) The install has a folder of its own
because `~/.local/share/AetherSDR` already holds user data. `LinuxInstall`
(`src/core`) does all of it.

- **`AetherSDR --install`**, run from the unpacked tree (the tarball, or
  `--appimage-extract`'s `squashfs-root`), copies the tree into
  `install/<version>`, makes it `current`, writes the menu entry from the
  tree's own `usr/share/applications/AetherSDR.desktop` with `Exec` and
  `TryExec` naming `current/AppRun`, links the tree's hicolor icons and the
  command through `current`, and starts it. A file of the user's own at a
  link's place is left alone. It runs before `QApplication` and writes no
  settings.
- **The switch** is what the helper's hand-over is on Windows, done by the
  app itself in `UpdateInstaller::launchHelper`: the staged tree is renamed
  into `install/<version>`, that folder's `AppRun --update-selftest` must
  pass (the same frozen contract, 60 s), and only then is a new link renamed
  over `current`. Every other version folder is then removed, except the one
  running, the one `current` named and any another AetherSDR runs from
  (found through `/proc/<pid>/exe`). A rename over a link is atomic, and the
  running tree is never touched, so there is no window with nothing
  runnable, no journal and no repair. `--install` of a version already
  installed copies the new tree beside it and trades the two in one step
  (`renameat2(RENAME_EXCHANGE)`), so `current` never dangles, even if the
  installer is killed mid-copy; the old copy is removed once nothing runs
  from it.
- If the self-test fails on **Restart to update**, the new folder is
  removed, `current` is unchanged and the release is remembered as rolled
  back; the window says so at once. As AetherSDR **quits**, a failure may be
  the session's rather than the release's (a logout takes the display away
  first), so the tree goes back to staging and is tried again at the next
  quit or Restart (a maintainer ruling), counted in
  `Updates.quitSwitchTries`. The third failure at quit is final: the
  release is removed and rolled back, and `RolledBack <version>` is written
  to `swap-result.txt` for the next start (a maintainer ruling). If
  `current` cannot be changed, the tree goes back to staging too. Success
  writes `Updated <version>`.
- If `current` already names that release or a newer one (installed by hand
  meanwhile), the staged tree is dropped and nothing is switched: never a
  step back. Cleanup never removes the folder `current` named.
- **Restart to update** then starts `current/AppRun` once this process has
  exited (a `/bin/sh` loop polling the pid, two minutes at most), so two
  instances never overlap.
- Install update is offered only when the running binary
  (`/proc/self/exe`: Qt reports the link's folder when started through
  `AppRun` or `current`) is `install/<version>/usr/bin/AetherSDR` and the
  install folder is writable. The AppImage, distribution packages and build
  folders keep **Get AetherSDR**.

## Install update (the app's side)

`UpdateController` (`src/core`) is the one place the app installs from.
MainWindow owns it, so a staged release outlives the Updates window that
started it; the window and the title-bar badge show its state:
**Idle**, **Staging** (with progress), **Staged**, and **Handed** (the
helper has it and waits for AetherSDR to exit).

- Install update is offered only when `UpdateInstaller::canInstallInPlace`
  says so. Windows: not a store install, no split elevated token,
  `AetherSDRUpdater.exe` beside the app, and the install's parent writable.
  Linux: an install `--install` made (see above).
  Otherwise, or once a release falls back from the full download, the
  window keeps **Get AetherSDR** in the browser. Beside Install update, a
  **Download release** link keeps the browser route (a maintainer ruling).
- Staging requests `fullFallback`, with GitHub's latest as the freeze
  check's tag. Closing the window while it stages cancels it (a maintainer
  ruling); a staged release stays.
- At startup, before any check, `stagedRelease()` restores what an earlier
  run staged. Each answer from the update check runs it again with the new
  latest, so a superseded release is discarded. It never runs while
  staging.
- Skip this version discards a staged (or staging) release of that
  version. A rolled-back version is never staged.
- **Restart to update** runs `revalidate()` and then hands the release to
  the helper with `--restart` on a worker thread (on Linux the switch and
  its self-test run there too, a maintainer ruling), in the state
  `Installing`; `restartReady` then closes the window, and AetherSDR quits
  normally. If the files changed since staging, the release is discarded
  and the window says to install again.
- As AetherSDR quits (the end of `MainWindow::closeEvent`), a staged
  release is revalidated and handed over without `--restart`, unless radio
  firmware is being written. Revalidating hashes the whole tree, so the
  quit waits for it.
- At the next start the status bar reports the helper's result for about
  10 seconds (`UpdateInstaller::lastSwapResult()`, read once in `main.cpp`).

## Bundle naming

`AetherSDR-<tag>-<label>-from-<from tag>.update.tar`, where `<label>` is
`Windows-x64`, `x86_64` or `aarch64`. The `.update.tar` suffix keeps bundles
out of `sign-release.yml`'s download patterns, so they are not in
`SHA256SUMS.txt`. The signed manifest, not `SHA256SUMS.txt`, is what covers
them.
