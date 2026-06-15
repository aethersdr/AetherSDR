# WebSDR Receive Module — Technical Specification

Status: **implementation-ready**. Branch: `feat/websdr-module`.
Companion to [`websdr-module.md`](websdr-module.md) (concept & rationale). This
document is the buildable spec: protocol, class interfaces, signals, threading,
integration points, settings, and acceptance criteria. All protocol facts below
were reverse-engineered from a live PA3FWM WebSDR instance and validated; the
audio codec additionally has a reference implementation in the `websdr-client`
repo.

---

## 1. Protocol reference

All public WebSDR servers run the same software; only host/port and band layout
differ. Two independent WebSockets, both on the same host:port as the HTTP UI.

### 1.1 Audio stream — `ws://<host>/~~stream?v=11`

- `binaryType = arraybuffer`. Tuning command (text frame):
  `GET /~~param?f=<kHz>&band=<idx>&lo=<kHz>&hi=<kHz>&mode=<m>&name=<str>`
- Mode integer: `0` = SSB/CW (USB/LSB/CW differ only via `lo`/`hi`), `1` = AM,
  `4` = FM.
- Binary frames are type-byte routed: `0xF0–0xFF` S-meter, `0x80` a-law block
  (128 samples), `0x90–0xDF` and `0x00–0x7F` compressed adaptive-predictor audio,
  `0x81` sample rate, `0x82` quantiser scale, `0x83` filter/mode, `0x84` silence,
  `0x85` true frequency. Full codec + frame layout: `websdr-client/PROTOCOL.md`
  and the C++ port spec in §3.2.
- Native audio rate `D` is signalled by `0x81` (commonly 7–12 kHz), mono.

### 1.2 Waterfall stream — `ws://<host>/~~waterstream<band>?format=<n>&width=<w>&zoom=<z>&start=<s>`

- Note: band index is part of the **path** (`~~waterstream0`, `~~waterstream1`…).
- `binaryType = arraybuffer`. Control command (text frame):
  `GET /~~waterparam?zoom=<z>&scale=<s>&slow=<n>&width=<w>&band=<idx>&start=<x>`
- We request **`format=1`** — the simplest row encoding: each payload byte
  produces **two** horizontal pixels as palette indices:
  - `pixel[2*e]   = 16*(byte & 0x0F) + 2`
  - `pixel[2*e+1] = byte & 0xF2`
- Rows are up to 1024 px wide; `width` sets the actual count. One binary message
  ≈ one waterfall row (newest line).
- **Meta frames** begin with byte `0xFF`:
  - `0xFF 0x01 …`: scale/reference. `freq32 = b[3] | b[4]<<8 | b[5]<<16 | b[6]<<24`;
    if `b[2] < 128`, `b[2]` is a level/scale hint; row payload resumes at byte 8.
  - `0xFF 0x02 …`: `count = b[2] | b[3]<<8` leading pixels are blanked (zeroed).
  - `0xFF 0xFF …`: escaped literal `0xFF` — strip one byte, the rest is row data.
  - A row not starting with `0xFF` is raw row data from byte 0.
- **Palette** (index 0–255 → RGB), replicated verbatim (8-bit wraparound as in the
  original `Uint8Array`):

  ```
  e in   0..63 : R=0,           G=0,                       B=2*e
  e in  64..127: R=3*e-192,     G=0,                       B=2*e
  e in 128..191: R=e+64,        G=256*sqrt((e-128)/64),    B=511-2*e
  e in 192..255: R=255,         G=255,                     B=512+2*e
  (all components stored into a uint8 → take & 0xFF)
  ```

> Format 9 (compressed) exists but is **out of scope**; we always request
> `format=1`.

### 1.3 Band metadata — `http://<host>/tmp/bandinfo.js`

JS literal: `nbands`, `ini_freq`, `ini_mode`, and `bandinfo[]` with
`{ centerfreq, samplerate (kHz), tuningstep, vfo, maxzoom, name, scaleimgs[] }`.
Band selection: pick the band whose `[centerfreq ± samplerate/2]` contains the
requested frequency. Parsed once on connect.

---

## 2. Module overview

