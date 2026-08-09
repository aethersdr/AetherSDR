# Mini-Pan — K4-style narrow scope

Design record for the mini-pan as shipped.

A small ±5/±10 kHz view of the spectrum around the active VFO, living in the
applet panel and floatable into its own window — usable when the main
panadapter is hidden (Minimal Mode) or alongside contest-logging software.

---

## 1. It is a view, not a radio object

The mini-pan **creates nothing on the radio**. It re-slices the FFT frames the
active slice's panadapter is already streaming down to a ±5 or ±10 kHz window
around that slice.

No dedicated panadapter, no slice. That is the whole architecture, and it is
what makes the feature free to open: no pan slot consumed against
`maxPanadapters()`, nothing to leak if the app exits uncleanly, no phantom
slice, no interaction with the active-pan lane, and nothing to re-establish
across a reconnect.

The data path is one pure function — `src/gui/MiniPanReslice.h`,
`resliceWindow()` — fed from `RadioModel::panFeedSpectrumReady` in
`MainWindow_Session.cpp` and rendered by `MiniPanScope`. It resamples the
sub-range around the VFO into a fixed-width output, interpolating between
source bins (so a narrow main pan upsamples smoothly rather than stair-stepping)
and padding with the frame's own floor outside the pan's edges (so the
frequency axis stays honest instead of smearing edge data across the gap).

### The trade: resolution follows the main pan

Because the bins come from the main pan, the useful detail is the **main pan's**
bin width — `panSpan / panXpixels` — not anything the mini-pan controls:

| Main pan span | bin width @ ~2800 px | real bins across ±5 kHz |
|---|---|---|
| 48 kHz | ~17 Hz | ~580 |
| 200 kHz | ~71 Hz | ~140 |
| 2 MHz | ~714 Hz | ~14 |

At ordinary operating spans this is ample. Zoomed right out the trace goes
visibly coarse — the window is upsampled from a handful of real bins.

