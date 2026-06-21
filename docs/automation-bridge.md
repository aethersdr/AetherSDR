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
| Click a button or move a slider programmatically | **Not yet** — `invoke()`/`get()` land in Phase 1. Today the bridge is read-only (snapshot + capture). |

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
  "enabled": true,
  "visible": true,
  "geometry": { "x": 1, "y": 104, "w": 1448, "h": 751 },  // GLOBAL screen coords
  "value": "42",                           // best-effort; see below
  "children": [ <node>, … ]                // present only if non-empty
}
```

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
| containers / custom-painted surfaces | omitted |

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

### Errors
Every failure is a one-line object: `{"ok":false,"error":"<message>"}` — e.g.
`widget not found: Foo`, `grab requires a target widget`, `unknown command: x`.

---

## Targeting a widget

`grab` (and, in Phase 1, `invoke`/`get`) resolve a `target` string in this
order — first match wins:

1. **Exact `objectName`** — the most stable handle. Prefer this.
2. **Class name** — full (`AetherSDR::SpectrumWidget`) or short
   (`SpectrumWidget`). Handy when a widget has no objectName (the panadapter is
   targeted as `SpectrumWidget`).
3. **`accessibleName`** — e.g. `"Panadapter spectrum display"`,
   `"Master volume"`.

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

**Snapshot → act → assert** (the loop you already use for web work): snapshot
with `dumpTree`, perform your action (today: via the real radio / UI; Phase 1
adds `invoke`), then `dumpTree` again and diff the `value`/`enabled` fields.

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
- **Read-only today.** Phase 0 is `ping` / `dumpTree` / `grab`. No clicking or
  value-setting yet — that's `invoke()`/`get()` in Phase 1.
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
| 1 | `invoke(name, action, value)` + `get(model, property)`; clear the a11y backlog | planned |
| 2 | Replay/fixture mode (recorded VITA-49 FFT + meters) → deterministic panadapter without hardware | planned |
| 3 | CI E2E matrix: `QT_QPA_PLATFORM=offscreen` + agent scenarios + per-OS perceptual golden diffs | planned |
| 4 | Computer-use / VNC kept as the *exploratory* tier (real GPU/WM smoke), not the regression backbone | planned |

## Source

- Server: [`src/core/AutomationServer.h`](../src/core/AutomationServer.h) /
  [`.cpp`](../src/core/AutomationServer.cpp)
- Startup wiring: [`src/main.cpp`](../src/main.cpp) (after `window.show()`)
- Driver: [`tools/automation_probe.py`](../tools/automation_probe.py)
- Log category: `lcAutomation` (`aether.automation`) — toggle in Help → Support.
