# FlexRadio Meter Learnings

This document captures the working conclusions from AetherSDR compression
meter debugging across FLEX-6000 and FLEX-8000 family radios. It records the
behavior we are matching, and the parts we intentionally are not deriving.

Related implementation context lives in [`tx-audio-signal-path.md`](tx-audio-signal-path.md).

## Core Rules

- Match meters by `source` and `name`, not by numeric ID. Flex meter IDs are
  assigned per session and differ by platform.
- Use the active-slice TX `COMPPEAK` meter directly for compression across
  model families. Do not derive compression from `AFTEREQ`, `SC_MIC`, `CODEC`,
  local PC mic audio, or any other fallback source.
- Compression TX-chain meters are repeated per active slice on multi-slice
  radios. AetherSDR resolves `COMPPEAK` by active TX slice before accepting a
  sample.
- `MeterModel` stores compression as a positive radio-provided amount:
  `0 dB` means none, `25 dB` means full-scale/heavy compression.
- The Phone/CW compression gauge and the TX S-meter compression face are
  visually reversed: they display the stored `0..25 dB` amount as `0..-25 dB`.
- Compression visibility is a UI/radio-state decision, not a meter derivation.
  The P/CW compression gauge is fed with `0 dB` unless the radio is
  transmitting and the speech processor is enabled.
- `met_in_rx=1` means the radio is allowed to publish TX-chain/mic metering
  during receive. It does not mean RF is keyed, and neither do `slice tx=1` or
  TX stream `tx=1`; those identify the selected TX slice or stream direction.
- UI meters that represent active TX-chain output, including compression and
  software ALC, should be gated by the raw radio interlock state
  `TRANSMITTING` (`RadioModel::isRadioTransmitting()`), not by slice `tx=1`,
  `met_in_rx`, stream presence, or local optimistic MOX intent.

## Direct COMPPEAK Model

AetherSDR treats the active-slice `COMPPEAK` sample as the compression value
supplied by the radio. Adjacent TX audio meters are still valuable for
diagnostics, but they are not compression inputs.

```text
compression_db = clamp(COMPPEAK, 0, 25)
pcw_gauge_db = -compression_db
```

Earlier derivation attempts paired `COMPPEAK` with adjacent TX audio meters and
then mapped the result into the reversed `0..-25 dB` gauge range. That masked
strong rows where `COMPPEAK` moved positive, because a positive sample was
effectively displayed as no compression. The current model keeps the radio
sample positive in `MeterModel` and reverses only at the visible gauge.

If no active-slice `COMPPEAK` meter exists, or if the active `COMPPEAK` meter
has not produced data, `MeterModel` marks compression unavailable and keeps the
value at `0 dB`. It does not synthesize a value from local audio activity.

## Multi-Slice Meter Resolution

FLEX radios can expose one TX waveform meter block per active slice. In a
captured FLEX-6600 four-slice manifest, `COMPPEAK` appeared in repeated TX
blocks such as `23`, `45`, `67`, and `89`. Those numeric IDs are not stable API
contracts; they are manifest slots for that session.

FlexLib only treats `Meter.SourceIndex` as a slice ID when
`Meter.Source == Meter.SOURCE_SLICE` (`"SLC"`). It attaches those meters to
`Slice.Meters` with `FindSliceByIndex(m.SourceIndex)`. `TX-` waveform meters
remain radio-level meters; FlexLib does not attach them to a slice or promise
that their `num` field is a slice index.

AetherSDR associates new TX waveform meters with the preceding `SLC` slice
context in the observed ordered meter manifest. The Flex decoder preserves
first-appearance wire order when grouping a status message's fields; it never
sorts the definitions by meter ID. Existing meter identities retain their
association when re-announced, including unit or description changes. A removed
or repurposed ID loses its old routes and samples, and removal or a non-TX/non-SLC
block invalidates the context for subsequent new definitions.

