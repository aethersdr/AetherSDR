---
title: "DVK Panel"
slug: "/dvk-panel"
description: "The DVK (Digital Voice Keyer) panel records and plays up to 12 voice messages, stored on the radio or on this computer, commonly used for contesting and repetitive voice operations."
status: "Supported"
applies_to: ["FlexRadio", "Hermes-Lite 2 (local keyer: beta)", "Networked Icom (local keyer: beta)"]
---

:::info[Status]

**Status:** Supported · **Applies to:** FlexRadio, Hermes-Lite 2 (local keyer: beta), Networked Icom (local keyer: beta)

:::

The DVK (Digital Voice Keyer) panel records and plays up to 12 voice messages, commonly used for contesting and repetitive voice operations. The messages are kept in one of two places:

- **Radio DVK** — on the radio, using the FlexRadio's own voice keyer. It requires the radio's SmartSDR+ subscription.
- **Local** — on this computer, recorded from the PC microphone and played through AetherSDR's transmit audio. It needs no subscription, works with any radio AetherSDR can transmit on, and the recordings go with you from radio to radio.

The panel, its buttons and the F1–F12 keys work the same way with either keyer.

The local keyer is verified on FlexRadio. On the Hermes-Lite 2 and Networked Icom radios it is **Beta**: it is expected to work, has not yet been tested on the air there, and the panel title says **Beta** while one of those radios is connected. Reports from those radios are welcome on [GitHub](https://github.com/aethersdr/AetherSDR/issues).

## Requirements

- **Mode:** the DVK plays on the **transmit slice**, so it is available when the TX slice is in a **voice mode** (USB, LSB, AM, SAM, FM, NFM, DFM). The indicator is grayed out in digital and CW modes, and switching to a non-voice mode closes the panel. A local message playing when the TX slice leaves voice mode stops at once.
- **Radio DVK:** a FlexRadio Aurora, FLEX-8000 series or FLEX-6000 series radio with an active SmartSDR+ subscription. See [SmartSDR+ license](#smartsdr-license).
- **Local:** a radio AetherSDR can transmit on. Recording uses the PC microphone, so on a FlexRadio the mic source must be **PC**.

The DVK indicator is hidden only on a radio that can neither transmit nor host a voice keyer, such as an RTL-SDR or an ANAN-G2.

## Using the DVK panel

### Opening DVK

Click the **DVK** indicator in the status bar at the bottom of the screen. The panel opens beside the panadapter. Its title names the keyer in use, for example **Digital Voice Keyer (Local)**.

The DVK panel and the [CWX Panel](./cwx-panel.md) share the same space — opening one closes the other.

When the window is short, scroll the list of recording slots; the panel title, status and transport buttons stay visible.

<img src="/img/screens/dvk-panel.png" width="370" alt="Digital Voice Keyer panel along the left edge of the window, status Idle. Twelve slots, F1 Recording 1 to F12 Recording 12, each read Empty. REC, STOP, PLAY and PREV buttons run along the bottom." />

*The DVK panel, opened from DVK in the status bar.*

### Choosing where recordings are kept

Right-click the **DVK** indicator, or choose **Settings → Voice Keyer Source...**. With the indicator focused, the context-menu key or **Shift+F10** opens the same menu.

- **Automatic by radio licence** — the default. Uses the Radio DVK when the radio has one and has not reported that SmartSDR+ is missing, and Local otherwise. The menu shows which one it chose, for example `(now: Local)`.
- **Radio DVK — recordings on the radio** — grayed out, with the reason, on a radio with no voice keyer or no SmartSDR+.
- **Local — recordings on this computer**.
- **Open Local Recordings Folder** — opens the folder the local recordings are kept in.

A change made while a recording, playback or preview is running waits for it to end, so STOP and the F-keys keep driving the keyer that started it. The status line says so, for example `On air · slot 1 · 2.6s of 3.0s · Radio keyer when this ends`.

If the source changes while you are renaming a slot or using its context menu or file dialog, the pending action is canceled. Open it again on the keyer you want to change.

### Recording

1. Click a slot to select it (the row takes the highlight).
2. Click **● REC** to start recording. The status line reads `Recording · slot 1 · 0.4s of 10.0s`. A Radio DVK slot holds up to 10 seconds, the radio's own limit; a local slot holds up to 30 seconds, enough for a contest exchange or a longer CQ.
3. Click **■ STOP** to end recording.
4. The recording is stored and the slot shows its duration.

Recording never transmits. With the local keyer:

- **VOX is held off while you record.** If VOX is on, AetherSDR turns it off before recording starts — only once the radio has confirmed it — so your voice cannot key the transmitter, and turns it back on afterwards. If you have just turned VOX off yourself and the radio has not confirmed it yet, recording asks the radio again and starts once it answers; VOX stays off afterwards. The status line shows `· VOX held off` meanwhile. If you change VOX yourself during the recording, your setting is kept, and turning VOX on stops the recording without saving it. On a radio with no VOX that AetherSDR controls, such as the Hermes-Lite 2, there is nothing to hold.
- **STOP works while recording is starting.** Pressing STOP before the radio has confirmed VOX is off cancels the recording.

### Playback (on-air)

- Click **▶ PLAY** or press the **F-key button** to transmit the recording.
- The radio keys TX automatically during playback and unkeys at the end.
- PLAY, the playing slot's F-key and its progress bar take the MOX (transmit) colors, and the bar fills in proportion to elapsed/total time.
- The status line reads `On air · slot 3 · 0.4s of 4.2s`.

### Preview (local)

- Click **◀ PREV** to hear the recording locally without transmitting.
- The progress bar fills cyan.
- The status line reads `Previewing · slot 3 · 0.4s of 4.2s`.

Only one operation runs at a time. STOP is enabled only while something is running, PLAY and PREV are disabled on an empty slot, and while a recording, playback, preview or WAV transfer is in progress the other start controls stay disabled. The button of the running recording, playback or preview stays enabled, so clicking it again stops it. A refused operation shows in the status line as, for example, `Play failed (slot 3): DVK requires an active SmartSDR+ subscription`.

### Managing slots

Right-click any slot row to access:

- **Rename…** — inline text editor (also available via double-click on name). A name holds up to 61 UTF-8 bytes; quotes and `|` are removed. Enter saves, Escape cancels.
- **Clear** — erase the recording (grayed out for empty slots and while the DVK is busy). The Radio DVK decides whether the slot keeps its name.
- **Import WAV…** — load a WAV file into this slot (see [WAV file requirements](#wav-file-requirements))
- **Export WAV…** — save the recording as a WAV file (grayed out for empty slots)

If an import or export fails, the destination is left intact — a failed export never leaves a half-written file over an existing one. With the Radio DVK, **Import WAV…** and **Export WAV…** stay grayed out for about 3 seconds after an import while the radio's file server finishes.

## Reference

### Recording slots

The panel shows 12 recording slots (F1–F12), each with:

- **F-key button** — click to toggle playback on-air
- **Recording name** — displayed next to the button
- **Duration** — shown on the right (or "Empty" if no recording)
- **Progress bar** — 3 px colored bar during active operations

Empty slots appear dimmed. Slots with recordings show bright text.

Screen readers announce each slot as, for example, "Slot 3: CQ Contest, 4.2s", and each F-key as "Play slot 3: CQ Contest".

### Keyboard shortcuts

| Key | Action |
|-----|--------|
| F1–F12 | Play that slot on-air; press again to stop. Works while the TX slice is in a voice mode, even with the panel closed. |
| Escape | Stop any active recording, playback, or preview (or cancel a rename) |

F1–F12 follow the transmit slice's mode: in voice modes they play DVK recordings, in CW they send CWX macros (see [CWX Panel](./cwx-panel.md)), and only one of the two responds.

### Local recordings folder

Local recordings are kept in `Documents/AetherSDR/VoiceKeyer`, one file per slot, named `slot-01.wav` to `slot-12.wav`. On Windows, when the Documents folder is redirected to OneDrive, the folder is inside OneDrive and the recordings sync with it.

A WAV file copied into the folder under a slot's name appears in that slot the next time you choose **Local** in the keyer menu. It is checked when it plays, so a file longer than 30 seconds is refused then.

### WAV file requirements

**Import WAV…** converts the file for you, so most WAV files import as they are. The two keyers accept slightly different files:

| | Radio DVK | Local |
|---|---|---|
| **Encoding** | PCM 8, 16, 24 or 32-bit, or 32-bit float | PCM 8, 16, 24 or 32-bit, or 32 or 64-bit float |
| **Header** | plain or WAVE_FORMAT_EXTENSIBLE | plain or WAVE_FORMAT_EXTENSIBLE |
| **Channels** | 1 to 8, mixed down to mono | 1 to 8, mixed down to mono |
| **Sample rate** | 8 kHz to 384 kHz | 8 kHz to 192 kHz |
| **Length** | at most 10 seconds | at most 30 seconds |
| **File size** | — | at most 100 MB |

Every file is resampled to 24 kHz. The Radio DVK receives it as 24 kHz mono 16-bit PCM; a local slot stores it as 24 kHz 16-bit PCM. Files that can't be converted are rejected with a specific error message, such as `Recording is 12.0 s long; a DVK slot holds at most 10 s`.

### Status bar indicator

The DVK label in the status bar has three visual states:

- **Bright cyan** — panel is open
- **Dim white** — available (voice mode), click to open
- **Dark gray** — unavailable (not in a voice mode, or the Radio DVK is not licensed)

The indicator can also be reached with the keyboard: Tab to it and press Enter.

### SmartSDR+ license

When the Radio DVK is in use and the radio reports that the feature is not licensed, or refuses a DVK command or transfer with the SmartSDR+ error, the **DVK indicator itself is disabled** — grayed out with an arrow cursor and the tooltip "Digital Voice Keyer — requires an active SmartSDR+ subscription". The F1–F12 shortcuts and any open panel are gated the same way, and the panel status reads `Disabled · SmartSDR+ required`. With **Automatic by radio licence** selected, the same report switches the panel to the local keyer instead.

AetherSDR only applies this once the radio positively says no; if the radio never reports the license, the Radio DVK stays available. If the radio later reports the license as enabled, for example after a subscription is activated mid-session, the Radio DVK becomes available again without reconnecting.

## Troubleshooting

### The DVK indicator is grayed out

The transmit slice is not in a voice mode; or **Radio DVK** is selected and the radio reports no SmartSDR+ license, or has no voice keyer of its own (a Hermes-Lite 2 or Networked Icom radio).

1. Switch the TX slice to a voice mode (USB, LSB, AM, SAM, FM, NFM or DFM).
2. Hover the indicator. If the tooltip says it requires an active SmartSDR+ subscription, renew SmartSDR+ on the radio, or right-click the indicator and choose **Local — recordings on this computer**.
3. On a radio with no voice keyer of its own, right-click the indicator, or choose **Settings → Voice Keyer Source...**, and pick **Local — recordings on this computer** or **Automatic by radio licence**.

### REC says to set the mic source to PC

The local keyer records the PC microphone, and the radio is taking its transmit audio from one of its own inputs.

1. Set **Mic source** to **PC** in the P/CW applet (see [TX Controls](./tx-controls.md)).
2. Click **● REC** again.

### Recording does not start: the radio did not confirm VOX is off

VOX is on, and the radio did not confirm that it had turned VOX off for the recording. Some radios never confirm it.

1. Turn VOX off yourself.
2. Click **● REC** again.
3. Turn VOX back on when you have finished recording.

### A local recording is empty or says no microphone audio arrived

The PC microphone is not capturing.

1. Check the microphone in [Audio Settings](./audio-settings.md).
2. Make sure your operating system allows AetherSDR to use the microphone.
3. Record again.

### The DVK indicator is missing

The radio can neither transmit nor host a voice keyer, as with an [RTL-SDR](./rtl-sdr.md) or an [ANAN-G2](./anan-g2.md). DVK is not available there.

### A WAV import is rejected

The file can't be converted, is too long for the keyer in use, or the radio is busy.

1. Read the message in the panel's status line.
2. If it says the file is too long, trim it to 10 seconds or less for the Radio DVK, or 30 seconds for Local.
3. If it names an unsupported encoding, save the file as PCM or 32-bit float WAV.
4. If it says the voice keyer or file server is busy, stop the running operation, wait a few seconds and try again.
5. Import it again with **Import WAV…**.

## See also

- [CWX Panel](./cwx-panel.md)
- [Keyboard Shortcuts](./keyboard-shortcuts.md)
- [Audio Settings](./audio-settings.md)
- [FlexRadio](./flexradio.md)
