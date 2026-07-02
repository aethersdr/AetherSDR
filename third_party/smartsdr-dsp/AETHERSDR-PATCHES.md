# AetherSDR Patches

This directory contains the GPL-3.0 `thumbDV_support` branch of
`n5ac/smartsdr-dsp`, trimmed to the ThumbDV waveform runtime sources needed by
AetherSDR's `aether-dstar-waveform` helper.

Imported source:

- Repository: https://github.com/n5ac/smartsdr-dsp
- Branch: `thumbDV_support`
- Commit: `762297a7d56f9e37c661c550e4b1cbd8b78091f1`

Local changes:

- Removed the bundled FTDI D2XX static library and replaced the D2XX API surface
  used by ThumbDV with `aether_serial_compat.c`, a POSIX termios serial backend.
- Replaced the original embedded-radio `main.c` with a small command-line entry
  point that accepts AetherSDR's `--host` and `--serial` arguments.
- Changed startup to connect directly to the supplied radio IP address instead
  of waiting for a matching discovery packet.
- Added compatibility headers for Linux-only `sys/prctl.h` and `linux/*`
  includes so the helper builds on macOS and Linux.
- Added an Apple-only unnamed semaphore shim because Darwin exposes `sem_init`
  but does not provide usable unnamed POSIX semaphores.
- Added `aether_vocoder_backend.*` as a thin ThumbDV-only wrapper for the
  vocoder calls used by the local helper.
- Applied CodeQL hardening to vendored runtime code that is compiled into the
  helper: widened command argument loops, promoted buffer byte-count
  multiplication before allocation/copy, matched printf format types, and opened
  the waveform config directly instead of checking it with `stat()` first.
- Build only the local helper executable; the historical Windows GUI,
  `.ssdr_waveform` package, IDE metadata, and binary artifacts are not vendored.
