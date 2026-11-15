# Debugging crashes in release builds

Release binaries ship without debug info. Every release also carries one
debug-symbol archive per platform build, attached next to the installers:

| Build | Release asset |
|---|---|
| Linux AppImage x86_64 / aarch64 | `AetherSDR-vX.Y.Z-x86_64-symbols.tar.xz`, `…-aarch64-symbols.tar.xz` |
| macOS DMG Apple Silicon / Intel | `AetherSDR-vX.Y.Z-macOS-apple-silicon-symbols.tar.xz`, `…-macOS-intel-symbols.tar.xz` |
| Windows x64 | `AetherSDR-vX.Y.Z-Windows-x64-symbols.tar.xz` |

A crash report from a user plus the archive for that exact version and
platform is enough to get function names and source lines. Nothing has to be
hosted anywhere else. Releases made before these archives existed have none.
Builds from source or from a distribution package are not covered either.

## For users: what to attach to the issue

Say which download you ran: AppImage, DMG or Windows installer. Help → File
an Issue fills in the version, CPU architecture and OS for you. Then attach
the crash report your operating system kept:

- **Linux** (systemd-coredump, the default on most distributions):
  ```sh
  coredumpctl list AetherSDR
  coredumpctl info <PID> > aethersdr-crash.txt
  ```
  Attach `aethersdr-crash.txt`. It lists the stack as module offsets, plus
  each module's build ID. Do **not** attach the core file itself. It is a copy
  of the program's memory and can contain passwords and tokens.
- **macOS:** open Console → Crash Reports, or
  `~/Library/Logs/DiagnosticReports/`, and attach the newest `AetherSDR-*.ips`.
  It names your user folder in file paths. Edit that out if you prefer.
- **Windows:** Windows keeps a crash dump only if local dumps are switched on.
  In an administrator PowerShell:
  ```powershell
  $k = 'HKLM:\SOFTWARE\Microsoft\Windows\Windows Error Reporting\LocalDumps\AetherSDR.exe'
  New-Item -Force $k | Out-Null
  New-ItemProperty -Force $k -Name DumpType -PropertyType DWord -Value 1 | Out-Null
  ```
  After the next crash, the dump is in `%LOCALAPPDATA%\CrashDumps\`
  (`AetherSDR.exe.<pid>.dmp`). `DumpType` 1 is a minidump: the stacks only,
  not the whole of memory. Stack memory can still hold fragments of what the
  app was doing. If that concerns you, ask in the issue for a private way to
  send it.

## For maintainers: symbolizing a report

Download the archive for the reporter's version and platform:

```sh
gh release download vX.Y.Z -R aethersdr/AetherSDR -p 'AetherSDR-vX.Y.Z-<platform>-symbols.tar.xz'
mkdir symbols && tar -xJf AetherSDR-vX.Y.Z-<platform>-symbols.tar.xz -C symbols
```

On Linux and macOS, `symbols/SYMBOLS.txt` lists each binary's build ID or
UUID. Check it against the report before trusting any result. Symbols from
another build give confident, wrong answers.

### Linux

`coredumpctl info` prints frames as `AetherSDR + 0x<offset>` and a
`Module … with build-id <id>` line per module. The debug file for that ID is
`symbols/.build-id/<first two hex digits>/<rest>.debug`:

```sh
eu-addr2line -f -i -e symbols/.build-id/ab/cdef….debug 0x<offset> 0x<offset> …
```

With the core file itself (your own machine, or one shared privately), gdb
finds the symbols by build ID. Set the directory *before* loading, with `-iex`:

```sh
gdb -iex 'set debug-file-directory symbols' -iex 'set debuginfod enabled off' \
    /path/to/AetherSDR /path/to/core -ex bt
```

Bundled Qt and other third-party libraries resolve to their exported
function names only.

### macOS

In the `.ips` report, each frame has an `imageIndex` and an `imageOffset`, and
`usedImages[imageIndex]` gives that image's `uuid`, `base` and `arch`. Check
the UUID against `SYMBOLS.txt`, then:

```sh
atos -arch arm64 -o symbols/AetherSDR.dSYM/Contents/Resources/DWARF/AetherSDR \
     -l <base> <base + imageOffset> …
```

Use `-arch x86_64` for the Intel build.

### Windows

The archive holds `AetherSDR.pdb`, the waveform helper's PDB and the PDBs of
every Qt DLL the installer ships. Point WinDbg or Visual Studio at the
extracted folder, or follow the cross-platform `rust-minidump` route in
[WINDOWS-STORE-MSIX.md](WINDOWS-STORE-MSIX.md#symbolizing-a-crash-dump-macos--linux).
PDBs match a dump by GUID, so a PDB from another build is ignored rather
than misread.

## How the archives are made

- **Linux and macOS:** the release workflows (`appimage.yml`, `macos-dmg.yml`)
  compile Release with `-g` added; with GCC and Clang the generated code is
  the same. `scripts/build/split-debug-symbols.sh` then moves the DWARF out
  before packaging and signing: into `.build-id/` debug files on Linux, into
  `.dSYM` bundles on macOS. Before attaching, each workflow reads the build ID
  or UUID back from the packaged AppImage or the signed app bundle, and
  attaches no archive if it does not match.
- **Windows:** `windows-installer.yml` archives the PDB set it already
  stages for the Store's `.appxsym`.
- **A symbol step never blocks a release.** The split and package steps are
  `continue-on-error`: if one fails, the installers still attach and get
  signed, the job annotations show the failure, and `/tag-release` warns
  that the archive is missing.
- **Signing:** the archives are not GPG-signed and not in `SHA256SUMS.txt`.
  They are developer files, and the `.tar.xz` name keeps them out of the
  signing job's download patterns.
