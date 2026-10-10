---
title: "CW Decoder"
slug: "/cw-decoder"
description: "AetherSDR includes a built-in CW (Morse code) decoder: ggmorse by default, with two optional neural decoders, DeepFist and DeepCW."
status: "Supported"
applies_to: ["FlexRadio", "Hermes-Lite 2 (experimental)", "Networked Icom (early; IC-7300MK2 supported)"]
---

:::info[Status]

**Status:** Supported · **Applies to:** FlexRadio, Hermes-Lite 2 (experimental), Networked Icom (early; IC-7300MK2 supported)

:::

AetherSDR includes a built-in CW (Morse code) decoder powered by [ggmorse](https://github.com/ggerganov/ggmorse) (MIT license). It automatically detects the CW tone pitch and keying speed, and displays decoded text in real time in a panel below the waterfall. It can decode what you receive, what you send, or both.

On Apple Silicon Macs, Linux and Windows you can also choose one of two neural decoders, **DeepFist** or **DeepCW**, which can copy weak or irregular signals that ggmorse misses. See [Choosing a decoder](#choosing-a-decoder).

The decoder works on every radio family that has a CW mode: FlexRadio, the [Hermes-Lite 2](./hermes-lite-2.md), and [Networked Icom](./networked-icom.md) radios (it opens when an Icom slice is in CW).

## Requirements

- The selected slice is in **CW** or **CWL**.
- On a FlexRadio, a **DAX RX channel** (1–8) assigned to the selected slice is recommended, so the decoder hears that slice alone. See [Which audio is decoded](#which-audio-is-decoded).

## Setup

Decoding is controlled in **Settings → Radio Setup... → Phone & CW**, next to **Decode:**

<img src="/img/screens/radio-setup-phone-cw.png" width="679" alt="Phone &amp; CW page headed FLEX-8600. Microphone has Mic Bias Voltage (BIAS), Mic +20 dB Boost and Level Meter During Receive (Enabled). CW has Iambic Enabled with modes A and B (B selected), Swap Dot/Dash, Sideband CWU (selected) or CWL, CWX Sync, and Decode RX (selected) or TX. Digital has RTTY Mark Default 2125 and RTTY Decode Enabled." />

*Radio Setup, Phone & CW page on a FLEX-8600: microphone, CW and digital settings.*

| Toggle | Default | What it decodes |
|---|---|---|
| **RX** | On | The received CW on the selected slice |
| **TX** | Off | Your own keying, from AetherSDR's sidetone — useful for checking paddle or bug timing |

Both appear in the same panel. Your own sending is shown in **cyan**, so it stands apart from the confidence-coloured received text.

## Using the CW decoder

When the selected slice is in **CW** or **CWL**, the decode panel appears below the waterfall, and it hides again when you switch to another mode.

Drag the grip on the panel's edge to resize it; the height is remembered. Right-click the text for font size and Clear.

<img src="/img/screens/cw-decoder-panel.png" width="1340" alt="The bottom of the panadapter in CW mode. The lower part of the waterfall shows CW signals as dotted vertical lines. Below it, the decoder pane's control row reads CW, (selected slice), the detected tone and speed, a Sens slider with two lock buttons, Pitch 500 to 700 Hz and WPM 15 to 40 range sliders, and CPY ALL, CPY VIS, A-, A+, CLR and close buttons, above the decoded-text line." />

*The CW decoder pane under the waterfall, with its sensitivity, pitch and speed controls.*

### Choosing a decoder

The decoder selector in the panel's toolbar picks which decoder reads the received CW. Only the selected decoder runs. Your own sending (the **TX** toggle) is always decoded by ggmorse.

| Decoder | Text appears | Typical use | Model |
|---|---|---|---|
| **ggmorse** (default) | About half a second after each character | Clean, steady signals | None; built in |
| **DeepFist** | About 2 seconds behind the signal | Weak and hand-sent CW | Downloaded on first use, about 13 MB |
| **DeepCW** | About 6–8 seconds behind the signal, a few characters at a time; it holds text back so later audio can correct it | Weak signals | Downloaded on first use, about 15 MB |

No decoder is best on every signal. Try each on the signals you work.

The neural decoders' models are not part of the AetherSDR download. The first time you select one, AetherSDR downloads its model from the model's own published source and checks it before use; the status beside the selector shows the progress. **Cancel** stops a download, and **Retry** appears if it fails. Once downloaded, the model is kept and works offline. If a model cannot be downloaded, the decoder shows **Model unavailable** and you can switch back to ggmorse.

With a neural decoder selected:

- **Sens**, the lock buttons and the Pitch and WPM ranges are unavailable; they apply to ggmorse only.
- Characters are coloured by the model's own confidence, on the same green-to-red scale, and are never hidden.
- Callsign contact cards and the MQTT text stream come from ggmorse only.
- AetherSDR's **Zero Beat** button is unavailable; it needs ggmorse's pitch estimate. A FlexRadio's own **Autotune** (on radios with SmartSDR+) works with any decoder.

The neural decoders are not available on Intel Macs; there the selector does not appear and ggmorse decodes as before.

### How it works

The decoder processes the received audio on a separate worker thread. It detects the dominant tone frequency, then uses a Goertzel filter and timing analysis to decode Morse characters.

- **Auto pitch detection** within the pitch search range
- **Auto speed detection** within the speed search range
- **Cost function**: measures how well the detected on/off timing matches ideal Morse ratios; this drives the confidence colours

Retuning resets the decoder state; pitch and speed locks survive a retune.

### Which audio is decoded

The decoder follows the **selected slice**, independently of speaker volume or mute:

- **On a FlexRadio, assign a DAX RX channel to the selected slice** (1–8). The decoder then reads that slice's audio alone, regardless of speaker gain, mute, or other audible slices.
- Without a DAX channel, it uses the shared receive audio, which mixes every audible slice and follows speaker gain and mute. The panel shows **RX: shared audio**; hover it for the explanation.

### Reading the confidence colours

Received characters are colour-coded by decode confidence (see [Confidence colours](#confidence-colours)). Green text can be trusted. Yellow is usually correct. Orange and red should be read sceptically — they often appear during QSB, QRM, or with operators who have irregular spacing.

### Callsign lookup

When the decoder copies a station identifying itself (`DE <call>`), and QRZ.com lookups are enabled, a **contact card** for that station appears beside the decoded text. Click the callsign to open its QRZ.com page. See [Callsign Lookup](./callsign-lookup.md).

### MQTT

Decoded text is published to the MQTT topic `aethersdr/cw/decode`, one JSON message per character, with `"rx": false` marking text from the TX decoder. Received text is published only while ggmorse is the selected decoder. See [MQTT Station Automation](./mqtt-station-automation.md).

### Tips for best results

- **Tune accurately**: Centre the CW signal in your filter passband. The decoder auto-detects the pitch, but a clean signal helps.
- **Use narrow filters**: CW filter widths of 200–500 Hz reduce noise and competing signals that confuse the decoder.
- **APF helps**: The Audio Peaking Filter (APF) narrows the audio around the CW pitch, giving the decoder a cleaner signal.
- **Give it a DAX channel**: On a FlexRadio, a DAX RX channel on the selected slice keeps other slices and your speaker settings out of the decode.
- **Narrow the search ranges or lock**: If the decoder wanders between signals, narrow the Pitch and WPM ranges, or lock pitch and speed once it has the right station.
- **Strong signals decode best**: Weak signals near the noise floor produce mostly red/orange text.
- Client-side noise reduction is voice-only and turns itself off in CW, so it does not affect the decoder.

## Reference

### Panel controls

| Control | What it does |
|---|---|
| **Decoder** | ggmorse, DeepFist or DeepCW (Apple Silicon, Linux and Windows); see [Choosing a decoder](#choosing-a-decoder) |
| **Stats** | Detected pitch (Hz) and speed (WPM); with a neural decoder, its status |
| **Sens** | 0–100 (default 30). Hides low-confidence characters: 0 shows everything, higher values show only confident decodes. |
| **🔒P** | Lock the decoder's pitch at the current value |
| **🔒S** | Lock the decoder's speed at the current WPM |
| **Pitch** | Pitch search range, 300–1200 Hz (default 500–700 Hz) |
| **WPM** | Speed search range, 5–60 WPM (default 15–40 WPM) |
| **CPY ALL** / **CPY VIS** | Copy all decoded text, or just the visible text, to the clipboard |
| **A− / A+** | Decoded-text size (8–32 px), remembered |
| **CLR** | Clear the decoded text |
| **✕** | Close the decoder panel |

### Confidence colours

Colours are based on ggmorse's cost function:

| Colour | Confidence | Cost range | Meaning |
|-------|-----------|------------|---------|
| Green | High | < 0.15 | Clean, well-timed CW |
| Yellow | Medium | 0.15–0.35 | Slightly imperfect timing |
| Orange | Questionable | 0.35–0.60 | May be incorrect |
| Red | Low | 0.60–1.00 | Likely noise or bad decode |

### Technical details

- Library: [ggmorse](https://github.com/ggerganov/ggmorse) by Georgi Gerganov (MIT license)
- Neural decoders: DeepFist ([n9bc/DeepFist](https://github.com/n9bc/DeepFist)) and DeepCW ([e04/deepcw-engine](https://github.com/e04/deepcw-engine), AGPL-3.0); both run with ONNX Runtime, and their models are downloaded, not bundled
- ggmorse is built in — no external dependency
- Runs on a dedicated worker thread — does not block audio playback
- CPU usage: negligible

## Known issues

- The decoder shows no text on some FLEX-8600 setups ([#6158](https://github.com/aethersdr/AetherSDR/issues/6158)).
- The pitch search range is not kept when a slice is reopened, even with both range locks engaged ([#5921](https://github.com/aethersdr/AetherSDR/issues/5921)).
- There is no setting to stop the automatic QRZ contact card from appearing for decoded callsigns ([#5454](https://github.com/aethersdr/AetherSDR/issues/5454)).
- The squelch threshold line can appear on the panadapter in CW, although squelch is unavailable there ([#6276](https://github.com/aethersdr/AetherSDR/issues/6276)).

## Troubleshooting

### The decode panel doesn't appear

The panel follows the mode of the selected slice.

1. Select the slice you want to decode.
2. Switch it to **CW** or **CWL**.
3. Check that **RX** is on next to **Decode:** in **Settings → Radio Setup... → Phone & CW**.

### The decode changes with speaker volume, mute or other slices

The decoder has no DAX channel on the selected slice, so it uses the shared receive audio. The panel shows **RX: shared audio**.

1. On a FlexRadio, assign a DAX RX channel (1–8) to the selected slice.
2. Check that **RX: shared audio** is no longer shown.

### The decoder jumps between stations

Another signal inside the pitch or speed search range is pulling the decoder away.

1. Narrow the filter to 200–500 Hz, or turn on APF.
2. Narrow the **Pitch** and **WPM** search ranges around the station you want.
3. Once it copies the right station, lock pitch (**🔒P**) and speed (**🔒S**).

### Text is mostly orange or red

The signal is weak, poorly tuned or irregularly sent.

1. Centre the signal in the filter passband.
2. Narrow the filter or turn on APF.
3. Raise **Sens** to hide low-confidence characters.

## See also

- [CWX Panel](./cwx-panel.md) — sending CW from the keyboard
- [RTTY Operation](./rtty-operation.md) — the built-in RTTY decoder works the same way
- [DAX Virtual Audio](./dax-virtual-audio.md)
- [Callsign Lookup](./callsign-lookup.md)
- [ggmorse](https://github.com/ggerganov/ggmorse)