```
   ws ~~stream (audio)      ws ~~waterstream<band> (rows)
            │                          │
   ┌────────┴──────────────────────────┴────────┐  ◄── WEBSDR worker thread
   │                WebSdrSource                  │      (moveToThread + init())
   │  QWebSocket m_audioWs   QWebSocket m_wfWs     │
   │  WebSdrAudioDecoder     WebSdrWaterfallDecoder│
   │  state machine, reconnect backoff             │
   └───┬───────────────────────────────┬──────────┘
       │ webSdrAudioReady(QByteArray)   │ webSdrRowReady(WebSdrRow)   [auto-queued]
       │ webSdrStateChanged(State)      │ webSdrSMeter(int)
       ▼                                ▼
  AudioEngine (source gate)        WebSdrPanel (QDockWidget)        [MAIN]
       ▼                                ▼ paintEvent (QImage waterfall)
   QAudioSink                      corner dock / detached window
```

- **Threading:** one dedicated worker thread (`m_webSdrThread`), created with the
  established `moveToThread` + `init()` slot pattern (see `RadioConnection`,
  `FreeDvClient`). Both `QWebSocket`s and both decoders live on it. All output is
  via auto-queued signals to MAIN (panel) and to the AUDIO thread (audio).
- **Authority:** the module is strictly read-only toward Flex. It never calls
  `RadioModel`/`SliceModel`, never emits `commandReady`, never touches TX.

---

## 3. New classes

### 3.1 `WebSdrSource` — `src/core/WebSdrSource.{h,cpp}`

```cpp
class WebSdrSource : public QObject {
    Q_OBJECT
public:
    enum class State { Disconnected, Connecting, Connected, Streaming, Error };

    explicit WebSdrSource(QObject* parent = nullptr);

    struct BandInfo { double centerFreqKHz; double sampleRateKHz; QString name; };

public slots:
    void init();                          // runs on worker thread; creates sockets
    void connectToServer(const QString& host /*host:port*/);
    void disconnectFromServer();
    void tune(double freqKHz, const QString& mode);  // mode: usb/lsb/cw/cwu/cwl/am/fm
    void setWaterfallWidth(int px);       // ≤ 1024
    void setZoom(int zoom);               // 0..maxzoom for the band

signals:
    void stateChanged(WebSdrSource::State, const QString& detail);
    void bandsResolved(const QVector<WebSdrSource::BandInfo>&, int selectedBand);
    void audioReady(const QByteArray& pcm24kStereoF32);   // → AudioEngine
    void rowReady(const WebSdrRow& row);                  // → WebSdrPanel
    void sMeter(int dbm);

private:
    // m_audioWs (~~stream?v=11), m_wfWs (~~waterstream<band>...)
    // m_audioDec, m_wfDec; m_resampler; reconnect QTimer w/ exponential backoff
    // current band/freq/mode/zoom; bandinfo cache
};
```

- **State machine:** `Disconnected → Connecting → Connected` (sockets open,
  bandinfo fetched) `→ Streaming` (first audio/row decoded). Any socket error →
  `Error`, then a single reconnect attempt with **exponential backoff**
  (2s, 4s, 8s, capped 30s) — *no reconnect storms* (etiquette, §8).
- `tune()` sends `GET /~~param?…` on the audio socket and, if the band changed,
  reopens the waterfall socket on the new `~~waterstream<band>` path.
- `connectToServer()` fetches `bandinfo.js` (QNetworkAccessManager) before opening
  the waterfall socket; emits `bandsResolved`.

### 3.2 `WebSdrAudioDecoder` — `src/core/WebSdrAudioDecoder.{h,cpp}`

- Direct C++ port of the verified decoder (`websdr-client/websdr_client.py`,
  class `WebsdrDecoder`): 256-entry a-law table + 20-tap adaptive predictor,
  persistent state (`coeff[20]`, `hist[20]`, `ie`, `re`, `oe`, `ae`, `D`).
- API:
  ```cpp
  void feed(const QByteArray& frame);   // decode one binary WS message
  QVector<int16_t> takeSamples();       // drained Int16 @ nativeRate()
  int  nativeRate() const;              // D (Hz)
  int  sMeter() const;
  ```
- Pure DSP, no Qt threading concerns; owned by `WebSdrSource`. Unit-testable
  against captured frames (§9).

### 3.3 `WebSdrWaterfallDecoder` — `src/core/WebSdrWaterfallDecoder.{h,cpp}`

