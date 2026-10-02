# Packaging an experimental digital FM checkpoint

This recipe is for local, explicitly requested test artifacts. It does not
change `ENABLE_HD_FM`'s default or publish, sign a release, or install drivers.
Record the exact source revision and actual toolchain for each platform. Build
`AetherSDR` and `aetherd` with `ENABLE_HD_FM=ON` and `ENABLE_RTL=ON`, run the
relevant socket-free receiver tests, and preserve the separate decoder-OFF result.
Recorded-IQ decoding and disconnected startup are not live RF or audible proof.

## Matching source and notices

Prepare a source archive at the exact signed application revision, including
all tracked application sources, vendored source, original notices, local
patches, manifests, CMake files, setup and packaging scripts. `git archive`
does not include submodules: enumerate gitlinks and add their exact revisions
if present. Do not archive an entire working directory or its `.git` directory.

Add complete matching source for externally supplied libraries whose licenses
require it, with their exact upstream version/commit, retrieved source hash,
binary hash, build/configure recipe and any local changes. In particular,
nrsc5 and patched FAAD2 are already in the application source tree, but FFTW
and RTL-SDR are not supplied by those decoder manifests. Include the source
for the actual FFTW/RTL/libusb and Qt runtimes in the payload as applicable;
a URL or a header/import library alone is not a matching source archive.
Use the dependency caches and original build records to identify them before
fetching anything. Do not label uncertain provenance as verified.

Preserve nrsc5's original license, full GPLv3 text, and license-source record;
FAAD2's COPYING, README, AUTHORS and the Nero acknowledgment; both source
manifests; and external runtime licenses. Keep original per-file notices in
the source archive. The source archive's own README records reproducible build
steps and dependencies. Never include private RF captures, user settings,
credentials, PDBs or build caches in the shareable source or binary archive.

## Windows staging

Use PowerShell 7 from the existing native MSVC/Qt environment. Supply a fresh
output directory, the verified matching-source archive and its SHA-256, an
external-dependency notices directory, and a public-safe `BUILD-INFO.json`:

```json
{
  "revision": "<full application commit>",
  "platform": "windows-x64",
  "hdFm": true,
  "rtl": true,
  "toolchain": { "c": "<actual>", "cxx": "<actual>", "qt": "<actual>" },
  "signing": "<observed Authenticode result>",
  "binarySha256": { "AetherSDR.exe": "<sha256>", "aetherd.exe": "<sha256>" },
  "buildOptions": ["<actual options, without private paths>"],
  "dependencies": ["<matching versions and provenance>"],
  "testScope": ["<checks actually completed>"]
}
```

Invoke `packaging/windows/stage-hd-portable.ps1` with `-BuildDir`, `-OutputDir`,
`-Revision`, `-SourceArchive`, `-SourceSha256`, `-DependencyNoticesDir`,
`-BuildInfo` and `-QtDir`. It copies only the application/daemon/helper files
and staged DLLs, plus the known loose DFNR model filenames when present, runs
`windeployqt` and the existing app-local MSVC runtime
stager and dependency audit, and adds the notices, source, friend README,
isolated-settings launcher and per-file hashes. It does not create the ZIP or
claim successful startup. Do not mutate a shared Qt install to satisfy a
deployment failure; identify the missing module and stage matching files in
task-owned paths if the deployment tool needs a separate resolution copy.
An active external-model DFNR runtime must have its model beside the executable;
the stager refuses that configuration when the known model payload is missing.

The existing import audit indexes DLLs by filename throughout the payload.
That is intentionally more permissive than Windows' loader. The evaluation stager
additionally checks that each shipped import is beside the executable or its
importing plugin. Inspect loaded module paths in the clean-start test too.
Test `qwindows.dll` as well as the offscreen plugin.

## Clean-extraction verification

1. Create a ZIP from the staged payload and extract into a new task-owned
   directory. Check every `FILES-SHA256.json` entry against the extracted bytes.
2. Launch from that extracted directory with `PATH` restricted to
   `%SystemRoot%\System32;%SystemRoot%`; clear `QT_PLUGIN_PATH`,
   `QT_QPA_PLATFORM_PLUGIN_PATH`, `QML2_IMPORT_PATH` and `QML_IMPORT_PATH`.
   Use a fresh `AETHER_SETTINGS_DIR` outside the payload. Set
   `AutoConnectToLastRadio=False` through the app's `--config set` CLI, and
   verify its `--config path` resolves to the isolated directory.
3. Prefer the automation MCP's fresh-instance lifecycle. Where unavailable,
   use the documented bridge with a unique socket and identity, an ephemeral
   in-memory token, `AETHER_AUTOMATION_NO_TX=1`, and no
   `AETHER_AUTOMATION_ALLOW_TX`. Never write or echo the token. The launcher
   only selects an isolated settings store; it is not a general network or
   credential sandbox.
4. Confirm authenticated `ping`/`whoami`, source identity, TX refusal and
   disconnected radio state. Inspect runtime modules and plugin paths for
   accidental dependency on Qt, MSVC or dependency development directories.
   Verify native decoder availability through the real decoder tests; a
   disconnected WFM widget alone cannot prove runtime decode readiness.
5. Exercise native Windows plugin startup in a task-owned instance, shut down
   only that process, and preserve the result separately. Do not connect a
   radio, change a driver, kill existing apps or alter the active user's session.
6. Retrieve the ZIP, separate source archive, checksums and readable report to
   the Mac. Recompute the retrieved hashes. Publish only after a separate
   explicit authorization. Keep test profiles and logs outside the friend ZIP.

## macOS bundle

Deploy the matching Qt and non-system runtime dependencies inside a separate
`.app`, rewrite only task-owned load paths, and audit all Mach-O imports and
architectures recursively. Standard system frameworks remain host-provided.
Test with developer search paths removed and an isolated `AETHER_SETTINGS_DIR`;
check the bundled Qt plugins load from the bundle. Preserve the running primary
app and launcher. Report observed code-signing/notarization status accurately:
an ad-hoc signature is not a Developer ID signature or notarization.

Create or refresh the task-specific launcher only after the bundle passes.
Local Apple Silicon evidence does not qualify Intel Mac, and Windows x64
evidence does not qualify Windows ARM. Preserve private fixture evidence
separately from the artifacts intended for sharing.
