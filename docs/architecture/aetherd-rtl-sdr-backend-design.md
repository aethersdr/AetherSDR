# RTL-SDR backend

**Status:** Experimental implementation for issue #4797
**Family:** `rtl`
**Transport:** local USB through `librtlsdr`

## Scope

The RTL-SDR backend is a receive-only `IRadioBackend` implementation. It
provides one panadapter and one slice, discovers local USB dongles, converts
unsigned 8-bit IQ on a worker thread, and emits spectrum, waterfall, and
24 kHz stereo audio through the existing backend seam.

`librtlsdr` and its float-precision FFTW dependency are optional. The
`ENABLE_RTL` option permits an explicit opt-out; otherwise CMake defines
`AETHER_BACKEND_RTL` only when both libraries are present. Discovery compiles
as an unavailable stub when the backend is disabled so normal builds retain
the same source graph.

## Components

- `RtlSdrDiscovery` enumerates USB devices off the GUI thread and feeds the
  existing discovered-radio list. Duplicate or blank USB serials fall back to
  a stable scan index for that connection attempt.
- `RtlSdrBackend` owns radio state, validates requests, translates seam verbs,
  and advertises a strictly receive-only capability set.
- `RtlSdrWorker` owns the opened device handle. It runs
  `rtlsdr_read_async`, performs control transfers only between async-read
  sessions, and closes the handle only after the read loop exits.
- `RtlSdrDdc` performs frequency translation, decimation, basic analog-mode
  demodulation, FFTW spectrum generation, display pacing, and slice audio
  mute/gain/pan.

There is no intermediate ring buffer: each librtlsdr callback converts and
processes its block on the reader thread. This bounds memory use but means DSP
must keep pace with USB input.

## Connection and state

RTL-SDR is discovered as a local device; it is deliberately not accepted by
the automation bridge's `connect ip` verb. A future USB-specific bridge verb
should identify a dongle by enumerated serial or index.

The client owns tuning, passband, sample rate, and RF gain. Universal values
use `RestoredRadioState` fields; tuner gain is stored under the schema's
`rfGain` extension domain. PPM and explicit direct-sampling overrides are
runtime extension controls and are not persisted by the current generic state
schema. Direct sampling is selected automatically when tuning below 24 MHz.

Supported sample-rate detents are 225001, 250000, 300000, 1000000,
1536000, 1843200, 2000000, 2400000, and 3000000 Hz. Requests snap to the
nearest detent, and the resulting span is echoed through
`panCenterBandwidthChanged`.

## Threading and shutdown

The GUI thread never performs a USB control transfer after streaming starts.
A control setter stores its latest value atomically and cancels the async read.
The worker consumes the retune flag before applying pending values, preserving
any request that arrives during that application window, then restarts the
read.

Shutdown repeatedly cancels while waiting for the reader. If a broken USB
stack does not acknowledge cancellation within the bounded wait, the backend
detaches the worker until it finishes rather than destroying a running
`QThread` or closing a device handle still in use.

## Current limitations

- No transmit, radio-side meters, memories, or multi-slice operation.
- Demodulation is intentionally basic; passband values are state/UI controls,
  not yet a sharp selectable DSP filter.
- PPM, direct-sampling override, and offset tuning are extension verbs rather
  than first-class controls.
- Live hardware validation depends on an attached RTL-SDR dongle. The unit
  test covers seam declarations, state validation/reset, discovery build
  availability, DDC output, and sample-rate snapping without opening hardware.

## Dependencies

- Arch: `rtl-sdr`
- Debian/Ubuntu: `librtlsdr-dev`
- Fedora: `rtl-sdr-devel`
- macOS: `librtlsdr`
- FFTW float library and pkg-config module: `fftw3f`

Windows and AppImage builds currently compile the feature out unless their
dependency bundles supply `librtlsdr` through the same CMake contract.
