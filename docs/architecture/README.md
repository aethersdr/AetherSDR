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

Code-level reviewers should also skim the corresponding header files
in `src/core/` and `src/models/`.