Definitions are registered in either the slice map or the explicit TX
`sourceIndex` fallback map, never both. A contextual meter cannot become another
slice's fallback when that slice's own meter disappears. The fallback keeps the
legacy arithmetic `txBase = min(TX sourceIndex >= 8) - min(SLC sourceIndex)`
(or just the TX minimum when no SLC is present), then looks up
`txBase + activeTxSlice`. This is compatibility behavior for context-free
manifests, not a FlexLib guarantee. The observed SLC association
also handles the FLEX-8400M 4.2.18 shape observed on hardware: slice A's TX
block used `num=0`, while slice B's used `num=9`. Arithmetic over those values
cannot recover slice IDs, but their preceding SLC blocks identify slices 0 and
1 directly. This block-order association comes from captured firmware behavior;
FlexLib's SLC-only ownership rule does not itself guarantee TX block ordering.
Runtime updates resolve the active TX slice to that block's meter ID.

The implementation intentionally derives a slice/source key and then looks up
the manifest ID for that key. It does not calculate final meter IDs directly.

| Radio family / manifest shape | Slice/source resolution | Compression meter | Model value | UI gauge value |
|---|---|---|---|---|
| FLEX-6000/8000 ordered per-slice blocks | Use the most recent `SLC` source index as manifest context, then resolve by `activeTxSlice` at runtime | `COMPPEAK` mapped to `activeTxSlice` | `clamp(COMPPEAK, 0, 25)` | `-modelValue` |
| TX block without preceding SLC context | Fall back to the explicit TX `sourceIndex` map | `COMPPEAK` at the resolved TX source | `clamp(COMPPEAK, 0, 25)` | `-modelValue` |
| Exactly one implicit COMPPEAK meter and no explicit fallback entries | Follow the single modulator across receivers, including before a TX slice is selected | The unique implicit meter | Same conversion as above | Same conversion as above |

The ALC/COMPPEAK singleton fallback never volunteers an explicitly associated
meter for a different active slice. SC_MIC/SC_FILT_1/SC_FILT_2 require active-slice resolution
without that fallback so filter diagnostics cannot combine unrelated chains.
ALC resolves its conversion unit from the selected definition and resets to its
presentation floor, -20 dBFS, at startup, disconnect, TX-slice changes and active
meter removal. That reset is already in gauge units even for Percent meters.

Issue #2040 describes the 6600 failure mode this avoids: the old scalar meter
index approach was last-match-wins, so a multi-slice session could bind to the
highest-numbered slice's compression meter while the VITA meter stream was
publishing values for a different active TX slice.

## ALC vs. HWALC and RX Gating

Flex exposes at least two ALC-looking concepts:

- `HWALC` is the external Hardware ALC RCA voltage. It is useful telemetry for
  diagnostics, but it is normally zero unless the radio has an external HWALC
  connection. It should not drive the operator-facing ALC gauge.
- `ALC` is the post-software-ALC SSB peak in the TX chain. This is the meter
  users expect to see on the Phone/CW ALC gauges.

The important display rule is that `ALC` is still a TX-chain meter. On captured
FLEX-8000 sessions, with `met_in_rx=1` and PC mic streaming active while the
operator was not transmitting, the radio defined and updated TX-chain meters in
RX. Some of those quiescent values can sit near `0 dBFS`; if the UI blindly
maps them into the `-20..0 dBFS` ALC range, the gauge pins full red while the
radio is receiving.

AetherSDR therefore initializes and resets the P/CW ALC gauges to the empty
floor (`-20 dBFS`) and only displays `TX-/ALC` samples while the radio
interlock reports actual RF transmit (`state=TRANSMITTING`). This is the same
class of guard as the compression gauge: TX-chain samples may exist in RX, but
they are not an active TX warning until the radio is keyed.

Do not use these as an ALC display gate:

- `slice ... tx=1`: selected TX slice, not PTT.
- `stream ... type=dax_tx ... tx=1`: TX-direction stream, not PTT.
- `stream create type=remote_audio_tx`: mic stream setup for PC audio,
  VOX, and `met_in_rx`, not PTT.
- `met_in_rx=1`: request for TX-chain metering during receive, not PTT.
- local optimistic `TransmitModel::isTransmitting()` alone: useful for edge
  alignment, but it can be true before RF is confirmed.

## Diagnostic Logging

Enable `Meters` in Help > Support to capture compression diagnostics. The log
emits a `compression meter map` row for each discovered `COMPPEAK` meter, then
throttles live `compression summary` rows to twice per second unless the state
changes. The summary includes active TX slice, resolved TX waveform
`sourceIndex`, whether the implicit slice map was used, `COMPPEAK` meter ID,
the raw converted `COMPPEAK` sample, displayed model value, and availability.

