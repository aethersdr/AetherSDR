# In-App Update Manifest — Schema 1

**Status:** CI side (RFC #5304 Phase 2). The app-side `UpdateStager` that
reads this is a later PR and implements against this document.

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
  which the Phase 5 tarball channel installs.
- `full`: the whole payload, the fallback when no patch applies or anything
  fails to verify. Its `sha256` and `size` are as binding as a bundle's: release
  assets can be replaced by anyone with write access, and the signed manifest
  is what pins them. For Linux, `full` is the AppImage, a container the
  tarball channel cannot install without squashfs tools. The Phase 5 tarball
  is the Linux fallback the client will use; until it exists, a Linux client
  that cannot patch offers the release page.
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
   `size`, and its window limit at `window_log` (at most 31). A member that
   would exceed either is a hard fail.
5. For each `patch` op, check the current file against `from_sha256` before
   using it. A mismatch is a hard fail, not a best effort (Principle VII).
6. After all ops, check every entry in `files` against the staged tree. Any
   mismatch abandons the staged tree and falls back to `full`.
7. Files in the install that are not in `files` are left alone. That covers
   the installer's own `unins000.*` on Windows.
8. Swap on quit, keeping the previous tree for rollback.

The release workflow applies every bundle to the real earlier release, by
this contract, before anything is signed, so a bundle that does not rebuild
its release never ships. The tests add a reference applier written from this
document. The client PR owns the negative cases and must test each: a bad signature, a replayed older manifest, a source
file that fails `from_sha256`, a corrupted bundle or member, a result that
fails its `sha256` or exceeds its `size`, a `latest/` that disagrees with
GitHub, and paths or symlink targets that escape the install (absolute, `..`,
a write through a link, or a link pointing outside it).

## Bundle naming

`AetherSDR-<tag>-<label>-from-<from tag>.update.tar`, where `<label>` is
`Windows-x64`, `x86_64` or `aarch64`. The `.update.tar` suffix keeps bundles
out of `sign-release.yml`'s download patterns, so they are not in
`SHA256SUMS.txt`. The signed manifest, not `SHA256SUMS.txt`, is what covers
them.
