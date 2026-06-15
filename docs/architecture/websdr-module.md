# WebSDR Receive Module — Concept

Status: **proposal / pre-implementation**. Branch: `feat/websdr-module`.
Author: Jan (svabi79 fork). This document is the design we agreed before writing
code. It is intentionally fork-local and optional — see *Scope & upstream*.

## 1. Goal

Add a public [WebSDR](https://www.websdr.org/) (PA3FWM software) as a secondary,
**listen-only** receive source inside AetherSDR — "another antenna for the ears."
A small live waterfall for the WebSDR shows up in a corner / detachable window,
and the user can switch the speaker audio over to it. The connected Flex radio
stays the authority and master at all times.

### Non-goals (explicit)

- **No TX** through the WebSDR. Receive only.
- **No control of the Flex** by the module, and **no Flex model writes**
  (`RadioModel` / `SliceModel` / command bus are off-limits).
- **No overlay** on the Flex panadapter and **no reuse** of the Flex
  `SpectrumWidget` / `PanadapterModel`.
- Not a general multi-source SDR abstraction. One Flex + one WebSDR panel.

## 2. Guiding principle — Flex is master, WebSDR is a passive bolt-on

> The WebSDR module only **reads** from its own WebSocket and **owns** three
> things: its socket, its own waterfall widget, and (optionally, while switched
> on) the audio sink. It never writes into the Flex model, never sends Flex
> commands, never participates in TX.

This keeps the module a true add-on, keeps upstream divergence minimal, and makes
it impossible for the WebSDR path to disturb the Flex master. It is also what
makes the waterfall cheap: by rendering into its **own** widget with its **own**
data we avoid the Flex-coupled spectrum/model layer entirely.

## 3. Placement decision

| Option | Decision | Reason |
|--------|----------|--------|
| Overlay on the Flex panadapter | **Rejected** | `SpectrumWidget` is a `QRhiWidget` GPU hot path; the WebSDR band span ≠ Flex pan span, so two independent frequency axes would have to be reconciled on the GPU path. High risk, confusing UX. |
| Mini-waterfall docked in a corner | **Chosen (default)** | Self-contained widget, own data, touches nothing Flex-side. Low friction, always visible. |
| Separate / detachable window | **Chosen (same impl)** | AetherSDR already has detachable pop-out windows for pans. The dock panel reuses that detach pattern, so "corner *or* own window" is a runtime user choice, not a second implementation. |

**Result:** a `WebSdrPanel` implemented as a `QDockWidget` with a simple
self-rendered mini-waterfall, poppable into its own window via the existing
detach mechanism. Rendering uses plain `QImage`/`QPainter` (a mini view does not
need the QRhi GPU pipeline).

## 4. Components

```
        ws://host:8901/~~stream?v=11           (audio)
        ws://host:8901/~~<waterfall path>      (FFT/waterfall — see §7 spike)
                     │
              ┌──────┴───────────────┐   ◄── new WEBSDR worker thread
              │   WebSdrSource        │       (QWebSocket, moveToThread,
              │   - QWebSocket(s)     │        mirrors FreeDvClient pattern)
              │   - WebSdrAudioDecoder│
              │   - WebSdrWfDecoder   │
              └───┬───────────────┬───┘
                  │ audioReady     │ waterfallRowReady   [auto-queued signals]
                  ▼ (24k st f32)   ▼
        AudioEngine.feedWebSdr…   WebSdrPanel.updateWfRow()   [MAIN]
        (source gate)                  │
                  ▼                    ▼ paintEvent (QImage)
              QAudioSink           mini waterfall in dock / pop-out
```

### New classes

- **`WebSdrSource`** (`src/core/`): owns the `QWebSocket`(s), sends the
  `GET /~~param?…` tune command, drives the decoders, lives on a dedicated
  worker thread created with the established `moveToThread` + `init()` slot
  pattern. Copy structure from `FreeDvClient` (already QWebSocket-based).
- **`WebSdrAudioDecoder`** (`src/core/`): C++ port of the verified decoder
  (a-law table + adaptive-predictor codec). Reference implementation and protocol
  notes live in the standalone `websdr-client` repo (`websdr_client.py`,
  `PROTOCOL.md`). ~100 lines.
- **`WebSdrWfDecoder`** (`src/core/`): parses the WebSDR waterfall/FFT stream
  into spectrum rows. **Depends on the §7 spike.**
- **`WebSdrPanel`** (`src/gui/`): `QDockWidget` with the mini-waterfall + a small
  control header (host, frequency, mode, connect, "audio → here" toggle).
- **`WebSdrSettings`** (`src/core/`): persisted host list, last freq/mode,
  panel visibility/geometry.

### Touched existing code (kept minimal)

- **`AudioEngine`** — add a receive-audio **source gate**: Flex RX (current
  default) ↔ WebSDR. Today `feedAudioData()` is fed only by
  `PanadapterStream::audioDataReady()` and `startRxStream()` is tied to the Flex
  connect lifecycle. We add a way to (a) accept WebSDR PCM frames and (b) mute /
  bypass the Flex feed while WebSDR is selected. One source at a time initially;
  mixing is a later option. *This is the only non-trivial edit in the
  audio-only milestone.*
- **`MainWindow`** — register/show the dock widget; menu/toolbar entry.
- **`CMakeLists.txt`** — new sources behind an optional `HAVE_WEBSDR` flag,
  reusing the existing `Qt6::WebSockets` dependency.

## 5. Audio path

- Internal pipeline format is **24 kHz stereo float32** (`AudioEngine`
  `DEFAULT_SAMPLE_RATE = 24000`). WebSDR audio is mono at ~7–12 kHz.
- Conversion in `WebSdrSource`: decode → resample to 24 kHz (reuse
  `Resampler`) → duplicate mono to stereo → emit `webSdrAudioReady(QByteArray)`.
- `AudioEngine` gate:
  - **Flex (default):** unchanged; WebSDR frames are dropped.
  - **WebSDR:** Flex `feedAudioData()` input is muted/bypassed and WebSDR frames
    are routed into the same downstream DSP/`QAudioSink` path.
- Switching is a user toggle on the panel; default and fallback is always Flex.
  Disconnecting or losing the WebSDR returns audio to Flex.

## 6. Waterfall path

- Rendered entirely in `WebSdrPanel` from `WebSdrWfDecoder` rows. **No** contact
  with `SpectrumWidget` / `PanadapterModel`.
- Mini view: `QImage` ring-buffer waterfall + a thin spectrum line, repainted on
  `waterfallRowReady`. Click-to-tune within the WebSDR span maps to a new
  `~~param` command (module-internal; never touches Flex tuning).
- Band scale labels can come from the WebSDR `bandinfo.js` (`centerfreq`,
  `samplerate`, per-band scale PNGs) fetched once on connect.

## 7. R&D — resolved

The audio protocol was already reverse-engineered (see
`websdr-client/PROTOCOL.md`). The **waterfall stream is now decoded too** (M0
spike done): a second WebSocket `~~waterstream<band>?format=…`, format-1 rows of
2 palette-indexed pixels/byte, with `0xFF`-prefixed meta frames, controlled via
`~~waterparam?…`. Full details and the palette are in the technical spec,
[`websdr-module-spec.md`](websdr-module-spec.md) §1.2, and in
`websdr-client/PROTOCOL.md`. No remaining unknowns block implementation.

## 8. Milestones & effort

| Milestone | Deliverable | Effort |
|-----------|-------------|--------|
| **M1 — Audio module** | `WebSdrSource` + `WebSdrAudioDecoder` on a worker thread; dock panel header (host/freq/mode/connect); audio source gate in `AudioEngine`; switchable, Flex stays default. No waterfall. | ~3–5 days |
| **M0 — Waterfall spike** | ✅ done — see `websdr-module-spec.md` §1.2. | — |
| **M2 — Mini-waterfall** | `WebSdrWfDecoder` + self-rendered waterfall in `WebSdrPanel`, detach-to-window, click-to-tune. | ~2–3 days on top of M0 |

Total to "switchable audio + mini-waterfall in a corner / own window":
**~6–10 days**, of which ~1–2 are pure reverse-engineering.

## 9. Risks

1. **`AudioEngine` is single-source and Flex-lifecycle-bound** — the source gate
   is the only sensitive edit; must not regress the Flex RX path.
2. **Waterfall protocol unknown** until the M0 spike (§7).
3. **WebSDR etiquette/ToS** — public servers; the module must keep sessions sane
   (no reconnect storms), expose the host so the user picks a tolerant instance,
   and document "prefer your own WebSDR install."
4. **Upstream scope** — see below.

## 10. Scope & upstream

This is a fork-local feature behind an optional `HAVE_WEBSDR` build flag. Given
the strict aethersdr contribution process and the Flex-centric product focus, it
is **not** assumed to be upstream-bound; it ships in the `svabi79` fork as an
optional module. Revisit upstreaming only if there is maintainer interest.

## 11. Reference

- Standalone decoder + verified protocol: `websdr-client` repo
  (`websdr_client.py`, `PROTOCOL.md`) — the audio codec, frame types and
  `~~param` tuning command, validated against a live capture.
- AetherSDR audio injection contract: `src/core/AudioEngine.h` (`feedAudioData`,
  `startRxStream`), `docs/architecture/audio-pipeline.md`.
- QWebSocket worker pattern to copy: `src/core/FreeDvClient.{h,cpp}`.
