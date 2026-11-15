# AetherSDR SmartSDR-DSP Adopted Fork Notes

This directory is an adopted, in-tree fork of the GPL-3.0
`thumbDV_support` branch of `n5ac/smartsdr-dsp`, trimmed to the ThumbDV
waveform runtime sources needed by AetherSDR's `aether-dv-waveform` helper.

The upstream repository is inactive for AetherSDR's purposes: the public
`n5ac/smartsdr-dsp` repository has not received code updates since 2019, and
this tree was imported from the tip of the historical `thumbDV_support` branch.
AetherSDR does not treat this directory as a patch stack to reapply during
future upstream resyncs. It is maintained in place under `third_party/` so the
original license, attribution, and source boundary remain visible.

Future changes to compiled vendored files should stay narrowly scoped, keep
upstream copyright/license headers intact, and be recorded in git history. When
the modification is material, update the local change summary below so the
repository itself remains the provenance and change notice for this maintained
fork.

Imported source:

- Repository: https://github.com/n5ac/smartsdr-dsp
- Branch: `thumbDV_support`
- Commit: `762297a7d56f9e37c661c550e4b1cbd8b78091f1`

Local changes:

- Removed the imported OpenDV-derived D-STAR framing/state implementation and
  its GPL-2.0-only definitions. The helper now links the independently authored
  GPL-3.0-or-later `third_party/crdv` protocol core; its clean-room provenance,
  integration contract, tests, and immutable source manifest are kept with the
  library. The retained Flex GPLv3 symbol clock/modulator and ThumbDV transport
  contain no dependency on the removed definitions.

- Removed the bundled FTDI D2XX static library and replaced the D2XX API surface
  used by ThumbDV with `aether_serial_compat.c`, using POSIX `termios` on
  Unix-like systems and native Win32 COM-port APIs on Windows.
- Replaced the original embedded-radio `main.c` with a small command-line entry
  point that accepts AetherSDR's `--host` and `--serial` arguments.
- Changed startup to connect directly to the supplied radio IP address instead
  of waiting for a matching discovery packet.
- Added compatibility headers for Linux-only `sys/prctl.h` and `linux/*`
  includes so the helper builds on macOS, Linux, and Windows.
- Added Windows compatibility headers for the POSIX threading, semaphore,
  socket, and sleep APIs used by the ThumbDV helper.
- Added an Apple-only unnamed semaphore shim because Darwin exposes `sem_init`
  but does not provide usable unnamed POSIX semaphores.
- Added `aether_vocoder_backend.*` as a thin ThumbDV-only wrapper for the
  vocoder calls used by the local helper.
- Applied CodeQL hardening to vendored runtime code that is compiled into the
  helper: widened command argument loops, promoted buffer byte-count
  multiplication before allocation/copy, matched printf format types, and opened
  the waveform config directly instead of checking it with `stat()` first.
- Applied additional runtime hardening to compiled helper code: clamped VITA
  waveform payload copies to the fixed HAL receive buffer capacity, clamped
  command token counts before argv termination, and made serial `FT_Write` retry
  partial/non-blocking writes instead of silently dropping vocoder frame bytes.
- Changed the D-STAR TX scheduler to insert legal null AMBE voice frames when
  the ThumbDV encoder has not produced a frame for a consumed 20 ms speech
  block, keeping the RF frame stream continuous during serial/vocoder underruns.
- Preserved the historical local D-STAR TX GMSK inversion default, which maps
  logical one to positive waveform samples at the Flex DFM boundary, and added
  `AETHER_DSTAR_TX_INVERT=0` as an explicit diagnostic override.
- Added the diagnostic override `AETHER_DSTAR_TX_GAIN=<float>` for scaling
  generated D-STAR TX waveform samples during deviation/level testing without
  rebuilding.
- Gated local D-STAR waveform TX by the radio interlock source so TUNE
  transmissions do not emit D-STAR sync/header/voice frames.
- Added rate-limited D-STAR RX/TX diagnostics for waveform input buffers,
  output buffers, D-STAR AMBE/audio activity, and ThumbDV encode/null-frame
  activity, and flush helper stdout so `QProcess` captures diagnostics promptly.
- Suppressed idle RX/TX diagnostic flood by default while preserving useful
  D-STAR startup, TX voice/output, end-pattern, tail-flush, RX reset, and
  AMBE/audio activity diagnostics. Set `AETHER_DSTAR_VERBOSE_RX_IDLE_DIAG=1`
  when the raw idle RX buffer cadence is needed.
- Fixed the local helper's end-of-transmission setter so key-up can clear a
  prior unkey request instead of leaving the global end flag stuck true.
