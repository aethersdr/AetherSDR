# Consolidated audio sink factory

> Status: **in progress** — foundation landed (pure negotiator + golden tests).
> Tracking issue: [#3306](https://github.com/aethersdr/AetherSDR/issues/3306).
> Companion doc: [`audio-pipeline.md`](audio-pipeline.md).

## Why

Every audio sink and source in AetherSDR currently answers two questions on its
own:

1. **"What rate / sample format does this device want, and how do I get my
   24 kHz canonical audio onto it?"** — reimplemented ~9 times with ~6 divergent
   fallback ladders and per-OS `#ifdef` branches that have drifted apart. This
   is the root of a recurring class of platform bugs (44.1k-only devices
   silently failing on some sinks, WASAPI Float32-only devices rejecting Int16,
   macOS Bluetooth-HFP mics delivering silence).
2. **"Which output device do I play to?"** — every *new* sink has historically
   forgotten to follow the user's selected output device and instead opened the
   system default. Each time, a reactive PR retrofits device-following:
   CW sidetone ([#2899]), the Aetherial/Pudu monitor + QSO playback ([#3378],
   closing [#3361]). This is the "**uncoupling**" the operators hit: change the
   output device and CW / RADE / the Aetherial-Audio Pudu recorder keep playing
   on the old endpoint.

The factory makes both answers a **single shared responsibility** so a new sink
inherits correct behaviour by construction instead of re-introducing the class.

## Architecture

```
                        ┌─────────────────────────────────────────────┐
                        │  AudioFormatNegotiator   (PURE, Qt6::Core)   │
                        │  negotiate(os, dir, DeviceCaps, policy)      │
                        │  buildLadder(...) · resamplerKindFor(...)    │
                        └───────────────▲─────────────────────────────┘
                                        │ injected DeviceCaps (no device I/O)
                        ┌───────────────┴─────────────────────────────┐
                        │  AudioDeviceNegotiator   (Qt Multimedia)     │
                        │  probe(QAudioDevice) -> DeviceCaps           │
                        │  negotiate(...) -> {QAudioFormat, kind, …}   │
                        │  formatLadder(...) for try-at-open backends  │
                        └───────────────▲─────────────────────────────┘
                                        │
                        ┌───────────────┴─────────────────────────────┐
                        │  AudioOutputRouter   (device ownership)      │
                        │  selected output/input device + change feed  │
                        │  registers sinks; restarts them on change    │
                        └───────────────▲─────────────────────────────┘
            RX speaker · CW sidetone · Pudu monitor · Quindar · QSO playback ·
            PC mic (TX) · TCI TX · RADE   (all consume the three layers above)
```

### Layer 1 — `AudioFormatNegotiator` (pure policy) ✅ landed

A dependency-free function (links only `Qt6::Core`) over an **injected**
`DeviceCaps` snapshot, with the **target OS as a parameter, not an `#ifdef`**.
That single design choice is what makes the whole policy testable headless: one
binary built on any CI runner exercises the Windows, macOS and Linux ladders
against every device shape (see `tests/audio_format_negotiation_test.cpp`). This
is the reason the historical bugs escaped CI — they were compiled behind
`Q_OS_*` and a Linux runner could only ever see the Linux path.

Key types (`src/core/AudioFormatNegotiator.h`):

- `TargetOs { Windows, MacOS, Linux }` — data, not `#ifdef`.
- `Direction { Output, Input }`.
- `SampleFmt { Int16, Float32 }` — a Qt-Multimedia-free mirror of
  `QAudioFormat::SampleFormat` (the live wrapper maps between them).
- `ResamplerKind { None, PreservePan, MonoCollapse }` and
  `ResamplerPolicy { RegenerateAtRate, PreservePan, MonoCollapse }`.
- `DeviceCaps` — `supportedRates`, `supportedFormats`, `channels`,
  `isBluetoothHfp`, `isFormatSupportedReliable`, `preferredRate/Format`.
- `buildLadder()` / `negotiate()` / `resamplerKindFor()`.

The ladder, in one place, owns:

| Concern | Rule | History |
|---|---|---|
| Windows RX preferred rate | force **48 kHz** + r8brain (24 k via WASAPI resampler artifacts under NR) | [#2120] / PR [#2123] |
| macOS RX preferred rate | **48 kHz** so A2DP devices stay off HFP/telephony | [#1705] |
| Linux RX preferred rate | native **24 kHz** (no WASAPI in path) — deliberate divergence | #3306 |
| 44.1 kHz fallback | **every** sink, every OS (was missing on RX/Quindar/QSO) | [#3385] |
| Int16 ↔ Float32 | both tried per rate; Float-first output, Int16-first input | [#2669] / [#1090] |
| `preferredFormat()` catch-all | final rung for Float32-only virtual drivers / WASAPI | [#3231] |
| macOS mic preferred-rate-first | avoids silent 48 k open on 16 k-native mics | PR [#2930] |
| macOS Bluetooth-HFP mic | native 8/16/24 k first, never forced to 48 k | [#2615] |
| Windows probe-at-open | skip unreliable `isFormatSupported`, try at open | [#2120] / [#2929] |

### Layer 2 — `AudioDeviceNegotiator` (live wrapper, Qt Multimedia) — next

Thin, the **only** platform-specific code. `probe(QAudioDevice, Direction)`
builds a `DeviceCaps` (rate probing via `isFormatSupported`, `preferredFormat`,
per-OS `isFormatSupportedReliable`, and — fed by the existing CoreAudio HAL
detection — `isBluetoothHfp`). Returns either a resolved `QAudioFormat` +
`ResamplerKind` for reliable backends, or the ordered `QAudioFormat` **ladder**
for try-at-open backends (Windows), so the caller walks it against
`QAudioSink::start()` exactly as today.

### Layer 3 — `AudioOutputRouter` (device ownership) — next

`AudioEngine` already owns the persisted device IDs (`AudioOutputDeviceId` /
`AudioInputDeviceId`) and is the only thing that calls `setOutputDevice()` /
`setInputDevice()`. Today it emits `outputDeviceChanged()` / `inputDeviceChanged()`
but **only `MainWindow::updatePcAudioTooltip()` listens** — the aux sinks do not,
which is the uncoupling. The router turns this into one ownership point:

- **single source of truth** for the resolved selected output/input device
  (and "follow the system default" when none is pinned);
- sinks **register** themselves; on a user device change *or* an OS-default
  change ([#2899] showed both must be handled) the router restarts the
  registered following sinks — no sink calls `QMediaDevices::defaultAudioOutput()`
  on its own ever again;
- one place to enforce the safety invariants below.

## Invariants the factory must preserve (regression guards)

These are load-bearing; each maps to a closed bug. The golden matrix encodes the
rate ones; the others are enforced in the router/wrapper.

1. **Two stereo resampler strategies stay distinct.** `PreservePan` (dual
   independent L/R r8brain) for RX speaker + QSO playback; `MonoCollapse`
   (`processStereoToStereo`) for TCI DAX TX + RADE. Conflating them collapsed
   VITA-49 pan ([#2403] / PR [#2459]).
2. **Resampler instances are per-stream / per-channel**, never shared — r8brain
   is stateful; a shared instance bled Slice A audio into Slice B ([#1815]).
3. **Unmute RX on every bail path.** Any sink that mutes live RX while it owns
   the device must `muteRxRequested(false)` on *every* early-return / failure,
   or RX stays dead until restart ([#3230]).
4. **DAX shared-ownership refcount.** The `dax_rx` stream is torn down only when
   neither a TCI client nor the DAX bridge nor RADE still needs it
   ([#2633] / [#2886]).
5. **No global WASAPI buffer cap.** A hard 100 ms ring cap for all device types
   was rejected; latency must derive from the existing `rxBufferCapMs` knob
   ([#3194] / [#3266]).
6. **Land incrementally, one sink per PR, with soak time** — the maintainer's
   explicit rule for audio-stack changes ([#3194] thread), matching #3306's
   "helper first, migrate sinks incrementally."

## Migration plan (one mergeable PR per step)

1. **Foundation** ✅ — `AudioFormatNegotiator` + golden matrix
   (`audio_format_negotiation_test`). Pure addition, zero behaviour change.
2. **Live wrapper** — `AudioDeviceNegotiator`, with a unit smoke test against the
   default device. Still no sink uses it yet.
3. **RX speaker** onto the wrapper — behaviour identical for normal devices; the
   *only* visible change is that a 44.1k-only output now works instead of
   failing (the #3306 regression the foundation guards). Soak on Win/Mac/Linux.
4. **PC mic (TX)** onto the wrapper — preserves macOS preferred-first / BT-HFP /
   Windows probe-at-open.
5. **`AudioOutputRouter`** + migrate the three uncoupled sinks (CW sidetone,
   Pudu monitor, QSO playback) and Quindar onto it — closes the uncoupling
   class so a future sink can't re-open it.
6. **TCI TX** — drive the resampler from the client-negotiated rate instead of
   the hardcoded `Resampler(48000, 24000)` (`TciServer.cpp`).
7. **PipeWire DAX** — replace the hand-rolled linear interpolator with the shared
   `Resampler`, keeping the 48 kHz node pinning.

### Out of scope (radio/client-authoritative — pass the target in, don't choose)

DAX IQ `daxiq_rate`; the TCI client-requested RX rate. These are inputs to the
factory, not decisions it makes.

<!-- references -->
[#1090]: https://github.com/aethersdr/AetherSDR/pull/1090
[#1705]: https://github.com/aethersdr/AetherSDR/issues/1705
[#1815]: https://github.com/aethersdr/AetherSDR/pull/1815
[#2120]: https://github.com/aethersdr/AetherSDR/issues/2120
[#2123]: https://github.com/aethersdr/AetherSDR/pull/2123
[#2403]: https://github.com/aethersdr/AetherSDR/issues/2403
[#2459]: https://github.com/aethersdr/AetherSDR/pull/2459
[#2615]: https://github.com/aethersdr/AetherSDR/pull/2615
[#2633]: https://github.com/aethersdr/AetherSDR/pull/2633
[#2669]: https://github.com/aethersdr/AetherSDR/pull/2669
[#2886]: https://github.com/aethersdr/AetherSDR/issues/2886
[#2899]: https://github.com/aethersdr/AetherSDR/pull/2899
[#2929]: https://github.com/aethersdr/AetherSDR/issues/2929
[#2930]: https://github.com/aethersdr/AetherSDR/pull/2930
[#3193]: https://github.com/aethersdr/AetherSDR/issues/3193
[#3194]: https://github.com/aethersdr/AetherSDR/pull/3194
[#3231]: https://github.com/aethersdr/AetherSDR/pull/3231
[#3230]: https://github.com/aethersdr/AetherSDR/pull/3230
[#3266]: https://github.com/aethersdr/AetherSDR/issues/3266
[#3361]: https://github.com/aethersdr/AetherSDR/issues/3361
[#3378]: https://github.com/aethersdr/AetherSDR/pull/3378
[#3385]: https://github.com/aethersdr/AetherSDR/issues/3385
