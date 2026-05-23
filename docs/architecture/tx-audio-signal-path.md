# FlexRadio TX Audio Signal Path

Documented from FLEX-8600 firmware v4.1.5 meter definitions.
All meters are source `TX-` unless noted. Units are dBFS unless noted.

For capture-backed 8000/6000-series compression formulas and FPS notes, see
[`flex-meter-learnings.md`](flex-meter-learnings.md).

For the complete current client-side audio ordering, sample formats, DAX/RADE
branches, metering taps, downmixing, and packetization details, see
[AetherSDR Audio Pipeline](audio-pipeline.md). This page focuses on how the
client-shaped PC mic voice stream relates to FlexRadio firmware meters.

## Client-side TX DSP (AetherialAudio, before the radio)

Since v0.8.15 AetherSDR applies its own client-side DSP chain to the
PC mic voice TX stream before VITA/Opus packetization. The radio receives
the already-shaped voice signal and treats it identically to any other
PC-mic input (enters at SC_MIC, meter 26).

DAX/TCI TX and RADE are intentionally not part of this voice strip.
DAX/TCI bypasses client voice DSP in `AudioEngine::feedDaxTxAudio()`.
RADE branches early from `AudioEngine::onTxAudioReady()` and bypasses
Opus voice TX.

As of v0.8.18 the full **AetherialAudio** chain is in place: seven
ordered stages, drag-to-reorder inside the CHAIN widget, single-click
to bypass, double-click to open the floating editor.

```
PC mic capture (QAudioSource)
  │
  ▼
┌───────────────────────────────────────────────────────────────────┐
│  CHAIN widget — drag-drop ordered TX DSP pipeline                  │
│                                                                     │
│  [GATE] → [EQ] → [DESS] → [COMP] → [TUBE] → [PUDU] → [VERB]        │
│                                                                     │
│  ClientGate    — downward expander / noise gate                     │
│  ClientEq      — 10-band parametric, 4 filter families, #1660      │
│  ClientDeEss   — sidechain-filtered de-esser                        │
│  ClientComp    — Pro-XL-style compressor + brickwall limiter, #1661 │
│  ClientTube    — dynamic tube saturator (3 models)                  │
│  ClientPudu    — exciter (Aphex-even / Behringer-odd harmonics)     │
│  ClientReverb  — Freeverb (disabled by default)                     │
│                                                                     │
│  Audio thread loads the packed chain order once per block and       │
│  dispatches each stage to its per-stage apply helper.               │
└─────────┬──────────────────────────────────────────────────────────┘
          │
          ▼  (meters: per-stage inputPeak/outputPeak/GR, ClientEq FFT
          │   tap, ClientPudu wet RMS, ClientReverb wet RMS)
          │
          ▼
     PC mic gain → Quindar → final limiter → meters/scopes
          │
          ▼
     Opus remote_audio_tx / VITA encode → UDP → radio
```

The chain is bypassed entirely on the DAX/TCI TX path so digital-mode
tones (WSJT-X, fldigi) reach the radio unshaped. PC mic voice TX runs
through the full chain.

Post-encoding, the radio sees this stream as any other PC-mic source
and runs it through the firmware TX chain below.

## Firmware Signal Flow

