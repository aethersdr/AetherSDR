# FAAD2 HDC decoder in AetherSDR

This source subset comes from [FAAD2](https://github.com/knik0/faad2) tag
`2.11.2`, commit `673a22a3c7c33e96e2ff7aae7c4d2bc190dfbf92`, with exactly the
HDC-support patch shipped by nrsc5 commit
`0225922b6f68109df39d07391f4d855464598ab8` applied. No other vendor edits are made.

Patch: `../nrsc5/upstream/support/faad2-hdc-support.patch`

SHA-256: `e25e8d69c20269757c66ba38c730fbde4588393d981d45d86472beea5cffc117`

The patch's original header credits Clayton Smith and patch commit
`813725c185276045641e7df90329dbeab5029434`. `SOURCE-MANIFEST.json` records each
included file's pristine and patched SHA-256 plus all omitted upstream paths.
It retains the original `COPYING`, `README`, `AUTHORS`, `ChangeLog`, public
headers, libfaad sources, version metadata and both non-library files touched
by the patch (`CMakeLists.txt`, `frontend/main.c`). Those last two are retained
for exact patch reproduction and are never evaluated or compiled by Aether.

## License and acknowledgment

FAAD2 declares GPL-2.0-or-later; AetherSDR distributes this combined work under
GPLv3. Preserve `upstream/COPYING`, the original README, and every per-file
copyright notice. The required acknowledgment is:

**Code from FAAD2 is copyright (c) Nero AG, www.nero.com**

## Build and reproduction

`cmake/AetherHdFm.cmake` builds only static `aether_faad_hdc`, with
`HDC_SUPPORT`, upstream's floating-point configuration and DRC setting, and the
GNU `-ffloat-store` setting. The ordinary, DRM and fixed-point variants and the
FAAD CLI are not targets. This decoder is linked privately by `aether_nrsc5`.

To reproduce, obtain the exact FAAD revision, copy the upstream paths in the
manifest into a fresh directory, and verify each `upstreamSha256`. From that
directory run `git apply --check /absolute/path/to/faad2-hdc-support.patch`
followed by `git apply /absolute/path/to/faad2-hdc-support.patch`. Every
`vendoredSha256` must then match. Patch failure must fail the import; no fuzz,
partial application or ignored error is acceptable. Builds never apply patches
or download sources. The checked-in patched files are the build inputs.
