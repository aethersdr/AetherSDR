---
title: "RTTY Operation"
slug: "/rtty-operation"
description: "RTTY (Radio Teletype) is a digital mode that encodes text as two alternating audio tones — a Mark tone and a Space tone."
status: "Supported"
applies_to: ["FlexRadio", "Hermes-Lite 2 (experimental)"]
---

:::info[Status]

**Status:** Supported · **Applies to:** FlexRadio, Hermes-Lite 2 (experimental)

:::

RTTY (Radio Teletype) is a digital mode that encodes text as two alternating audio tones — a **Mark** tone and a **Space** tone. AetherSDR supports RTTY with the radio's **RTTY** mode, a **built-in RTTY decoder** that shows received text below the waterfall, and the usual DAX and CAT path for external RTTY software. The decoder also works on the [Hermes-Lite 2](./hermes-lite-2.md).

Squelch switches itself off when a slice enters RTTY, because it would break the decode, and AetherSDR's client-side noise reduction is voice-only and stays off in RTTY.

## Setup

### Mark frequency

The radio's default Mark tone is **RTTY Mark Default** in **Settings → Radio Setup... → Phone & CW** (**Digital** group). The default is **2125 Hz**, which is the worldwide amateur RTTY standard. Other common values are listed under [Mark frequencies](#mark-frequencies).

The radio stores a global default (`rtty_mark_default`) that applies to all new RTTY sessions. AetherSDR reads this from the radio on connect.

### Shift

The shift is the frequency difference between the Mark and Space tones. The standard amateur RTTY shift is **170 Hz**. Other shifts exist (for example 850 Hz for military/commercial) but are uncommon on the amateur bands.

The shift is set per slice via `rtty_shift` in the radio's slice status.

## Using RTTY

### Tuning with the Mark and Space lines

