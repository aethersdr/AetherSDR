---
title: "Diversity and ESC"
slug: "/diversity-and-esc"
description: "Diversity reception uses two SCUs to receive the same signal from two different antennas simultaneously."
status: "Supported"
applies_to: ["FlexRadio"]
---

:::info[Status]

**Status:** Supported · **Applies to:** FlexRadio

:::

Diversity reception uses two SCUs to receive the same signal from two different antennas simultaneously. **ESC (Enhanced Signal Clarity)** combines these signals with adjustable phase and gain to steer nulls toward interference and peaks toward desired signals — real-time antenna beamforming from your shack.

## Requirements

| Requirement | Details |
|-------------|---------|
| **Radio** | A dual-SCU FlexRadio. AetherSDR reads diversity support from the radio model's capabilities (as FlexLib does), so dual-SCU models including the ML, CL, MLS and CLS variants show the DIV button |
| **Antennas** | Two antennas connected to different ports (e.g., ANT1 + ANT2, or ANT1 + RX_A) |
| **License** | DIV_ESC feature requires SmartSDR+ or higher |
| **Firmware** | v4.x recommended |

Single-SCU radios (for example the FLEX-6400 and FLEX-6500) do not support diversity mode, and the DIV button does not appear. Diversity is a FlexRadio feature; it is not offered on other radio families.

## Setup

1. Open the VFO widget's **Audio tab** (speaker icon)
2. Scroll to the bottom — the **DIV** button appears alongside the L/R audio pan slider
3. Click **DIV** to enable diversity reception

The radio will create a child slice on the second SCU. You'll see a second VFO overlay appear on the panadapter with a different antenna assignment.

## Using diversity and ESC

### How diversity works

When you enable DIV on a slice:

1. The radio creates a **child slice** on the second SCU, locked to the parent's frequency
2. Both SCUs receive the same signal from different antennas
3. The radio combines both signals into a single audio output on the parent slice
4. The child slice's independent audio is muted — you hear the combined result on the parent

The child slice appears in AetherSDR with its own VFO overlay (showing a different antenna), but its audio controls are irrelevant while ESC is active — the parent handles everything.

### ESC controls

When DIV is active on a slice, the **ESC panel** appears below the DIV button in the VFO audio tab. ESC is only shown on the **parent** slice — the child slice shows DIV but not ESC controls.

<img src="/img/screens/diversity-esc.png" width="612" alt="Two VFO flags. Slice A has its audio tab open, with DIV and ESC lit green, a phase slider at 0° with a +180 button, a gain slider at 1.00, a polar display of the ESC phase and gain, and an ESC level meter. To its right, the diversity child slice D on ANT2 shows the same frequency, 14.250.000." />

*Diversity on a FLEX-8600: the DIV button adds a child slice on the second antenna, and ESC opens the beam-steering controls.*

#### ESC Toggle

Click **ESC** to enable Enhanced Signal Clarity beamforming. Without ESC enabled, diversity simply combines both antennas with default settings. With ESC, you can steer the combination.

#### Phase Slider (P)

The horizontal phase slider adjusts the phase offset between the two antenna signals:

- **Range:** 0° to 360° in 5° steps
- **Effect:** Rotates the beam pattern — different phase angles will null out interference arriving from different directions
- **Protocol:** Sent to the radio as radians (matching SmartSDR)

#### Gain Slider (G)

The vertical gain slider adjusts the amplitude balance between antennas:

- **Range:** 0.0 to 2.0
- **Default:** 1.0 (equal balance)
- **Effect:** Values below 1.0 favour the primary antenna, above 1.0 favours the secondary
- **Use case:** When one antenna picks up more noise than signal, reduce its contribution

#### Polar Display

The circular display shows the current phase and gain as a dot on a polar plot:

- **Angle** = phase (0° at top, clockwise)
- **Distance from centre** = gain (centre = 0.0, edge = 2.0)
- **Crosshairs** mark 0°/90°/180°/270° and the mid-gain reference circle (gain = 1.0)

As you adjust the phase and gain sliders, the dot moves in real time. You can also **click anywhere on the polar plot** to set phase and gain directly at that point.

#### ESC Signal Meter

