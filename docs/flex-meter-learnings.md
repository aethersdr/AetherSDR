# FlexRadio Meter Learnings

This document captures the working conclusions from MasterSDR compression
meter debugging across FLEX-8000 series radios and a FLEX-6600. It is meant to
record capture-backed behavior, not API guesses.

Related implementation context lives in `docs/tx-audio-signal-path.md`.

## Core Rules

- Match meters by `source` and `name`, not by numeric ID. Flex meter IDs are
  assigned per session and differ by platform.
- Compression TX-chain meters are repeated per active slice on multi-slice
  radios. Track `COMPPEAK`, `AFTEREQ`, and `SC_MIC` by active slice. Some
  radios expose distinct TX waveform `sourceIndex` values, while 8000-series
  captures can repeat `TX- num=0` blocks after each `SLC` slice block.
- Do not synthesize compression when required radio meters are unavailable. A
  missing compression input should make the compression meter unavailable, not
  approximate a value from local mic audio.
- This work preserves the existing Phone/CW gauge presentation. Availability is
  tracked in `MeterModel`; the UI still receives `0 dB` when no valid
  compression pair is available, rather than adding new N/A rendering.
- `COMPPEAK` is a dBFS signal-level tap near the speech processor/clipper. It
  is not, by itself, the SmartSDR compression display value.
- The MasterSDR P/CW compression gauge is reversed: `0 dB` means no visible
  compression and `-25 dB` means full-scale/heavy compression.

## Compression Formulas

### FLEX-8000 Series

The 8000-series model validated against SmartSDR uses `AFTEREQ` and
`COMPPEAK`:

```text
compression_lift_db = max(0, COMPPEAK - AFTEREQ)
display_db = -clamp(compression_lift_db, 0, 25)
```

`AFTEREQ` is the processor input reference after the TX equalizer. `COMPPEAK`
is the processor/clipper-stage tap. Both meters advertise `20 fps`, so the two
inputs are naturally well matched for a derived compression display.

If either meter is missing, MasterSDR should not show a derived compression
value.

### FLEX-6600 / 6000-Series Evidence

The captured FLEX-6600 manifest does not expose `AFTEREQ`. The relevant TX
meters observed were:

- `CODEC` as TX meter `21`
- `SC_MIC` as TX meter `22`
- `COMPPEAK` as TX meter `23`

The strongest candidate formula is:

```text
compression_lift_db = max(0, COMPPEAK - SC_MIC)
display_db = -clamp(compression_lift_db, 0, 25)
```

Why this is the current model:

- Raw `COMPPEAK` does not behave like the SmartSDR compression display. In
  strong RF-output rows it often goes positive and would clamp to `0 dB`, just
  when compression should be visible.
- The opposite direction, `SC_MIC - COMPPEAK`, was effectively dead in the
  user captures and stayed at `0 dB`.
- `CODEC` is a plausible alternate reference, but in the 6600 captures it was
  consistently about 3 dB lower than `SC_MIC`, which would make
  `COMPPEAK - CODEC` read slightly heavier. `SC_MIC` is the better named
  pre-processor mic-chain reference.

The remaining engineering caution is timing. On the 6600, `SC_MIC` advertises
`10 fps` while `COMPPEAK` advertises `20 fps`. MasterSDR guards the derived
6600 compression value by requiring the reference sample and `COMPPEAK` sample
to be close in time before marking the compression meter available.

## Multi-Slice Meter Resolution

FLEX radios can repeat the TX waveform meter block once per active slice. In a
captured FLEX-6600 four-slice manifest, `SC_MIC` / `COMPPEAK` appeared as
`22/23`, `44/45`, `66/67`, and `88/89`. Those numeric IDs are not stable API
contracts; they are manifest slots for that session.

MasterSDR stores compression meter IDs by explicit TX waveform `sourceIndex`
when the manifest provides one. In 8000-series captures where repeated TX
blocks all report `num=0`, MasterSDR keys those block-local meters to the most
recent `SLC` slice context from the manifest. The formula remains
family-specific by available meter name: use active-slice `AFTEREQ` when
present, otherwise active-slice `SC_MIC`, paired with active-slice `COMPPEAK`.

The implementation intentionally derives a slice/source key and then looks up
the manifest IDs for that key. It does not calculate the final meter IDs
directly.

