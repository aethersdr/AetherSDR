# Implementation Plan — K4-Style Mini-Pan (narrow bandwidth scope)

Status: implemented — this is the design record for the mini-pan as shipped.
The open questions at the end are answered above (spike results, §4, §9.0);
they are kept as the trail of what was decided and why, not as live work.

---

## Spike results — live FLEX-6500, 2026-07-28 (branch `spike/minipan`)

A throwaway spike (`RadioModel::createMiniPan` + a `MiniPanWidget` floating window,
View → "Mini-Pan (spike)") validated the architecture end-to-end against the real radio:

**Confirmed:**
- **Dedicated narrow pan works.** A second `display panafall create` pan set to
  `bandwidth=0.010` (10 kHz) coexists with the main 200 kHz pan. Read back from the radio:
  `0x40000001 bandwidth=0.010000`, `min_bw=0.004920` — exactly as Gate #0 predicted.
- **Resolution is K4-class and decoupled from the main pan.** Mini-pan ran at
  26 Hz/bin (382 px) / 7 Hz/bin (1433 px) vs. the main pan's 140 Hz/bin at 200 kHz —
  5–20× finer, independent of main-pan zoom. Path #2 is vindicated.
- **§4 RESOLVED → custom scope, not reused `SpectrumWidget`.** `SpectrumWidget` exposes
  no chrome-stripping API (only `setShowGrid`/`overlayMenu()`/`setWaterfallLive`) — its
  overlay menu, dBm strip, time axis and waterfall region can't be cleanly removed. A slim
  ~90-line custom `MiniPanScope` (filled trace + centre hairline + passband band + ±span
  corner labels) gives the clean K4 window the reused widget could not.
- **Mode-aware passband works.** The active slice's `filterLow/filterHigh` render as the
  translucent band, correctly offset for USB — the §5 "what does centre mean per mode"
  affordance, visible.
- **Width-driven xpixels honored.** The radio accepts per-frame `xpixels` re-pushes; a
  150 ms debounce is required (an undebounced push fired ~40 commands as the window settled).

**Issues found → must fix in the feature PRs:**
1. **Mini-pan must not become the "active pan."** VERIFIED FACT: `get_state pan active`
   reported the mini-pan (`0x40000001`) as active, and its `x_pixels` tracked the *main*
   widget's width (1433), not the mini-pan scope's — so the active-pan lane drives the wrong
   xpixels source. (NOT verified: whether this regresses the main pan's VFO-follow — the tune
   test spawned a new slice B at 14.074 while slice A stayed at 14.100 on the main pan, which
   may be correct per-slice behavior. Design the PR2 fix to the verified fact.) The mini-pan
   needs its own owned-but-non-active pan lane, exempt from active-pan and pan-follows-VFO.
   *Downstream symptom of this same bug:* the mini-pan pan has no slice, so when `tune`
   targeted the (mis-)active pan it spawned a phantom **slice B** on it. Decoupling the
   active-pan lane removes the phantom-slice spawn too.
2. **Centre authority.** The mini-pan should center the VFO itself (dead-centre) and not be
   subject to pan-follows-VFO offsetting (observed radio centre 14.0788 vs. intended 14.074).
3. **Active-slice rebind (§5).** Following only the slice active at open; tuning that creates
   a new active slice isn't tracked. Rebind on active-slice change.
4. **Model sync for widget-less pan.** `get_state pan` briefly reported the mini-pan at the
   200 kHz default before the bandwidth echo synced the `PanadapterModel`. Harmless to the
   direct-widget feed, but introspection/persistence should read authoritative state.

---

## 0. What we're building (from the K4 reference screenshot)

A small, detachable top-level window containing:

1. **Large frequency readout** of the followed VFO (e.g. `7.198.000`).
2. **A narrow spectrum *trace*** (yellow line/fill on dark navy) — a live pan/scope
   line, **not a waterfall**. No falling-water history to render.