```
PC Mic Audio (Opus via remote_audio_tx) — arrives with client-side
                                          voice DSP already applied
  │
  ▼
┌─────────────────────────────┐
│  Opus Decode                │
│  (radio firmware)           │
└─────────┬───────────────────┘
          │
          ▼
┌─────────────────────────────┐
│  SC_MIC (meter 26)          │  ◄── "Signal strength of MIC output"
│  src=TX-, unit=dBFS         │      PC audio enters TX chain here
│  fps=10                     │
└─────────┬───────────────────┘
          │
          │   Hardware Mic (BAL/LINE/ACC)
          │     │
          │     ▼
          │   ┌───────────────────────┐
          │   │  CODEC ADC            │
          │   │  MICPEAK (meter 1)    │  ◄── "Signal strength of MIC output in CODEC"
          │   │    src=COD-, fps=40   │      Peak, hardware mic only
          │   │  MIC (meter 2)        │  ◄── "Average Signal strength of MIC output in CODEC"
          │   │    src=COD-, fps=20   │      Average, hardware mic only
          │   └───────┬───────────────┘
          │           │
          ▼           ▼
┌─────────────────────────────┐
│  Mic Level + Mic Boost      │  ◄── "transmit set mic_level=XX"
│  (radio-side gain)          │      "transmit set mic_boost=X"
└─────────┬───────────────────┘
          │
          ▼
┌─────────────────────────────┐
│  CODEC (meter 24)           │  ◄── "Signal strength of CODEC output"
│  src=TX-, fps=10            │      Combined output after mic gain
└─────────┬───────────────────┘
          │
          ▼
┌─────────────────────────────┐
│  TX AGC (input)             │
│  TXAGC (meter 25)           │  ◄── "Signal strength post AGC/FIXED GAIN"
│  src=TX-, fps=10            │
└─────────┬───────────────────┘
          │
          ▼
┌─────────────────────────────┐
│  Equalizer (8-band)         │  ◄── "eq txsc" command
│  AFTEREQ (meter 27)         │  ◄── "Signal strength after the EQ"
│  src=TX-, fps=20            │      Compression reference tap
└─────────┬───────────────────┘
          │
          ▼
┌─────────────────────────────┐
│  Speech Processor           │  ◄── "transmit set speech_processor_enable/level"
│  (Compander + Clipper)      │
│  COMPPEAK (meter 28)        │  ◄── "Signal strength before CLIPPER (Compression)"
│  src=TX-, fps=20            │      Paired with AFTEREQ for compression gauge
└─────────┬───────────────────┘
          │
          ▼
┌─────────────────────────────┐
│  TX Filter 1                │  ◄── "transmit set lo=XX hi=XX"
│  SC_FILT_1 (meter 29)       │  ◄── "Signal strength after Filter 1"
│  src=TX-, fps=20            │
└─────────┬───────────────────┘
          │
          ▼
┌─────────────────────────────┐
│  SW ALC (SSB Peak)          │
│  ALC (meter 30)             │  ◄── "Signal strength after SW ALC (SSB Peak)"
│  src=TX-, fps=10            │      Used for P/CW applet ALC indicator
└─────────┬───────────────────┘
          │
          ▼
┌─────────────────────────────┐
│  RM TX AGC                  │  ◄── Remote TX AGC (PC audio level normalization?)
│  RM_TX_AGC (meter 31)       │  ◄── "Signal strength after RM TX AGC"
│  src=TX-, fps=10            │
└─────────┬───────────────────┘
          │
          ▼
┌─────────────────────────────┐
│  TX Filter 2                │
│  SC_FILT_2 (meter 32)       │  ◄── "Signal strength after Filter 2"
│  src=TX-, fps=10            │
└─────────┬───────────────────┘
          │
          ▼
┌─────────────────────────────┐
│  TX AGC (final)             │
│  TX_AGC (meter 33)          │  ◄── "Signal strength after TX AGC"
│  src=TX-, fps=10            │
└─────────┬───────────────────┘
          │
          ▼
┌─────────────────────────────┐
│  Ramp (key shaping)         │  ◄── CW/digital key envelope
│  B4RAMP (meter 34)          │  ◄── "Signal strength before the ramp"
│  src=TX-, fps=10            │
│  AFRAMP (meter 35)          │  ◄── "Signal strength after the ramp"
│  src=TX-, fps=10            │
└─────────┬───────────────────┘
          │
          ▼
┌─────────────────────────────┐
│  Power Control / Atten      │  ◄── "transmit set rfpower=XX"
│  POST_P (meter 36)          │  ◄── "After all processing, before power attenuation"
│  src=TX-, fps=10            │
│  ATTN_FPGA (meter 37)       │  ◄── "After Fine Tune, HW ALC, SWR Foldback (FPGA)"
│  src=TX-, fps=10            │
└─────────┬───────────────────┘
          │
          ▼
┌─────────────────────────────┐
│  PA (Power Amplifier)       │
│  FWDPWR (meter 8)           │  ◄── "RF Power Forward" (dBm → watts)
│  src=TX-, fps=20            │
│  REFPWR (meter 9)           │  ◄── "RF Power Reflected" (dBm)
│  src=TX-, fps=20            │
│  SWR (meter 10)             │  ◄── "RF SWR"
│  src=TX-, fps=20            │
│  PATEMP (meter 11)          │  ◄── "PA Temperature" (degC)
│  src=TX-, fps=0             │
│  HWALC (meter 3)            │  ◄── "Voltage at Hardware ALC RCA Plug" (dBFS)
│  src=TX-, fps=20            │      Permanently 0 without an external HWALC
│  ALC   (meter 33)           │      connection.  Telemetry-only consumer.
│  src=TX-, fps=20            │  ◄── "Post-software-ALC SSB peak" (dBFS).
│                             │      Drives the Phone + CW panel ALC gauges.
└─────────┬───────────────────┘
          │
          ▼
        ANT1/ANT2 → Antenna (or external amplifier/tuner)
```

