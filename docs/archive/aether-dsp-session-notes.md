# AetherDSP Settings + NR4 — Session Notes

Development notes from the `feature/aether-dsp-settings` branch.
Preserved for context if this work needs to be picked up again.

## Suggested Release Notes

### New Features
- **NR4 noise reduction** — new client-side spectral noise reduction powered
  by libspecbleach. Offers 7 tunable parameters including reduction depth,
  smoothing, whitening, masking, and three noise estimation algorithms.
  Enable from the VFO DSP tab or spectrum overlay.
- **AetherDSP Settings dialog** — new settings dialog for fine-tuning NR2
  and NR4 client-side noise reduction parameters. Access from the Settings
  menu or right-click any NR2/NR4 button.
- **DSP tooltips** — all DSP buttons, sliders, and receiver controls now
  show helpful descriptions on hover. No more guessing what NRL, NRS, ANFT,
  or NRF do.

### Improvements
- NR2 parameters (gain method, NPE method, AE filter, reduction depth,
  smoothing, voice threshold) are now user-configurable and persist across
  sessions.
- Keyboard shortcut NR cycle extended: Off -> NR -> NR2 -> NR4 -> Off.
- NR4 integrates with per-band settings, MIDI mapping, and digital mode
  auto-disable.

## What We Learned

### libspecbleach Integration
- libspecbleach uses FFTW3 float precision (`fftwf`), same as our NR2 — no
  new dependency needed.
- The library processes mono float audio; our pipeline is stereo int16 at
  24 kHz. The SpecbleachFilter wrapper handles the conversion
  (stereo -> mono -> process -> mono -> stereo).
- 40ms frame size at 24 kHz = 960 samples per frame, introducing ~40ms
  latency (acceptable for RX audio).
- Parameters are applied via a dirty flag pattern: main thread sets atomics,
  audio thread checks and applies on next `process()` call.

### NR4 UI Integration Patterns
- NR2/RN2/BNR all follow the same 3-layer pattern: overlay button -> VFO
  button -> MainWindow sync lambda. NR4 copies this exactly.
- Right-click popup (DspParamPopup) is a reusable generic builder — just
  call `addSlider()`, `addCheckbox()`, `addRadioGroup()` and `finalize()`.
- The AetherDspDialog wiring is duplicated in 3 places (Settings menu, NR2
  right-click "More...", NR4 right-click "More..."). A helper method would
  reduce this, but matches existing patterns.

### HAVE_SPECBLEACH Guard Strategy
- Initially guarded NR4 AudioEngine methods with `#ifdef HAVE_SPECBLEACH`,
  but this caused compilation issues when calling from unguarded MainWindow
  code.
- Solution: make the API always available with stub no-ops when not compiled,
  matching the BNR pattern. Only the `SpecbleachFilter` unique_ptr member is
  guarded.
- libspecbleach is bundled (not external), so the guard is mostly for
  completeness.

### Tooltip Research
- Thetis has ~200 DSP tooltips in `setup.designer.cs` and `console.resx` —
  comprehensive but often too terse ("Noise Blanker") or too technical.
- SmartSDR documentation from FlexRadio community is excellent for
  understanding *what each feature does* and *when to use it*.
- Best tooltip style: start with what it does in plain English, optionally
  add when/why. 1-2 sentences max.
- `\u2014` (em dash) works in Qt tooltips for readable formatting.

### Mutual Exclusion
- NR2, RN2, BNR, and NR4 are all mutually exclusive client-side DSP filters.
  Each `setXxxEnabled(true)` disables the other three.
- The radio-side NR is independent — it can run alongside any client-side
  filter.
- Digital modes (DIGU/DIGL/RTTY) auto-disable all client-side DSP to avoid
  corrupting DAX data streams.

## Key Files Modified

| File | Changes |
|------|---------|
| `src/core/SpecbleachFilter.h/.cpp` | New — libspecbleach wrapper |
| `src/core/AudioEngine.h/.cpp` | NR4 enable/disable, 7 param setters, mutual exclusion, stubs |
| `src/gui/AetherDspDialog.h/.cpp` | New — settings dialog with NR2 + NR4 tabs |
| `src/gui/DspParamPopup.h/.cpp` | New — right-click parameter popup builder |
| `src/gui/MainWindow.h/.cpp` | NR4 wiring, sync, persistence, MIDI, shortcuts, tooltips popup |
| `src/gui/SpectrumOverlayMenu.h/.cpp` | NR4 button (idx 12), signals, tooltips |
| `src/gui/VfoWidget.h/.cpp` | NR4 button, signals, tooltips |
| `src/gui/RxApplet.cpp` | Receiver control tooltips |
| `third_party/libspecbleach/` | Bundled library (LGPL-2.1) |
| `CMakeLists.txt` | ENABLE_SPECBLEACH option, libspecbleach sources |