Below the polar display, a level bar shows **signal strength after ESC processing** in dBm. This is a real-time readout from the radio's SLC/ESC meter (10 fps). Watch this meter while adjusting phase and gain to find the optimal null or peak:

- **Higher dBm** = more signal (or more noise)
- **Lower dBm** = deeper null

The goal is usually to find the phase angle where the ESC meter shows the most improvement in SNR — maximum desired signal with minimum interference.

### Finding the Best Null

1. Enable DIV and ESC
2. Listen to the signal with interference
3. Slowly sweep the phase slider from 0° to 360°
4. Watch the ESC meter — you'll hear interference peak and null as you rotate
5. Stop at the phase angle where interference is minimised
6. Fine-tune the gain slider to optimise further

### Band Noise Reduction

Even without a specific interference source, diversity + ESC can reduce overall band noise:

1. Enable DIV and ESC
2. Set gain to 1.0 (balanced)
3. Sweep phase slowly while listening to background noise level
4. The noise floor will vary with phase — find the minimum

### Tips

- **Different antenna types work best** — e.g., a vertical + a horizontal dipole give the most diversity benefit since they respond differently to noise sources
- **Same-band, different-polarisation antennas** are ideal for diversity
- **Phase settings are frequency-dependent** — what works on 20m may not be optimal on 40m
- **Client noise reduction keeps the two antennas apart.** Every AetherSDR noise-reduction method processes left and right separately, so if you pan the two diversity slices to opposite ears, each ear keeps its own antenna (see [DSP Noise Mitigation](./dsp-noise-mitigation.md))
- **A diversity child is not a split partner** — the SPLIT/SWAP badges ignore it (see [Split Operation](./split-operation.md))
- **ESC state is not saved** — you'll need to re-adjust after band changes or reconnects

## Reference

### Protocol details

For developers and protocol researchers:

| Command | Description |
|---------|-------------|
| `slice set <id> diversity=1` | Enable diversity (creates child slice) |
| `slice set <id> diversity=0` | Disable diversity (removes child slice) |
| `slice set <id> esc=on` | Enable ESC beamforming |
| `slice set <id> esc=off` | Disable ESC beamforming |
| `slice set <id> esc_phase_shift=<radians>` | Set phase (0 to 2π) |
| `slice set <id> esc_gain=<float>` | Set gain (0.0 to 2.0) |

- ESC commands are only sent from the **diversity parent** slice (per FlexLib Slice.cs:3367)
- SmartSDR uses `esc=on`/`esc=off` (not `1`/`0`)
- Phase is sent in **radians**, displayed as degrees in the UI
- Phase steps are 5° (~0.087 radians), matching SmartSDR pcap
- The radio echoes ESC state in slice status: `esc=0 esc_gain=1.000000 esc_phase_shift=0.000000`
- When ESC is enabled, the radio sets the child slice to `audio_mute=1 audio_level=0` — the child's audio is absorbed into the parent's ESC processing
- Feature gated by `DIV_ESC` license (status: `license feature name=DIV_ESC enabled=1 reason=PLUS`)

## Known issues

- With diversity on for two slices on different panadapters, tuning one slice moves the other ([#4891](https://github.com/aethersdr/AetherSDR/issues/4891)).
- Turning on DIV for a second slice silently breaks up the diversity pair that is already running, with no message ([#5352](https://github.com/aethersdr/AetherSDR/issues/5352)).

## Troubleshooting

### The DIV button isn't there

Your radio model doesn't have dual SCUs, or AetherSDR hasn't detected it yet. The button only appears on dual-SCU models.

1. Check that your radio is a dual-SCU model (see [Requirements](#requirements)).

### The ESC controls are missing

ESC appears only on the **parent** slice, and only while DIV is on.

1. Open the **Audio** tab on the slice where you clicked **DIV**, not on the child slice.
2. If DIV is off, click **DIV** first; the ESC panel appears below it.

### My ESC settings were lost

ESC state is not saved, so it resets after band changes and reconnects.

1. Re-adjust the phase and gain (see [Finding the best null](#finding-the-best-null)).

## See also

- [VFO Widget](./vfo-widget.md)
- [Multi-Slice Operation](./multi-slice-operation.md)
- [Split Operation](./split-operation.md)
- [DSP Noise Mitigation](./dsp-noise-mitigation.md)
- [FlexRadio](./flexradio.md)