## Meter Summary Table

| ID | Source | Name | Unit | FPS | Description | Used in AetherSDR |
|----|--------|------|------|-----|-------------|-------------------|
| 1 | COD- | MICPEAK | dBFS | 40 | Hardware mic peak level | P/CW mic gauge (BAL/LINE/ACC) |
| 2 | COD- | MIC | dBFS | 20 | Hardware mic average level | P/CW mic gauge (BAL/LINE/ACC) |
| 3 | TX- | HWALC | dBFS | 20 | External HWALC RCA voltage (zero without external connection) | SliceTroubleshootingDialog telemetry only |
| 33 | TX- | ALC | dBFS | 20 | Post-software-ALC SSB peak | P/CW ALC gauge (Phone + CW panels, mirrored) |
| 8 | TX- | FWDPWR | dBm | 20 | Forward power | TX applet, S-Meter power, Tuner |
| 9 | TX- | REFPWR | dBm | 20 | Reflected power | — |
| 10 | TX- | SWR | SWR | 20 | Standing wave ratio | TX applet, Tuner |
| 11 | TX- | PATEMP | degC | 0 | PA temperature | Status bar |
| 24 | TX- | CODEC | dBFS | 10 | CODEC output (post mic gain) | — |
| 25 | TX- | TXAGC | dBFS | 10 | Post AGC/fixed gain | — |
| 26 | TX- | SC_MIC | dBFS | 10 | MIC output (PC audio entry point) | P/CW mic gauge (PC mic); 6000-series compression reference |
| 27 | TX- | AFTEREQ | dBFS | 20 | Post equalizer / processor input reference | 8000-series compression derivation |
| 28 | TX- | COMPPEAK | dBFS | 20 | Processor/clipper-stage level tap | P/CW compression derivation |
| 29 | TX- | SC_FILT_1 | dBFS | 20 | Post TX filter 1 | — |
| 30 | TX- | ALC | dBFS | 10 | Post SW ALC (SSB peak) | P/CW ALC indicator |
| 31 | TX- | RM_TX_AGC | dBFS | 10 | Post remote TX AGC | — |
| 32 | TX- | SC_FILT_2 | dBFS | 10 | Post TX filter 2 | — |
| 33 | TX- | TX_AGC | dBFS | 10 | Post final TX AGC | — |
| 34 | TX- | B4RAMP | dBFS | 10 | Before key ramp | — |
| 35 | TX- | AFRAMP | dBFS | 10 | After key ramp | — |
| 36 | TX- | POST_P | dBFS | 10 | Post all processing, pre power control | — |
| 37 | TX- | ATTN_FPGA | dBFS | 10 | Post FPGA attenuation | — |

## Notes

- **PC mic path**: Audio enters at SC_MIC (meter 26), bypassing the CODEC ADC entirely.
  The COD- meters (MICPEAK/MIC) only respond to hardware mic inputs.
- **mic_level**: Radio-side gain applied after CODEC/SC_MIC, before TXAGC. Affects both
  hardware and PC mic paths.
- **met_in_rx=1**: Tells the radio to meter incoming remote_audio_tx during RX for
  VOX detection and P/CW mic level display.
- **Meter IDs are dynamic**: The radio assigns meter IDs on connection. The IDs shown
  here are from one session — match by name, not by ID.
- **Multi-slice TX chains**: Radios can expose one TX waveform meter block per
  active slice. AetherSDR resolves compression meters through the active TX
  slice. FLEX-6600 captures expose distinct TX waveform `sourceIndex` values,
  while FLEX-8000 captures can repeat `TX- num=0` blocks after each `SLC`
  slice block. In both cases `COMPPEAK` is paired with the matching `AFTEREQ`
  or `SC_MIC` from the same slice. The code derives the active TX source or
  implicit slice context first, then looks up the manifest IDs for that slice;
  it never assumes fixed IDs like `22/23`.
- **Compression meter input**: AetherSDR derives the compression gauge from
  radio-provided TX meters only. FLEX-8000 series radios use
  `max(0, COMPPEAK - AFTEREQ)`. 6000-series radios that do not expose `AFTEREQ`
  use `max(0, COMPPEAK - SC_MIC)`. `COMPPEAK` is a dBFS level tap, not a
  ready-to-display gain-reduction meter. If the required pair is missing or not
  fresh enough, `MeterModel` marks the compression value unavailable and emits
  `0 dB` to preserve the existing gauge presentation; it does not fall back to
  local PC mic level or a raw `COMPPEAK` display.