3. **±5.0 kHz / ±10.0 kHz span** labels, VFO centered.
4. **A translucent passband band** highlighting the active slice's filter around center.

It stays visible and updating when the main `SpectrumWidget`/waterfall is hidden
(Minimal Mode), because it is its own top-level window with its own data stream.

---

## 1. Architecture decision — **Path #2: dedicated narrow pan stream** (recommended)

The issue asks us to choose between:

- **Path #1** — client-side re-slice of the primary pan's FFT bins.
- **Path #2** — a second lightweight radio-side pan (`display panafall create`) treated
  as its own `PanadapterModel` instance.

### Why Path #2 wins

**Resolution (the deciding factor).** The radio sends bins into a pixel grid whose width
is the *rendering widget's* current pixel width — `xpixels` is re-pushed as widget width
via `SpectrumWidget::dimensionsChanged` → `display pan set … xpixels=`
(MainWindow_Wiring.cpp:2492). The create-time `xpixels=1024` (RadioModel.cpp:3221) is
immediately overridden by the live widget width. So bin width = `panSpanMhz / widgetWidthPx`.
For Path #1 the mini-pan re-slices the *primary* pan, whose span is whatever the operator
zoomed to and whose width is the main spectrum widget width (often 2000–2560 px):

| Primary pan span | bin width ≈ span/2560px | bins across ±5 kHz (10 kHz) |
|---|---|---|
| 48 kHz | ~19 Hz | ~530 (good) |
| 200 kHz | ~78 Hz | ~128 (ok) |
| 2 MHz | ~781 Hz | **~13 (inadequate)** |

The target user (contesters running a *wide* situational-awareness pan) is exactly the
case where Path #1 collapses to ~13 points across the whole scope — a line trace can't
render the K4 detail from that. Path #2 gets its **own dedicated 10 kHz pan**, whose bins
= `10 kHz / miniPanWidthPx` (≈300 px → ~33 Hz/bin) **independent of the main pan's zoom**.
That's a ~24× resolution advantage on a 2 MHz main pan, and it reproduces the K4 view for
*any* main-pan state.

**GATING FINDING — minimum FLEX pan span. RESOLVED (live on FLEX-6500, fw 1.4.0.0,
2026-07-28): `min_bw = 0.004920 MHz (4.92 kHz)`, `max_bw = 14.745601 MHz`.**
`SpectrumWidget`'s zoom clamp defaults to `m_minBwMhz{0.010}` ("10 kHz default … safe
default for unknown radios", SpectrumWidget.h:1281) — but that's a conservative *fallback*;
the real per-model floor is radio-reported via `panBandwidthLimitsChanged` →
`PanadapterModel::setBandwidthLimits` (RadioModel.cpp:566-577, PanadapterModel.h:66-73),
and it appears in the `display pan <id> … min_bw=… max_bw=…` status fields.
**Consequence:** the ±5 kHz mode (10 kHz span) clears the 4.92 kHz floor with 2× margin —
**no bounded-Path-#1 fallback needed on this hardware.** Path #2 stands clean. (Still
clamp the requested span to the pan's reported `minBandwidthMhz()` for models that report
a higher floor — the fallback logic remains a cheap guard, just not exercised on the 6500.)
Empirical bonus: the live main pan reported `x_pixels=1358` (its widget width), confirming
`xpixels` tracks live widget width, not the create-time `1024`.

