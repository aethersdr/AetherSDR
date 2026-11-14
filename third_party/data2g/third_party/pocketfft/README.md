# pocketfft (vendored)

`pocketfft_hdronly.h` from https://github.com/mreineck/pocketfft, branch
`cpp`, fetched 2026-07-28. BSD-3-Clause — see `LICENSE`, which is the
licence block from the header itself.

**Vendored rather than fetched at configure time.** It is a single
header with no build system of its own, and a `FetchContent` would make
every CI job on three platforms depend on GitHub being reachable during
`cmake` — a network dependency in the one place a build should not have
one. Updating means replacing one file.

## Why this library

`docs/native-app.md` chose it because *"`scipy.fft` **is** pocketfft.
Same algorithm, same rounding — parity vectors match bit-for-bit instead
of to an argued tolerance."*

**That is no longer quite true, and the difference matters for how
vectors are classified.** SciPy has since moved its FFT backend to
`ducc0` (`scipy.fft._duccfft`), which is pocketfft's successor by the
same author. The lineage and the algorithms are shared, but the two are
not guaranteed to produce identical bits, and there is no reason to
assume they do.

This costs nothing here, because FFT-derived golden vectors are
tolerance-class anyway under the rule in `tools/gen_golden_vectors.py`:
an FFT associates its sums in an implementation-defined order, so it
could never have been bitwise across platforms even against an identical
library. The practical consequence is only that the C++ FFT is checked
against the reference by value, like every other non-reproducible
operation, rather than being expected to match exactly.
