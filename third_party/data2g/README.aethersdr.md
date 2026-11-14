# Data2G core (vendored)

The `data2g_core` target's sources from [Data2G](https://github.com/arodland/Data2G),
an open HF data modem, for AetherModem's HF data work (RFC #7159).

| | |
|---|---|
| Upstream | https://github.com/arodland/Data2G |
| Release | v0.5.0 |
| Commit | `7e1b98c5812839b6dee05d1a336d20793ed8bf92` |
| License | Artistic License 2.0 (`LICENSE`), with the Data2G Interface Exception 1.0 (`LICENSE_exception`) |
| Also | Ion notice, Apache-2.0 (`LICENSE_ion`); pocketfft, BSD-3-Clause (`third_party/pocketfft/LICENSE`) |

## What is here

Every file is byte for byte as upstream ships it. Nothing is patched.

- `core/` — upstream `native/core/`, less the four device-backend directories
  that upstream builds as separate libraries and excludes from `data2g_core`
  itself (`core/*/qt/`, `core/*/android/`, `core/*/hamlib/`,
  `core/*/miniaudio/`). This is exactly the file set upstream's
  `native/CMakeLists.txt` globs for the target.
- `core/rig/bridge.cpp` and `core/rig/bridged.cpp` are here but **not
  compiled**. They are a TCP socket client that only upstream's Android app
  uses; upstream leaves them out of the target on Windows, and AetherSDR
  leaves them out on every platform, so the built core has no network code.
- `core/generated/` — frozen tables written by upstream's
  `tools/gen_native_tables.py`: constants, submodes, constellations, LDPC
  shifts, polar designs, the deflate dictionary and the rate shifter's trained
  model (`predictor.cpp`). They are the on-air format. Never edit them.
  Their provenance is upstream's `docs/outcome-training-data.md` and
  `data2g/codes_data/README.md`.
- `third_party/pocketfft/` — the single-header FFT upstream vendors.
- `LICENSE`, `LICENSE_ion`, `LICENSE_exception` — upstream's, unchanged. The
  exception lifts Artistic-2.0 section 8's "no direct interface" condition
  for an application that is a significant work of its own, which is what
  permits the host and KISS ports of RFC #7159 Stage 3. It covers the
  copyright holder's own material; upstream notes two commits by another
  contributor that it does not yet cover, both in the Python reference and
  its tests, which are not vendored here.

## What is not here

Upstream's sound card backends, Hamlib rig control, the `data2g-host`,
`data2g-gui` and `data2g-web` apps, the Python reference implementation, the
tests and the studies. AetherSDR supplies its own audio and radio control.

## How it is built

`aether_data2g_core` in the root `CMakeLists.txt`, behind `ENABLE_DATA2G`.
It carries upstream's flags, which matter: no fast-math and no FMA contraction
(`/fp:precise` on MSVC), because the format is checked bit for bit against a
Python reference, and `-O3` under GCC on `polar.cpp`, `predictor.cpp` and
`gfldpc.cpp` only. `DATA2G_VERSION` is set there and must match the release
above.

The only consumer is `aether_hfdata` (`src/core/hfdata/`). The core's include
root has directories named `audio/`, `tnc/`, `host/` and `util/`, so it is
never put on the include path of the rest of the tree.

## Things to know

- The core's headers have members named `slots`. Qt's keyword macros rewrite
  that word, so a translation unit that needs both must include the core's
  headers before any Qt header. `src/core/hfdata/` is Qt-free for this reason;
  only tests include both.
- The core reads a few `DATA2G_*` environment variables for tuning and
  debugging (`DATA2G_THREADS`, `DATA2G_DD` and others). AetherSDR sets none.
- Left alone, the core keeps helper threads for the life of the process once
  it has decoded anything: a decode pool of `min(4, cores / 2)` threads, and
  pocketfft's own pool of one thread per CPU core. AetherSDR runs neither.
  The app sets the decode pool to one thread
  (`Data2gMonitor::setDecodeThreads(1)`), and the build defines
  `POCKETFFT_NO_MULTITHREADING`. Measured on the monitor, decode time is the
  same with and without them. The adapter's own test pins the pool to 2 so
  the pooled path stays exercised.
- Upstream calls the software beta and says the on-air protocol may still
  change. The protocol is versioned, so a mismatch is refused, not garbled.

## Refreshing

1. Pick an upstream release tag and record its commit above.
2. Replace `core/` with upstream `native/core/` less the four backend
   directories, and `third_party/pocketfft/` with upstream's. Copy `LICENSE`,
   `LICENSE_ion` and `LICENSE_exception`.
3. Update `DATA2G_VERSION` in the root `CMakeLists.txt`, the version check in
   `tests/data2g_monitor_test.cpp`, and the `THIRD_PARTY_LICENSES` entry.
4. Compare upstream's `native/CMakeLists.txt` for changed flags, new
   per-file options or changes to the target's file list, and carry them
   over. Read what is new in `core/` for anything that can key the radio or
   open a socket: RFC #7159's transmit admission table has to list every
   keying path.
5. Build and run `data2g_monitor_test` and `data2g_replay_test`. The second
   replays a session recorded from upstream 0.4.3: if it fails after a
   re-pin, read `tests/fixtures/data2g/README.md` before changing it.
