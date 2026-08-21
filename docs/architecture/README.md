# docs/architecture/

Engineering deep-dives describing how AetherSDR's internal pipelines,
data formats, and platform integrations work. Read these when you're
about to modify the relevant subsystem; they capture the kind of
context that would otherwise live in tribal knowledge.

- [`pipelines.md`](pipelines.md) — thread topology, data flow,
  cross-thread signal map, GPU rendering notes.
- [`audio-pipeline.md`](audio-pipeline.md) — the client RX/TX audio
  graph end to end.
- [`tx-audio-signal-path.md`](tx-audio-signal-path.md) — client-side
  TX DSP stages and how they reach the radio's firmware.
- [`websdr-sourced-slice.md`](websdr-sourced-slice.md) — design for a
  VFO/slice that takes audio from a WebSDR feed instead of the radio,
  the RX-antenna-menu UX, and the aux-source / DSP-routing split.
- [`flex-meter-learnings.md`](flex-meter-learnings.md) — capture-backed
  notes on the radio's 15 transmit meters across firmware revisions.
- [`vita49-format.md`](vita49-format.md) — VITA-49 packet layout for
  panadapter, waterfall, audio, and meter streams.
- [`tci-discovery.md`](tci-discovery.md),
  [`tci-receivers.md`](tci-receivers.md) — ExpertSDR3 TCI v2.0
  integration notes.
- [`multi-pan-pitfalls.md`](multi-pan-pitfalls.md) — 20 numbered
  lessons learned from the multi-panadapter rollout.
- [`recenter-policy.md`](recenter-policy.md) — when AetherSDR re-centers
  the panadapter view on a slice change.
- [`mainwindow-decomposition.md`](mainwindow-decomposition.md) — the
  `MainWindow_*.cpp` TU map and a decision guide for where new
  `MainWindow` code belongs (read before touching anything `MainWindow*`).
- [`acom-600s-amplifier-design.md`](acom-600s-amplifier-design.md) — design
  note for ACOM S-series amplifier support (serial or ser2net), a peripheral
  accessory alongside the existing PGXL/TGXL integrations. Implemented as a
  dedicated `AcomConnection`/`AcomApplet` pair, deliberately independent of
  `AmpModel`/`AmpApplet` — see the doc's design-reversal section for why.
- [`spe-expert-amplifier-design.md`](spe-expert-amplifier-design.md) — design
  note for SPE Expert amplifier support (1.3K-FA/1.5K-FA/2K-FA, serial or
  ser2net), the second peripheral amplifier following the ACOM precedent:
  a dedicated `SpeConnection`/`SpeApplet` pair with a polled ASCII/CSV
  status protocol.

- [`radio-capabilities-map.md`](radio-capabilities-map.md) — every
  `RadioCapabilities` field, what each backend declares, and where the value is
  read. Read before adding a field: the struct defaults to `false`, so a
  backend that omits one silently declares the feature absent. Also records the
  fields nothing reads yet, including the `maxSlices`/`maxPanadapters` bypass.

- [`aetherd-hl2-backend-design.md`](aetherd-hl2-backend-design.md) — the design
  note for the Hermes-Lite 2 backend: what the Python spike proved and how it
  was ported behind the `IRadioBackend` seam. HL2 ships raw IQ and nothing else,
  so this is the first backend that owns an engine-side WDSP chain.
- [`hl2-multi-ddc-test-matrix.md`](hl2-multi-ddc-test-matrix.md) — the manual
  test matrix for running up to four independent DDCs on one HL2, marked by who
  can answer each row (simulator / unit test / real hardware). Re-run the `HW`
  rows after any refactor of the receiver index space.

The HL2 bring-up narrative itself is [`docs/HERMES.md`](../HERMES.md) — read §15
and §5 before touching `src/core/backends/hl2/`. The Python probes that
predate the backend live in `tools/hl2/`.

Code-level reviewers should also skim the corresponding header files
in `src/core/` and `src/models/`.