**Minimal Mode (a non-issue for both, but cleanest for #2).** Verified in
`MainWindow::toggleMinimalMode` (MainWindow.cpp): entering minimal mode only
`setUpdatesEnabled(false)` on **non-floating** applet spectrum widgets and
`m_splitter->hide()`. It does **not** send `display pan remove`, does not unregister the
stream, does not resize to zero — and it **explicitly skips floating pans** ("they remain
visible in their own top-level window"). A mini-pan built as its own top-level window is
never touched. Its dedicated stream keeps flowing.

**Independence.** Path #2's data, center, and bandwidth are decoupled from whatever the
operator does to the main pan (zoom, pan, hide). That independence *is* the feature.

### Cost & mitigation

- Consumes one radio pan slot, gated by `maxPanadapters()` (RadioModel.cpp:3201). On
  exhaustion the radio path emits `panadapterLimitReached(limit, model)`
  (RadioModel.cpp:3212). We must handle this gracefully (see §7).
- `display panafall create` allocates **both** a pan *and* a waterfall stream. We only
  need the FFT/pan stream — we simply never register/subscribe the waterfall stream for
  the mini-pan (leave `wfStreamId` untapped). Minor radio-side waste, no client cost.

### Fallback (documented, not v1 scope)

If no pan slot is available, degrade to Path #1 (re-slice primary pan bins) with a
visible "reduced resolution" affordance. Keep the widget's data-source behind an
interface so this can be added later without reworking the UI.

---

## 2. New components

| Component | File(s) | Base / model-on |
|---|---|---|
| `MiniPanWidget` | `src/gui/MiniPanWidget.{h,cpp}` | `QWidget(parent, Qt::Window)`, modeled on `FloatingContainerWindow` (containers/FloatingContainerWindow.cpp) |
| Frequency readout | inside MiniPanWidget | reuse existing VFO digit rendering if factored out; else a styled `QLabel` |
| Scope trace | reuse **`SpectrumWidget`** configured minimal (no waterfall, no grid chrome), OR a slim custom paint widget | see §4 |
| Bandwidth selector | small `QComboBox` (5 kHz / 10 kHz) in the mini-pan chrome | — |
| Title bar w/ dock+close | model on `ContainerTitleBar` (float-toggle `⧉`/`↙` + close), **not** `FramelessWindowTitleBar` | containers/ContainerTitleBar.cpp:96 |

### Window scaffolding recipe (copy from `FloatingContainerWindow`)

- `QWidget(parent, Qt::Window)`; read `FramelessWindow` setting, OR in
  `Qt::FramelessWindowHint`, re-apply via `setWindowFlags`.
- `WA_DeleteOnClose=false`, `WA_QuitOnClose=false`, `WA_StyledBackground`; `applyAppTheme(this)`.
- 0-margin `QVBoxLayout`. `FramelessResizer::install(this)` last (FramelessResizer.h:36).
- `QTimer m_saveTimer` (single-shot 400 ms) debounces geometry saves on move/resize.
- `setFramelessMode(bool)` / `setAlwaysOnTop(bool)` using the save-geometry → flip-flag →
  restore-if-visible idiom (FloatingContainerWindow.cpp:153,168).

---

## 3. Data source — a second `PanadapterModel`

Reuse the existing pipeline as a second instance (radio → `PanadapterStream` →
`RadioModel` → mini-pan):

1. **Create** a narrow pan. Add `RadioModel::createMiniPan(...)` (variant of
   `createPanadapter()`, RadioModel.cpp:3199) that, on create, pushes:
   - `display pan set <id> xpixels=<miniPanWidthPx> ypixels=<h>` — **note:** `xpixels`
     tracks the mini-pan scope's live widget width, not a fixed 512; if the scope reuses
     `SpectrumWidget` its `dimensionsChanged` re-push (MainWindow_Wiring.cpp:2492) drives
     this automatically. Resolution = span / width (see §1).
   - `display pan set <id> bandwidth=0.010` (10 kHz span for ±5 kHz; 0.020 for ±10 kHz) —
     clamped to the pan's radio-reported `minBandwidthMhz()` (§1 gating finding),
   - `display pan set <id> center=<followedVfoCenterMhz>` (see §5 for mode-aware center),
   - `min_dbm/max_dbm` matching the mini-pan's dBm scale.
   Track the returned `panId` in a dedicated member (not the primary-pan map consumers
   iterate for rendering, to avoid the main routing loop pushing bins into the wrong
   widget — see MainWindow_Session.cpp:1028). Register its `panStreamId` with
   `PanadapterStream` and set its dBm range / yPixels.
2. **Subscribe** to `RadioModel::panFeedSpectrumReady(streamId, binsDbm, ns)`
   (RadioModel.h:649) — same tap S-History/adaptive-filter use
   (MainWindow_Session.cpp:1080). Filter `streamId == miniPan->panStreamId()`, forward
   bins to the scope via `updateSpectrum(bins)`.
3. **Cleanup** on window close / radio disconnect: `removePanadapter(miniPanId)`
   (`display pan remove` + `display panafall remove`, RadioModel.cpp:3245) and
   unregister the stream.

---

## 4. Rendering — reuse `SpectrumWidget` (recommended) vs. custom

**Recommendation: reuse `SpectrumWidget`** in a stripped configuration rather than
reimplement dBm scaling, bin→pixel resampling, EMA smoothing, and the passband overlay.

- Feed via `updateSpectrum(binsDbm)` (SpectrumWidget.cpp:7131) — bins already dBm-scaled.
- `setFrequencyRangeImmediate(centerMhz, bandwidthMhz)` (SpectrumWidget.h:141) with
  center = VFO, bandwidth = 0.010 / 0.005.
- `setDbmRange(min,max)` (SpectrumWidget.h:234).
- Passband band: `setSliceOverlay(...)` (SpectrumWidget.h:550) + `setVfoFrequency`
  (SpectrumWidget.h:294) give the center marker and filter shading for free.
- Suppress waterfall + heavy grid chrome (config flags / minimal styling) so it renders
  as the compact K4 trace.

If `SpectrumWidget` proves too heavy to strip cleanly (it is ~8k lines, optionally
`QRhiWidget` under `AETHER_GPU_SPECTRUM`), fall back to a slim custom `QWidget`
`paintEvent` that resamples bins across width using the same proportional mapping as
`SpectrumWidget::mhzToX/xToMhz` (SpectrumWidget.cpp:8063). Decide during spike (§9).

---

## 5. VFO tracking & mode-aware center (acceptance criterion #4 substance)

- Followed VFO = `MainWindow::activeSlice()->frequency()` (SliceModel::frequency, MHz,
  SliceModel.h:41).
- Connect `SliceModel::frequencyChanged(double)` (SliceModel.h:276) → debounce → send
  `display pan set <miniPanId> center=<centerMhz>` and `scope->setFrequencyRangeImmediate`
  and update the big readout.
- Connect active-slice change (MainWindow `m_activeSliceId`) → re-bind the
  `frequencyChanged` connection to the new active slice and recenter.
- Debounce center pushes (e.g. 50–100 ms) so fast tuning doesn't flood the radio with
  `display pan set center=` commands.

**Design decision — what "center" means per mode (this IS criterion #4, not a detail).**
The K4 mini-pan is primarily a *CW tuning aid*, so getting the center reference right per
mode is the crux, not polish:
- **CW**: the displayed carrier (`slice.frequency()`) is offset from the audible signal by
  the CW pitch/sidetone. Decide whether the scope centers on the carrier (marker sits where
  the received signal *should* land when zero-beat) or on carrier±pitch. Recommended: center
  on `slice.frequency()` and draw the passband/pitch offset via `setSliceOverlay`
  (SpectrumWidget.h:550) so the operator sees the offset rather than hiding it.
- **SSB/DIGU**: passband is sideband-offset from the suppressed carrier. Center on
  `slice.frequency()`, let the passband band show the sideband extent.
- **RTTY**: mark/space around the slice; passband band conveys the shift.
In all modes, `setVfoFrequency` (SpectrumWidget.h:294) + `setSliceOverlay` render the center
marker and filter shading, making the mode offset *visible* — verify each of CW/RTTY/SSB/DIGU
against a real slice (Automation Bridge / live radio) since the offset is exactly what
criterion #4 is testing.

---

## 6. Menu, persistence, frameless propagation

- **View-menu toggle**, checkable, modeled on Minimal Mode
  (MainWindow_Menus.cpp:957): `viewMenu->addAction("Mini-Pan\tCtrl+…")`,
  `setCheckable(true)`, restore checked from `AppSettings` `"MiniPanOpen"` boolean,
  `connect(toggled → show/hide mini-pan)`. Consider also a title-bar/VFO-area button
  later; menu is the v1 entry point.
- **Lazy construct + track** via `showOrRaisePersistent<MiniPanWidget>(m_miniPan, ...)`
  (MainWindow.h:1514) so it joins the tracked-dialogs list and `WA_DeleteOnClose` flow.
- **Geometry persistence**: `saveGeometry().toBase64()` under a dedicated key
  (e.g. `"MiniPanGeometry"`) into `AppSettings::instance()` (**reference**, `.value(...)`,
  include `"core/AppSettings.h"`); `AppSettings` stores values as text (SQLite TEXT rows
  since RFC #4603, XML before that), so the binary `QByteArray` from `saveGeometry()`
  needs base64 to round-trip. Debounced in-memory on move/resize; `AppSettings::save()`
  on close. (PanFloatingWindow.cpp:98 idiom.)
- **Open-state restore at startup**: if `"MiniPanOpen"=="True"`, reopen via
  `QTimer::singleShot(0, ...)` after initial layout (MainWindow_Menus.cpp:779 idiom).
- **Bandwidth selection** persisted client-side (`"MiniPanBandwidthKHz"`), per the
  radio-authoritative-settings policy (display-only sub-view, not radio state).
- **Frameless toggle propagation**: add `if (m_miniPan) m_miniPan->setFramelessMode(on);`
  to `MainWindow::setFramelessWindow` (MainWindow.cpp:7452), or route via the tracked
  `m_persistentDialogs` list.

---

## 7. Lifecycle & edge cases

- **Pan slot exhausted**: handle `RadioModel::panadapterLimitReached(limit, model)`
  (RadioModel.cpp:3212) — surface a non-blocking notice ("No panadapter slot free for
  Mini-Pan"), leave the View toggle unchecked. (Future: degrade to Path #1.)
- **Radio disconnect/reconnect**: tear down the mini-pan's model+stream on disconnect;
  recreate on reconnect if `"MiniPanOpen"`. Never leak a radio-side pan.
- **Bandwidth change (5↔10 kHz)**: resend `display pan set <id> bandwidth=…` and
  `scope->setFrequencyRangeImmediate`; persist selection.
- **No active slice / TX / mode without a slice**: hide trace or show "—"; the issue
  scopes to CW/RTTY/SSB/DIGU — verify the followed slice exists in those modes.
- **Multiple radios / model differences**: `maxPanadapters()` varies by model
  (RadioModel.h:249); don't assume a free slot.
- **Close == dock vs. close == hide**: decide semantics. For a tuning aid, `closeEvent`
  should hide + set `"MiniPanOpen"=False` + `removePanadapter`, not merely dock.

---

## 8. Testing (per project constitution)

- **Offscreen widget tests** (Principle VIII, see commit ae6012bf `run widget tests
  offscreen`): construct `MiniPanWidget`, feed synthetic bins to `updateSpectrum`, assert
  trace geometry / center mapping / bandwidth switch. Run with `QT_QPA_PLATFORM=offscreen`.
- **Data-source unit test**: given a `panFeedSpectrumReady` frame on the mini-pan's
  streamId, assert bins reach the scope and non-matching streamIds are ignored.
- **Automation Bridge** (per operator preference — verify GUI via the bridge, not ad-hoc;
  docs/automation-bridge.md): script open-from-View-menu, assert window present, tune the
  VFO and assert center follows, toggle Minimal Mode and assert the mini-pan stays visible
  and updating, restart and assert geometry+open-state persist.
- **Live radio** (optional, operator has FLEX-6500): confirm real narrow pan resolution
  and that the slot-limit path is reachable.

---

## 9. Suggested phasing (PR breakdown)

0. **Gate #0 — live-radio bandwidth probe. ✅ DONE (FLEX-6500 fw 1.4.0.0, 2026-07-28):**
   read `min_bw=0.004920` / `max_bw=14.745601` from the `display pan` status. ±5 kHz clears
   the floor with margin; Path #2 confirmed, no fallback needed on this radio. (Probe
   script: `scratchpad/flex_probe2.py` — connect :4992, `display panafall create`, read
   `min_bw`/`max_bw`, `display pan remove`.)
1. **Spike (throwaway/branch):** add `createMiniPan` + a bare `SpectrumWidget` in a
   `Qt::Window`, hard-coded 10 kHz, subscribe to `panFeedSpectrumReady`. Answer: does the
   width-driven `xpixels` re-push give usable bins at 10 kHz? Does stripped `SpectrumWidget`
   render acceptably small, or is a slim custom paint widget less effort? → resolves §4.
2. **PR 1 — window shell + persistence:** `MiniPanWidget` frameless top-level, View-menu
   toggle, geometry + open-state persistence, frameless propagation. No live data yet
   (static/placeholder trace). Fully testable offscreen.
3. **PR 2 — data pipeline:** `RadioModel::createMiniPan` / cleanup, stream registration,
   `panFeedSpectrumReady` subscription, VFO tracking, big frequency readout.
4. **PR 3 — polish:** bandwidth 5/10 kHz selector, passband overlay, dBm scaling, slot-
   limit + disconnect/reconnect handling, mode coverage (CW/RTTY/SSB/DIGU), Minimal-Mode
   verification, Automation-Bridge test suite.

Use `git ship` (AGENTS.md) to batch-ship; branch off `main`, PR with auto-squash-merge.

---

## 10. Concrete file touch-list

**New:**
- `src/gui/MiniPanWidget.{h,cpp}`
- (maybe) `src/gui/MiniPanTitleBar.{h,cpp}` if a custom dock/close bar is needed
- tests under `tests/` (offscreen widget test + data-source test)
- `docs/…` short feature note (NOT a `CHANGELOG.md` entry — that file is
  release-prep only, and every PR that touches it conflicts with every other)

**Modified:**
- `src/models/RadioModel.{h,cpp}` — `createMiniPan`, mini-pan id member, cleanup
- `src/gui/MainWindow.{h,cpp}` — `m_miniPan` QPointer, `showOrRaise…`, `setFramelessWindow`
  propagation, active-slice re-bind
- `src/gui/MainWindow_Menus.cpp` — View-menu toggle + startup restore
- `src/gui/MainWindow_Session.cpp` — mini-pan `panFeedSpectrumReady` subscription
- `CMakeLists.txt` — add new sources
- `docs/style/dialog-patterns.md` — optional: note the floating-window (non-dialog) case

---

## Open questions

**Gate #0 (live radio, before code — §9.0):** does `display pan set <id> bandwidth=0.010`
succeed on the FLEX-6500, and what `minBandwidthMhz()` does the model report? — decides
whether ±5 kHz is a real dedicated pan or a bounded client re-slice.

**Spike (§9.1):**
1. Does the width-driven `xpixels` re-push yield usable bins at a 10 kHz span in a small
   window (~33 Hz/bin @300px)? — confirms Path #2 resolution in practice.
2. Can `SpectrumWidget` be stripped to a clean compact trace, or is a slim custom paint
   widget less effort? — decides §4.
3. Is there a pan-only `display pan create` (no waterfall) to avoid the wasted WF stream,
   or must we use `display panafall create` and ignore the WF? — resource nicety.
4. Exact close semantics (hide-and-keep-pan vs. remove-pan-on-close) for a tuning aid.
5. Per-mode center reference (CW pitch offset, SSB/DIGU sideband) — §5 design decision,
   validate against real slices.
