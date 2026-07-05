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
| Key the radio (MOX/PTT/Tune) | **Only deliberately** — `invoke` refuses transmit controls by design; the dedicated [transmit verbs](#transmit-verbs--gated) (`key`/`cwx`/`txtest`/`atu`) work **only** under `AETHER_AUTOMATION_ALLOW_TX=1` (see [TX safety](#tx-safety)). |
| Read client-side DSP / window / floor state | **Yes** — `get dsp`, `dumpTree` `windowState`, `floors`. |

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

KiwiSDR compression can be forced for diagnostic runs by adding
`AETHER_KIWI_SND_COMP=1` and/or `AETHER_KIWI_WF_COMP=1` at launch. These are
receive-only automation knobs: SND changes the outbound sound setup request
from `SET compression=0` to `SET compression=1`; W/F changes the outbound
waterfall setup request from `SET wf_comp=0` to `SET wf_comp=1`. The runtime
still decodes the actual observed frame layout. The `get kiwi` snapshot exposes
top-level `diagnosticSoundCompressionRequested` and
`diagnosticWaterfallCompressionRequested` fields, plus connected profiles'
per-stream `compressedRequested`, so automation can assert that the process
launched with the intended diagnostic mode and then separately check
`compressedObserved`. Kiwi profile `state` may also report
`busy`, `waiting`, `camping`, or `camp_disconnected`; the profile `metadata`
object includes typed busy/camping fields such as `campStatus`,
`campReceiverChannel`, `campQueuePosition`, `campQueueWaiters`, and
`campQueueReloadRecommended` when the server reports them.

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

Every request is one verb. The table below is the **complete** catalog, grouped
by category; each verb links to its detailed section. A ⚠️ marks the
transmit-gated verbs (refused unless `AETHER_AUTOMATION_ALLOW_TX=1` — see
[TX safety](#tx-safety)).

| Category | Verb | One-liner |
|---|---|---|
| **Introspection** | [`ping`](#ping) | Handshake; returns app + version. |
| | [`dumpTree`](#dumptree) | ARIA-style snapshot of the whole widget tree. |
| | [`grab <target> [path]`](#grab) | PNG of one widget (GPU-correct for the panadapter). |
| | [`grab pan <index> [path]`](#grab) | Raw spectrum surface of a specific pan. |
| | [`grab pan-visible <index> [path]`](#grab) | Pan applet incl. VFO/flag overlays (alias `pan-composite`). |
| | [`floors`](#floors) | Per-pan measured noise + display floor (dBm). |
| | [`whoami`](#whoami) | This bridge instance: pid, socket, label, station, `txAllowed`. |
| **Drive** | [`invoke <target> <action> [v]`](#invoke) | Click/toggle/set/selectRow/submit/trigger a control (TX-guarded). |
| | [`close <target>`](#close) | Close the target's top-level window. |
| | [`drag <target> "<dx> <dy>"`](#drag-alias-mouse) | Synthesize press→move→release (alias `mouse`). |
| | [`showMenu <target>`](#showmenu-alias-openmenu) | Pop a button's drop-down menu (alias `openMenu`). |
| | [`contextMenu <target> [x y]`](#contextmenu) | Trigger a custom right-click menu. |
| | [`menu list \| open <name>`](#menu) | Enumerate / pop a menu-bar menu. |
| | [`resize <w> <h> [target]`](#resize) | Resize a window (drives panadapter `x_pixels`). |
| | [`window <state> [target]`](#window) | maximize / restore / minimize / fullscreen. |
| | [`scrollTo <target>`](#scrollto-alias-ensurevisible) | Scroll a widget into its scroll-area viewport. |
| **State (`get`)** | [`get audio`](#get) | Audio-engine stream/buffer snapshot. |
| | [`get dsp`](#get-dsp) | Client-side AetherDSP NR state (NR2…BNR). |
| | [`get radio \| transmit \| eq \| meters`](#get) | Radio / TX-chain / EQ / meters snapshots. |
| | [`get slice[s] \| pan[s]`](#get) | Slice & panadapter model snapshots. |
| | [`get panstats`](#get-panstats) | Per-panadapter render-cost counters (profiling). |
| | [`get clients`](#get-clients) | Radio client roster + foreign-pan-write forensics (#3977). |
| | [`get sync`](#get-sync) | Receive-Sync (Auto Assist) state. |
| | [`get wavestats`](#get-wavestats) | WAVE/strip scope paint-cost counters. |
| | [`get dax`](#get-dax) | DAX RX channel-ownership table (holders/streams, #3305). |
| **Connection** | [`connect …`](#connect--disconnect) | list / show / hide / local / ip / wait. |
| | [`disconnect`](#connect--disconnect) | Normal user disconnect. |
| **Tuning & slices** | [`tune <mhz>`](#tune) | Set the active slice frequency (VFO; not keying). |
| | [`slice <action>`](#slice) | add/remove/select/tx/txant/rxant/rxsource. |
| **Display / pans** | [`pan <action>`](#pan) | create / center / close a panadapter. |
| | [`streams [radio\|resync\|reset]`](#streams) | Radio-side display-stream leak detector. |
| | [`txwaterfall on\|off`](#txwaterfall) | Toggle "show TX in waterfall". |
| **DAX / TCI** | [`tci start\|status\|stop`](#tci) | In-process TCI client simulator (WSJT-X-shaped). |
| **Observability** | [`log <action>`](#log) | Runtime log-category control + ring-buffer tail/subscribe. |
| | [`mark <text>`](#mark) | Drop a sequenced timeline marker. |
| | [`audioCapture <action>`](#audiocapture) | Bounded PCM capture for sync diagnostics. |
| | [`record <action>`](#record) | Drive the client QSO WAV recorder. |
| **Identity** | [`station <name>`](#station) | Set this client's MultiFlex station name. |
| **QRZ lookup** | [`qrz <action>`](#qrz) | Callsign-lookup status / cache probe / lookup / CW-spot simulation. |
| **Transmit ⚠️** | [`key ptt on\|off` / `key mox`](#key) | Key/unkey via PTT / MOX. |
| | [`cwx send <text> \| speed <wpm> \| stop`](#cwx) | Drive the CWX CW keyer. |
| | [`txtest twotone\|off`](#txtest) | Two-tone test signal. |
| | [`atu bypass\|start`](#atu) | ATU bypass (no TX) / tune cycle (keys TX). |
| | [`testtone on [hz] [db] \| off`](#testtone) | Client TX test tone into the mic path. |

> **Two request forms, always interchangeable.** Bare line (`get slice active mode`)
> or JSON (`{"cmd":"get","model":"slice","selector":"active","property":"mode"}`).
> The JSON field names per verb are noted in each section; positional order is
> shown in the bare-line examples.

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
  "windowState": "maximized",              // top-level windows only: normal|maximized|minimized|fullscreen
  "value": "42",                           // best-effort; see below
  "text": "NR2",                           // checkable buttons: the label (value would just be "checked")
  "checked": false,                        // checkable buttons: explicit boolean check-state
  "range": { "min": 0, "max": 100 },       // numeric controls only (slider/spinbox)
  "items": ["LSB","USB","AM","CW"],        // QComboBox only: full option list
  "currentIndex": 1,                       // QComboBox only: selected index
  "panIndex": 0,                           // SpectrumWidget only: pass to `grab pan`/`pan close`
  "noiseFloorDbm": -99.68,                 // SpectrumWidget only: measured floor (see `floors`)
  "displayFloorDbm": -99.17,               // SpectrumWidget only: display floor
  "gaugeLabel": "71.3°C",                  // HGauge only: centred bar label (live overlay text)
  "gaugeValue": 71.3,                      // HGauge only: current numeric value
  "gaugeRange": { "min": 0, "max": 120, "redStart": 70, "yellowStart": 55 },  // HGauge only: scale + zones
  "gaugeTicks": "0,30,55,70,90,120",       // HGauge only: comma-joined tick labels
  "sliceId": 0,                            // present on widgets tagged with a slice
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
- `windowState` — on top-level windows: `normal` / `maximized` / `minimized` /
  `fullscreen`, so a `window` action (or a manual maximize) is assertable.
- `text` + `checked` — on a **checkable** button, the label and an explicit
  boolean state. `value` alone reports only `"checked"`/`"unchecked"`, which hid
  *which* control it was (e.g. the six DSP method buttons NR2…BNR all read
  `"checked"`); `text` restores the identity.
- `noiseFloorDbm` / `displayFloorDbm` — a `SpectrumWidget`'s live measured floors
  (the same values [`floors`](#floors) returns), for numeric floor assertions.
- `gaugeLabel` / `gaugeValue` / `gaugeRange` / `gaugeTicks` — an `HGauge`'s
  centred bar label, current numeric value, scale (`min`/`max`/`redStart`/`yellowStart`),
  and tick labels. `HGauge` is a custom-painted widget with no `Q_OBJECT`, so it
  otherwise serializes as a bare `QWidget` carrying only its `accessibleName`;
  these fields make the horizontal bar gauges (PA temp / supply / fan on the
  Radio Hardware applet, and the TX SWR / forward-power / ALC / mic / compression
  meters) numerically assertable — e.g. proving the MtrApplet °C↔°F toggle
  switches the PA-temp scale from `0–120` (ticks `0,30,55,70,90,120`) to `32–248`
  (ticks `32,86,131,158,194,248`) and updates the live overlay text, without a
  screenshot. Published only under `AETHER_AUTOMATION` (zero cost otherwise).

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

**`grab pan <index> [path]`** captures a *specific* pan's raw spectrum surface
in a multi-pan layout, keyed on the `panIndex` from `dumpTree`. Plain
`grab SpectrumWidget` always resolves the first one, so it can't reach pan 1+.
This is the GPU framebuffer only; child overlays such as VFO flags are not part
of this image.

```json
→ {"cmd":"grab","target":"pan","selector":"1","path":"/tmp/pan1.png"}
← {"ok":true,"target":"pan1","class":"SpectrumWidget","panIndex":1,
   "path":"/tmp/pan1.png","width":2280,"height":686}
```

**`grab pan-visible <index> [path]`** captures the operator-visible pan applet,
including the indexed pan's spectrum surface and child overlays such as VFO
flags. Use this for screenshots of what the user sees. `pan-composite` is an
alias.

```json
→ {"cmd":"grab","target":"pan-visible","selector":"1","path":"/tmp/pan1-visible.png"}
← {"ok":true,"target":"pan-visible1","class":"PanadapterApplet","panIndex":1,
   "surfaceClass":"SpectrumWidget","path":"/tmp/pan1-visible.png",
   "width":2280,"height":710}
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
| `wheel` | any visible widget | one wheel notch: `-1` or `1` |
| `setText` | `QLineEdit` | the text (side-effect-free — does **not** submit) |
| `submit` | `QLineEdit` | optional text, then fires `returnPressed` (retune / login / send) |
| `setCurrentText` | `QComboBox` (item text) / `QTabBar` (tab label, case-insensitive — reaches deferred setup-dialog tabs) | text |
| `setCurrentIndex` | `QComboBox` / `QTabBar` | integer index |
| `selectRow` | `QAbstractItemView` (`QTableWidget`/`QTreeWidget`/`QListWidget`) | integer row index |
| `trigger` / `click` / `toggle` | visible `QMenu` `QAction` | — |
| `setChecked` | checkable visible `QMenu` `QAction` | `true`/`false`/`on`/`off`/`1`/`0` |

**`submit` vs `setText`.** `setText` only sets the field — deliberately
side-effect-free, because several bridge-reachable fields wire irreversible
actions to `returnPressed` (SmartLink login, manual-connect host, DX-cluster
send). `submit` is the explicit opt-in that sets (optional) then fires
`returnPressed` — use it to commit a frequency entry, a login, or a cluster
command.

**`selectRow`** selects a whole row in an item view (sets the current index
**and** a full-row selection), so a dialog's row-scoped buttons (Tune / Edit /
Remove / Disable) — which read the view's current row or selection — become
drivable; plain `invoke click` on those buttons is a no-op until a row is
selected. The reply echoes `selectedRow` and `selectedRowText` (first-column
text) as the round-trip confirmation. Row index is **order-sensitive**:
re-`dumpTree` (or re-read) after any sort, filter, or insert.

```json
→ {"cmd":"invoke","target":"Scheduled nets","action":"selectRow","value":"0"}
← {"ok":true,"target":"Scheduled nets","class":"QTableWidget","action":"selectRow",
   "selectedRow":0,"selectedRowText":"✓"}
```

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
> (`mox/ptt/transmit/cwx`) remains as a logged belt-and-suspenders
> fallback for any keying control that predates the marker. The fallback is
> **whole-token anchored**: the name is split on camelCase humps and separators,
> and a deny-word must equal a complete token — so `aprsSvcWXBOT` (svc + wxbot)
> no longer false-matches `cwx`, while `moxButton` still matches `mox`. The list
> was deliberately narrowed from the old `…/tune/atu/vox/…` set: `tune`/`atu`/`vox`
> false-blocked **RX-only** buttons like **"Tune Now"** (net/spot retune) and VOX
> toggles, and the genuine keying TUNE/ATU buttons all carry `markTxKeying()`
> anyway, so the marker still covers them (#3918). Setpoint
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
| `audio` | — | audio-engine snapshot (RX/TX stream state, mute, buffer counters, KiwiSDR TX mute gate) |
| `dsp` | — | client-side AetherDSP noise-reduction state — see [`get dsp`](#get-dsp) |
| `radio` | — | radio snapshot (name, model, version, connected, fullDuplex, transmitting, txPower, paTemp, slice/pan counts) |
| `transmit` | — | TX-chain snapshot: RF/tune power, mic/processor/monitor, VOX/AM/DEXP, TX filter, CW (speed/pitch/breakin/delay/sidetone/iambic/monitor), ATU, APD. Validate that a TX/Phone/CW applet control reached the radio model. |
| `equalizer` (or `eq`) | — | 8-band RX+TX graphic EQ: `rxEnabled`/`txEnabled` and `rx`/`tx` band maps keyed by label (`63`…`8k`). Validate EQ-applet slider changes. |
| `meters` | — | `{all:[…]}` — every radio meter with `name`, `value`, `unit`, `low`/`high`, `description`, and **`age_ms`** (staleness): a meter that updates has small `age_ms` and a tracking `value`. |
| `slices` | — | array of all slice snapshots |
| `slice` | `active` (default) / `tx` / `<sliceId>` | one slice (sliceId, letter, frequency, mode, filterLow/High, rxAntenna, nb/nr/anf + levels, **squelch/squelchLevel, agcMode/agcThreshold, apf/apfLevel**, **adaptiveFilterEnabled/adaptiveMinLowCut/adaptiveMaxHighCut/adaptiveMinSnr/adaptiveResponse/adaptiveSplatter/adaptiveActive** (SSB adaptive RX filter — `adaptiveActive` is the live AUTO-fit state), txSlice, …) |
| `pans` | — | array of all panadapter snapshots |
| `pan` | `active` (default) / `<panId>` e.g. `0x40000000` | one pan (centerMhz, bandwidthMhz, min/maxDbm, rxAntenna, rfGain, fps) |
| `panstats` | `<panIndex>` / `<objectName>` (default: all) | per-panadapter render-cost counters — see [`get panstats`](#get-panstats) |
| `wavestats` | `—` / scope objectName | waveform-scope paint/append counters — see [`get wavestats`](#get-wavestats) |
| `clients` | — | connected-client roster, per-pan ownership, foreign dBm-write counters and evictions — see [`get clients`](#get-clients) |
| `dax` | — | DAX RX channel-ownership table — see [`get dax`](#get-dax) |

Add a trailing **property** name to any single-object form to get just that
field: `get slice active mode` → `{"value":"LSB"}`.

### `get panstats`
Per-panadapter (SpectrumWidget) frame-cost counters — how much GUI-thread time
each pan spends preparing frames, split by pipeline section, for before/after
rendering-cost proofs without a profiler attach. Counters are always on and
cost a few integer adds per frame.

```json
→ {"cmd":"get","model":"panstats"}
← {"ok":true,"model":"panstats","pans":[{
   "panIndex":0,"renderMode":"2D","renderer":"GPU QRhi (Metal; Apple M1 Ultra)",
   "widthPx":2280,"heightPx":1302,"dpr":2.0,"leanMode":false,"sinceMs":60012,
   "fftFramesPerSec":29.6,"ingestMsPerSec":8.1,
   "gpuFramesPerSec":29.6,"gpuFrameMsPerSec":97.4,"avgGpuFrameUs":3290.0,
   "fftBuildMsPerSec":64.2,"fftVboBytesPerSec":42049536.0,
   "overlayRebuildsPerSec":0.1,"overlayRebuildMsPerSec":1.9,
   "overlayUploadBytesPerSec":3964928.0,"wfUploadBytesPerSec":18240.0,
   "paintsPerSec":0.0,"paintMsPerSec":0.0,
   "overlayDirtyCauses":{"smartMtr":2,"detect":1,"other":3}}]}
```

| field | meaning |
|---|---|
| `gpuFrameMsPerSec` | **the headline number** — main-thread ms consumed per wall-second preparing + encoding this pan's GPU frames |
| `ingestMsPerSec` | `updateSpectrum()` cost (EMA smoothing, noise floor, waterfall fallback pacing) |
| `fftBuildMsPerSec` / `fftVboBytesPerSec` | FFT trace resample + vertex bake cost and VBO upload volume |
| `overlayRebuilds*`, `overlayUploadBytesPerSec` | static-overlay QPainter repaints (should be ~0/s when idle) |
| `overlayDirtyCauses` | first-cause attribution for each overlay rebuild (`smartMtr`, `detect`, `other`) |
| `wfUploadBytesPerSec` | waterfall texture upload volume |
| `paintsPerSec` / `paintMsPerSec` | software-QPainter path (non-zero only before QRhi init or in non-GPU builds) |

`selector` filters by pan index (`get panstats 0`) or objectName. `property`
`reset` zeroes the counters after the read so successive reads measure
disjoint intervals: `get panstats 0 reset`.

### `get clients`
Multi-session forensics (#3977/#3951): every client connected to the radio,
which of them have written **our** pans' dBm range, and which stale
predecessor sessions this client has evicted. `get pans` shows the symptom
(`minDbm` drifting between polls); this shows the culprit. Pan snapshots
(`get pan`/`pans`) also carry `clientHandle` + `ownedByUs` for ownership
assertions.

```json
→ {"cmd":"get","model":"clients"}
← {"ok":true,"model":"clients","evictionEnabled":false,
   "ourHandle":"0x443a5d3c","station":"Shack",
   "clients":[
     {"handle":"0x443a5d3c","station":"Shack","program":"AetherSDR","source":"","isUs":true},
     {"handle":"0x42ffe1c4","station":"Shack","program":"AetherSDR","source":"","isUs":false}],
   "foreignPanWrites":[
     {"handle":"0x42ffe1c4","dbmWrites":3,"lastPanId":"0x40000000",
      "lastMs":1783125692000,"evicted":true}],
   "evictedHandles":["0x42ffe1c4"]}
```

| field | meaning |
|---|---|
| `clients` | radio's client roster (from `sub client all`): handle, station, program, `isUs` |
| `foreignPanWrites` | per-handle tally of `min_dbm`/`max_dbm` status writes some OTHER client made against a pan whose radio-confirmed owner is us — the #3951 zombie signature |
| `evictedHandles` | stale same-station/same-program sessions whose `client disconnect` the radio **acknowledged** (confirmed, not merely attempted), via pan-reclaim or the 3-strike foreign-write rule |
| `evictionEnabled` | whether the 3-strike eviction may act. **Off by default** — detection and forensics always run; the force-disconnect requires `AppSettings["StaleSessionDefense"]` = `{"EvictionEnabled": true}`. The pan-reclaim eviction (scoped to our own pre-reconnect handle) is always active |

Counters and eviction marks are per-connection: they reset on disconnect,
because the radio recycles handle values across sessions.

Test recipe for session-fight classes of bugs: connect a second client to the
same pan (or replay `display pan set … min_dbm=…` from a raw TCP session —
see `tools/zombie_session_sim.py`), then assert `foreignPanWrites`
increments. With `EvictionEnabled` true and the offender's station+program
matching ours (it must be a **GUI** client to appear in the roster — a
`--bind-client-id` non-GUI zombie is tallied and logged but never evicted),
`evicted` flips true once the radio acknowledges the disconnect and the
offender's connection drops.

### `get dsp`
Client-side **AetherDSP** noise-reduction state — the counterpart to the
radio-side `nr`/`nb`/`anf` in `get slice`. There is no widget that exposes which
of the six AudioEngine NR modules is active and how it's tuned, so this is the
only non-screenshot way to assert it.

```json
→ {"cmd":"get","model":"dsp"}
← {"ok":true,"model":"dsp","dsp":{
   "active":"none",
   "methods":{
     "NR2":{"enabled":false,"available":true},
     "NR4":{"enabled":false,"available":true},
     "MNR":{"enabled":false,"available":true},
     "DFNR":{"enabled":false,"available":false},
     "RN2":{"enabled":false,"available":true},
     "BNR":{"enabled":false,"available":false}},
   "tuning":{
     "nr2":{"gainMax":0.6,"gainSmooth":0.85,"qspp":0.2,"gainMethod":2,"npeMethod":0,"aeFilter":true},
     "nr4":{"reductionDb":10,"smoothing":0,"whitening":0,"maskingDepth":50,"suppression":50,"noiseMethod":0,"adaptiveNoise":true},
     "mnr":{"strength":1},
     "dfnr":{"attenLimitDb":100,"postFilterBeta":0},
     "bnr":{"intensity":1}}}}
```

- `active` — the name of the **one** enabled module (the modules are mutually
  exclusive), or `"none"`. There is **no** top-level `enabled` field.
- `methods.<NAME>` — `enabled` (engine state) and `available` (whether this build
  has the backend; `available:false` reflects a compile flag such as
  `HAVE_NVIDIA_AFX` (BNR) or `HAVE_DFNR`, so the button exists but is dimmed).
- `tuning` — per-module slider params: engine getters (`mnr`/`dfnr`) and the
  persisted `bnr` intensity, merged with the AppSettings-persisted
  NR2/NR4/DFNR-beta values. (BNR is the in-process NVIDIA AFX denoiser since
  #3902 — no container, so it exposes only `intensity`.)
- A trailing property narrows it: `get dsp active` → `{"value":"NR2"}`.

### `get wavestats`
Per-scope paint/append counters from every `WaveformWidget` instance — the
sidebar WAVE applet (`waveAppletScope`) plus the Aetherial strip's TX/RX
waveform panels (`stripWaveformScope`). This is the no-profiler way to prove a
rendering-cost change: `paintMsPerSec` is the main-thread paint budget the
scope actually consumed, in milliseconds per wall-clock second.

```json
→ {"cmd":"get","model":"wavestats"}
← {"ok":true,"model":"wavestats","scopes":[{
   "name":"waveAppletScope","windowTitle":"AetherSDR","windowClass":"AetherSDR::MainWindow",
   "floating":false,"visible":true,"tx":false,"paused":false,
   "mode":"Scope","fps":60,"windowMs":1000,"sampleRate":48000,
   "widthPx":244,"heightPx":110,"sinceMs":40012,
   "paintCount":2381,"paintsPerSec":59.5,"avgPaintUs":312.4,"maxPaintUs":1893,
   "paintMsPerSec":18.6,"appendsPerSec":124.9,"samplesPerSec":47980.1}]}
```

- `paintMsPerSec` — `avgPaintUs × paintsPerSec / 1000`; the headline number.
- `mode` uses the applet's UI names: `Scope` / `Envelope` / `History` / `Bands`.
- `floating` + `windowClass` — which top-level surface hosts the scope
  (`MainWindow` docked, `FloatingContainerWindow` popped out, or the strip).
- Counters accumulate from app start; a selector narrows to one scope
  (`get wavestats waveAppletScope`) and the pseudo-property `reset` zeroes
  the counters after the read (`get wavestats "" reset`) so successive reads
  measure disjoint intervals.
- Hidden scopes keep counting appends (the data feed stays live) but never
  paint — `paintsPerSec` 0 with a nonzero `appendsPerSec` is the expected
  hidden-widget signature, not a bug.

### `tune`
Set the **active slice's** frequency in MHz — the most fundamental control the
custom-painted `VfoWidget` couldn't expose. RX/config only; despite the name it
does **not** key (cf. `atu tune`, which does). Honors the per-slice VFO lock.

```json
→ {"cmd":"tune","value":"7.175"}
← {"ok":true,"tune":7.175,"sliceId":0,"letter":"A"}
```

Refused with `refused: slice A is VFO-locked` when the slice is locked. To
recenter the *pan* (band change) rather than move the slice within it, use
[`pan center`](#pan).

### `slice`
Slice lifecycle, TX assignment, antennas, and receive source. All actions are
RX/config — none keys the transmitter. `add`/`remove`/`tx` are async
(radio-authoritative); re-poll `get slices`.

```json
→ {"cmd":"slice","action":"add","value":"14.074"}
← {"ok":true,"slice":"add","freq":14.074,"requested":true,"sliceCount":2}

→ {"cmd":"slice","action":"tx","value":"1"}
← {"ok":true,"slice":"tx","id":1,"requested":true}
```

| `action` | `value` | effect |
|---|---|---|
| `add` | optional `<mhz>` | create a slice (radio-wide slot capacity is pre-checked; refused at the slice limit, naming any foreign occupant) |
| `remove` | `<sliceId>` | remove a slice (refuses the last one) |
| `select` | `<sliceId>` | make a slice the active slice (`slice set <id> active=1`) |
| `tx` | `<sliceId>` | make a slice the TX slice — the external-split transition; radio enforces single-TX |
| `txant` / `rxant` | `<port>` e.g. `ANT2` | set the TX/RX antenna of the TX (else active) slice; validated against the slice's antenna list — establish the dummy-load antenna before any TX-safety gate, then read back with `get slice tx txAntenna` |
| `rxsource` (alias `source`) | see below | select the slice's receive source (Flex / virtual-Kiwi) |

#### `slice rxsource`
Selects the receive source for a slice through the same virtual-Kiwi path as
the GUI RX antenna menus. The source selector is not a static list: it resolves
against the operator's saved Kiwi receiver profiles by configured name, display
name, profile id, virtual antenna token, or endpoint. Use `flex`, `none`, or
`clear` to return the slice to Flex audio.

```json
→ {"cmd":"slice","action":"rxsource","value":"7 K4JK"}
← {"ok":true,"slice":"rxsource","id":7,"source":"kiwi",
   "profileName":"K4JK","requested":true}

→ {"cmd":"slice","action":"rxsource","value":"7 flex"}
← {"ok":true,"slice":"rxsource","id":7,"source":"flex","requested":true}
```

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

### `hover`
Synthesize a pointer **hover** over a widget (no button pressed, unlike `drag`)
so hover-driven UI is provable end-to-end. The bare form fires a `QEnterEvent`
plus a no-button `QMouseMove` at the widget centre; the `leave` form fires a
`QEvent::Leave` so a driver can watch a fade-after-exit timer.

```json
→ {"cmd":"hover","target":"Forward power gauge"}
← {"ok":true,"target":"Forward power gauge","class":"QWidget","action":"enter","x":1572,"y":993}
→ {"cmd":"hover","target":"Forward power gauge","action":"leave"}
← {"ok":true,"target":"Forward power gauge","class":"QWidget","action":"leave", ...}
```

Used to prove the TX meter mouse-over value readout: the SWR / forward-power /
ALC / mic-level / compression `HGauge`s pop a `DragValuePopup` badge (the same
one the sliders flash) showing the live numeric value while hovered, which fades
one second after the pointer leaves. Grab the badge with `grab DragValuePopup`
— note each `HGauge` owns its own popup, so with several meters hovered the name
resolves to the first-created one; hover a single meter per instance for an
unambiguous grab.

### `scrollTo` (alias `ensureVisible`)
Scroll the target's nearest `QScrollArea` ancestor so the widget sits in the
viewport. Widgets parked below the fold of a scroll area receive **no paint
events at all** until scrolled into view (macOS clips paint delivery to the
exposed area), so a driver must bring them on screen before measuring,
hovering, or grabbing live content — e.g. the Aetherial strip's waveform
panel at the bottom of the strip's scroll column.

```json
→ {"cmd":"scrollTo","target":"stripWaveformScope"}
← {"ok":true,"target":"stripWaveformScope","class":"WaveformWidget",
   "scrollArea":"QScrollArea","vScroll":812,"hScroll":0,"inViewport":true}
```

`vScroll`/`hScroll` echo the resulting scrollbar positions and `inViewport`
confirms the widget's rect now intersects the viewport — assert on it before
trusting a follow-up measurement.

Related targeting change: when several widgets match a class, accessibleName,
or objectName target, resolution now prefers a **visible** match (every scroll
area owns hidden `QScrollBar`s next to the visible one; the strip owns a
hidden TX scope next to the visible RX one). Hidden widgets remain addressable
when they're the only match, so hidden-container grabs keep working.

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

### `contextMenu`
Trigger a widget's **custom right-click context menu** — the kind built on demand
in a `customContextMenuRequested` handler (`Qt::CustomContextMenu`) or an
overridden `contextMenuEvent` (`Qt::DefaultContextMenu`), which `showMenu` can't
reach because it only follows `QToolButton`/`QPushButton::menu()`. We synthesize a
`QContextMenuEvent` at the widget center (or an optional `x y` local offset) and
route it through the widget's `event()`, so Qt dispatches by the widget's context
policy automatically — `CustomContextMenu` emits `customContextMenuRequested`,
`DefaultContextMenu` calls the overridden `contextMenuEvent`. Like `showMenu`, the
trigger is posted onto the GUI event loop with the owning window raised first (the
handler pops a `QMenu` that runs its own event loop). Returns `deferred:true`;
`dumpTree` to read the opened menu, then `invoke` an item by text/path.

```json
→ {"cmd":"contextMenu","target":"SMeterWidget"}
← {"ok":true,"target":"SMeterWidget","class":"SMeterWidget","x":40,"y":12,"deferred":true}

→ {"cmd":"contextMenu","target":"SMeterWidget","value":"40 12"}
← {"ok":true,"target":"SMeterWidget","class":"SMeterWidget","x":40,"y":12,"deferred":true}
```

Section-title rows (a disabled `QWidgetAction` + `QLabel`, the app's idiom for
menu headers since `QMenu::addSection` text doesn't render under the app styling)
serialize with `"type":"header"` and the label's text, so titles are assertable
instead of blank rows.

### `menu`
Enumerate or pop a **menu-bar** menu. On macOS the native menu bar reparents its
menus to top-level `QMenu`s, so `dumpTree` finds them but `menuBar()->actions()`
is empty — this verb walks the real menu set either way.

```json
→ {"cmd":"menu","action":"list"}
← {"ok":true,"menus":[{"title":"View","actions":[
     {"text":"Default Dark","checkable":true,"checked":true,"enabled":true}, …]}, …]}

→ {"cmd":"menu","action":"open","value":"Settings"}
← {"ok":true,"menu":"open","title":"Settings"}
```

`menu open <name>` pops the menu (non-blocking `popup()`, so it can't deadlock the
socket handler); follow with `dumpTree` to read it and `invoke <label> trigger` to
choose an item. To drive a menu item whose menu is **closed**, you don't even need
`menu open` — `invoke "<label>" trigger` resolves a menu-bar `QAction` anywhere,
opening dialogs (AetherControl…, Network…, Radio Setup…) headlessly.

### `resize`
Resize a top-level window so the panadapter `x_pixels` (== `SpectrumWidget` width)
reaches a realistic value for headless render-size fidelity. Without a `target`,
the main window is resized. `full`/`default` → 1920×1080.

```json
→ {"cmd":"resize","value":"1600 900"}
← {"ok":true,"requested":{"w":1600,"h":900},"actual":{"w":1600,"h":900},"spectrumWidth":1340}
```

Returns `spectrumWidth` (the panadapter width that becomes `x_pixels` after the
~300 ms `dimensionsChanged` debounce re-pushes it to the radio). It resizes the
**window**, not `x_pixels` directly, so the local FFT decoder and the radio stay
in sync.

### `window`
Drive a top-level window's state. `resize` only ever sets explicit geometry, so an
un-maximize was previously unprovable; `window restore` does it, and `dumpTree`
carries `windowState` (`normal`/`maximized`/`minimized`/`fullscreen`) on every
window node so the result is assertable (#3918). Without a `target`, the main
window is used.

```json
→ {"cmd":"window","action":"maximize"}
← {"ok":true,"action":"maximize","windowState":"maximized","geometry":{"w":1400,"h":800}}
```

| `action` | aliases | effect |
|---|---|---|
| `maximize` | `max` | `showMaximized()` |
| `restore` | `normal`, `unmaximize` | `showNormal()` (un-maximize / un-fullscreen) |
| `minimize` | `min` | `showMinimized()` |
| `fullscreen` | `full` | `showFullScreen()` |

State changes are synchronous (no nested event loop), so the reply's
`windowState` is authoritative. `resize` and `window` share window-target
resolution (`topLevelWindowForTarget`): a child `target` resolves to its
`window()`.

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

### `connect` / `disconnect`
Connect through the same dialog and model path as the visible **Connect to
Radio** workflow. Requests are scheduled onto the next Qt event-loop turn so
the bridge does not run modal connection-conflict UI inside the local-socket
read callback.

```json
→ {"cmd":"connect","action":"list"}
← {"ok":true,"count":1,"radios":[{"serial":"1234-5678","model":"FLEX-8600",
   "address":"192.168.1.50","port":4992,"status":"Available"}]}

→ {"cmd":"connect","action":"show"}
← {"ok":true,"connect":"show","requested":true,"deferred":true,"wasVisible":false}

→ {"cmd":"connect","action":"local","value":"serial 1234-5678"}
← {"ok":true,"connect":"local","selector":"serial","serial":"1234-5678",
   "requested":true,"deferred":true}

→ {"cmd":"connect","action":"ip","value":"10.0.0.25"}
← {"ok":true,"connect":"ip","target":"10.0.0.25","requested":true,"deferred":true}

→ {"cmd":"connect","action":"wait","value":30000}
← {"ok":true,"connected":true,"elapsedMs":4210,"radio":{"connected":true,...}}

→ {"cmd":"disconnect"}
← {"ok":true,"disconnect":true,"requested":true,"deferred":true}
```

Bare-line forms are also accepted:

```text
connect list
connect show
connect hide
connect local first
connect local serial 1234-5678
connect ip 10.0.0.25
connect wait 30000
disconnect
```

`connect show` and `connect hide` idempotently show/raise or hide the
modeless **Connect to Radio** dialog for visual debugging; they do not toggle,
so `connect show` is safe when the dialog is already open. `connect local first`
captures the first currently discovered local radio's serial before scheduling
the request, so the response and deferred connect target stay consistent.
`connect local serial <serial>` selects by discovery serial. `connect ip
<host-or-ip>` uses the manual **Connect by IP** probe path; if the probe finds a
radio, the panel emits its normal `connectRequested` signal and `MainWindow`
performs the standard Multi-Flex/client-slot checks before `RadioModel`
connects. `connect wait <timeout_ms>` holds that request's response until
`RadioModel::connectionStateChanged(true)` or timeout, which is the preferred
unattended "request then assert" flow.

### `streams`
Radio-side display-stream inventory + leak detector (#3856). `get pans` can never
show a radio-side leak — the client tears down its own view on the radio's
`removed` echo, so it always looks clean. This verb reports two **independent
radio-authoritative** views, plus a reset:

**`streams` — Layer A (VITA-49 UDP truth).** Streams the radio is *still
transmitting* for an id the client no longer owns: a stream we once registered
and let go of that keeps arriving (e.g. a panafall closed without `display
panafall remove`, on firmware that keeps streaming — the #268 class).

```json
→ {"cmd":"streams"}
← {"ok":true,"scope":"udp",
   "registeredPanStreams":["0x40000000"],
   "registeredWfStreams":["0x42000000"],
   "orphanStreams":[{"streamId":"0x42000001","kind":"waterfall","packets":214,"age_ms":48}],
   "orphanCount":1}
```
An orphan whose `packets` climbs across reads with small `age_ms` is a **live
leak**; one that stops growing was a brief in-flight tail. (Keyed off
*ever-registered ∧ not-now-registered*, so it stays detectable after `pan close
all` and never mis-flags a freshly-created stream's registration lag.)

**`streams radio` — Layer B (status-bookkeeping truth).** The radio's full
display-object set (every pan + waterfall it reports, accumulated from status and
pruned on `removed`), classified `ours` / `foreign` / `orphan`, with **leaked
waterfalls** = those whose parent panadapter no longer exists. This catches the
resource-level lingering Layer A *can't* see — a waterfall the radio keeps
allocated but no longer streams (the #3843 case on firmware that stops the UDP on
pan-removal).

```json
→ {"cmd":"streams","action":"radio"}
← {"ok":true,"scope":"radio",
   "pans":[{"panId":"0x40000000","clientHandle":"0x5a3","ownership":"ours"}],
   "waterfalls":[
     {"waterfallId":"0x42000000","clientHandle":"0x5a3","ownership":"ours","parentPanId":"0x40000000","parentMissing":false},
     {"waterfallId":"0x42000001","clientHandle":"0x5a3","ownership":"orphan","parentPanId":"0x40000001","parentMissing":true}],
   "radioPanCount":1,"radioWaterfallCount":2,
   "orphanPanCount":0,"orphanWaterfallCount":1,"foreignPanCount":0,"foreignWaterfallCount":0,
   "leakedWaterfalls":["0x42000001"],"leakCount":1}
```

**`streams reset`** — clear the Layer-A orphan tally to re-baseline a before/after
measurement.

| `action` | layer | effect |
|---|---|---|
| — (default) | A | UDP-orphan inventory: registered streams + orphan streams (`streamId`, `kind`, `packets`, `age_ms`) |
| `radio` (alias `inventory`) | B | radio-authoritative display-object set: pans + waterfalls classified ours/foreign/orphan, plus `leakedWaterfalls` |
| `resync` (alias `refresh`) | B | re-subscribe (`sub pan all`) to force the radio to re-dump every display object, then re-poll `streams radio` |
| `reset` | A | clear the orphan tally |

**`streams resync`** closes the one gap Layer B can't see on its own: a waterfall
the radio keeps allocated as a resource but no longer streams, *and* whose
client-side view was already torn down (so both layers looked clean). It
re-subscribes so the radio re-dumps its present-tense set; the response is just a
trigger — re-poll `streams radio` after it settles to read the refreshed
inventory.

```json
→ {"cmd":"streams","action":"resync"}
← {"ok":true,"scope":"radio","resync":"requested",
   "hint":"re-poll 'streams radio' after ~500ms for the refreshed set"}
```

The `~500ms` is a **best-effort hint, not a contract** — the re-dump is async; if
`streams radio` still looks stale, poll again. Returns
`not connected — cannot resync display inventory` with no radio.

All `streams` actions are read-only / RX; none keys the transmitter (`resync`
sends only the `sub pan all` subscription command).

### `txwaterfall`
Toggle the radio's **show-TX-in-waterfall** display flag (`transmit set
show_tx_in_waterfall`), which gates whether keyed-up TX renders FFT-derived rows
in the waterfall. Off by default; enable it so a test can confirm CWX / tune /
ATU energy appears in the waterfall, not just in the FFT trace. This is a display
toggle — it does **not** key the transmitter.

```json
→ {"cmd":"txwaterfall","value":"on"}
← {"ok":true,"txwaterfall":true,"note":"radio echoes status; re-read with get transmit showTxInWaterfall"}
```

Accepts `on`/`off` (also `1`/`0`, `true`/`false`, `enable`/`disable`). The radio
echoes the change asynchronously — re-read with `get transmit showTxInWaterfall`.

### `get dax`
Read the centralized DAX RX channel-ownership table (#3305): which consumers
(`bridge` / `tci` / `rade`) hold each channel, the radio-side stream id, and
whether a `stream create` is in flight — plus each slice's `dax=` assignment.
This is the assertion surface for DAX/TCI lifecycle tests: storm regressions
(#4009), co-hold survival across a bridge or TCI teardown (#3363), and
grace-window stream removal, all without log-grepping.

```json
→ {"cmd":"get","model":"dax"}
← {"ok":true,"model":"dax",
   "channels":[{"channel":1,"streamId":"0x4000008","createPending":false,
                "holders":["bridge","tci"]}],
   "slices":[{"sliceId":0,"daxChannel":1}]}
```

Semantics to assert against: a channel with holders and `streamId=0x0` +
`createPending=true` is mid-create; a channel with a stream and **no** holders
is inside the 1.5 s removal grace window (it disappears once the removal
lands); a channel entry that persists with holders across a consumer teardown
proves the co-hold path.

### `tci`
In-process TCI **client** simulator. Connects to this app's own TCI server
over loopback and speaks the WSJT-X dialect: drain the init burst until
`ready;`, then send `audio_samplerate:48000;` + `audio_start:0;`, then count
the binary audio frames the server pushes. Removes the external-WebSocket
dependency for TCI/DAX lifecycle testing. Requires the TCI server to be
running (toggle via `invoke tciEnable click` if needed).

```json
→ {"cmd":"tci","action":"start"}            // optional value = port
← {"ok":true,"action":"start","port":50001} // default = the TciPort setting (50001 unless changed)

→ {"cmd":"tci","action":"status"}
← {"ok":true,"running":true,"connected":true,"ready":true,"audioStarted":true,
   "binaryFrames":412,"binaryBytes":1687552,"textMessages":37,"msSinceLastFrame":18}

→ {"cmd":"tci","action":"stop","value":"abrupt"}   // omit value for graceful audio_stop + close
← {"ok":true,"action":"stop","abrupt":true,"binaryFrames":412, …}
```

`stop abrupt` closes the socket without `audio_stop` — the WSJT-X
watchdog-reconnect shape — so a test can assert the server's debounced DAX
release and the manager's grace-window `stream remove` (watch with `get dax`).
`binaryFrames` climbing at a steady rate (~47/s at 48 kHz) is the "audio is
actually flowing" assertion; `msSinceLastFrame` spiking while `audioStarted`
is true means the stream went silent.

### `get sync`
Read the Receive Sync state used by the spectrum overlay and Auto Assist
(`sync`, alias `receiveSync`).

```json
→ {"cmd":"get","model":"sync"}
← {"ok":true,"model":"sync","status":"locked",
   "effectiveOffsetMs":470,"candidateResidualMs":0,
   "candidateConfidence":0.54,"candidatePeakCorrelation":0.77}
```

Useful fields:

| field | meaning |
|---|---|
| `status` / `statusText` | `searching`, `locked`, `coasting`, etc. |
| `effectiveOffsetMs` | Applied presentation offset; positive delays Flex relative to Kiwi |
| `candidateResidualMs` | Latest measured residual at the output-stage estimator point |
| `candidateAbsoluteOffsetMs` | Current applied offset plus residual candidate |
| `candidateConfidence` / `candidatePeakCorrelation` | Matcher quality for the latest estimate |
| `stableEstimateCount` | Count of consecutive near-equal candidate offsets |
| `lastAcceptedLock` | Whether the latest estimator pass changed/confirmed the applied lock |
| `flex*BufferMs`, `kiwi*BufferMs`, `playbackQueuedMs` | Current live-to-ear staging counters |

### `audioCapture`
Bounded, automation-only PCM capture for receive-sync diagnostics. It is active
only inside an `AETHER_AUTOMATION=1` process, is read-only, and does not change
audio routing or playback buffers.

```json
→ {"cmd":"audioCapture","action":"start","value":"5000 raw,post,output,final"}
← {"ok":true,"active":true,"durationMs":5000,
   "raw":true,"post":true,"output":true,"final":true}

→ {"cmd":"audioCapture","action":"read","path":"/tmp/aether-audio-capture.json"}
← {"ok":true,"path":"/tmp/aether-audio-capture.json","chunkCount":812,"capturedBytes":4874240}
```

Capture points:

| point | contents |
|---|---|
| `raw` | Flex/Kiwi float32 stereo PCM as it enters AudioEngine, useful for arrival-timing diagnostics |
| `post` | Source-tagged Flex/Kiwi float32 stereo after client DSP/resampling, just before each source output FIFO |
| `output` | Source-tagged Flex/Kiwi float32 stereo as the final mixer consumes each source FIFO; this is the Auto Assist estimator timing point |
| `final` | Final mixed float32 stereo bytes accepted by the RX audio sink |

The JSON file contains chunks with `point`, `source`, optional `sourceId`,
`sampleRate`, `channels`, `format: "float32le"`, `startNs`, `frames`, and
base64 `pcmBase64`. Use `audioCapture status` for metadata only and
`audioCapture stop` to stop early.

### `floors`
Per-pan **measured FFT noise floor** and the **display floor** (dBm), read off the
live spectrum without a screenshot — the numeric way to assert post-TX floor
recovery (#3804) or that the waterfall auto-range settled.

```json
→ {"cmd":"floors"}
← {"ok":true,"floors":[{"panIndex":0,"noiseFloorDbm":-99.68,"displayFloorDbm":-99.17,"visible":true}]}
```

One entry per `SpectrumWidget` that has a real measurement; the same numbers
appear per-node in `dumpTree` (`noiseFloorDbm`/`displayFloorDbm`/`panIndex`).

### `whoami`
Identify **this** bridge instance among concurrent bridges (each app process gets
its own per-pid socket + discovery entry).

```json
→ {"cmd":"whoami"}
← {"ok":true,"pid":34758,"name":"aethersdr-automation-34758",
   "socket":"/var/folders/…/aethersdr-automation-34758",
   "label":"","station":"Claude","txAllowed":false,"version":"26.6.5"}
```

`txAllowed` reports whether `AETHER_AUTOMATION_ALLOW_TX` is set for this process —
check it before assuming a keying verb will work. `label` is
`AETHER_AUTOMATION_LABEL` (a human tag for the instance).

### `mark`
Drop a **sequenced timeline marker** into the log ring, then bracket a sequence
with `log tail since=<seq>` to capture exactly the events between two marks.

```json
→ {"cmd":"mark","value":"pre-tune"}
← {"ok":true,"seq":4992,"mono_us":34216461,"text":"pre-tune"}
```

Use the returned `seq` as the `since=` anchor for a later `log tail`.

### `log`
Runtime control of the Qt logging categories plus a ring-buffer tail and a live
push subscription — the observability suite. All diagnostic; nothing keys.

```json
→ {"cmd":"log","action":"categories"}
← {"ok":true,"categories":[{"id":"aether.connection","label":"Connection / Commands","enabled":true}, …]}

→ {"cmd":"log","action":"set","value":"aether.dsp on"}
← {"ok":true,"id":"aether.dsp","enabled":true}

→ {"cmd":"log","action":"tail","value":"50 since=4992"}
← {"ok":true,"events":[{"seq":5013,"t":"15:14:16.630","mono_us":34276567,"lvl":"D","cat":"aether.automation","msg":"…"}],
   "seq":5015,"oldest":12}
```

| `action` | `value` | effect |
|---|---|---|
| `categories` | — | list every category (`id`, `label`, `enabled`) |
| `get` | `<id>` | one category's enabled state |
| `set` | `<id> on\|off` (id `all` = every category) | toggle a category at runtime |
| `reset` | — | restore the operator's persisted category prefs |
| `tail` | `[n] [since=<seq>]` | newest `n` ring events, optionally only `seq > since` |
| `subscribe` / `unsubscribe` | — | start / stop a live push of new events on this connection |

`tail` also returns `oldest` (the oldest `seq` still resident): if your `since <
oldest`, earlier matching events were evicted and the window is a truncated
suffix, not a complete bracket.

### `record`
Drive the client-side **QSO WAV recorder** (the same one behind the manual record
button), so a live test can capture audio and verify SSB + CW/CWX is recorded.
Not a transmit action — no gate.

```json
→ {"cmd":"record","action":"start"}
← {"ok":true,"record":"start","recording":true,"path":"/…/QSO_2026….wav"}

→ {"cmd":"record","action":"stop"}
← {"ok":true,"record":"stop","recording":false,"durationSecs":7,"path":"/…/QSO_2026….wav"}
```

`start` / `stop` / `status` (default) / `path` / `dir <path>` (set the output
directory).

### `station`
Set this GUI client's **MultiFlex station name** (FlexLib `SetClientStationName`)
so other clients on the radio see the agent driving. This is per-client and
session-scoped — it is **never** the radio-wide callsign (`radio callsign`), which
is persisted on the front panel.

```json
→ {"cmd":"station","value":"Claude"}
← {"ok":true,"station":"Claude"}
```

Must be a single token (no spaces) and requires a connected radio. The agent name
is applied automatically on connect and the user's real name is restored when the
bridge stops.

### `qrz`
QRZ.com callsign-lookup subsystem (CW decoder contact card + View → Callsign
Lookup). Four actions; none touch the radio and none key TX.

```json
→ {"cmd":"qrz","action":"status"}
← {"ok":true,"enabled":true,"hasCredentials":true,"cacheEntries":42,
   "hasOwnLocation":true}

→ {"cmd":"qrz","action":"cached","value":"KI6BCJ"}
← {"ok":true,"found":true,"entry":{"call":"KI6BCJ","nameFmt":"…","grid":"CM97",
   "stale":false,"photoPath":"/…/qrz-photos/KI6BCJ.jpg", …}}

→ {"cmd":"qrz","action":"lookup","value":"W1AW"}
← {"ok":true,"queued":true,"call":"W1AW","note":"async — poll `qrz cached W1AW`…"}

→ {"cmd":"qrz","action":"spottext","value":"CQ CQ DE KI6BCJ KI6BCJ K"}
← {"ok":true,"fed":"CQ CQ DE KI6BCJ KI6BCJ K"}
```

- `status` — enable flag, credential presence, lookup-cache entry count, and
  whether an own position (radio GPS/grid, or the operator's own QRZ record)
  is available for card distance/bearing.
- `cached <call>` — cache probe; returns the entry (plus `stale`, 7-day TTL,
  and `photoPath` when a photo is cached) or `found:false`. Never hits the
  network — safe to poll after `lookup`.
- `lookup <call>` — queue a real lookup through the service (cache-first;
  network only on miss/stale). Async: poll `qrz cached <call>` for arrival.
- `spottext <text>` — feed text into the **CW callsign spotter** as if the CW
  decoder produced it. Drives the real detection path ("DE <call> <call>" →
  service → contact card on the CW decode panel), so an agent can prove the
  end-to-end screen-pop with no radio, no live CW, and — with a seeded cache —
  no QRZ account. Verify with `grab callsignCard` / `dumpTree`.

Bare-line forms: `qrz status`, `qrz cached KI6BCJ`, `qrz lookup W1AW`,
`qrz spottext CQ CQ DE KI6BCJ KI6BCJ K`.

---

## Transmit verbs ⚠️ (gated)

These verbs **key the live transmitter** and are refused unless the app was
launched with `AETHER_AUTOMATION_ALLOW_TX=1` (the same rail as a keying `invoke`).
A force-unkey watchdog (`AETHER_AUTOMATION_TX_MAX_MS`, default 20 s) drops any
continuous key that runs too long, and the bridge force-unkeys on stop. **Verify
the TX antenna is your dummy load before keying** (`get slice tx txAntenna`, or
set it with `slice txant ANT2`). Unkey/stop sub-actions are always allowed.

### `key`
PTT / MOX keying via `RadioModel::setTransmit` — the exact path the space-bar PTT
filter and the `mox_toggle` shortcut take, which `invoke` can't reach.

```json
→ {"cmd":"key","action":"ptt","value":"on"}    # gated
← {"ok":true,"key":"ptt","state":"on"}

→ {"cmd":"key","action":"ptt","value":"off"}   # always allowed
← {"ok":true,"key":"ptt","state":"off"}
```

`key ptt on|off` keys/unkeys (also `press`/`release`, `1`/`0`). `key mox` is a
**toggle**: keyed → unkeys (allowed), idle → keys (gated). Keying arms the
force-unkey watchdog.

### `cwx`
Drive the CWX CW keyer — the easy repro for post-TX FFT-floor recovery (#3804).

```json
→ {"cmd":"cwx","action":"send","value":"CQ TEST DE W1AW"}   # gated
← {"ok":true,"cwx":"send","chars":15}
```

| `action` | `value` | gated? | effect |
|---|---|---|---|
| `send` | `<text>` | **yes** | key CW for the string (arms the watchdog) |
| `speed` (alias `wpm`) | `<5–100>` | no | set keyer speed |
| `stop` (alias `abort`/`clear`) | — | no | abort the keying buffer |

Stage the slice into a CW mode first (`invoke sliceModeCombo setCurrentText CW`)
for the radio to actually emit.

### `txtest`
Two-tone TX test signal (for IMD / PA / meter measurements).

```json
→ {"cmd":"txtest","action":"twotone"}   # gated
← {"ok":true,"txtest":"twotone"}

→ {"cmd":"txtest","action":"off"}        # always allowed (alias stop)
← {"ok":true,"txtest":"off"}
```

### `atu`
Antenna-tuner control. `bypass` is relay-only (takes the tuner out of circuit so
meters see the raw load) and does **not** transmit; `start` runs a tune cycle that
**keys TX**.

```json
→ {"cmd":"atu","action":"bypass"}   # no TX, always allowed
← {"ok":true,"atu":"bypass"}

→ {"cmd":"atu","action":"start"}     # gated (alias tune)
← {"ok":true,"atu":"start"}
```

### `testtone`
Inject a **client-side test tone** into the TX mic/audio path (frequency Hz +
level dB). It does not key by itself — it only reaches the air if you also key
(which is gated) — so the verb itself is ungated, but it stages a TX signal and
belongs with the transmit group.

```json
→ {"cmd":"testtone","action":"on","value":"1000 -10"}
← {"ok":true,"testtone":"on","freqHz":1000,"levelDb":-10}

→ {"cmd":"testtone","action":"off"}
← {"ok":true,"testtone":"off"}
```

---

### Errors
Every failure is a one-line object: `{"ok":false,"error":"<message>"}` — e.g.
`widget not found: Foo`, `blocked: '…' looks transmit-related …`,
`no slice for selector 'tx'`, `no local radios have been discovered`,
`timed out waiting for radio connection`, `unknown action: x`,
`unknown command: x`.

---

## Targeting a widget

`grab` and `invoke` resolve a `target` string in this order — first match wins:

0. **VFO shortcuts** — `"vfo slice 1"` or `"vfo:slice:1"` targets the VFO
   flag for slice 1. `"vfo 1"` or `"vfo:1"` targets the first VFO flag inside
   the `SpectrumWidget` whose `panIndex` is 1, mirroring `grab pan 1`.
   Prefer the slice form when a pan contains multiple VFOs.
1. **Scoped `"<scope>/<name>"`** — disambiguates a control whose
   `accessibleName` appears in more than one applet (e.g. `"AF gain"` and
   `"Squelch threshold"` exist in **both** `RxApplet` and `PanadapterApplet`).
   `<scope>` matches an ancestor by objectName, class, or accessibleName;
   `<name>` is resolved within that subtree. Use `"RxApplet/AF gain"` vs
   `"PanadapterApplet/AF gain"`. Falls through to flat matching if it doesn't
   resolve, so a literal `/` in a name still works.
2. **Exact `objectName`** — the most stable handle. Prefer this.
3. **Class name** — full (`AetherSDR::SpectrumWidget`) or short
   (`SpectrumWidget`). Handy when a widget has no objectName (the panadapter is
   targeted as `SpectrumWidget`).
4. **`accessibleName`** — e.g. `"Panadapter spectrum display"`,
   `"Master volume"`.
5. **Button text** — last resort, e.g. `"Send"`, `"Transmit"`. Lowest priority,
   so a real objectName/accessibleName always wins; first match in tree order.
6. **Visible popup-menu action** — exact `QAction` objectName, visible text,
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
  `QRhiWidget::grabFramebuffer()` for raw `SpectrumWidget`/`grab pan` captures
  because plain `QWidget::grab()` returns an empty surface for a GPU widget.
  For screenshots that must include child overlays such as VFO flags, use
  `grab pan-visible <index>`.
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
