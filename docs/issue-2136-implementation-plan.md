# Implementation Plan: Issue #2136 — FlexWaveformModel

**Date:** 2026-05-16
**Branch:** `feat/flex-waveform-model`
**Implemented:** Yes

---

## Objective

Add protocol compliance for the three `waveform` status sub-shapes introduced in
FlexRadio firmware v4.2.18. Data model only — no UI. `FlexWaveform` prefix used
throughout to avoid naming collision with the existing audio waveform visualization
classes (`WaveformWidget`, `StripWaveform`).

See also: [issue-2136-feasibility-report.md](issue-2136-feasibility-report.md)

---

## File Manifest

| File | Action | CODEOWNERS Tier |
|------|--------|-----------------|
| `src/models/FlexWaveformModel.h` | NEW | Tier 1 |
| `src/models/FlexWaveformModel.cpp` | NEW | Tier 1 |
| `src/models/RadioModel.h` | MODIFY | Tier 1 |
| `src/models/RadioModel.cpp` | MODIFY | Tier 1 |
| `CMakeLists.txt` | MODIFY | **Tier 3 (@ten9876 only)** |
| `tests/flex_waveform_model_test.cpp` | NEW | Tier 2 (bot-approvable) |

---

## FlexWaveformModel.h

```
struct FlexWaveformEntry {
    QString name;
    QString version;
    bool    isContainer{false};
    QString displayName() const;   // "Name Version" or "Name" if version empty
};

class FlexWaveformModel : public QObject {
    // Accessors: waveforms(), wfpPowered(), wfpReady(), wfpIpAddress()
    // Status: handleInstalledList(), handleContainerStatus(), handleWfpStatus()
    // Commands: requestUninstall(), requestRemoveContainer(), requestRestart()
    // Lifecycle: clear()
    // Signals: waveformsChanged(), wfpStatusChanged(), commandReady(QString)
};
```

---

## FlexWaveformModel.cpp

- `handleInstalledList`: split `kvs["installed_list"]` on `,` → entries;
  split each on `QChar(0x7F)` (DEL/ASCII 127) → `[name, version]`;
  guard `tokens.size() != 2`; replace non-container entries; emit `waveformsChanged()`
- `handleContainerStatus`: extract name/version; `kvs.contains("removed")` →
  remove entry, else add/update; emit `waveformsChanged()`
- `handleWfpStatus`: map power/ready (case-insensitive)/ipaddr; emit `wfpStatusChanged()`
- Commands emit `commandReady("waveform uninstall/remove_container/restart " + name)`
- `clear()`: reset all state, emit both signals

---

## RadioModel.h Changes (3 locations)

1. `#include "FlexWaveformModel.h"` with other model includes
2. `FlexWaveformModel& flexWaveformModel()` accessor with other sub-model accessors
3. `FlexWaveformModel m_flexWaveformModel;` private member alongside other sub-models

---

## RadioModel.cpp Changes (4 locations)

1. **Constructor** (after navtexModel commandReady connect):
   wire `m_flexWaveformModel.commandReady` → `sendCmd(cmd)`

2. **Subscription sequence** (after `sub spot all`):
   `sendCmd("sub waveform all");`
   Source: FlexLib Radio.cs:2301

3. **Disconnect cleanup** (after `m_tnfModel.clear()`):
   `m_flexWaveformModel.clear();`

4. **`onStatusReceived()`** (end of dispatch chain, after TNF/radio blocks):
   ```cpp
   if (object == QLatin1String("waveform")) { … handleInstalledList … }
   if (object == QLatin1String("waveform container")) { … handleContainerStatus … }
   if (object == QLatin1String("waveform wfp_status")) { … handleWfpStatus … }
   ```

---

## CMakeLists.txt

Add `src/models/FlexWaveformModel.cpp` to `MODEL_SOURCES` (after NavtexModel.cpp).

Add `flex_waveform_model_test` executable (after navtex_model_test block).

---

## Tests (flex_waveform_model_test.cpp)

Five test cases:

| Test | Validates |
|------|-----------|
| `installedListTwoEntries` | Split on `,` and DEL; populates non-container waveforms |
| `installedListEmptyEntry` | Empty token skipped without crash |
| `containerAdd` | name + version added with `isContainer = true` |
| `containerRemove` | bare-word `removed` key removes correct entry |
| `wfpStatus` | `power=on` → true; `ready=false` → false; ipaddr stored verbatim |

---

## Implementation Flow Executed

1. `git checkout main`
2. `git pull origin main` — picked up 4-file update (TransmitModel changes)
3. `git checkout -b feat/flex-waveform-model`
4. Wrote all 6 files per plan
5. `cmake --build build -j$(nproc)` — verified clean
6. Audited build output — no warnings from our files
7. Squash all commits to one
8. `git push fork feat/flex-waveform-model`
9. PR opened against `ten9876/MasterSDR`
