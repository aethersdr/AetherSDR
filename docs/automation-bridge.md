# Agent Automation Bridge

> **AI agents (Claude, Codex, …) read this first.** This doc is written for
> *you*, an agent working in this repo who needs to introspect or capture the
> running GUI — to verify a change, assert on UI state, or grab the panadapter.
> Everything below is copy-pasteable. Skip to [Quickstart](#quickstart) and go.

AetherSDR is a **Qt 6 Widgets** native app — no QML, no web layer, so there is
no DOM or browser tooling to drive. The automation bridge is the in-process
substitute: an opt-in command channel that exposes the widget tree and lets you
capture any widget (including the GPU panadapter) as a PNG. It is the
deterministic, cross-OS way to do "snapshot → act → assert" testing of the UI.

Introduced in issue
[#3646](https://github.com/aethersdr/AetherSDR/issues/3646) (Phase 0). Off in
production; it only exists when you ask for it via an env var.

---

## When to use it

| Goal | Use the bridge? |
|---|---|
| Assert a control's state after a change (slider value, button checked, label text) | **Yes** — `dumpTree`, read the `value` field. No screenshot needed. |
| Confirm a widget exists / is enabled / has the right accessibleName | **Yes** — `dumpTree`. |
| Capture what the panadapter/waterfall actually rendered | **Yes** — `grab SpectrumWidget`. |
| Visually check a dialog or applet layout | **Yes** — `grab <widget>` → view the PNG. |
| Click a button or move a slider programmatically | **Yes** — `invoke <target> <action> [value]`. |
| Read live model truth (freq, mode, center, dBm, NB/NR) | **Yes** — `get radio\|slice\|pan …`. Assert on state, no pixels. |
| Key the radio (MOX/PTT/Tune) | **No** — `invoke` refuses transmit controls by design (see [TX safety](#tx-safety)). |

---

## Quickstart

```bash
# 1. Build with the bridge available (it's compiled in unconditionally;
#    the env var below is what turns it on at runtime).
cmake --build build --parallel

# 2. Launch the app with the bridge enabled.
AETHER_AUTOMATION=1 ./build/AetherSDR.app/Contents/MacOS/AetherSDR &   # macOS
#   AETHER_AUTOMATION=1 ./build/AetherSDR &                            # Linux/Windows

# 3. Drive it. The dependency-free probe needs no Qt:
python3 tools/automation_probe.py ping
python3 tools/automation_probe.py demo --out /tmp/phase0   # → tree.json + panadapter.png
```

`demo` produces the two canonical artifacts: a semantic snapshot of the UI
(`tree.json`) and a PNG of the live panadapter (`panadapter.png`). View the PNG
to confirm a visual change; parse the JSON to assert on control state.

For headless / CI runs, add `QT_QPA_PLATFORM=offscreen` — no display required.

On macOS, do not host the bridge from a Codex-style sandboxed command. The
native Cocoa platform can abort during `QApplication` startup if pasteboard or
HIServices are unavailable, before AetherSDR reaches the automation bridge; with
`QT_QPA_PLATFORM=offscreen`, the same sandbox can still deny the `QLocalServer`
socket the bridge needs. Launch outside the command sandbox instead:

```bash
QT_QPA_PLATFORM=offscreen AETHER_AUTOMATION=1 ./build/AetherSDR.app/Contents/MacOS/AetherSDR &
```

---

## How it works (the contract)

- **Transport:** a `QLocalServer` — an `AF_UNIX` socket on macOS/Linux, a named
  pipe on Windows. No TCP port, no network exposure.
- **Framing:** newline-delimited. You send one request per line; you get back
  exactly one compact-JSON response line.
- **Request line** is *either* a bare command or a JSON object — both work:
  - `dumpTree`
  - `grab SpectrumWidget /tmp/pan.png`
  - `{"cmd":"grab","target":"SpectrumWidget","path":"/tmp/pan.png"}`
- **Discovery:** on startup the app writes the resolved socket path to
  `${TMPDIR:-/tmp}/aethersdr-automation.json`, so you never have to guess the
  platform-specific endpoint:
  ```json
  {"socket":"/var/folders/.../aethersdr-automation","name":"aethersdr-automation","pid":7326,"version":"26.6.3"}
  ```
  `tools/automation_probe.py` reads this automatically. Override the socket name
  at launch with `AETHER_AUTOMATION_SOCKET=<name>`.

### Driving it without the probe

Any language can talk to it; it's just a Unix socket and line-delimited JSON.
Raw shell example:

```bash
SOCK=$(python3 -c 'import json,os,tempfile; print(json.load(open(os.path.join(tempfile.gettempdir(),"aethersdr-automation.json")))["socket"])')
printf '{"cmd":"ping"}\n' | nc -U "$SOCK"
```

---

## Verbs

### `ping`
Connectivity / handshake.

```json
→ {"cmd":"ping"}
← {"ok":true,"app":"AetherSDR","version":"26.6.3"}
```

### `dumpTree`
ARIA-style semantic snapshot of **every** top-level `QWidget` hierarchy. This is
your "DOM snapshot" for controls.

```json
→ {"cmd":"dumpTree"}
← {"ok":true,"roots":[ <node>, <node>, … ]}
```

Each `<node>`:

```jsonc
{
  "class": "AetherSDR::SpectrumWidget",   // C++ class (full, namespaced)
  "objectName": "masterVolume",            // present only if set
  "accessibleName": "Master volume",       // present only if set
  "toolTip": "Clear the displayed SWR sweep trace.",  // present only if set
  "enabled": true,
  "visible": true,
  "geometry": { "x": 1, "y": 104, "w": 1448, "h": 751 },  // GLOBAL screen coords
  "value": "42",                           // best-effort; see below
  "range": { "min": 0, "max": 100 },       // numeric controls only (slider/spinbox)
  "items": ["LSB","USB","AM","CW"],        // QComboBox only: full option list
  "currentIndex": 1,                       // QComboBox only: selected index
  "panIndex": 0,                           // SpectrumWidget only: pass to `grab pan`/`pan close`
  "keying": true,                          // present only on TX-keying controls (invoke refuses these)
  "actions": [ <action>, … ],              // QMenu only: popup actions and state
  "children": [ <node>, … ]                // present only if non-empty
}
```

The `range` lets a driver validate against the real bounds (scale) and detect
**circular/wrapping** controls without guessing: if `setValue(max)` doesn't stick
but a mid value does, the control wraps (e.g. a 0–360° phase slider where step 72
≡ 0°) — classify it as wrapping, not broken.

**The `value` field** is the fast path for state assertions — it's filled in
for common controls so you can assert without a screenshot:

| Widget | `value` |
|---|---|
| `QAbstractSlider` (sliders, scrollbars, dials) | numeric position, e.g. `"42"` |
| `QAbstractButton` checkable (checkbox, toggle) | `"checked"` / `"unchecked"` |
| `QAbstractButton` non-checkable (push button) | its text |
| `QComboBox` | current text |
| `QLineEdit` | current text |
| `QSpinBox` / `QDoubleSpinBox` | numeric value |
| `QProgressBar` | numeric value |
| `QLabel` | its text |
| `QAction` inside a `QMenu` | label text, or `"checked"` / `"unchecked"` for checkable actions |
| containers / custom-painted surfaces | omitted |

`QMenu` nodes also expose their `QAction` entries as action objects under
`actions` and as synthetic `children`, so popup menus can be inspected while
another request is blocked inside `QMenu::exec()`. Action objects include the
display `text`, check state, enabled/visible state, global `geometry` when the
menu is visible, and metadata such as `toolTip`, `statusTip`, and `data` when
present.

**Extra observable fields** (all non-destructive — no control is stepped):

- `toolTip` — any widget's hint text, so distinctions that live only in the
  tooltip are assertable (e.g. two "Clear" buttons: *Clear all bookmarks* vs
  *Clear the displayed SWR sweep trace.*).
- `items` + `currentIndex` — a `QComboBox`'s full option list and active index,
  so you can verify the available choices without stepping (and applying) each
  selection.
- `panIndex` — a `SpectrumWidget`'s pan index in a multi-pan layout; pass it to
  `grab pan <index>` or `pan close <index>`.

### `grab`
PNG capture of a single widget.

```json
→ {"cmd":"grab","target":"SpectrumWidget","path":"/tmp/pan.png"}
← {"ok":true,"target":"SpectrumWidget","class":"SpectrumWidget",
   "path":"/tmp/pan.png","width":2896,"height":1502,"bytes":2248854}
```

- `path` is optional. If omitted, the PNG is written to
  `${TMPDIR}/aether-grab-<target>.png` and the path is returned.
- The panadapter is a GPU (`QRhiWidget`) surface; the bridge does the correct
  framebuffer readback for it, so the capture is the *real* rendered spectrum,
  not a blank.

**`grab pan <index> [path]`** captures a *specific* pan's spectrum surface in a
multi-pan layout, keyed on the `panIndex` from `dumpTree`. Plain
`grab SpectrumWidget` always resolves the first one, so it can't reach pan 1+.

```json
→ {"cmd":"grab","target":"pan","selector":"1","path":"/tmp/pan1.png"}
← {"ok":true,"target":"pan1","class":"SpectrumWidget","panIndex":1,
   "path":"/tmp/pan1.png","width":2280,"height":686}
```

An unknown index returns `{"ok":false,"error":"no pan with index N","available":[0]}`.

### `invoke`
Drive a control deterministically — no pixel-hunting. Resolves `target` exactly
like `grab`.

```json
→ {"cmd":"invoke","target":"Master volume","action":"setValue","value":"35"}
← {"ok":true,"target":"Master volume","class":"QSlider","action":"setValue","newValue":"35"}
```

`newValue` echoes the control's state *after* the action (same field `dumpTree`
reports) — a free round-trip confirmation.

**Disabled controls are refused.** Qt's `setValue()`/`setChecked()` still mutate
a *disabled* widget, so without a guard the bridge would report a happy
`newValue` while the radio never sees the change (the control is greyed out for
a reason — wrong mode, not connected, …). `invoke` on a disabled widget returns
`{"ok":false,"disabled":true,"error":"refused: '…' is disabled …"}` instead, so
the no-op is an explicit, assertable signal.

| `action` | applies to | `value` |
|---|---|---|
| `click` | any `QAbstractButton` | — |
| `toggle` | any `QAbstractButton` (checkable → toggle, else click) | — |
| `setChecked` | checkable button | `true`/`false`/`on`/`off`/`1`/`0` |
| `setValue` | slider / scrollbar / spinbox | integer (or number for double-spin) |
| `setText` | `QLineEdit` | the text |
| `setCurrentText` | `QComboBox` | item text |
| `setCurrentIndex` | `QComboBox` | integer index |
| `trigger` / `click` / `toggle` | visible `QMenu` `QAction` | — |
| `setChecked` | checkable visible `QMenu` `QAction` | `true`/`false`/`on`/`off`/`1`/`0` |

<a name="tx-safety"></a>
> **🚨 TX safety.** `invoke` **refuses any control that keys the transmitter**,
> returning `{"ok":false,"error":"blocked: …"}` and never calling the widget. A
> test bridge must never key a live transmitter by accident.
>
> The guard is **marker-driven, not name-driven**. Genuinely-keying controls
> (MOX/PTT, TUNE, ATU, CWX CW send, AX.25 packet/APRS send) are tagged at their
> creation site with `markTxKeying()` — the `aetherTxKeying` dynamic property —
> and the guard refuses anything carrying it. This is authoritative: a control
> is blocked because it was *declared* keying, not because its label matched a
> word, so it catches keying buttons like **"Send"** that no keyword would. A
> marked control shows `"keying": true` in `dumpTree`, so you can see what's
> off-limits before you try. A button-scoped name heuristic
> (`mox/ptt/tune/atu/transmit/vox/cwx`) remains as a logged belt-and-suspenders
> fallback for any keying control that predates the marker. The fallback is
> **whole-token anchored**: the name is split on camelCase humps and separators,
> and a deny-word must equal a complete token — so `aprsSvcWXBOT` (svc + wxbot)
> and `temperature` no longer false-match `cwx`/`atu`, while `moxButton` and
> `Auto-Tune` still do. Setpoint
> **sliders/combos** like `Tune power`, `RF power`, or `VOX level` are never
> blocked — moving a value setter can't transmit.
>
> To deliberately drive a keying control (e.g. hardware-in-the-loop on a dummy
> load), set `AETHER_AUTOMATION_ALLOW_TX=1` in the app's environment at launch.
> Adding a new keying control? Call `markTxKeying(theButton)` — see
> `src/core/TxKeyingMarker.h`.

### `get`
Read live model state — assert on truth without a screenshot. Requires a radio
model (present once the app is running; fields are empty until a radio
connects).

```json
→ {"cmd":"get","model":"radio"}
← {"ok":true,"model":"radio","radio":{"connected":true,"model":"FLEX-8400M",
   "transmitting":false,"txPower":0,"sliceCount":1,"panCount":1, …}}

→ {"cmd":"get","model":"slice","selector":"active","property":"frequency"}
← {"ok":true,"model":"slice","property":"frequency","value":3.6}
```

| `model` | `selector` | returns |
|---|---|---|
| `radio` | — | radio snapshot (name, model, version, connected, transmitting, txPower, paTemp, slice/pan counts) |
| `transmit` | — | TX-chain snapshot: RF/tune power, mic/processor/monitor, VOX/AM/DEXP, TX filter, CW (speed/pitch/breakin/delay/sidetone/iambic/monitor), ATU, APD. Validate that a TX/Phone/CW applet control reached the radio model. |
| `equalizer` (or `eq`) | — | 8-band RX+TX graphic EQ: `rxEnabled`/`txEnabled` and `rx`/`tx` band maps keyed by label (`63`…`8k`). Validate EQ-applet slider changes. |
| `slices` | — | array of all slice snapshots |
| `slice` | `active` (default) / `tx` / `<sliceId>` | one slice (sliceId, letter, frequency, mode, filterLow/High, rxAntenna, nb/nr/anf + levels, **squelch/squelchLevel, agcMode/agcThreshold, apf/apfLevel**, txSlice, …) |
| `pans` | — | array of all panadapter snapshots |
| `pan` | `active` (default) / `<panId>` e.g. `0x40000000` | one pan (centerMhz, bandwidthMhz, min/maxDbm, rxAntenna, rfGain, fps) |

Add a trailing **property** name to any single-object form to get just that
field: `get slice active mode` → `{"value":"LSB"}`.

### `close`
Close the target's **top-level window**. Resolves `target` like `grab`, then
closes `target->window()` — so a child control closes its dialog. This reaches
the custom frameless title-bar close (a clickable `QLabel`, accessibleName
*Close window*) that `invoke … click` can't target, and works for any window.

```json
→ {"cmd":"close","target":"Theme actions"}
← {"ok":true,"target":"Theme actions","class":"ThemeEditorDialog",
   "title":"Theme Editor — Default Dark","deferred":true}
```

`deferred:true`: the close runs on the next main-loop turn (a `closeEvent` may
pop a confirm dialog), so re-read `dumpTree` to confirm the window is gone.

### `drag` (alias `mouse`)
Synthesize a `press → move → release` gesture so a resize grip or slider handle
is provable end-to-end, not just via seed + read-back.

```json
→ {"cmd":"drag","target":"QSizeGrip","value":"140 90"}
← {"ok":true,"target":"QSizeGrip","class":"QSizeGrip","dx":140,"dy":90}
```

`value` is `"<dx> <dy>"` in pixels from the widget centre. Global coordinates are
computed once from the press point (a `QSizeGrip` moves as the window resizes, so
re-mapping mid-drag would overshoot) — a `140 90` grip drag grows the window by
exactly 140×90.

### `showMenu` (alias `openMenu`)
Pop a `QToolButton`/`QPushButton` drop-down menu. The show is posted onto the GUI
event loop with the owning window raised + activated first — showing the native
popup from inside the socket-read callback, or while the app is backgrounded,
re-enters Cocoa and segfaults. Returns `deferred:true`; `dumpTree` to read the
opened menu. A button with no menu returns an error.

```json
→ {"cmd":"showMenu","target":"Theme actions"}
← {"ok":true,"target":"Theme actions","class":"QPushButton","deferred":true}
```

### `pan`
Panadapter lifecycle — create or tear down a pan regardless of how it was opened.

```json
→ {"cmd":"pan","action":"add"}
← {"ok":true,"pan":"add","requested":true,"panCountBefore":1}

→ {"cmd":"pan","action":"close","value":"1"}
← {"ok":true,"pan":"close","requested":true,
   "closed":[{"panId":"0x40000001","waterfallId":"0x42000001","resolved":true}]}
```

| `action` | `value` | effect |
|---|---|---|
| `create` (alias `add`) | — | create a new panadapter (panafall). The only UI path is an unaddressable label. |
| `center` | `<mhz>` | recenter the active pan — the band-change lever (a plain `tune` only moves the slice and clamps to the pan's RF range, #292). |
| `close` (alias `remove`) | `<panId>` (`0x…`) / `<index>` (panIndex) / `active` / `all` | close pan(s). Sends `display pan remove` **and** `display panafall remove`, so a panafall-created pan closes without the slice-removal workaround. A single target won't close the last pan; `all` will. |

All are async (the radio echoes the change) — re-poll `get pans`. Every `pan`
action is RX/config only; none keys the transmitter.

### Errors
Every failure is a one-line object: `{"ok":false,"error":"<message>"}` — e.g.
`widget not found: Foo`, `blocked: '…' looks transmit-related …`,
`no slice for selector 'tx'`, `unknown action: x`, `unknown command: x`.

---

## Targeting a widget

`grab` and `invoke` resolve a `target` string in this order — first match wins:

0. **Scoped `"<scope>/<name>"`** — disambiguates a control whose
   `accessibleName` appears in more than one applet (e.g. `"AF gain"` and
   `"Squelch threshold"` exist in **both** `RxApplet` and `PanadapterApplet`).
   `<scope>` matches an ancestor by objectName, class, or accessibleName;
   `<name>` is resolved within that subtree. Use `"RxApplet/AF gain"` vs
   `"PanadapterApplet/AF gain"`. Falls through to flat matching if it doesn't
   resolve, so a literal `/` in a name still works.
1. **Exact `objectName`** — the most stable handle. Prefer this.
2. **Class name** — full (`AetherSDR::SpectrumWidget`) or short
   (`SpectrumWidget`). Handy when a widget has no objectName (the panadapter is
   targeted as `SpectrumWidget`).
3. **`accessibleName`** — e.g. `"Panadapter spectrum display"`,
   `"Master volume"`.
4. **Button text** — last resort, e.g. `"Send"`, `"Transmit"`. Lowest priority,
   so a real objectName/accessibleName always wins; first match in tree order.
5. **Visible popup-menu action** — exact `QAction` objectName, visible text,
   tooltip, status tip, or data value. Use `invoke <label-or-data> trigger` to
   choose a menu item.

To find a target: run `dumpTree`, search the JSON for the `accessibleName` or
`class` you want, and use its `objectName` if it has one. Roughly half of
`src/gui/` is annotated with `setObjectName`/`setAccessibleName`; finishing that
backlog (see [`docs/a11y.md`](a11y.md), enforced by
[`tools/check_a11y.py`](../tools/check_a11y.py)) directly improves what you can
target here.

---

## Recipes

**Assert on state (no pixels) — the default.**
```python
tree = bridge.request({"cmd": "dumpTree"})
node = find(tree["roots"], accessibleName="Master volume")
assert node["value"] == "42" and node["enabled"]
```

**Capture for a genuinely-visual check.**
```python
r = bridge.request({"cmd": "grab", "target": "SpectrumWidget", "path": "/tmp/pan.png"})
assert r["ok"] and r["width"] > 0
# then view /tmp/pan.png, or perceptual-diff it against a golden (Phase 3)
```

**Drive a control and confirm the model followed.**
```python
bridge.request({"cmd": "invoke", "target": "sliceModeCombo", "action": "setCurrentText", "value": "USB"})
assert bridge.request({"cmd": "get", "model": "slice", "selector": "active", "property": "mode"})["value"] == "USB"
```

**Snapshot → act → assert** (the loop you already use for web work): snapshot
with `dumpTree`/`get`, drive the change with `invoke`, then `get` (or another
`dumpTree`) and assert the `value`/model field changed. Keep transmit out of the
loop — the guard blocks it, and so should your scenarios.

Prefer **structural** assertions (`dumpTree` values) over screenshots wherever
possible — they're exact, fast, and identical across OSes. Reserve `grab` +
image comparison for assertions that are *inherently* visual (did the waterfall
actually paint? is the layout right?), because a live spectrum is
non-deterministic noise and won't golden-match until replay mode (Phase 2)
lands.

---

## Gotchas

- **Off by default.** No `AETHER_AUTOMATION` → no server, zero overhead, no
  socket. This is intentional; never enable it in a shipped build.
- **`invoke` can't key the radio.** Transmit controls are refused unless
  `AETHER_AUTOMATION_ALLOW_TX=1` — see [TX safety](#tx-safety). Don't disable the
  guard just to get a test green.
- **`get` needs a model.** It reads the active-session `RadioModel`; fields are
  empty/zero until a radio connects. Run it once connected, or assert on
  `connected` first.
- **GPU panadapter capture.** `SpectrumWidget` is a `QRhiWidget` when built with
  `AETHER_GPU_SPECTRUM` (the default). The bridge uses
  `QRhiWidget::grabFramebuffer()` for it — plain `QWidget::grab()` returns an
  empty surface for a GPU widget, so don't reimplement capture that way.
- **Live spectrum isn't golden-able.** Pixels off a live radio are noise.
  Deterministic visual diffs need the recorded-fixture replay mode (Phase 2).
- **Stale socket after a crash.** On a hard kill the C++ destructor may not run,
  leaving the socket + discovery file behind. This self-heals: the next launch
  clears the stale socket (`removeServer`) and rewrites the discovery file.
- **Geometry is global.** `geometry` is in screen coordinates (via
  `mapToGlobal`), so it correlates with computer-use/screenshots if you ever
  cross-check.

---

## Roadmap (issue #3646)

| Phase | Adds | Status |
|---|---|---|
| 0 | `dumpTree` + `grab` over `QLocalServer` behind `AETHER_AUTOMATION` | **done** |
| 1 | `invoke <target> <action>` (TX-guarded) + `get radio\|slice\|pan` model snapshots | **done** |
| 2 | Replay/fixture mode (recorded VITA-49 FFT + meters) → deterministic panadapter without hardware | planned |
| 3 | CI E2E matrix: `QT_QPA_PLATFORM=offscreen` + agent scenarios + per-OS perceptual golden diffs | planned |
| 4 | Computer-use / VNC kept as the *exploratory* tier (real GPU/WM smoke), not the regression backbone | planned |

## Source

- Server: [`src/core/AutomationServer.h`](../src/core/AutomationServer.h) /
  [`.cpp`](../src/core/AutomationServer.cpp)
- Startup wiring: [`src/main.cpp`](../src/main.cpp) (after `window.show()`)
- Driver: [`tools/automation_probe.py`](../tools/automation_probe.py)
- Validation sweep: [`tools/automation_validate.py`](../tools/automation_validate.py)
  — records → 3-value scale probe (with circular/wrapping classification) →
  model cross-check → restore, over every value-bearing applet control;
  scoped-targets duplicates, skips disabled + keying controls, and prints a
  findings table with timing. The reusable form of the QA sweep.
- Log category: `lcAutomation` (`aether.automation`) — toggle in Help → Support.
