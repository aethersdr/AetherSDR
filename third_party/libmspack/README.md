# libmspack (vendored subset)

This directory vendors a subset of [libmspack](https://www.cabextract.org.uk/libmspack/)
for in-tree CAB + LZX decompression, used by `FirmwareStager::extractFromMsi()`
to unpack `.ssdr` firmware files from FlexRadio's WiX MSI installers.

## Version

- **Upstream version**: libmspack 0.11alpha (released 2023-02-05)
- **Source archive**: https://www.cabextract.org.uk/libmspack/libmspack-0.11alpha.tar.gz
- **License**: GNU LGPL v2.1 (see `COPYING.LIB`)

## Why vendored

Earlier MSI-extraction prototypes shelled out to `7z` via QProcess. That
introduced a runtime dependency that is unreliable on Windows
(uncommonly installed) and disallowed in sandboxed environments
(Snap, Flatpak, App Sandbox). Vendoring libmspack gives us a fully
self-contained extraction path.

We use only a small subset of libmspack's functionality: read-only CAB
decompression with LZX (and MSZIP / Quantum, which the CAB dispatcher
references unconditionally). Encoders and unrelated formats (CHM, HLP,
KWAJ, LIT, OAB, SZDD) are intentionally not vendored.

## File inventory

The following files are copied verbatim from upstream `mspack/`:

| File | Purpose |
|---|---|
| `mspack.h` | Public API |
| `system.h`, `system.c` | I/O abstraction layer |
| `cab.h`, `cabd.c` | CAB decompressor |
| `mszip.h`, `mszipd.c` | MSZIP decoder |
| `lzx.h`, `lzxd.c` | LZX decoder (primary path for MSI cabs) |
| `qtm.h`, `qtmd.c` | Quantum decoder (referenced by CAB dispatcher) |
| `crc32.h`, `crc32.c` | CRC-32 validation |
| `readbits.h`, `readhuff.h` | Bit-stream + Huffman helpers |
| `macros.h` | Utility macros |

## License compatibility

libmspack is LGPL-2.1. AetherSDR is GPL-3.0. The two are compatible:
LGPL-2.1 §3 explicitly allows opting into "any later version" of the
GPL, which includes GPL-3. The vendored source headers retain their
original copyright notices, and `COPYING.LIB` is preserved here.

## Updating

To pull in a newer libmspack release:

1. Download the new source archive from the upstream site
2. Replace each file listed above with the upstream version of the same name
3. Re-run the AetherSDR test suite, particularly `firmware_extract_test`
4. Update the version + date in this README

If upstream adds new internal includes that the vendored files reference,
copy those too. Run a build to find missing-header errors.

If a CVE is announced in upstream, cherry-pick the affected file(s).