| Radio family / manifest shape | Slice/source resolution | Reference meter | Compression meter | Gauge formula |
|---|---|---|---|---|
| FLEX-6600 / 6000-style explicit TX source | `txBase = min(TX sourceIndex >= 8) - min(SLC sourceIndex)`, then `activeTxSource = txBase + activeTxSlice` | `SC_MIC` at `activeTxSource` | `COMPPEAK` at `activeTxSource` | `-clamp(max(0, COMPPEAK - SC_MIC), 0, 25)` |
| FLEX-8000-style explicit TX source | Same explicit TX-source lookup when the manifest provides per-slice TX source indices | `AFTEREQ` at `activeTxSource` | `COMPPEAK` at `activeTxSource` | `-clamp(max(0, COMPPEAK - AFTEREQ), 0, 25)` |
| FLEX-8000-style repeated `TX- num=0` blocks | Use the most recent `SLC` source index as manifest context, then resolve by `activeTxSlice` at runtime | `AFTEREQ` mapped to `activeTxSlice` | `COMPPEAK` mapped to `activeTxSlice` | `-clamp(max(0, COMPPEAK - AFTEREQ), 0, 25)` |

Issue #2040 describes the 6600 failure mode this avoids: the old scalar
indexes were last-match-wins, so a multi-slice 6600 session could bind to the
highest-numbered slice's `SC_MIC` / `COMPPEAK` pair while the VITA meter stream
was publishing values for a different active TX slice.

## Diagnostic Logging

Enable `Meters` in Help > Support to capture compression diagnostics. The log
emits a `compression meter map` row for each discovered `COMPPEAK`, `AFTEREQ`,
or `SC_MIC` meter, then throttles live `compression summary` rows to twice per
second unless the state changes. The summary includes active TX slice,
waveform `sourceIndex`, selected reference meter, reference and `COMPPEAK`
values, sample skew, computed lift, displayed gauge value, and availability
reason.

## Meter FPS Comparison

| Meter | 8000 Series FPS | FLEX-6600 FPS | Notes |
|---|---:|---:|---|
| `MICPEAK` | 40 | 40 | Codec hardware mic peak |
| `MIC` | 20 | 20 | Codec hardware mic average |
| `HWALC` | 20 | 20 | External Hardware ALC RCA voltage — zero without an external HWALC connection.  Used by SliceTroubleshootingDialog telemetry only; do **not** drive UI ALC gauges from this meter. |
| `ALC` | 20 | 20 | Post-software-ALC SSB peak (dBFS).  This is the meter that drives the in-app ALC gauges (mirrored across Phone and CW panels). |
| `FWDPWR` | 20 | 20 | Forward RF power |
| `REFPWR` | 20 | 20 | Reflected RF power |
| `SWR` | 20 | 20 | RF SWR |
| `PATEMP` | 0 | 0 | PA temperature |
| `CODEC` | 10 | 10 | TX codec/mic-chain level |
| `TXAGC` | 10 | Not seen | Present in 8000 captures |
| `SC_MIC` | 10 | 10 | TX mic-chain tap; 6600 compression reference when `AFTEREQ` is absent |
| `AFTEREQ` | 20 | Not present | 8000 compression reference |
| `COMPPEAK` | 20 | 20 | Processor/clipper-stage tap |
| `SC_FILT_1` | 20 | 10 | Post TX filter 1 |
| `ALC` | 10 | 10 | SW ALC / SSB peak |
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

The P/CW level meter is intentionally separate from the compression derivation.
The FPS-testing experiment that drove voice/CW level strictly from `SC_MIC` was
backed out. Current behavior keeps the existing UI path: hardware mic meters
for hardware inputs, and client-side PC metering for PC audio. The Phone/CW
level gauge and `HGauge`/`MeterSmoother` dampening are not changed by the
compression work.

`SC_MIC` is used as a 6000-series compression reference only when `AFTEREQ` is
not present. It is not used as a strict replacement for the P/CW level meter.

## Two-Tone Tune Observations

Two-tone tune is useful for producing deterministic RF output and checking the
radio-side signal path. During two-tone testing, the transmitted RF can contain
only the radio-generated two-tone audio even while local PC mic meters move.
For this reason, MasterSDR should not use local mic activity to drive the
compression gauge.

The current implementation does not add special UI behavior for two-tone tune;
this note records why future work should be careful not to derive compression
from local mic activity during radio-generated test tones.

## Open Questions

- Confirm the 6600 scale against SmartSDR with a capture that includes the
  visible SmartSDR Comp reading at the same time as `SC_MIC` and `COMPPEAK`.
