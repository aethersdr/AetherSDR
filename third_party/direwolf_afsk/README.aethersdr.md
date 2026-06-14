# AetherSDR direwolf-afsk subset

This directory contains an algorithmic derivation of the Dire Wolf "profile A"
AFSK demodulator, rewritten in self-contained C++ for AetherSDR's VHF 1200 baud
AX.25 receive path.

Upstream: https://github.com/wb2osz/direwolf
Reference release: Dire Wolf 1.7
Referenced files: `src/demod_afsk.c` (profile A), `src/dsp.c`
License: GPL-2.0-or-later (Dire Wolf) — compatible into AetherSDR's GPL-3.0-or-later

This is an algorithmic derivation, not a verbatim copy. The algorithm
(bandpass prefilter → IQ mixing → RRC lowpass → amplitude → peak/valley AGC →
DPLL) is derived from profile A in `demod_afsk.c`. Filter design helpers are
derived from `dsp.c`. All code was rewritten in idiomatic C++20.

## Files

- `AetherAFSKDemod.h` — class definition (adapted into the demod pipeline via
  the `AfskDemodWrapper<>` template in `AetherAx25LibmodemShim`; no
  `sinc_corr_afsk_demodulator` alias is provided)
- `AetherAFSKDemod.cpp` — full implementation

## Usage

Used unconditionally for the `Ax25ModemProfile::Vhf1200` (1200 baud Bell 202)
demodulation path in `AetherAx25LibmodemShim`. The `Ax25ModemProfile::Hf300`
path continues to use `aether_libmodem_core::sinc_corr_afsk_demodulator`.

## Intentionally omitted

All Dire Wolf application infrastructure: audio backends, KISS TNC server,
GPS/APRS clients, digipeater, IGate, configuration parser, IL2P/FX.25,
multi-channel engine, PTT drivers, and build system.

## Refresh notes

1. Review upstream changes to `src/demod_afsk.c` and `src/dsp.c` for any
   profile-A algorithm improvements.
2. Re-derive and rewrite into `AetherAFSKDemod.cpp` as needed.
3. Validate decode rate against Direwolf and Graywolf on live APRS audio.