An earlier draft of this plan proposed the opposite architecture: a **dedicated
narrow radio pan** (`display panafall create` at 10 kHz), whose resolution would
be independent of the main pan's zoom. That was implemented and rejected in
review (#4562). The resolution win was real but the cost was not acceptable:
the second pan consumed a slot, the FLEX firmware auto-created a slice on it
that had to be chased down and removed, it competed for the active-pan lane, it
survived on the radio after quit and came back as an orphan pan on the next
launch, and it had to be specially re-adopted across every reconnect (#4566).
Every defect found in that PR traced back to owning a radio object. A view that
owns nothing cannot have any of them.

### Appearance follows the main pan

The mini-pan is a magnifier on the main trace, so it renders with the main
pan's Display settings rather than its own:

| Display control | How it reaches the mini-pan |
|---|---|
| FFT AVG | radio-side pan property (`display pan set … average=`) — already in the bins |
| FFT FPS | radio-side (`requestPanDisplayRates`) — frames arrive at that rate |
| FFT Line (colour, width) | mirrored from the source pan's `SpectrumWidget` |
| FFT Fill (colour, alpha) | mirrored from the source pan's `SpectrumWidget` |
| FFT Floor + FFT Scale | mirrored from the widget's `refLevel()` / `dynamicRange()` |
| Heat Map | mirrored; the ramp itself is shared (`gui/FftHeatMap.h`) |
| Grid | mirrored — same token, dotted pen and dB ladder (`gui/SpectrumGrid.h`) |

The first two need no code at all — re-slicing the same pan's frames carries
them. The rest are pulled per frame in `feedMiniPanFromPanFrame` rather than
wired signal-by-signal: the reads are plain members, the scope's setters are
change-gated, and pulling cannot drift out of sync or miss a control nobody
remembered to connect.

The vertical window is the **widget's** `refLevel()` / `dynamicRange()`, not
the pan's `min_dbm`/`max_dbm`. FFT Floor slides `refLevel` client-side
(`applyNoiseFloorAutoAdjust`) and only pushes a damped, thresholded dBm range
to the radio afterwards — so mirroring the model tracks a lagging echo rather
than the scale the main pan is drawing with, and the floor slider appears to do
nothing in the mini-pan. `PanadapterModel::minDbm()/maxDbm()` is the fallback
for a pan with no applet in the stack.

The scope does **no** local auto-scaling — a second opinion on the noise floor
would disagree with the main pan about how tall a signal is.

The heat-map ramp (`gui/FftHeatMap.h`) and the dB grid ladder
(`gui/SpectrumGrid.h`) are shared with `SpectrumWidget` rather than copied: two
definitions would mean the same signal rendering in two different colours, and
the two views ruling at different dB values, at two zoom levels.

The mini-pan draws no VERTICAL grid rules. The main pan's frequency spacing is
chosen for its own span, so inside a 10 kHz window it yields one line or none —
and the centre hairline already marks the only frequency of interest.

Both grids now read `color.spectrum.grid`. The main pan previously drew with
`color.background.1` while *declaring* `color.spectrum.grid` in its token list,
so the token named for the job was the one nothing used.

Only the chrome the main pan has no equivalent of — the passband band, the
centre hairline, the ±span labels, the grid — stays on theme tokens.

---

## 2. It is an applet, not a View-menu window

`MainWindow::toggleMinimalMode` strips the title bar to heartbeat + logo +
restore/feature buttons — **there is no menu bar in Minimal Mode**, which is
precisely when the operator wants a narrow scope. The applet panel, by contrast,
is reparented into the central layout and shown: it *is* the Minimal Mode UI.

So the mini-pan is an applet (`MPAN`, tray button `MINI`, off by default) and
there is no View-menu entry point.

Riding the container framework also means the window behaviour is inherited
rather than hand-rolled — float-out into a top-level window (so it still floats
over a logging app), always-on-top (#2430), geometry persistence and
close==hide are all `ContainerWidget` / `FloatingContainerWindow`, driven from
the standard `ContainerTitleBar`.

---

## 3. Components

| Component | File | Notes |
|---|---|---|
| Applet shell | `src/gui/MiniPanApplet.{h,cpp}` | Frequency readout + scope + span context menu. Holds no radio/slice references. |
| Scope render | `src/gui/MiniPanScope.{h,cpp}` | Filled trace, centre hairline, passband band, ±span labels. Paints from theme tokens. |
| Re-slice | `src/gui/MiniPanReslice.h` | Pure function; unit-tested without a radio or a widget. |
| Settings | `src/core/MiniPanSettings.{h,cpp}` | One nested `MiniPan` object (Principle V). |
| Glue | `MainWindow.cpp`, `MainWindow_Session.cpp` | Feed, and centre/passband from the followed VFO. |

`SpectrumWidget` is deliberately not reused: it exposes no way to strip its
overlay menu, dBm strip, time axis and waterfall region, so a compact K4-style
trace needs its own ~130-line paint.

---

## 4. Feed lifecycle

The applet's own `showEvent`/`hideEvent` drive `feedWanted(bool)`. That single
hook catches every way the tile can come and go — tray toggle, container close
button, float, dock, layout apply — and MainWindow starts or stops consuming
frames on it. A hidden applet costs nothing per frame, and because nothing
radio-side is created or destroyed there is no state to get out of sync.

Centre and passband come from the active slice (`frequencyChanged`,
`filterChanged`), rebound on active-slice change. All of it is local: nothing is
sent to the radio, so tuning needs no debounce.

---

## 5. Persistence

Only the ±5/±10 kHz span is feature-owned, and it lives as one nested `MiniPan`
object in `AppSettings` per Constitution Principle V — never flat keys. The
accessor validates it, so a hand-edited value can only ever be one of the two
spans the menu offers.

Geometry, visibility and always-on-top belong to the container framework, which
already owns them for every applet; keeping copies here would be a second
source of truth for the same window.

Nothing the radio owns is persisted (Principle III).

---

## 6. Testing

`tests/mini_pan_widget_test.cpp`, offscreen:

- the re-slice mapping — a signal at the VFO lands dead centre (an off-by-one
  here would put a signal at a frequency it is not on), a window outside the
  pan reads as floor rather than clamped edge data, a straddling window pads
  the outside half, and degenerate inputs return empty;
- the scope render API against synthetic and empty frames;
- the show/hide feed lifecycle, exact edges;
- span persistence, including that the radio-echo path does not overwrite the
  operator's stored choice, and that the config is one object with no flat keys.

Geometry, float/dock and always-on-top are not covered here — that behaviour is
the container framework's, and `container_widget_test` / `container_manager_test`
own it.