`AFTEREQ` and `SC_MIC` are no longer logged as compression map participants
because they do not drive the compression display.

## Meter FPS Comparison

| Meter | 8000 Series FPS | FLEX-6600 FPS | Notes |
|---|---:|---:|---|
| `MICPEAK` | 40 | 40 | Codec hardware mic peak |
| `MIC` | 20 | 20 | Codec hardware mic average |
| `HWALC` | 20 | 20 | External Hardware ALC RCA voltage — zero without an external HWALC connection. Used by SliceTroubleshootingDialog telemetry only; do **not** drive UI ALC gauges from this meter. |
| `ALC` | 20 | 20 | Post-software-ALC SSB peak (dBFS). This is the meter that drives the in-app ALC gauges, but only while the radio interlock reports `TRANSMITTING`. |
| `FWDPWR` | 20 | 20 | Forward RF power |
| `REFPWR` | 20 | 20 | Reflected RF power |
| `SWR` | 20 | 20 | RF SWR |
| `PATEMP` | 0 | 0 | PA temperature |
| `CODEC` | 10 | 10 | TX codec/mic-chain level |
| `TXAGC` | 10 | Not seen | Present in 8000 captures |
| `SC_MIC` | 10 | 10 | TX mic-chain tap; diagnostic/level-path context, not a compression fallback |
| `AFTEREQ` | 20 | Not present | Post-EQ tap; diagnostic context, not a compression input |
| `COMPPEAK` | 20 | 20 | Radio-provided compression value used by AetherSDR |
| `SC_FILT_1` | 20 | 10 | Post TX filter 1 |
| `ALC` | 10 | 10 | SW ALC / SSB peak. Observed FPS varies by manifest; match by source/name and keep the UI gated on actual radio TX. |
| `RM_TX_AGC` | 10 | Not seen | Present in 8000 captures |
| `PRE_WAVE_AGC` | Not seen | 10 | Present in 6600 manifest |
| `SC_FILT_2` | 10 | 10 | Post TX filter 2 |
| `PRE_WAVE` | Not seen | 10 | Present in 6600 manifest |
| `B4RAMP` | 10 | 10 | Before ramp/key envelope |
| `AFRAMP` | 10 | 10 | After ramp/key envelope |
| `POST_P` | 10 | 10 | After processing, before power attenuation |
| `GAIN` | Not seen | 10 | Present in 6600 manifest |
| `ATTN_FPGA` | 10 | Not seen | Present in 8000 path documentation |

## P/CW Level Meter

The P/CW level meter is intentionally separate from compression. The
FPS-testing experiment that drove voice/CW level strictly from `SC_MIC` was
backed out. Current behavior keeps the existing UI path: hardware mic meters
for hardware inputs, and client-side PC metering for PC audio.

Changing compression to direct `COMPPEAK` does not alter the Phone/CW level
gauge, `HGauge`, `SMeterWidget`, or `MeterSmoother` level dampening behavior.

### Level Meter During Receive

The Radio Setup "Level Meter During Receive" toggle controls the radio-side
`met_in_rx` transmit setting. AetherSDR does not persist a separate local
preference for this control; it sends `transmit set met_in_rx=0/1` and then
mirrors the `met_in_rx` value reported by transmit status.

The `remote_audio_tx` stream should still be created for remote mic audio and
VOX-related audio transport, but creating that stream must not force
`met_in_rx` on. If AetherSDR sends `transmit set met_in_rx=1` during startup,
it overwrites the radio/profile-owned setting before the operator can observe
whether the previous toggle state persisted. The startup path should therefore
leave `met_in_rx` untouched and let the radio-reported value drive the meter
gate.

When `met_in_rx=false` and the radio is not transmitting, the P/CW level meter
display should be blanked/gated for both radio-provided and client-side mic
level paths. The gate is display behavior; it should not require stopping the
remote TX audio stream or adding local persistence state.

## Two-Tone Tune Observations

Two-tone tune is useful for producing deterministic RF output and checking the
radio-side signal path. During two-tone testing, the transmitted RF can contain
only the radio-generated two-tone audio even while local PC mic meters move.
For this reason, AetherSDR should not use local mic activity to drive the
compression gauge.

The current implementation does not add special UI behavior for two-tone tune;
this note records why future work should be careful not to derive compression
from local mic activity during radio-generated test tones.