```cpp
struct WebSdrRow {
    QVector<QRgb> pixels;     // already palette-mapped, width px
    quint32 freqRef = 0;      // from 0xFF 0x01 meta (optional)
};

class WebSdrWaterfallDecoder {
public:
    void setWidth(int px);
    bool feed(const QByteArray& frame, WebSdrRow& out);  // true if a row produced
private:
    static QRgb palette(int idx);   // the §1.2 formulas, precomputed LUT[256]
};
```

- Implements format-1 decode + the three meta-frame cases. Palette is a static
  `QRgb[256]` LUT built once from the §1.2 formulas.

### 3.4 `WebSdrPanel` — `src/gui/WebSdrPanel.{h,cpp}`

- A `QDockWidget` (default docked bottom-right; floatable/detachable via the
  standard `QDockWidget` feature flags — same UX as the existing pop-out pans).
- Contents:
  - **Header row:** host combo (recent hosts), frequency spin, mode combo
    (USB/LSB/CW/CWU/CWL/AM/FM), Connect/Disconnect, **"Audio → WebSDR"** toggle,
    S-meter, state/latency label.
  - **Waterfall area:** `QImage` ring-buffer (height = N rows), repainted on
    `rowReady`; newest row at top, scroll down. Plain `QPainter` — **not** the
    QRhi pipeline.
  - Optional thin spectrum strip above the waterfall (peak of recent rows).
- **Click-to-tune:** clicking/dragging in the waterfall maps the x-pixel to a
  frequency within the current band span and calls `WebSdrSource::tune()`.
  Internal only — never tunes the Flex.
- Reflects `stateChanged`; on disconnect/error, the "Audio → WebSDR" toggle drops
  back to Flex automatically.

### 3.5 `WebSdrSettings` — `src/core/WebSdrSettings.{h,cpp}`

`QSettings`-backed, under group `webSdr/`:

| key | type | meaning |
|-----|------|---------|
| `recentHosts` | QStringList | host:port MRU |
| `lastHost` | QString | autofill |
| `lastFreqKHz` | double | |
| `lastMode` | QString | |
| `zoom` | int | |
| `panelVisible` | bool | dock shown |
| `panelFloating` / `panelGeometry` | bool / QByteArray | detached state |
| `audioPreferred` | bool | restore "Audio → WebSDR" on launch? (default false) |

---

## 4. AudioEngine integration — the source gate

The **only** non-trivial edit to existing code. Today `feedAudioData()` is fed
solely by `PanadapterStream::audioDataReady()` and the RX sink lifecycle is tied
to the Flex connect. We add a small, thread-safe gate.

### 4.1 Additions to `AudioEngine`

```cpp
enum class RxSource { Flex, WebSdr };

Q_INVOKABLE void setRxSource(RxSource);          // default Flex
RxSource rxSource() const;                        // atomic read
Q_INVOKABLE void feedWebSdrAudio(const QByteArray& pcm24kStereoF32);
```

- `m_rxSource` is a `std::atomic<RxSource>` (default `Flex`).
- In `feedAudioData()` (Flex path): if `m_rxSource == WebSdr`, **drop** the Flex
  buffer (early return before DSP) so the speaker isn't double-fed.
- `feedWebSdrAudio()`: if `m_rxSource == WebSdr`, route the buffer into the *same*
  downstream DSP + `QAudioSink` path `feedAudioData()` uses; otherwise drop.
- The RX sink (`startRxStream`) must be able to run when WebSDR is selected even
  if no Flex is connected — decouple sink-open from the Flex connect so the panel
  can `startRxStream()` on demand. (Audio worker thread; use the existing
  `Q_INVOKABLE`/`QMetaObject::invokeMethod` pattern.)
- **Fallback:** switching back to Flex, or WebSDR disconnect, restores the Flex
  feed with no sink restart. WebSDR audio never reaches the radio (no TX path).

### 4.2 Format contract

`WebSdrSource` always emits **24 kHz stereo float32** (pipeline native). WebSDR
native (mono, `D` Hz) → `Resampler` to 24 kHz → mono duplicated to L/R. No change
to downstream DSP.

---

## 5. MainWindow / wiring

- Own the `WebSdrSource` + worker thread (lazily created when the panel is first
  enabled), and the `WebSdrPanel` dock.