RTTY transmits binary data by switching between two audio frequencies, Mark and Space (see [Mark and Space tones](#mark-and-space-tones)). The difference between them is the **Shift** — by default 170 Hz, the standard amateur RTTY shift.

When you switch to RTTY mode, AetherSDR displays two dashed vertical lines on the panadapter labelled **M** (Mark) and **S** (Space). These lines show exactly where the two RTTY tones sit in the audio passband, and they move with your VFO frequency.

When you tune to an RTTY signal, position the two dashed lines so that the Mark line sits on the higher-frequency tone and the Space line sits on the lower-frequency tone. When both tones fall on their respective lines, the signal is properly tuned and can be decoded.

<img src="/img/screens/rtty-ms-lines.png" width="680" alt="Panadapter zoomed to a few kilohertz in RTTY mode. A shaded passband holds a dashed red line marked S and a dashed green line marked M, 170 Hz apart, beside the VFO flag reading 14.083.500 in RTTY mode." />

*RTTY mode on a narrow span: the red S (space) and green M (mark) lines sit inside the passband.*

```
Panadapter view (RTTY mode):

    S           M
    |           |        ← dashed lines on panadapter
    ┊    ╱╲     ┊╱╲
    ┊   ╱  ╲    ╱  ╲
    ┊──╱────╲──╱────╲──
    ┊ ╱      ╲╱      ╲
  Space     Mark
  tone      tone
  (1955 Hz) (2125 Hz)

  ←── 170 Hz shift ───→
```

The M/S lines appear only in **RTTY** mode. A DIGL slice shows the normal carrier marker, not RTTY cues.

### Built-in RTTY decoder

When the selected slice is in **RTTY**, a decoder pane opens below the waterfall and shows the decoded text (Baudot/ITA2, with LTRS/FIGS shift tracking). Its controls are listed under [Decoder controls](#decoder-controls).

The stats bar updates about twice a second with the mark and space levels, the SNR and **LOCKED** / **UNLOCK**. Mark, Shift, Baud, REV and Sens are remembered.

<img src="/img/screens/rtty-decoder-pane.png" width="1331" alt="The bottom of a zoomed panadapter in RTTY mode. Under the waterfall, the decoder pane's control row reads RTTY, (selected slice), Mark: Auto, Shift: 170, Baud: 45.45, a REV button, a Sens slider, a green tuning bar with a lock status, and CPY ALL, CLR and close buttons, above an empty text area." />

*The RTTY decoder pane under the waterfall.*

#### Which audio is decoded

Like the [CW Decoder](./cw-decoder.md), the RTTY decoder follows the **selected slice**. On a FlexRadio, assign a **DAX RX channel** to that slice and the decoder reads it alone, regardless of speaker gain, mute or other slices. Without one it uses the shared receive audio and shows **RX: shared audio**.

#### Closing and reopening the pane

Closing the pane with **✕** turns the decoder off, and it stays closed on later RTTY slices. To bring it back, open **Settings → Radio Setup... → Phone & CW** and set **RTTY Decode** (in the **Digital** group) to **Enabled**.

### Using external RTTY software

Many operators use external RTTY software (MMTTY, fldigi and so on) connected through AetherSDR's DAX virtual audio and CAT control:

1. **Enable DAX** — Assign a DAX channel (for example DAX 1) to the slice and make sure DAX is running in the **DAX** applet. This routes the slice's receive audio to a virtual audio device your RTTY software can open.
2. **Set up CAT** — In the **CAT** applet, enable a port (rigctld, TS-2000 or Flex dialect, or a virtual serial port on Linux/macOS). Your RTTY software uses it to key the radio and read the frequency.
3. **Select RTTY mode** — Switch the slice to RTTY mode. The radio applies the correct filter bandwidth and enables its RTTY demodulator.
4. **Use DIGU/DIGL for AFSK** — If your RTTY software generates audio-frequency shift keying (AFSK) rather than expecting the radio to do FSK, use DIGU or DIGL mode instead of RTTY. The Mark/Space lines and the built-in decoder do not appear in DIGU/DIGL, since the demodulation is handled by your software.

See [DAX Virtual Audio](./dax-virtual-audio.md) and [CAT Control](./cat-control.md) for setup details.

## Reference

### Mark and Space tones

| Tone | Meaning | Default frequency |
|------|---------|-------------------|
| **Mark** (M) | Binary 1 — the "key down" tone | 2125 Hz above the carrier |
| **Space** (S) | Binary 0 — the "key up" tone | 2125 − 170 = 1955 Hz above the carrier |

### Decoder controls

| Control | Options | What it does |
|---|---|---|
| **Mark** | **Auto**, 2125, 2210, 1700, 1275, 1000, 915, 850, 500 Hz | The Mark tone. **Auto** follows the slice's mark setting from the radio, live. |
| **Shift** | 45, 50, 75, 100, **170**, 182, 200, 240, 425, 450, 500, 850 Hz | Mark-to-space shift |
| **Baud** | **45.45**, 50, 75, 100, 110, 150, 300 | Symbol rate |
| **REV** | On / off | Reverse polarity — space above mark (inverted signal) |
| **Sens** | 0–100 (default 0) | Display squelch: hides characters the decoder is not confident about, so noise between transmissions doesn't fill the pane. 0 shows everything; about 38 hides what the stats bar calls UNLOCK; 100 keeps only near-certain copy. Display only — nothing is retuned. |
| **CPY ALL** | — | Copy all decoded text to the clipboard |
| **CLR** | — | Clear the text |
| **✕** | — | Close the decoder pane |

### Mark frequencies

| Mark frequency | Usage |
|----------------|-------|
| 2125 Hz | Amateur radio standard (default) |
| 2295 Hz | Commercial / maritime RTTY |
| 1275 Hz | Some European stations |

### Recommended RTTY software

| Software | Platform | Notes |
|----------|----------|-------|
| **fldigi** | Linux, macOS, Windows | Full-featured, supports many digital modes |
| **MMTTY** | Windows | Classic RTTY-specific decoder |
| **WSJT-X** | Linux, macOS, Windows | Primarily FT8/FT4, but connects via the same DAX/CAT path |

### Typical RTTY operating frequencies

| Band | Frequency range | Notes |
|------|----------------|-------|
| 80m | 3.580 – 3.600 MHz | RTTY/Data segment |
| 40m | 7.040 – 7.080 MHz | RTTY activity centre ~7.080 |
| 30m | 10.130 – 10.150 MHz | RTTY ~10.143 |
| 20m | 14.080 – 14.100 MHz | RTTY activity centre ~14.085 |
| 15m | 21.080 – 21.100 MHz | RTTY ~21.085 |
| 10m | 28.080 – 28.100 MHz | RTTY ~28.085 |

## Known issues

- The squelch threshold line can still appear on the panadapter in RTTY, although squelch is off in that mode ([#6276](https://github.com/aethersdr/AetherSDR/issues/6276)).

## Troubleshooting

### Mark/Space lines are in the wrong position

The Mark frequency may not match the station you're receiving. Most amateur stations use 2125 Hz Mark / 170 Hz Shift.

1. If the lines don't align with the signal's two tones, check **RTTY Mark Default** in **Settings → Radio Setup... → Phone & CW**.

### I don't see the M/S lines

The Mark and Space lines only appear in **RTTY** mode. They do not appear in DIGU, DIGL, or other modes.

1. Make sure the slice mode selector shows RTTY.

### The decoder pane doesn't appear

The pane opens only for a selected slice in RTTY mode, and closing it with **✕** turns the decoder off.

1. Select the slice and set it to RTTY.
2. If you closed the pane earlier, set **RTTY Decode** to **Enabled** in **Settings → Radio Setup... → Phone & CW**.

### The decoder prints gibberish between transmissions

The decoder is printing noise while it has no lock.

1. Raise **Sens** on the decoder bar (around 38 hides unlocked copy).

### Audio sounds like two alternating tones but no decode

The tones are not on the M and S lines, or the decoder settings don't match the signal.

1. Tune your VFO until both tones sit on their respective dashed lines.
2. Check that **Shift**, **Baud** and **REV** match the signal.
3. If you use external software, verify the DAX channel is routing audio correctly.

## See also

- [CW Decoder](./cw-decoder.md)
- [DAX Virtual Audio](./dax-virtual-audio.md)
- [CAT Control](./cat-control.md)
- [WSJT-X Integration](./wsjt-x-integration.md)
- [Hermes-Lite 2](./hermes-lite-2.md)
