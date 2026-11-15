# Bridge-fit scoring rubric

Score each candidate on how completely the **design → code → prove** lifecycle
can run, where *prove* uses the AetherSDR agent automation bridge. Hand this file
(plus the issues JSON and an index range) to each scoring subagent.

## Three axes, 1–5 each

- **design_clarity** — is the root cause / desired behavior obvious and small?
  (5 = trivial and unambiguous; 1 = vague, needs investigation or a design RFC)
- **code_size** — how little code does the fix likely touch?
  (5 = a few lines / one TU; 1 = cross-cutting or architectural)
- **provability** — can the bridge *visibly* demonstrate before vs after?
  (5 = clean GUI/control/meter proof; 1 = not observable through the bridge)

`total = sum`. Assign `bridge_fit: high|med|low`. Reserve **high** for clear,
small GUI/control/meter bugs or regressions the bridge can prove end-to-end.

## What the bridge CAN prove (provable)

- `dumpTree` / `get` properties / `grab` (and `grab pan <i>`) screenshots
- `invoke` controls (buttons, menu actions, sliders) and observe resulting state
- `submit` line-edit commits; `drag` slider handles / resize grips / scale drags
- `showMenu` dropdowns; `menu open` + `invoke "<label>" trigger` for dialogs
- `close` windows; `pan add` / `pan close` lifecycle (deterministic crash repro)
- `slice tx|txant|rxant`, `key ptt|mox`, `cwx send` — **TX-keying requires explicit authorization and a verified dummy-load route**
- `floors` / `displayFloorDbm` FFT-floor reads; meters via `get`

## What the bridge CANNOT prove well — screen these out hard

These are the buckets that quietly sink a "fix today" plan. Mark `low` unless a
new verb genuinely changes the calculus:

- **audio / DSP fidelity** — needs ears or a spectrum analyzer, not a screenshot
  (NR quality, AGC heuristics, modulation, sidetone, audio-path gain staging)
- **timing / protocol races** — sub-second, non-deterministic ordering
- **specific external hardware we don't have** — USB mic/PTT, MIDI, FlexControl,
  PowerMate, RC-28, Maestro, LDG/TGXL/PGXL amps, Antenna Genius, Ulanzi, HID pads
- **multi-radio / MultiFLEX capacity** scenarios
- **CI / build / packaging / dependency / sanitizer infra** — no GUI surface
- **large new features** with broad design surface (a feature is not a papercut)

## Output: one JSON line per issue

```json
{"number":3505,"kind":"bug|regression|ux|feature|infra","design_clarity":4,
 "code_size":4,"provability":4,"total":12,"bridge_fit":"high|med|low",
 "one_line":"<=15-word why, naming the bridge verb that proves it"}
```

Keep `one_line` concrete and verb-anchored — "DIGU→USB shouldn't enable squelch;
mode `invoke` + `get` squelch" beats "squelch bug, should be quick".