- Fixed the local helper scheduler so RX buffers delivered during an active TX
  cycle do not reset the D-STAR TX modulator state or clear the pending
  end-of-transmission tail. The receive demodulator is reset only after TX tail
  completion or an explicit post-unkey reset request.
- Fixed the final TX flush loop to snapshot the remaining circular-buffer
  length before draining the partial packet instead of using a changing loop
  bound while consuming samples.
- Corrected D-STAR framing and memory safety: the bit-sync preamble is one
  symbol per encoded bit and defaults to 250 ms so radio ramp and receiver clock
  recovery complete before frame sync. `AETHER_DSTAR_TX_PREAMBLE_MS` provides a
  bounded 64-bit-to-one-second diagnostic override. Header convolutional coding
  is bounded to 330 input/660 output bits, endian reversal covers only the 328
  header/CRC bits, and the complete
  41-byte header is transported through slow data. PFCS calculation no longer
  depends on host byte order, and the TX last frame is exactly the specified
  48-bit pattern without the adopted source's undocumented 22-symbol suffix.
- Replaced the mismatched ten-sample/symbol Gaussian FIR and 4.8 kHz peaking IIR
  with the GPLv2-or-later MMDVM modem's 15-tap BT 0.35 Gaussian interpolator at
  the Flex stream's native five samples/symbol. The FIR is zero-initialized and
  fully reset, the normalized TX sample gain defaults to 1.0, generated samples
  are hard-limited to +/-0.98, and non-finite samples or gain overrides become
  silence.
- Replaced the blocking TX flush with a tested, atomic idle/active/ending
  lifecycle, later extended with an explicit draining phase. Interlock handling
  now requires the D-STAR slice to be the TX
  slice, remembers TUNE across source-less UNKEY, rejects duplicate/terminal
  transitions, treats slice status fields atomically so `in_use=0` overrides a
  stale `tx=1`, preserves header-era ThumbDV output in a fixed sequence-tagged
  AMBE FIFO, and finishes a bounded tail at interruptible 24-kHz packet deadlines
  even when no later mic buffer arrives. The drain keeps PTT-edge speech first,
  trims only the newest excess at 280 ms to fit firmware 4.2.18's measured
  roughly 345 ms UNKEY window, and reserves time for the 48-bit EOT pattern.
  An unkey before the default 250 ms preamble completes is explicitly aborted:
  firmware returns to READY too soon to finish the protected header, so no stale
  vocoder or framing data is allowed into the next transmission.
- Finite-checked and saturated the 24-to-8 kHz ThumbDV encoder input to signed
  16-bit PCM, and surfaced waveform VITA send failures instead of discarding
  the listener-send result.
- Made ThumbDV queue purging bypass playback buffering gates, tagged pending
  vocoder requests by generation to reject stale responses across TX/RX
  transitions, made response validation/insertion atomic with queue flushing,
  and reset old request state after a serial-device reconnect.
- Maintained VITA packet counts per output stream, rejected malformed slow-data
  message indices and malformed header chunk lengths, rejected undersized RX
  AMBE output buffers, rejected short/non-128-sample waveform VITA input, and
  stopped parsing short/partial ThumbDV serial reads.
- Changed waveform output from a zero-valued sample-count timestamp to current
  UTC plus real-time fractional picoseconds, matching current Flex waveform
  clients and the radio's waveform input timestamp mode.
- Send waveform output through the same UDP socket and source port used for
  waveform input. The adopted helper announced port 5000 for input but replied
  from a separate unbound socket; current FlexRadio and FreeDV waveform clients
  use one announced socket bidirectionally.
- Added persisted, validated MYCALL/suffix/URCALL/RPT1/RPT2/message launch
  configuration and focused sanitizer-backed modem, lifecycle, queue, VITA, and
  process tests.
- Added constant-space waveform delivery telemetry: the helper aggregates 188
  RX or TX timestamp intervals before emitting one directional metric line.
  RX reports effective rate, VITA gaps, inferred 128-sample source deficits,
  turnaround, and queue depth; TX reports mic-stream rate/gaps plus one final
  partial report with null AMBE, PCM, send-failure, bounded-queue, and tail
  counters. No packet capture or per-packet production logging is enabled by
  this telemetry.
- Replaced the historical invalid waveform filter depth `2` with the
  current-client-compatible value `256`, while retaining the helper's
  128-sample VITA packet contract, and added an opt-in packet metadata sidecar
  for correlating raw RX captures with VITA counters, timestamps, and arrival
  time.
- Trimmed config-file line endings so blank lines are not transmitted as
  malformed API commands, and added an opt-in setup trace for correlating
  waveform commands with radio responses and assigned stream IDs.
- Build only the local helper executable; the historical Windows GUI,
  `.ssdr_waveform` package, IDE metadata, and binary artifacts are not vendored.