- **P/CW level display**: The Phone/CW level meter UI and smoothing are
  unchanged by the compression derivation. `AFTEREQ` and `SC_MIC` are used only
  as compression reference taps.
- **Unit conversion**: dBm meters (FWDPWR) need watts = 10^(dBm/10)/1000.
  SWR is raw. degC/degF use raw/64.0f. Volts/Amps use raw/1024.0f.

## Agent Notes: 10 MHz TXO Calibration

This is not part of the TX audio chain, but it is a related FlexRadio protocol
lesson from issues #1237/#2095 and belongs somewhere agents will check before
guessing at radio commands.

- **Do not use `radio calibrate`** for the Radio Setup → RX frequency-offset
  Start button. FLEX-6600 firmware v4.1.5 rejects it with `0x50000016`
  (`unknown command`).
- **Do not hide calibration when GPSDO is present**. SmartSDR/Mac still exposes
  the frequency-offset controls with a GPSDO installed. Let the operator choose
  the oscillator/reference source (`TCXO`, `GPSDO`, `External`, or `Auto`) rather
  than deciding that calibration is unnecessary.
- **Use the SmartSDR/FlexLib command sequence**:
  1. `radio set cal_freq=<MHz>`
  2. `radio set freq_error_ppb=0`
  3. `radio pll_start`
- **Second source verification**: the reporter's SmartSDR TCP capture showed
  `radio set freq_error_ppb=0` followed by `radio pll_start`, with the radio
  broadcasting `pll_done=0` while running and `pll_done=1 freq_error_ppb=<value>`
  when complete. The official FlexLib API v2.10.1 source confirms the same
  command: `Radio.StartOffsetEnabled=false` sends `radio pll_start`, while
  `CalFreq`, `FreqErrorPPB`, and `pll_done` are parsed as radio status fields.
- **Completion signal**: track `radio` status messages containing `pll_done`.
  `pll_done=0` means calibration has started/in progress. `pll_done=1` means the
  Start button can be re-enabled; if `freq_error_ppb` is present on that status,
  it is the completed calibration result to show/log.
- **Event-order gotcha**: resetting `freq_error_ppb` can produce stale
  `pll_done=1` status before the new `radio pll_start` run has actually reported
  `pll_done=0`. Do not treat `pll_done=1` as completion for the active button
  press until `pll_done=0` has been observed for that run; otherwise the UI can
  complete on the previous/zeroed value and then get stuck showing
  `Calibrating...` when the delayed command response arrives.
- **Timeout gotcha**: make timeout callbacks run-specific. A prior run's
  20-second timer can fire after that run completed and during a later
  calibration. If the callback only checks a shared `active` flag, it can mark
  the newer run as `No response` before the radio's real `pll_done=1` result
  arrives.
- **Debug workflow**: ask reporters to enable protocol logging and capture the
  full lifecycle, not just TX/RX command lines:
  `QT_LOGGING_RULES="aether.protocol.debug=true" ./AetherSDR`. Useful breadcrumbs
  are request, `pll_start` response code/body, every `pll_done` transition,
  final `freq_error_ppb`, and timeout if `pll_done=1` never arrives.
- **Capture gotcha**: Flex command traffic is TCP, normally port 4992. UDP ports
  4993/4994 are VITA/discovery/data and will not contain the command stream.
  If a Wireshark file has no TCP stream, it cannot prove which command SmartSDR
  sent.

## Future: TX Audio Path Meter Panel

A dedicated panel showing all TX-chain meters as a vertical stack of horizontal
bars would let users visualize the impact of each processing stage:

```
SC_MIC    ████████████░░░░░░░░░░  -12 dBFS   (mic input)
CODEC     █████████████░░░░░░░░░  -10 dBFS   (post mic gain)
TXAGC     █████████████░░░░░░░░░  -10 dBFS   (post AGC)
AFTEREQ   ████████████░░░░░░░░░░  -12 dBFS   (post EQ)
COMPPEAK  ██████████████░░░░░░░░   -8 dBFS   (pre-clipper)
SC_FILT_1 ████████████░░░░░░░░░░  -12 dBFS   (post filter 1)
ALC       ███████████░░░░░░░░░░░  -14 dBFS   (post SW ALC)
POST_P    █████████░░░░░░░░░░░░░  -18 dBFS   (final output)
FWDPWR    ████████████████░░░░░░   85 W       (RF power)
```

This would be invaluable for diagnosing TX audio issues — you can immediately
see where in the chain the signal is being attenuated or clipped.
