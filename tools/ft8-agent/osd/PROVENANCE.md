# osd174_91 — provenance

## What this is

A C port of **`lib/ft8/osd174_91.f90`** from **WSJT-X**, the ordered-statistics
decoder for the (174,91) FT8 code, by **Steve Franke K9AN** and **Joe Taylor K1JT**.

Fetched from the WSJT-X SourceForge repository at `master`, 2026-11-14
(`https://sourceforge.net/p/wsjt/wsjtx/ci/master/tree/lib/ft8/osd174_91.f90`),
409 lines of Fortran.

Added for **[#7114](https://github.com/aethersdr/AetherSDR/issues/7114) ruling 3**:
*"Port WSJT-X's `osd174_91` into the agent before it ships, and measure the decode
rate against the same reference corpus with and without it."*

## ⚠ Licence — this directory is GPL-3.0, the library beside it is MIT

WSJT-X is **GPL-3.0-or-later**, so this port is too. The `ft8_lib/` directory next
to it is **MIT** (Kārlis Goba) and its own PROVENANCE.md documents that carefully.

That is why the port lives **here and not inside `ft8_lib/`**: the licence boundary
should be visible in the directory layout rather than buried in a file header.
AetherSDR is GPL-3.0-or-later, so the combination is fine; mixing the two trees
would just make the next person's audit harder.

## The one real porting decision

The Fortran builds its generator matrix by **encoding 91 unit vectors**
(`osd174_91.f90:37-62`, via `encode174_91_nocrc`). Done literally, that would drag
the **LDPC encoder** back into this agent — and `ft8_lib/PROVENANCE.md` records that
`ft8/encode.c` was *deliberately removed* so that "no transmit code path" is a
property of the source rather than a claim about it. Reintroducing an encoder for a
receive-only feature would quietly undo that.

It is not necessary. Encoding a unit vector `e_i` only selects row/column `i` of the
generator, and **`kFTX_LDPC_generator` is still vendored** in
`ft8_lib/ft8/constants.c:30` — it survived the encoder's removal and is referenced
nowhere else today. So `build_generator()` reads it directly:

- `j < 91` → the systematic part, the identity
- `j >= 91` → bit `i` of parity row `j-91`, MSB-first across 12 bytes

**No transmit code was added.**

## Other changes from the Fortran

- **0-indexed throughout.** Fortran is 1-indexed and column-major; the loop bounds
  were rewritten rather than transliterated, because that is where ports of this
  routine usually go wrong.
- **`g2` (the transpose of `genmrb`) is never materialised.** `mrbencode91` indexes
  `genmrb` the other way round instead.
- **`indexx`** is a local insertion sort rather than the shared WSJT-X heapsort.
  Only the ordering matters, and 174 elements sort in microseconds.
- **The box tables** (`boxit91`/`fetchit91`, preprocessing rule 2) are a Fortran
  `COMMON` block sized `fp(0:525000)`, about 2 MB. Here they are heap-allocated on
  demand and freed, and only for depths that use them (3 and up), because the whole
  waveform package is bounded at 512 KB.
- **`get_crc14`** is not ported: ft8_lib already has `ftx_extract_crc` /
  `ftx_compute_crc`, and the CRC is checked exactly the way `decode.c` checks it.

## Measured — 22 upstream reference WAVs, 353 scoreable messages

Corpus is `kgoba/ft8_lib` at the pinned commit `9fec6ca`, `test/wav`. It is **not
vendored** (only one file is, for the selftest); fetch it for measurement. Scored by
message text, since the ground-truth `.txt` files format their metadata differently.

Run on x86-64 (Aether-Linux), one binary, depth set at runtime via `AETHER_FT8_OSD`:

| depth | decoded | rate | median | p90 | max |
|---|---|---|---|---|---|
| **0 (OSD not called)** | 259/353 | **73.4 %** | 30 ms | 32 ms | 33 ms |
| 1 | 266/353 | 75.4 % | 57 ms | 64 ms | 65 ms |
| **2** | 273/353 | **77.3 %** | 67 ms | 75 ms | 76 ms |
| 3 | 276/353 | 78.2 % | 117 ms | 134 ms | 136 ms |
| **4 (WSJT-X's FT8 setting)** | 279/353 | **79.0 %** | 1071 ms | 1227 ms | 1259 ms |

**The depth-0 arm reproduces the pre-OSD baseline exactly** (259, and
`ft8_lib/PROVENANCE.md`'s independently measured 73%). That is the positive control:
had the control arm drifted, the comparison would prove nothing.

⚠ **Depth 0 in `osd_depth_t` is "order 0", NOT "off".** It still runs the Gaussian
elimination and the order-0 re-encode, and recovers frames on its own — it scored
262, three above the true baseline. The caller in `decode.c` therefore treats depth 0
as *do not call OSD at all*. Getting this wrong overstates the baseline and understates
the gain.

### Reading the table

Depth 4 buys **+0.8 points over depth 3 for roughly 9× the time**. On x86 its 1071 ms
median already eats half the ~2 s budget, and the FLEX-6300's single Cortex-A8 is
several times slower than this machine — so depth 4 is very unlikely to fit on the
radio, while depth 3 (117 ms here) has real headroom.

**The on-radio numbers are the ones that decide it, and they are not in this table
yet.** Everything above is x86. ⛔ Do not quote these as the radio's figures: an
earlier qemu-vs-native comparison in this project differed by more than 10×.

## Build

Compiled in for every target (`-DAETHER_WITH_OSD=1`). The depth is a **runtime**
setting, so "with and without" is two runs of one binary rather than two builds that
could differ in more than the thing being measured.

```
AETHER_FT8_OSD=0   OSD is not called (the control)
AETHER_FT8_OSD=1..6  search depth; default 2
```

Package size with OSD: **103,403 bytes**, against the 512 KB bound (was 96,243).
