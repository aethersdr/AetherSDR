# Feasibility Report: Issue #2136 — Protocol v4.2.18 Waveform Sub-Shape Parser

**Date:** 2026-05-16
**Status:** Completed — implemented as `FlexWaveformModel` on branch `feat/flex-waveform-model`

---

## Summary

FlexRadio firmware v4.2.18 introduced three `waveform` status sub-shapes that AetherSDR
had no handler for. This report assessed the feasibility of adding protocol compliance.

**Verdict:** Straightforward. Medium-low effort, low risk. The parser already disambiguated
the three sub-types correctly; the only work was adding a model and dispatch.

---

## Current State at Time of Analysis

Zero handling existed. A global grep of `src/` confirmed no branch in
`RadioModel::onStatusReceived` matched any `waveform*` object — messages fell through
silently. The `WaveformWidget.cpp` and `StripWaveform.cpp` files in `src/gui/` are
unrelated (they render audio waveforms in the WAVE applet). The naming risk was
mitigated by using the `FlexWaveform` prefix for all new protocol-side identifiers.

---

## Parser Behavior

`CommandParser::parseLine` splits on the last space before the first `=`. For the three
v4.2.18 sub-shapes this produces three distinct `object` strings with no parser changes
required:

| Wire message | `object` | `kvs` |
|---|---|---|
| `waveform installed_list=FREEDV\x7Fv1,RTTY\x7Fv2` | `"waveform"` | `{installed_list: "..."}` |
| `waveform container name=foo version=1.0 removed` | `"waveform container"` | `{name, version, removed: ""}` |
| `waveform wfp_status power=on ready=true ipaddr=…` | `"waveform wfp_status"` | `{power, ready, ipaddr}` |

---

## Wire Formats — Verified Against FlexLib v4.2.18.41174

All formats were verified directly from FlexLib source at
`~/FlexLib/FlexLib_API_v4.2.18.41174/`.

### Shape 1 — `installed_list` (Radio.cs `UpdateWaveformsInstalledList`, line 11162)

```
S<handle>|waveform installed_list=<name1><ver1>,<name2><ver2>,...
```

- Entries separated by `,`
- Name/version within each entry separated by `` (DEL, ASCII 127)
- Empty entries logged and skipped — guard required

### Shape 2 — `container` (Radio.cs `DockerUpdateWaveformList`, line 11188)

```
S<handle>|waveform container name=<name> version=<ver> [removed]
```

- `removed` is a **bare-word key with no `=` value**
- Detection: `kvs.contains("removed")` — same pattern as `display pan removed` at
  RadioModel.cpp line 3577

### Shape 3 — `wfp_status` (Radio.cs `ParseWfpStatus`, line 11292)

```
S<handle>|waveform wfp_status power=<on|off> ready=<true|false> ipaddr=<ip>
```

- `power`: case-insensitive compare to `"on"` → bool
- `ready`: case-insensitive compare to `"true"` → bool
- `ipaddr`: raw string

---

## Subscription

`sub waveform all` is required — confirmed at FlexLib Radio.cs line 2301. Waveform
status is not broadcast automatically without a subscription.

---

## Commands — Exact Wire Strings (Radio.cs lines 8484–8499)

```
waveform uninstall <name>         — remove legacy (non-Docker) waveform
waveform remove_container <name>  — remove Docker container waveform
waveform restart <name>           — restart Docker waveform
```

---

## Data Model (from Waveform.cs)

FlexLib's `Waveform` is a record with `Name`, `Version`, `IsContainer`, and a computed
`DisplayName`. WFP status properties: `IsWfpPowered` (bool), `IsWfpReady` (bool),
`WfpIPAddress` (string).

---

## CODEOWNERS Impact

| File | Tier | Required Approver |
|------|------|-------------------|
| `src/models/FlexWaveformModel.{h,cpp}` | Tier 1 (default) | @ten9876 or @jensenpat |
| `src/models/RadioModel.{h,cpp}` | Tier 1 (default) | @ten9876 or @jensenpat |
| `CMakeLists.txt` | **Tier 3 (maintainer-only)** | **@ten9876 only** |
| `tests/flex_waveform_model_test.cpp` | Tier 2 (bot-approvable) | @AetherClaude or above |

Note: CODEOWNERS protects `src/core/RadioModel.{h,cpp}` (stale path from earlier layout).
The actual files are in `src/models/` — Tier 1 only.

---

## Risk Assessment

| Dimension | Assessment |
|---|---|
| Protocol risk | Low — parser correct, messages were benign-ignored |
| Code complexity | Low — follows TnfModel template exactly |
| Regression risk | Low — new dispatch blocks, no existing code modified structurally |
| Blocker at analysis time | None after FlexLib subscription confirmed |
| Estimated effort | ~2–3 hours |
