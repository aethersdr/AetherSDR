# AetherSDR libmodem-core subset

This directory contains a deliberately small, internal subset of
`iontodirel/libmodem`, copied for AetherSDR's experimental receive-only
AX.25 packet decoder window.

Upstream: https://github.com/iontodirel/libmodem
Commit: `a8a387bc235230b4cefba64c86a5d5363677997c`
License: MIT, preserved in `LICENSE.libmodem` and in each copied source file.

Copied files:

- `bitstream.h`
- `bitstream.cpp`
- `demodulator.h`
- `demodulator.cpp`

Intentionally omitted:

- libmodem application and pipeline code
- audio backends: ALSA, WASAPI, CoreAudio, WAV, TCP audio
- data streams, serial/TCP KISS servers, PTT, TUI, JSON config
- Boost, fmt, ftxui, nlohmann_json, libsndfile, cxxopts
- upstream tests that require Dire Wolf, Python, scipy, or numpy
- full FX.25/IL2P Reed-Solomon support

Dependency removals:

- `bitstream.cpp` was trimmed to AX.25-only behavior and no longer includes
  `correct.h`.
- FX.25 and IL2P entry points are present only as empty stubs so copied adapter
  declarations continue to compile without libcorrect. AetherSDR Phase 1 does
  not call them.

Refresh notes:

1. Clone upstream libmodem.
2. Copy only the files above.
3. Re-apply the AX.25-only trim to remove libcorrect-backed FX.25/IL2P code.
4. Build `aether_libmodem_core` and run the AX.25 shim tests.

Manual RX/TX test notes:

- Open **View > AetherModem...**. The action is listed immediately under
  **Propagation Conditions**.
- For HF packet/APRS, choose **300 baud HF**, tune an appropriate frequency,
  and use SSB/DIG receive mode with the audio passband covering 1600/1800 Hz.
- For VHF packet/APRS, choose **1200 baud VHF** and use the normal FM packet
  receive path.
- Enable decode and try both Normal and Inverted tone polarity. USB/LSB receive
  paths can invert tone sense.
- Confirm accepted AX.25 UI frames appear in the scrolling log.
- For the experimental 300 baud HF TX path, enter either raw payload text or
  full `SRC>DST,path:payload` monitor syntax in the transmit field. The window
  packetizes it as an AX.25 UI frame and feeds AFSK into the app-owned DAX TX
  stream.
- For diagnostics in the normal AetherSDR logfile, enable the
  **AetherModem** logging category (`aether.ax25`) from the app's logging
  controls.

This is an experimental first modem window, not a production packet TNC.
