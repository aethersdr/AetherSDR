---
title: "VFO Widget"
slug: "/vfo-widget"
description: "Every slice has a VFO widget (the \"flag\") floating on the spectrum next to its frequency marker."
---

Every slice has a VFO widget (the "flag") floating on the spectrum next to its frequency marker. It shows the frequency and a signal meter, and its tabs give quick access to the slice's audio, DSP, mode, RIT/XIT and DAX settings.

<img src="/img/screens/vfo-flag.png" width="316" alt="VFO flag for slice A. The top row shows the RX antenna ANT1, the TX antenna ANT1, the filter width 2.7K, SPLIT, a red TX badge and the slice letter A. The large frequency readout reads 14.250.000, above a signal meter with a dBm reading. The bottom row has the speaker, DSP, USB, X/RIT and DAX tabs. A column of small close, lock, record and play buttons sits to the left of the flag." />

*The VFO flag for slice A, with its close, lock, record and play buttons on the left edge.*

## Using the VFO flag

### Frequency display

- Shows the slice frequency in `MM.KKK.HHH` format.
- **Double-click** to type a frequency directly. Accepts `14.225` (MHz), `14225` (kHz), `14225000` (Hz) or `14.225.000` (dotted).
- On XVTR bands: `1446` = 144.6 MHz (the decimal goes after the third digit).
- Click the slice letter badge to **collapse** the flag to a compact label; click the collapsed label to expand it again. Right-click → Add Spot works in both forms.

### Meter

Below the frequency is the signal meter. **Click it** to choose between the classic **S-Meter** bar and **SmartMTR**, which adds min/max extremes markers, numeric values and a transmit meter (Mic Level, SWR, Power or Compression). The choice applies to every flag. Full details are on [Meters](./meters.md).

<img src="/img/screens/vfo-meter-selector.png" width="316" alt="VFO flag with the meter selector open below the tab row: S-Meter (selected) and SmartMTR buttons, a Show extremes checkbox, Extremes speed Medium, Show values None, TX meter None and a Show meter type checkbox." />

*Clicking the meter under the frequency opens the meter selector on the flag.*

### Slice buttons

Beside the flag:

- **✕** — close the slice
- **🔓 / 🔒** — lock the frequency (prevents tuning; a "LOCKED" notice appears if you try)
- **⏺** — record slice audio (pulses red while recording)
- **▶** — play the recording (enabled once one exists)

#### Record & Play

Where recordings are made is set in **Settings → Radio Setup... → Audio → Recording**:

- **Client Side** (the default) records on this computer, including AetherSDR's CW sidetone.
- **Radio Side** records in the radio (FlexRadio). On radios without a recorder, Radio Side is dimmed with the reason, and REC/PLAY record on this computer instead.

To transmit a radio-side recording as a voice keyer, press MOX first, then ▶.

### Badges

- **TX** — red when this slice is the transmit slice; grey otherwise. Click a grey TX badge to move transmit to this slice.
- **SPLIT** / **SWAP** — split operation. Click **SPLIT** to create a transmit slice; on the transmit slice the badge reads **SWAP**. Right-click for the split menu. See [Split Operation](./split-operation.md).
- **⇄** — this slice is linked to another with **Link Slice**. See [Multi-Slice Operation](./multi-slice-operation.md).

### Filter widen and narrow shortcuts

The **Filter Widen** / **Filter Narrow** actions (keyboard, MIDI or controller) change the filter width by 100 Hz in the right direction for the mode: USB, CWU, DIGU and RTTY move the upper edge; LSB, CWL and DIGL move the lower edge. See [Keyboard Shortcuts](./keyboard-shortcuts.md).

## Reference

### Tabs

Click a label along the bottom of the flag to open its tab. Controls the slice's radio cannot use are dimmed, with the reason in the tooltip.

#### 🔊 Audio

<img src="/img/screens/vfo-audio-tab.png" width="316" alt="VFO flag with the speaker tab open below the frequency. It holds an AF slider at 50, a SQL slider at 20, an AGC mode selector set to Med with its threshold slider at 65, and a DIV button next to a left-right pan slider." />

*The VFO flag with the audio tab open.*

- **AF** slider with mute
- **Pan** (left/right) with a centre marker
- **SQL** — squelch on/off and level
- **AGC-T** — AGC mode (Off / Slow / Med / Fast) and threshold

#### DSP

The radio's own noise-reduction and filter toggles for the current mode, with level sliders where they apply: **NR**, **NB**, **ANF**, **NRL**, **NRS**, **RNN**, **NRF**, **ANFL**, **ANFT**, plus **APF** in CW and **MN** (manual notch) on radios that have it. Right-click a button for a quick parameter popup.

