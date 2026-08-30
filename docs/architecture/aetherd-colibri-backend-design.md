# aetherd — ColibriNANO backend design

Status: implemented (first cut, RX-only)
Family id: `colibri`
Protocol authority: the ColibriNANO USB receiver is driven exclusively through
Expert Electronics' own `colibrinano_lib` dynamic library. The authority for
every call signature, sample-rate table, preamp range and tuning bound in this
backend is the library's published C API header set (`common.h`, `LibLoader.h`
in <https://github.com/maksimus1210/ColibriNANO_lib>), which is the vendor's
reference client for the device. No USB traffic is spoken directly: the DLL owns
the FTDI transport, and this backend never reaches below its API.

## What the device is

A direct-sampling USB receiver: 122.88 MHz ADC, one DDC, IQ delivered to a
process callback as normalized `float` pairs at one of nine rates
(48 k … 3.072 MHz). Controls: tuning frequency (`setFrequency`, Hz, uint32),
preamp/attenuator (`setPream`, −31.5…+6 dB), sample rate (chosen at `start`).
RX only — there is no transmitter, no keying line, and no telemetry beyond an
ADC-overload flag piggybacked on every IQ callback.

## Shape of the backend

The Hermes-Lite 2 backend is the template: raw IQ in, host-side DSP chain below
the seam (RFC §5.5 — "DSP location is invisible here"). The differences all
subtract:

- ONE receiver, fixed. `maxSlices = maxPanadapters = 1`; `createPanadapter()`
  stays `false`. The pan id is `colibri-0`, the slice id is 0, and the four-way
  index map HL2 needs collapses to constants.
- RX-only: `canTransmit=false`, `hostModulates=false`, `setKeying()` a no-op.
  The engine TX guard (RFC §6) sits above the seam and refuses keying on the
  capability alone.
- The pan SPAN is the sample rate, exactly as on the HL2 — but rate changes
  restart the stream (`stop` → `start(newRate)`) instead of poking a register.
- No wire protocol object: `ColibriDevice` wraps the DLL and stands where
  `MetisClient` stands, on the same one-I/O-thread model.

## Threading

Identical discipline to `Hl2Backend`, with one extra hop the DLL forces on us:

- The backend owns a `QThread` (`colibri-io`). `ColibriDevice` and
  `ColibriRxDsp` are created parentless and moved onto it.
- The DLL invokes the RX callback from ITS OWN internal thread. That thread is
  not ours and `ColibriRxDsp` is not thread-safe against the control path, so
  the callback only copies the block and emits a queued signal
  (`iqBlockReady`) whose receiver context is the device object — the block is
  therefore processed on the I/O thread. This is the one place the Colibri path
  is one queue deeper than HL2's DirectConnection fan-out, and it is not
  optional.
- Counters the GUI thread reads (`linkStats()`, `healthSnapshot()`) are
  atomics in the device, never reads into I/O-thread state.
- Control verbs travel GUI → I/O via queued `invokeMethod`, blocking only for
  the rate-change reconfigure (same order rule as HL2: the DSP must expect the
  new rate before the stream delivers it — here trivially satisfied because
  `stop` precedes the reconfigure and `start` follows it, on one event loop).

## IQ handedness

`ColibriRxDsp` carries `Config::wireAnalytic`. The HPSDR wire is the conjugate
of the analytic convention and WDSP's RXA selects the opposite sign to its
passband bounds (both measured — see `Hl2RxDsp::processIqBlock`), so:

- `wireAnalytic=true` (default, expected for this library): spectrum takes the
  wire RAW, demodulator takes the CONJUGATE. Conjugating an analytic wire
  reproduces the HPSDR wire byte-for-byte, so the demod path and the
  slice-shift sign (`shift = slice − NCO`) are inherited from HL2 unchanged.
- `wireAnalytic=false`: the HL2 arrangement verbatim (spectrum conjugated,
  demod raw).

The flag is persisted in `ColibriSettings` so live calibration against a known
signal is one settings flip, not a rebuild.

## Persistence

`ColibriSettings`, one root key `"Colibri"` (Principle V): remembered span,
optional DLL path override, `wireAnalytic`. Per-radio operating state (RFC
#4603) declares `Tuning | Passband | SpanRate | RfGain`; the preamp rides the
`rfGain` extension sub-object. The device reports no serial through the API, so
identity is `colibrinano-<index>`.

## Discovery

`ColibriDiscovery` polls `ColibriLib::deviceCount()` on a timer and emits the
same `RadioInfo` signals the Flex and HL2 sweeps emit (`family="colibri"`,
`address=LocalHost` — synthetic, never dialed, per the sim precedent). Polling
is suspended while a device is open: enumerating the FTDI bus under a running
stream is not documented safe by the authority, so we do not do it.

## Boundary notes

- The DLL header vocabulary appears only under `src/core/backends/colibri/`
  (EB3).
- `ownsRxAudio()` is true; `MainWindow::backendFeedsEngineDirectly()` stays
  false — audio reaches the engine through the RadioModel relay, exactly like
  HL2 (see the #4537 comment block before changing either).
- `finalize()` is deliberately never called: the library wants it "before
  program close", but a static-destruction-order call into a DLL that owns
  live FTDI handles is a worse risk than letting process teardown reclaim it.