- Connections (all auto-queued across threads):
  - `WebSdrSource::audioReady` → `AudioEngine::feedWebSdrAudio`
  - `WebSdrSource::rowReady` / `sMeter` / `stateChanged` / `bandsResolved`
    → `WebSdrPanel`
  - `WebSdrPanel` "Audio → WebSDR" toggle → `AudioEngine::setRxSource` (+ ensure
    `startRxStream`)
  - `WebSdrPanel` connect/tune/zoom → `WebSdrSource`
- A View-menu / toolbar entry toggles the dock. No changes to any Flex model.

---

## 6. Build wiring

- New sources behind an optional `HAVE_WEBSDR` definition, gated on the already
  present `Qt6WebSockets_FOUND` (reuse `Qt6::WebSockets`, no new dependency).
- Mirror the existing `HAVE_WEBSOCKETS` block in `CMakeLists.txt`. When
  WebSockets is absent, the module compiles out cleanly and the dock entry is
  hidden.

---

## 7. Data formats & conversions

| Stage | Format |
|-------|--------|
| WebSDR audio on the wire | type-routed binary, a-law / adaptive predictor, mono @ `D` Hz |
| `WebSdrAudioDecoder` out | Int16 mono @ `D` Hz |
| `WebSdrSource` audio out | float32 **stereo @ 24 kHz** (resampled, mono→LR) |
| WebSDR waterfall on the wire | format-1 rows, 2 palette indices/byte, + `0xFF` meta |
| `WebSdrWaterfallDecoder` out | `WebSdrRow` of `QRgb`, width px |

---

## 8. Lifecycle, errors, etiquette

- **One** audio + **one** waterfall socket per server; no parallel sessions.
- Reconnect: single attempt with exponential backoff (2→30 s cap). On repeated
  failure, surface `Error` in the panel and stop — never hammer.
- Disconnect / app close: close both sockets cleanly; if WebSDR was the audio
  source, revert to Flex.
- Public-server etiquette is a first-class concern: the host is user-chosen, the
  panel shows the selected instance, and docs recommend a self-hosted WebSDR for
  heavy use. No automated polling beyond the live streams.

---

## 9. Test plan

- **Unit — `WebSdrAudioDecoder`:** feed captured `~~stream` frames (recorded via
  the `websdr-client` tool), assert sample count, native rate, and that output
  matches the Python reference within tolerance (no rails, plausible RMS).
- **Unit — `WebSdrWaterfallDecoder`:** feed captured `~~waterstream` frames;
  assert row width, palette mapping for known indices, and correct handling of
  the three `0xFF` meta cases.
- **Integration (manual, live):** connect to a real instance, tune CW/SSB/FM,
  confirm audio on switch, waterfall scrolls, click-to-tune works, detach/redock,
  reconnect after a forced socket drop.
- **Regression:** with the module idle/absent, Flex RX audio + panadapter are
  byte-for-byte unchanged (source gate defaults to Flex; `HAVE_WEBSDR` off path).

---

## 10. Milestones & acceptance

| Milestone | Done when… | Effort |
|-----------|------------|--------|
| **M0 — Waterfall spike** | ✅ **DONE** — protocol documented in §1.2 (and `websdr-client/PROTOCOL.md`). | — |
| **M1 — Audio module** | ✅ **IMPLEMENTED** — connect/tune via dock header; audio switchable to WebSDR and back; Flex stays default. Audio decoder verified bit-exact (0 diff / 46080 samples) vs the Python reference; full app builds + links (MSVC 2019 / Qt 6.8.3). Live on-device A/B test pending. | done |
| **M2 — Mini-waterfall** | ✅ **IMPLEMENTED** — `WebSdrSource` opens `~~waterstream<band>`; `WebSdrWaterfallDecoder` (format 1 + palette, validated against a live capture) feeds a self-rendered scrolling waterfall in `WebSdrPanel`; detachable; click-to-tune via band span. Builds + links. Live on-device test pending. | done |

Total ~5–8 days (M0/M1/M2 implemented; manual live test outstanding).

---

## 11. Open questions (resolved/remaining)

- ~~Waterfall stream format~~ — **resolved** (§1.2).
- Mix vs switch audio: spec is **switch** (one source at a time). Mixing Flex +
  WebSDR into one sink is a deliberate later option, not in M1/M2.
- `0xFF 0x01` `freqRef` exact units (Hz vs band-relative) — not needed for
  rendering; confirm only if we add a numeric frequency readout on the panel.