The same tab has the **AetherRX** and **AetherTX** launchers, always the same width, which open AetherSDR's own receive and transmit audio chains. The client-side noise reduction methods (NR2, RN2, NR4, NNR, DFNR, BNR, MNR) live in **AetherRX**, not on this tab; see [Aetherial Audio](./aetherial-audio.md) and [DSP Noise Mitigation](./dsp-noise-mitigation.md).

The **DSP** tab label is colour-coded: green when any radio DSP or client noise reduction is active, grey when everything is off, cyan while the tab is open.

On FM, NFM, DFM and DSTR the tab becomes **OPT** (repeater offset and direction, simplex and reverse; CTCSS tones on FM, NFM and DFM only).

<img src="/img/screens/vfo-dsp-tab.png" width="316" alt="VFO flag with the DSP tab open below the frequency. A grid of toggle buttons reads NR, NB, ANF, NRL, NRS, RNN, NRF, ANFL, ANFT, and two launcher buttons, AetherRX and AetherTX." />

*The VFO flag with the DSP tab open on a FlexRadio: the radio's noise filters plus the AetherRX and AetherTX launchers.*

#### Mode tab

Labelled with the current mode (for example **USB**):

<img src="/img/screens/vfo-mode-tab.png" width="316" alt="VFO flag with the USB tab open below the frequency. It shows a mode drop-down set to USB, quick mode buttons USB (lit), CW and AM, filter presets 1.8K, 2.1K, 2.4K, 2.7K (lit), 2.9K, 3.3K, 4K and 6K, a Marker: 3px button, a lit Filter Edge button and an Adaptive RX filter checkbox, unticked." />

*The VFO flag with the mode tab (USB) open: mode selector, mode buttons, filter presets and the Adaptive RX filter switch.*

- mode selector and quick mode buttons;
- per-mode filter preset grid;
- **Marker:** button that cycles this slice's frequency marker Off → 1 px → 3 px, and a filter-edge toggle. These override the defaults set in **View → VFO Marker Size** (Off / 1 px / 3 px, default 3 px) and **View → VFO Filter Edge** (Show / Hide, default Show). Changing a View default updates every flag without overwriting a per-slice choice.

##### Adaptive RX filter (USB and LSB)

Under the filter presets in USB and LSB, **Adaptive RX filter** automatically fits the receive passband to the width of the signal you are hearing.

| Control | Choices |
|---------|---------|
| **Adaptive RX filter** | On / off |
| **Lo cut** / **Hi cut** | The limits the fit may use |
| **SNR** | Sensitive, Normal (default), Strong |
| **Speed** | Fast, Normal (default), Slow |
| **Splat** | Tight, Normal (default), Wide — splatter rejection |
| **Het** | Off (default) / On — narrows away from a strong heterodyne at the passband edge |

While a fit is applied, the filter readout shows **AUTO**. Adaptive fitting pauses while you transmit.

#### X/RIT

- RIT on/off, zero, step buttons and Hz readout
- XIT on/off, zero, step buttons and Hz readout

#### DAX

- **DAX Ch** — Off, or a DAX audio channel. The list follows the radio's capacity (up to 8 channels).

DAX IQ channels and the WFM satellite-data demodulator are in the panadapter's overlay **DAX** flyout, not on the flag. See [DAX Virtual Audio](./dax-virtual-audio.md) and [DAX IQ Streaming](./dax-iq-streaming.md).

## Known issues

- With **Client Side** recording, the slice recording stops capturing audio when you transmit, so your own transmission is not recorded ([#4141](https://github.com/aethersdr/AetherSDR/issues/4141)).

## Troubleshooting

### Tuning does nothing and a "LOCKED" notice appears

The slice's frequency is locked.

1. Click the **🔒** button beside the flag to unlock it.

### A control on the flag is dimmed

The slice's radio can't use that control.

1. Hover over the control; the tooltip gives the reason.

### The filter readout says AUTO

**Adaptive RX filter** is on and is fitting the passband to the signal (USB and LSB only).

1. Open the mode tab (labelled with the current mode).
2. Turn **Adaptive RX filter** off to set the filter yourself.

### ▶ (play) is greyed out

There is no recording for this slice yet.

1. Click **⏺** to record, then click it again to stop. **▶** is enabled once a recording exists.

### The flag is a small label with no controls

The flag is collapsed.

1. Click the collapsed label to expand it again.

## See also

- [Slice Colors](./slice-colors.md)
- [Meters](./meters.md)
- [Panadapter Controls](./panadapter-controls.md)
- [Split Operation](./split-operation.md)
- [Diversity and ESC](./diversity-and-esc.md)
- [Multi-Slice Operation](./multi-slice-operation.md)
