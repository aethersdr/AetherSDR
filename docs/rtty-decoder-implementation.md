# RTTY Decoder Implementation Notes

**Branch:** `feat/1392-add-rtty-decoder`  
**PR:** https://github.com/aethersdr/AetherSDR/pull/3336  
**Issue:** #1392

---

## Architecture

The decoder follows the same pattern as `CwDecoder`:

- `src/core/RttyDecoder.h` / `.cpp` — DSP and Baudot decode, worker thread
- `src/gui/PanadapterApplet` — RTTY panel UI, shown/hidden automatically on mode change
- `src/gui/MainWindow` — audio feed, routing, start/stop lifecycle

---

## DSP Algorithm

**Signal chain (per sample at 24 kHz):**

1. Stereo float32 → mono (downmix in `feedAudio`)
2. Two 2nd-order biquad bandpass filters — mark and space frequencies, bandwidth = 3× baud
3. Per-filter envelope: `env += alpha * (|out| - env)` where `alpha = 1 - exp(-baud / (sampleRate * 0.3))`
4. Schmitt trigger: 10% hysteresis band around the mark/space decision boundary
5. Falling-edge start-bit detection (mark → space transition)
6. Bit clock set to fire 1.5 bit periods after start edge; 25% correction on each subsequent transition
7. Stop-bit validation — characters with a space-tone stop bit are discarded
8. Baudot/ITA2 decode, LTRS/FIGS shift tracking

**Key constants:**
- `tau = 0.3/baud` — crossover in ~0.2 bit periods; fast enough for bit sync, slow enough to reject carrier ripple
- `hysteresis = 0.10` (10%) — prevents chattering at the mark/space boundary
- Filter BW = `3 × baud` — proven working at 45.45, 50, and 75 baud

---

## UI Panel

Appears automatically when the active slice enters RTTY or DIGL mode; hides on exit.

| Control | Options |
|---|---|
| Mark freq | Auto, 2125, 2210, 1700, 1275, 1000, 915, 850, 500 Hz |
| Shift | 45, 50, 75, 100, **170**, 182, 200, 240, 425, 450, 500, 850 Hz |
| Baud | **45.45**, 50, 75, 100, 110, 150, 300 |
| REV | Space above mark (LSB / inverted polarity) |

Stats bar updates ~2×/second: `M:xx% S:xx% SNR:xxdB  LOCKED/UNLOCK`

All settings persisted to AppSettings (flat key names — see bug #4 below).

---

## "Auto" Mark Frequency

When Mark combo = "Auto" (value 0):
- RTTY mode → decoder uses `SliceModel::rttyMark()` (radio's mark setting)
- DIGL mode → decoder uses `SliceModel::diglOffset()`

The decoder re-reads these live on `rttyMarkChanged`, `rttyShiftChanged`, and `diglOffsetChanged` signals, so tuning the radio's mark tone while decoding works without a mode cycle.

---

## Bugs Found and Fixed During Development

### 1. CR/LF not rendered
`\r` and `\n` passed through `insertHtml()` are treated as collapsed whitespace in HTML.  
**Fix:** `\r` → discard (no-op), `\n` → `<br>`. Standard RTTY CR+LF pairs produce one line break.

### 2. Spaces not rendered
Space characters in HTML `<span>` elements are collapsed.  
**Fix:** Escape spaces as `&nbsp;` before `insertHtml`.

### 3. Panel didn't appear on mode change
`refreshRttyDecodeState` was only wired to the active-slice-change path, not the mode-change handler that fires when the user switches an already-active slice to RTTY.  
**Fix:** Added `refreshRttyDecodeState()` call alongside `refreshCwDecodeState()` in all three relevant sites.

### 4. Settings not persisted — REV reset every launch
`AppSettings` validates XML element names before writing and silently drops any key containing `/`. All four RTTY keys used `"RttyDecoder/Foo"` form, so nothing was ever written to disk.  
**Fix:** Flattened to `RttyDecoderMarkHz`, `RttyDecoderShiftHz`, `RttyDecoderBaud`, `RttyDecoderReverse`.

### 5. Auto mark didn't track live radio changes
`rttyMarkChanged` / `diglOffsetChanged` updated the spectrum overlay but not the decoder.  
**Fix:** Wire both signals to call `refreshRttyDecodeState()` when the active slice changes.

### 6. Decoder started with no applet
`refreshRttyDecodeState` could call `start()` before `m_rttyDecoderApplet` was set, using stale default params.  
**Fix:** Guard — return early if `m_rttyDecoderApplet` is null.

### 7. FIGS shift state clobbered on param change
Changing baud/mark reset `figsMode` to LTRS mid-stream.  
**Fix:** Preserve `figsMode` across param changes; only reset filter and clock state.

---

## Tested Against

- 2125/1955 Hz, 45.45 baud, Rev=false (standard HF USB RTTY)
- 2125/2295 Hz, 45.45 baud, Rev=true (high-space / LSB)
- 915/745 Hz, 45.45 baud, Rev=false
- 50 baud synthetic signal
