# ft8_lib — vendored, pruned

Upstream: <https://github.com/kgoba/ft8_lib>
Pinned commit: `9fec6ca` ("non-standard callsigns; special CQ; field type annotation (#47)")
Licence: **MIT**, Copyright (c) 2018 Kārlis Goba — see `LICENSE`.

AetherSDR is GPL-3.0-or-later. MIT is permissive and GPL-compatible, so this
vendors cleanly. `fft/` is kissfft (BSD-3-Clause, Copyright (c) 2003-2010 Mark
Borgerding), already vendored inside ft8_lib upstream.

## What was removed, and why

The upstream tree is 45 MB; this copy is ~600 KB. Removed:

| removed | why |
|---|---|
| `.git/` (22 MB) | Vendored source, not a submodule. The pin is recorded above. |
| `test/` except one fixture (23 MB) | Only `test/wav/websdr_test6.{wav,txt}` is kept — the busiest reference file (30 messages), used as the decoder regression fixture. |
| `demo/`, `utils/`, `ft4_ft8_public/` | Upstream demo programs and reference material; not compiled here. |
| `Makefile` | Replaced by this agent's own build. |
| `common/audio.c`, `common/audio.h` | PortAudio device I/O. The agent gets audio from the radio's DAX stream, not a sound card, and we do not want the PortAudio dependency. |
| **`ft8/encode.c`, `ft8/encode.h`** | **Transmit-side waveform encoding.** Nothing in the tree referenced it (verified by grep), and removing it means "no transmit code path" is a property of this source tree rather than a claim about it. `message.c`'s own `ftx_message_encode*` text-packing functions are unrelated and remain — they parse and pack callsigns, which the decoder needs. |

Verified after pruning: `gcc -O2 -std=gnu11 -Wall -Wextra -I. -c ft8/*.c
common/*.c fft/*.c` builds 10 objects with no errors and no warnings.

## Known limitations of this decoder

Stated here because they are properties of the dependency, not of our code, and
they must reach the feature's documentation rather than surprise an operator:

- **No OSD (ordered-statistics decoding) and no signal subtraction.** These are
  the two passes behind WSJT-X's deepest decodes. Established by reading the
  source, not inferred.
- Measured decode rate against ft8_lib's own 22 reference WAVs: **73%**
  (265 of 362 messages), 2026-11-08. Independent reports put it near WSJT-X
  *depth 1* but well below normal depth on crowded bands.

⇒ A skimmer built on this finds roughly three-quarters of what WSJT-X finds. It
is useful for band-activity and propagation data; it is not a replacement for
WSJT-X at the operating position and must not be presented as one.

## Updating the pin

Re-clone at the new commit, re-apply the removals in the table above, re-run the
compile check, and re-measure the decode rate against the kept fixture before
changing the figures quoted here or in the feature's documentation.
