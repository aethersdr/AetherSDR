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
AETHER_AUTOMATION=1 AETHER_AUTOMATION_NO_AUTOCONNECT=1 ./build/AetherSDR.app/Contents/MacOS/AetherSDR &   # macOS
#   AETHER_AUTOMATION=1 AETHER_AUTOMATION_NO_AUTOCONNECT=1 ./build/AetherSDR &                            # Linux/Windows

# 3. Drive it. The dependency-free probe needs no Qt:
python3 tools/automation_probe.py ping
python3 tools/automation_probe.py demo --out /tmp/phase0   # → tree.json + panadapter.png
```

`demo` produces the two canonical artifacts: a semantic snapshot of the UI
(`tree.json`) and a PNG of the live panadapter (`panadapter.png`). View the PNG
to confirm a visual change; parse the JSON to assert on control state.

For headless / CI runs, add `QT_QPA_PLATFORM=offscreen` — no display required.
`AETHER_AUTOMATION_NO_AUTOCONNECT=1` suppresses saved-radio autoconnect during
bridge runs; use the `connect` verb when a test intentionally needs a radio.

Parallel worktrees should give each bridge a stable automation identity and a
human-readable agent name:

```bash
AETHER_AUTOMATION=1 \
AETHER_AUTOMATION_IDENTITY=issue-4166-worktree \
AETHER_AUTOMATION_AGENT_NAME=Codex-GPT-5.6 \
AETHER_AUTOMATION_SOCKET=aethersdr-4166 \
./build/AetherSDR.app/Contents/MacOS/AetherSDR
```

`AETHER_AUTOMATION_IDENTITY` deterministically selects a process-scoped Flex
GUI client UUID, so concurrent worktrees do not displace one another through
the radio's duplicate-client takeover behavior. If it is omitted, the socket,
automation label, or PID is used in that order. `AETHER_AUTOMATION_AGENT_NAME`
sets the station label shown to other Multi-Flex clients; the legacy
`AETHER_AUTOMATION_STATION` and then `AETHER_AUTOMATION_LABEL` are fallbacks,
followed by the neutral default `Automation`. The agent name is display-only
and is never used as the UUID because several worktrees may use the same LLM.
Automation identities never overwrite the user's persistent `GUIClientID`.

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
socket the bridge needs. When the MCP wrapper is configured, prefer its secure
[`app_instance`](#secure-fresh-build-handoff) launch. For a manual launch, run
outside the command sandbox instead:

```bash
QT_QPA_PLATFORM=offscreen AETHER_AUTOMATION=1 AETHER_AUTOMATION_NO_AUTOCONNECT=1 ./build/AetherSDR.app/Contents/MacOS/AetherSDR &
```

---

## MCP server — drive the bridge from any AI assistant

`tools/aether_mcp.py` wraps this bridge in the Model Context Protocol, so
an MCP-capable coding assistant (Claude Code, Cursor, Copilot, Codex CLI,
Gemini CLI, …) can validate your PR against the running app natively —
no socket scripting required. This is the recommended way for
contributors to self-verify UI changes before requesting review.

**Setup** (zero dependencies — plain Python 3):

1. Register the server with your assistant:
   - **Claude Code**: nothing to do — the repo's `.mcp.json` registers it;
     approve the prompt on first use. (Manual: `claude mcp add aethersdr
     -- python3 tools/aether_mcp.py`)
   - **Cursor / Windsurf / others**: add to your MCP config:
     ```json
     {"mcpServers": {"aethersdr-automation": {
        "command": "python3", "args": ["tools/aether_mcp.py"]}}}
     ```
   - **Windows**: use `python` (or `py -3`) instead of `python3`.
2. Launch through `app_instance` (recommended for a fresh proof build), or
   manually with `AETHER_AUTOMATION=1 ./build/AetherSDR`.

**Tools exposed** (25 typed tools): process handoff — `app_instance`;
introspection — `bridge_status`,
`dump_tree` (with a `filter` arg), `grab_widget` (PNG inline; optional
`path` for where the PNG is written, else a temp file),
`get_state`, `get_log`, `floors`, `streams`; driving — `invoke`, `gesture`
(on a target-not-found failure it appends `did_you_mean` candidates),
`shortcut`, `tune`, `slice`, `pan`, `record`, `mark`, `window`, `menu`;
assert/await — `assert_state` / `wait_for` (read a model property and
check/await a value — validation reads as pass/fail, not a manual diff);
connection — `connect` / `disconnect`; audio — `capture_audio`; and
`bridge_command` (raw escape hatch for everything else).

The verbs kept behind `bridge_command` on purpose: the low-level widget
primitives (`close`, `hover`, `tooltip`, `scrollTo`, `drag`, `showMenu`,
`contextMenu`, `rightClick`, `hitTest`, `clickAt` — `invoke`/`grab`
cover the common cases), the transmit-keying verbs (`key`, `txtest`,
`atu`, `cwx`, `testtone`, `txwaterfall` — gated by
`AETHER_AUTOMATION_ALLOW_TX`, deliberately less convenient), and the
niche/complex ones (`dss`, `layout`, `scale`, `panmessage`, `tci`,
`station`, `resize`, `qrz`).

**Prompts and resources.** The server also exposes an MCP **prompt**,
`validate_ui_change` (the loop below as a guided workflow — pass your
`widget` and `change`), and read-only **resources** that pull live state
as context without a tool round-trip: `aethersdr://widget-tree`,
`aethersdr://state/radio`, `.../slices`, `.../pans`, `aethersdr://verbs`.

A typical assistant validation loop for a PR:
`bridge_status` → `dump_tree filter=<your widget>` → `invoke` the
control you changed → `assert_state` / `wait_for` on the model property
that should have changed → `grab_widget` for a visual check.

**Access token.** Enabling the bridge in Radio Setup → Network mints a
random token (stored in your OS secret store via QtKeychain — macOS
Keychain / Windows Credential Manager / libsecret-KWallet, never in the
plaintext settings file). Copy it into your assistant's MCP config as the
`AETHER_MCP_TOKEN` environment variable; the bridge then rejects every
verb except `ping` without a matching token. Headless/CI can supply the
token via `AETHER_MCP_TOKEN` directly, which overrides the keychain.

### Secure fresh-build handoff

Use the typed MCP `app_instance` tool when the MCP wrapper already has
`AETHER_MCP_TOKEN` from a secure runtime source (for example macOS Keychain,
Credential Manager, or libsecret) but a newly built app has a different secret-
store requester identity:

```json
{"action":"launch","worktree":"/absolute/path/to/AetherSDR","label":"issue-4354-proof"}
```

`worktree` must be an absolute AetherSDR Git worktree. The wrapper runs only its
canonical `build/AetherSDR` artifact (`build/AetherSDR.app/Contents/MacOS/AetherSDR`
on macOS), directly without a shell. It passes the wrapper token only in the
child process environment using the app's existing `AETHER_MCP_TOKEN` runtime
override; the token is never a command-line argument, MCP result, discovery
field, log message, or settings value.

Each launch gets a unique explicit local socket and safe label. The wrapper
sets `AETHER_AUTOMATION_NO_AUTOCONNECT=1`, pins TX automation off with
`AETHER_AUTOMATION_NO_TX=1`, removes any inherited TX-enable flag, and refuses
success unless token-free `ping` reports that auth is required and authenticated
`whoami` matches the exact child PID, socket, and label with `txAllowed:false`.
All other MCP tools then target that owned socket. `status` inspects only the
owned process; `stop`, wrapper exit, or a failed launch terminates only that
process and releases its socket.

The tool fails closed if the wrapper has no token. It does not read, print, or
copy a secret from the app settings UI, so an assistant never needs to put the
credential in chat or a plaintext MCP configuration. Observe-only remains
operator-authoritative: if the saved setting is enabled, the launched instance
reports `readOnly:true` and mutating tools remain blocked.

What the token *does* and *doesn't* do: it opts a **specific** client in
and protects the secret at rest (nothing in a backed-up / synced /
screen-shared dotfile), across other user accounts, and over any network
reach. It is **not** a hard wall against a determined *same-user* process
on Linux/Windows — once your login keychain is unlocked, libsecret and
DPAPI hand the secret to any same-user caller (macOS, with its per-item
ACL prompt, is the exception). Treat it as "this app deliberately grants
this client access," not "same-user isolation."

The TX-safety gate is unchanged in spirit: the bridge refuses transmit-
keying controls regardless of who's calling (see [TX safety](#tx-safety)).
An assistant can only key your radio if **you** opt in — either by
launching with `AETHER_AUTOMATION_ALLOW_TX=1`, or by checking **"Allow
TX via MCP"** in Radio Setup → Network. That checkbox raises a one-time
confirmation spelling out that automated software will be able to
transmit and that you, the operator, remain responsible for all
emissions; once confirmed the choice persists. Toggling it drives the
same `m_txAllowed` gate live (enabling arms the force-unkey watchdog;
disabling force-unkeys immediately).

For proof-build process owners, `AETHER_AUTOMATION_NO_TX=1` pins this gate off
even when the operator preference was previously enabled. The Radio Setup
checkbox is disabled for that process and live attempts to enable it are
ignored. `app_instance launch` always sets this pin and verifies it through
authenticated `whoami` before returning success.

### Observe-only (read-only) mode

For a look-but-don't-touch session — handing an assistant visibility
without letting it change anything — check **"Observe only"** in Radio
Setup → Network. The bridge then refuses **every** mutating verb and
answers only pure-introspection reads (`ping`, `verbs`, `whoami`, `get`,
`dumpTree`, `grab`, the read-only `log` actions, `floors`, the inventory-only
`streams` actions, and `hitTest`). In particular, it blocks `log set/reset`
and `streams reset/resync/refresh`; the latter two stream actions clear local
diagnostics or request a fresh radio inventory. It is
enforced in the app, not in the MCP server, so no client can flip it
off; the refusal message points the operator back to the checkbox. The
toggle takes effect immediately on a running bridge — no restart — so
the intended flow works: start the app with the bridge off, check
"Observe only", then start the bridge. `ping` and `whoami` report the
current state as `readOnly`, and the MCP server surfaces it in
`bridge_status` as `bridge_read_only`. Headless/CI runs can pin it with
`AETHER_AUTOMATION_READONLY=1`.

![Observe only setting in Radio Setup → Network](assets/automation-observe-only.png)

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
  - `{"cmd":"grab","args":"SpectrumWidget /tmp/pan.png","token":"..."}`
    (the optional `args` string uses the same positional parser as a bare
    command, which is useful when authenticated requests also need a token)
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
| | [`verbs`](#verbs) | Machine-readable catalog of every verb + aliases + help. |
| | [`dumpTree`](#dumptree) | ARIA-style snapshot of the whole widget tree. |
| | [`grab <target> [path]`](#grab) | PNG of one widget (GPU-correct for the panadapter). |
| | [`grab pan <index> [path]`](#grab) | Raw spectrum surface of a specific pan. |
| | [`grab pan-visible <index> [path]`](#grab) | Pan applet incl. VFO/flag overlays (alias `pan-composite`). |
| | [`floors`](#floors) | Per-pan measured noise + display floor (dBm). |
| | [`whoami`](#whoami) | This bridge instance: pid, socket, label, station, `txAllowed`, `readOnly`. |
| **Drive** | [`invoke <target> <action> [v]`](#invoke) | Click/toggle/set/selectRow/submit/trigger a control (TX-guarded). |
| | [`close <target>`](#close) | Close the target's top-level window. |
| | [`drag <target> "<dx> <dy>"`](#drag-alias-mouse) | Synthesize press→move→release (alias `mouse`). |
| | [`dragAt <target> "<x> <y> <dx> <dy> [modifiers]"`](#dragat) | Drag from a target-local point with optional keyboard modifiers. |
| | [`gesture <phase>`](#gesture) | Hold press/move/release across requests for delayed-event tests. |
| | [`showMenu <target>`](#showmenu-alias-openmenu) | Pop a button's drop-down menu (alias `openMenu`). |
| | [`contextMenu <target> [x y]`](#contextmenu) | Trigger a custom right-click menu. |
| | [`rightClick <target> [x y]`](#rightclick) | Trigger a mousePressEvent-based right-click menu. |
| | [`hitTest <target> [x y]`](#hittest) | Read Qt's widget owner for a target-local point. |
| | [`clickAt [<target>] <x> <y>`](#clickat) | Click at a global (or target-local) point — fallback when name matching is ambiguous (TX-guarded). |
| | [`menu list \| open <name>`](#menu) | Enumerate / pop a menu-bar menu. |
| | [`resize <w> <h> [target]`](#resize) | Resize a window (drives panadapter `x_pixels`). |
| | [`window <state> [target]`](#window) | maximize / restore / minimize / fullscreen. |
| | [`shortcut <id>`](#shortcut) | Fire a ShortcutManager/MIDI action by id (TX-guarded). |
| | [`midi cc <0-127>`](#midi) | Inject a learned VFO Tune Knob CC event (RX-only). |
| | [`scrollTo <target>`](#scrollto-alias-ensurevisible) | Scroll a widget into its scroll-area viewport. |
| **State (`get`)** | [`get audio`](#get) | Audio-engine stream/buffer snapshot. |
| | [`get dsp`](#get-dsp) | Client-side AetherDSP NR state (NR2…BNR). |
| | [`get radio \| transmit \| eq \| meters`](#get) | Radio / TX-chain / EQ / meters snapshots. |
| | [`get gps`](#get) | GPS fix, location, satellite-count, time, course, and reference snapshot. |
| | [`get slice[s] \| pan[s]`](#get) | Slice & panadapter model snapshots. |
| | [`get flags`](#get) | VFO flag attachment state for slice-to-pan assertions. |
| | [`get cwx`](#get-cwx) | CWX keyer state + queue-drain watch (#3949). |
| | [`get panstats`](#get-panstats) | Per-panadapter render-cost counters (profiling). |
| | [`get renderstats`](#get-renderstats) | Combined 2D/3D pan, waterfall, DSS, scheduler, and WAVE profiling snapshot. |
| | [`get tracedebug`](#get-tracedebug) | Per-panadapter Flex/Kiwi FFT and 3D trace diagnostics. |
| | [`get clients`](#get-clients) | Radio client roster, GUI IDs + foreign-pan-write forensics (#3977/#4166). |
| | [`get sync`](#get-sync) | Receive-Sync (Auto Assist) state. |
| | [`get clock`](#get-clock) | AetherClock time-signal decode state (lock, station, decoded UTC, offset, quality). |
| | [`get wavestats`](#get-wavestats) | WAVE/strip scope paint-cost counters. |
| | `get waveforms` | Installed waveform list, WFP state, local D-STAR service/configuration, delivery health/metrics, and recent waveform status reports. |
| | [`get dax`](#get-dax) | DAX RX channel-ownership table (holders/streams, #3305). |
| | [`get txtimer`](#get-txtimer) | Status-bar transmit-timer state (visible/running/holding/fading/elapsed). |
| **Connection** | [`connect …`](#connect--disconnect) | list / show / hide / local / ip / wait. |
| | [`disconnect`](#connect--disconnect) | Normal user disconnect. |
| **Tuning & slices** | [`tune <mhz>`](#tune) | Set the active slice frequency (VFO; not keying). |
| | [`targettune <mhz>`](#targettune) | Absolute tune through the commanded-target and band-stack path. |
| | [`memory activate <index> [panId]`](#memory) | Recall a radio memory through the normal UI policy. |
| | [`slice <action>`](#slice) | add/remove/select/tx/mode/diversity/centerlock/txant/rxant/rxsource. |
| **GPS fixtures** | [`gps fixture <6000\|8000>`](#gps) | Disconnected-only GPS status fixture using each production wire format. |
| **Display / pans** | [`pan <action>`](#pan) | create / center / close a panadapter. |
| | [`panmessage <action>`](#panmessage) | Add, remove, clear, or list panadapter overlay messages for UI testing. |
| | [`dss <action>`](#dss) | Inject/read 3D stacked-trace + waterfall scrollback state. |
| | [`streams [radio\|resync\|reset]`](#streams) | Radio-side display-stream leak detector. |
| | [`txwaterfall on\|off`](#txwaterfall) | Toggle "show TX in waterfall". |
| **DAX / TCI** | [`tci start\|status\|stop`](#tci) | In-process TCI client simulator (WSJT-X audio or SDC IQ-skimmer profile). |
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
>
> Server-side, every verb is one entry in a single registry table
> (`AutomationServer::verbRegistry()`, #4174) that owns its name, aliases,
> bare-line parsing, and dispatch; the startup banner, the unknown-command
> error, and [`verbs`](#verbs) are all derived from it. When this table and
> the running app disagree, trust `verbs` — it cannot go stale.

### `ping`
Connectivity / handshake.

```json
→ {"cmd":"ping"}
← {"ok":true,"app":"AetherSDR","version":"26.6.3"}
```

### `verbs`
Machine-readable catalog of every verb the running build understands —
canonical name, aliases, and a one-line help string, straight from the server's
verb registry (#4174). Use it instead of hand-maintained verb lists in drivers;
`tools/automation_probe.py` passes any verb not in its own mapping table
through as a bare line, so new simple verbs work in the probe with no probe
changes.

```json
→ {"cmd":"verbs"}
← {"ok":true,"count":45,"verbs":[
    {"name":"ping","help":"liveness check → app + version"},
    {"name":"drag","aliases":["mouse"],"help":"drag <target> <dx> <dy> — synthesize press→move→release"},
    …]}
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
  "cursor": "pointinghand",                // present only if the widget owns a cursor (WA_SetCursor); shape name — see below
  "geometry": { "x": 1, "y": 104, "w": 1448, "h": 751 },  // GLOBAL screen coords
  "windowState": "maximized",              // top-level windows only: normal|maximized|minimized|fullscreen
  "value": "42",                           // best-effort; see below
  "text": "NR2",                           // checkable buttons: the label (value would just be "checked")
  "checked": false,                        // checkable buttons: explicit boolean check-state
  "range": { "min": 0, "max": 100 },       // numeric controls only (slider/spinbox)
  "sliderDown": false,                     // sliders only: real QAbstractSlider press ownership
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
- `cursor` — the widget's mouse-cursor **shape name**, reported only when the
  widget explicitly owns a cursor (`WA_SetCursor`); inheriting widgets omit it.
  Lets a driver assert **hover affordance** — a clickable control carries
  `pointinghand`, a text field `ibeam` — without observing the live OS cursor,
  which no `grab`/screenshot captures. Shape names: `arrow`, `pointinghand`,
  `ibeam`, `splith`, `splitv`, `sizehor`, `sizever`, `openhand`, `forbidden`,
  `wait`, `busy`, `cross`, `blank`, … (`other` for anything unmapped). Used to
  prove every interactive slice-flag field now signals clickability (#4036).

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
- **The panadapter message overlay is *not* in this framebuffer.** Connection
  status cards (e.g. the KiwiSDR "Not connected" card), interlock
  "Transmit disabled" warnings, and anything posted via [`panmessage`](#panmessage)
  are a sibling widget stacked over the surface — they are captured only by
  `grab pan-visible <index>`, never by `grab SpectrumWidget` / `grab pan <index>`.
  A flow that verifies disconnect/interlock state from a framebuffer grab will
  silently pass on a broken connection; use `grab pan-visible` (or read
  `panmessage list`) for that state.

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
| `audio` | — | audio-engine snapshot (RX/TX stream state, mute, buffer counters, KiwiSDR TX mute gate, Receive Presentation output-signal counters) |
| `dsp` | — | client-side AetherDSP noise-reduction state — see [`get dsp`](#get-dsp) |
| `radio` | — | radio snapshot (name, model, version, connected, fullDuplex, transmitting, txPower, paTemp, slice/pan counts) |
| `gps` | — | GPS status, tracked/visible counts, grid, radio-format coordinates, altitude, speed, course, UTC time, frequency error, and oscillator-reference state |
| `transmit` | — | TX-chain snapshot: RF/tune power, mic/processor/monitor, VOX/AM/DEXP, TX filter, CW (speed/pitch/breakin/delay/sidetone/iambic/monitor), ATU, APD. Validate that a TX/Phone/CW applet control reached the radio model. |
| `cwx` | — | CWX keyer + queue-drain watch — see [`get cwx`](#get-cwx) |
| `equalizer` (or `eq`) | — | 8-band RX+TX graphic EQ: `rxEnabled`/`txEnabled` and `rx`/`tx` band maps keyed by label (`63`…`8k`). Validate EQ-applet slider changes. |
| `meters` | — | `{all:[…]}` — every radio meter with `name`, `value`, `unit`, `low`/`high`, `description`, and **`age_ms`** (staleness): a meter that updates has small `age_ms` and a tracking `value`. |
| `slices` | — | array of all slice snapshots |
| `slice` | `active` (default) / `tx` / `<sliceId>` | one slice (sliceId, letter, frequency, mode, filterLow/High, rxAntenna, nb/nr/anf + levels, **squelch/squelchLevel, agcMode/agcThreshold, apf/apfLevel**, **adaptiveFilterEnabled/adaptiveMinLowCut/adaptiveMaxHighCut/adaptiveMinSnr/adaptiveResponse/adaptiveSplatter/adaptiveActive** (SSB adaptive RX filter — `adaptiveActive` is the live AUTO-fit state), txSlice, …) |
| `clock` | — | AetherClock snapshot: `state`/`stateName` (NoSignal/Acquiring/Locked), `station`/`stationName` (WWV/WWVH/WWVB), `decodedUtc` (ISO-8601, empty until a decode), `offsetMs` (decoded − host at the second edge; positive = host behind broadcast), `lockQuality` (0–100), `sliceId` (bound slice, −1 when stopped), `gpsTimeAvailable`. Validate applet Start/Tune/station-switch actions and lock progress without pixels. |
| `pans` | — | array of all panadapter snapshots |
| `pan` | `active` (default) / `<panId>` e.g. `0x40000000` | one pan (centerMhz, bandwidthMhz, min/maxDbm, rxAntenna, rfGain, fps, `transmitInhibited`, `transmitInhibitReason`) |
| `flags` (or `vfoFlags`) | `all` (default) / `<sliceId>` | VFO flag attachment snapshot: each flag’s slice id, expected radio pan id, attached UI pan id/index, geometry, visibility, and `attachedToExpectedPan`; also reports `missingSlices`. |
| `panstats` | `<panIndex>` / `<objectName>` (default: all) | per-panadapter render-cost counters — see [`get panstats`](#get-panstats) |
| `tracedebug` | `<panIndex>` / `<objectName>` (default: all) | per-panadapter Flex/Kiwi FFT and 3D trace diagnostics — see [`get tracedebug`](#get-tracedebug) |
| `wavestats` | `—` / scope objectName | waveform-scope paint/append counters — see [`get wavestats`](#get-wavestats) |
| `clients` | — | connected-client roster, per-pan ownership, foreign dBm-write counters and evictions — see [`get clients`](#get-clients) |
| `dax` | — | DAX RX channel-ownership table — see [`get dax`](#get-dax) |
| `waveforms` | — | installed legacy/Docker waveforms, WFP state, local Digital Voice service/mode configuration and delivery metrics, raw radio `mode_list` values, duplicate counts, last maintenance response, and recent `waveform status` reports |

Add a trailing **property** name to any single-object form to get just that
field: `get slice active mode` → `{"value":"LSB"}`.

For `get audio`, `receivePresentationOutputSignalEmitCount` counts output
chunks dispatched to Receive Presentation Sync analysis, while
`receivePresentationOutputSignalSuppressedCount` counts non-empty output chunks
that were captured for automation but skipped because no KiwiSDR audio source
was active.

The TX input endpoint also exposes in-memory capture-health evidence for TCI
handoffs: `buffer_bytes_available`, `buffer_capacity_bytes`,
`source_was_active`, `saturation_observed`, `tci_suppressed_callbacks`,
`full_buffer_during_tci_observations`, `idle_during_tci_transitions`,
`post_tci_local_tx_while_saturated`, and `last_mic_read_age_ms`.
`saturation_observed` is set when the capture buffer reaches its reported
capacity during TCI suppression. An Active-to-Idle transition with suppressed
callbacks and unread bytes remains a fallback for backends that do not expose a
useful capacity. The same evidence is written to the Audio Summary support log
only when Help → Support's **CAT/rigctld** logging toggle (the category used by
TCI debug logging) is enabled; TX capture-health summaries are off by default.

### `get cwx`
CWX keyer state, including the **queue-drain watch** that the #3949 fix relies
on. Firmware never emits `cwx queue=`, so the client detects a drained CWX buffer
by capturing the `radio_index` from the final `cwx send` reply and firing
`queueEmpty()` — which releases TX — once the live `cwx sent=` counter reaches the
batch end. `radio_index` is the batch's **first-char** queue position (verified on
FLEX-6500 fw 4.2.20.41343 — a 23-char send at `sent=48` replied `radio_index=49`
and `sent=` then climbed to 71), so `cwxEndIndex` is stored as
`radio_index + nChars - 1`. None of that state has a widget, so this is the only
non-hardware-poll way to assert the mechanism (cf. [`get dsp`](#get-dsp)).

```json
→ {"cmd":"get","model":"cwx"}
← {"ok":true,"model":"cwx","cwx":{
   "active":true,"tracking":true,"cwxEndIndex":14,"sentIndex":6,
   "speed":25,"speedStep":5,"delay":5,"qsk":false,"live":false}}
```

| field | meaning |
|---|---|
| `active` | `RadioModel::cwxActive` — a `cwx send` batch is in flight (TX keyed for it) |
| `tracking` | `true` while a queue-drain watch is armed (`cwxEndIndex >= 0`) |
| `cwxEndIndex` | the batch-end index = `radio_index + nChars - 1`, the value `sentIndex` must reach to release TX (`-1` = idle) |
| `sentIndex` | the radio's live `cwx sent=` counter (last char keyed) |
| `speed` / `speedStep` / `delay` / `qsk` / `live` | keyer settings |

**The drain proof:** on a keyed macro, `cwxEndIndex` jumps to the batch-end N when
the send reply arrives, `sentIndex` climbs to N as the radio keys each char, and
the frame it reaches N `tracking` flips back to `false` and `active` clears
(queueEmpty → `xmit 0`). Watching `cwxEndIndex` hold while `sentIndex` climbs to
meet it — over the full keying duration, not after one char — is the direct
evidence the batch-end index is right (radio_index is the batch **start**, so the
end is `radio_index + nChars - 1`). ESC mid-macro ([`cwx stop`](#cwx) / `clearBuffer`) resets
`cwxEndIndex` to `-1` so an aborted macro never triggers a spurious release. A
trailing property narrows it: `get cwx cwxEndIndex` → `{"value":14}`. Fields are
zero/`-1`/idle until a radio connects.

`get waveforms localDigitalVoice` includes the local helper's lifecycle,
implemented mode descriptors, registration name/verification, exclusive active
slice, persisted service settings, nested D-STAR routing, and delivery telemetry:
`health`, `healthDetail`, `metricsMode`, `metricsValid`, `metricsAgeMs`,
`rxRateHz`, cumulative and latest-window `vitaGaps` / `inferredSourceBlocks`,
turnaround, queue depth, and metric generation/sequence. TX observations expose
`txMetricsValid`, `txMetricsAgeMs`, `txRateHz`, cumulative and latest-window
`txVitaGaps` / `txNullFrames`, PCM clip/invalid counts, send failures, queue
depth, and tail samples/time. Version 3 TX telemetry also exposes
`txPreRollFrames`, `txPreRollDelayMs`, `txAmbeQueueMax`, cumulative and latest
AMBE underflow/overflow/sequence errors, vocoder submit failures and pending
depth, plus drain frames/timeouts/discarded frames. These fields distinguish
true VITA transport loss from D-STAR source-block deficits and TX
encoder/pre-roll/drain faults.

The nested `dstar` object exposes the friendly `route` selection (direct or
repeater origin, local CQ/station/repeater-area/custom destination, callsigns,
and separate repeater module letters), detected `serialDevices`, the latest 100
timestamped RX/TX `traffic` entries, and the full persisted `trafficCount`.
These reads are passive. Use generic `invoke` against the named D-STAR tab
controls to test configuration; no D-STAR configuration control keys TX.

The complete snapshot also includes `rawModeLists` and
`maximumDstrOccurrencesPerSlice`. The latter must be `1` after a successful
registration migration; unlike the UI mode combo, these values are captured
before backend deduplication.

### `waveform` - local service and registration maintenance

These actions never key the transmitter and do not require
`AETHER_AUTOMATION_ALLOW_TX`:

```text
waveform start dstar
waveform stop
waveform resync
waveform unregister <safe-registration-name>
```

`unregister` returns as soon as the command is queued. Poll `get waveforms`
until `lastCommand.pending` is false, require `lastCommand.code == 0`, then
poll the raw mode lists after the automatic slice-status resync. The verb is
generic; the bridge does not embed or preserve an old registration name.

### `get renderstats`

Combined rendering-analysis snapshot for before/after automation. It returns
every `panstats` entry, every WAVE/strip `wavestats` entry, the shared pan
scheduler, and non-overlapping headline totals. The totals cover measured
GUI-thread FFT ingest, native/Kiwi waterfall ingest, GPU frame preparation,
software fallback painting, and WAVE painting. DSS timings are reported
separately because they are a subset of FFT/waterfall ingest.

```json
→ {"cmd":"get","model":"renderstats","selector":"reset"}
← {"ok":true,"model":"renderstats",
   "totals":{"panCount":1,"visiblePanCount":1,"waveScopeCount":1,
     "fftFramesPerSec":24.9,"gpuFramesPerSec":25.1,
     "fftIngestMsPerSec":4.2,"nativeWaterfallUpdateMsPerSec":3.8,
     "gpuFrameMsPerSec":2.7,"wavePaintMsPerSec":0.0,
     "measuredMainThreadMsPerSec":10.7,
     "hiddenWaterfallUpdatesPerSec":0.0,
     "hiddenDssHistoryRowsPerSec":0.0,
     "waterfallAllocatedBytes":583680,
     "dssAllocatedBytes":37847040},
   "pans":[...],"scopes":[...],"renderScheduler":{...}}
```

Use `get renderstats reset`, wait for a fixed observation interval, then read
`get renderstats reset` again. This gives disjoint samples across pan, waterfall,
3DSS, scheduler, and WAVE counters with one command. `measuredMainThreadMsPerSec`
is instrumented GUI-thread work, not whole-process CPU percentage; use it for
causal comparisons while keeping the radio/display configuration fixed.

### `get panstats`
Per-panadapter (SpectrumWidget) frame-cost counters — how much GUI-thread time
each pan spends preparing frames, split by pipeline section, for before/after
rendering-cost proofs without a profiler attach. Counters are always on and
cost a few integer adds per frame.

```json
→ {"cmd":"get","model":"panstats"}
← {"ok":true,"model":"panstats","pans":[{
   "panIndex":0,"renderMode":"2D","renderer":"GPU QRhi (Metal; Apple M1 Ultra)",
   "widthPx":2280,"heightPx":1302,"dpr":2.0,"sinceMs":60012,
   "fftFramesPerSec":29.6,"ingestMsPerSec":8.1,
   "gpuFramesPerSec":29.6,"gpuFrameMsPerSec":97.4,"avgGpuFrameUs":3290.0,
   "fftBuildMsPerSec":64.2,"fftVboBytesPerSec":42049536.0,
   "overlayRebuildsPerSec":0.1,"overlayRebuildMsPerSec":1.9,
   "overlayUploadBytesPerSec":3964928.0,"wfUploadBytesPerSec":18240.0,
   "paintsPerSec":0.0,"paintMsPerSec":0.0,
   "overlayDirtyCauses":{"smartMtr":2,"detect":1,"other":3}}],
   "renderScheduler":{"enabled":true,"requests":612,"flushes":301,
   "coalescedRequests":288,"avgWidgetsPerFlush":1.9}}
```

| field | meaning |
|---|---|
| `gpuFrameMsPerSec` | **the headline number** — main-thread ms consumed per wall-second preparing + encoding this pan's GPU frames |
| `ingestMsPerSec` | `updateSpectrum()` cost (EMA smoothing, noise floor, waterfall fallback pacing) |
| `fftBuildMsPerSec` / `fftVboBytesPerSec` | FFT trace resample + vertex bake cost and VBO upload volume |
| `overlayRebuilds*`, `overlayUploadBytesPerSec` | static-overlay QPainter repaints (should be ~0/s when idle) |
| `overlayDirtyCauses` | first-cause attribution for each overlay rebuild (`smartMtr`, `detect`, `other`) |
| `previewOverlayTransformsPerSec` | GPU-only remaps of the retained frequency-overlay texture during pan/zoom; these should not produce matching full-image rebuilds/uploads |
| `previewOverlayCommitRefreshes` | exact CPU overlay refreshes performed when pan/zoom previews commit |
| `previewScaleRefreshesPerSec`, `previewScalePaintMsPerSec`, `previewScaleUploadBytesPerSec` | narrow frequency-scale strip refreshes during preview; separates the small correctness update from full-pan overlay work |
| `wfUploadBytesPerSec` | waterfall texture upload volume |
| `nativeWaterfall*` / `kiwiWaterfall*` | source-specific ingest rate and GUI-thread cost; `HiddenUpdates` identifies background Flex/Kiwi work |
| `waterfallVisibleRows*` / `waterfallHistoryRows*` | viewport and retained compact-intensity-history write rates/cost (history is written only for the visible source) |
| `dssLiveRows*` / `dssHistoryRows*` | 96-row live 3D surface work versus deep retained scrollback work; `dssHiddenLiveRowsPerSec` exposes the hidden-Flex live-ring warming (#4081) — hidden sources retain no deep history |
| `waterfallAllocatedBytes` / `dssAllocatedBytes` | current plus cached Flex/Kiwi/profile storage; waterfall history is counted by actually allocated lazy chunks, not logical capacity |
| `paintsPerSec` / `paintMsPerSec` | software-QPainter path (non-zero only before QRhi init or in non-GPU builds) |
| `renderScheduler` | shared panadapter repaint scheduler counters; `coalescedRequests` and `avgWidgetsPerFlush` show cross-pan request coalescing |

`selector` filters by pan index (`get panstats 0`) or objectName. `property`
`reset` zeroes the counters after the read so successive reads measure
disjoint intervals: `get panstats reset` resets all panes plus the shared
scheduler counters, and `get panstats 0 reset` resets one pane.

> **Removed field:** `leanMode` (boolean) was dropped when Lean Mode was
> removed from the app — scripts that keyed on it should stop; every pan now
> always renders the full-quality path.

### `get tracedebug`
Per-panadapter `SpectrumWidget` trace diagnostics for proving Flex/Kiwi display
source behavior without screenshots. This is intentionally diagnostic rather
than user-facing state: use it to compare the currently displayed source, hidden
background histories, separate 2D/3D trace positions, and the 3D floor anchor
used by the stacked trace renderer.

```json
→ {"cmd":"get","model":"tracedebug","selector":"0"}
← {"ok":true,"model":"tracedebug","pans":[{
   "panIndex":0,"name":"SpectrumWidget","renderMode":"3D",
   "kiwiWaterfallActive":false,
   "noiseFloorPosition":75,
   "flexNoiseFloorPosition":75,"kiwiNoiseFloorPosition":68,
   "dssFloorDepth":6,
   "flexDssFloorDepth":6,"kiwiDssFloorDepth":10,
   "dssFloorDbm":-120.5,"dssSpanDb":90.0,
   "flexDssRows":96,"kiwiDssRows":96,
   "kiwiFftTraceFloorDbm":-124.0,
   "kiwiDisplayFloorDbm":-110.0,
   "flexBins":{"count":768,"finiteCount":768},
   "kiwiBins":{"count":768,"finiteCount":768}}]}
```

`selector` filters by pan index (`get tracedebug 0`) or objectName. Key fields:

- `kiwiWaterfallActive` — whether this pan is displaying Kiwi spectrum/waterfall
  (`false` means Flex is displayed; audio and meters are separate concerns).
- `flexNoiseFloorPosition` / `kiwiNoiseFloorPosition` — the source-specific 2D
  trace position values restored when toggling displays.
- `flexDssFloorDepth` / `kiwiDssFloorDepth` — the source-specific 3D floor-depth
  values restored when toggling displays.
- `flexDssRows` / `kiwiDssRows` — rolling 3D history row counts for both display
  sources; useful for checking that hidden histories continue updating.
- `kiwiFftTraceFloorDbm` versus `kiwiDisplayFloorDbm` — distinguishes the FFT
  trace floor used by 3D placement from the waterfall color floor.

### `get rhi`
Per-panadapter `QRhiWidget` **surface geometry and native-widget topology** —
the widget size, devicePixelRatio, pinned color-buffer extents, full-frame
overlay/background textures, the waterfall image/texture pair, and (on macOS)
native-leaf/ancestor isolation — so automation can assert the fractional-scale
alignment and upload-size invariants exercised by pop-out reparenting (#4091,
#4319) together with the bounded native-view hierarchy from #4339.

```json
→ {"cmd":"get","model":"rhi"}
← {"ok":true,"model":"rhi","pans":[{
   "panIndex":0,"name":"","visible":true,"widthPx":1100,"heightPx":455,"dpr":0.85,
   "gpu":true,"renderer":"GPU QRhi (D3D11; Intel(R) HD Graphics 520)",
   "colorBufferAutoSized":false,"colorBufferW":936,"colorBufferH":388,
   "expectedEvenW":936,"expectedEvenH":388,"evenAligned":true,
   "overlayTextureW":936,"overlayTextureH":388,
   "backgroundTextureW":936,"backgroundTextureH":388,
   "waterfallTextureW":936,"waterfallTextureH":194,
   "waterfallImageW":936,"waterfallImageH":194,
   "waterfallTextureMatchesImage":true,
   "fullFrameTexturesEvenAligned":true,
   "fullFrameTexturesMatchColorBuffer":true}]}
```

| field | meaning |
|---|---|
| `dpr` | effective device-pixel ratio (fractional when `QT_SCALE_FACTOR` ≠ integer) |
| `colorBufferAutoSized` | `true` when the widget lets QRhiWidget auto-size (`fixedColorBufferSize` unset); `false` when pinned |
| `colorBufferW` / `colorBufferH` | the pinned device-pixel color buffer, or the unset sentinel `-1,-1` when auto-sized |
| `expectedEvenW` / `expectedEvenH` | what an even-aligned pin should be for the current size — assert `colorBufferW/H` matches without recomputing the formula |
| `evenAligned` | both pinned dimensions are even (the #4091 invariant); `false` when auto-sized |
| `overlayTextureW/H` / `backgroundTextureW/H` | full-frame RGBA texture extents, or `-1,-1` before GPU initialization |
| `waterfallTextureW/H` / `waterfallImageW/H` | live GPU waterfall texture and retained CPU waterfall image extents |
| `waterfallTextureMatchesImage` | the CPU waterfall image fits within its GPU texture (texture ≥ image in both dimensions), so the upload is safe; holds across pop-out initialization even when the texture is floored larger than a small retained image (#4319) |
| `fullFrameTexturesEvenAligned` | both full-frame textures have even width and height (the #4319 invariant) |
| `fullFrameTexturesMatchColorBuffer` | both full-frame textures exactly match the pinned color buffer |
| `nativeWindow` | macOS only: `true` when the `SpectrumWidget` currently has an actual native child window (`windowHandle()` exists); expected for the default Metal path and `false` with `AETHER_PAN_NO_NATIVE_WINDOW=1` |
| `nativeAncestorsBlocked` | macOS only: whether the leaf has `WA_DontCreateNativeAncestors`, preventing its native-window request from promoting the surrounding QWidget tree |
| `nativeAncestorCount` | macOS only: number of QWidget ancestors marked `WA_NativeWindow`; the isolated default Metal path expects `0` |

`selector` filters by pan index (`get rhi 0`) or objectName. On non-GPU builds
each entry reports `gpu:false` and omits the buffer fields. The three native
topology fields are emitted only on macOS; other platforms omit them.

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
   "widthPx":244,"heightPx":110,"nativeWindow":false,
   "nativeAncestorsBlocked":false,"nativeAncestorCount":0,"sinceMs":40012,
   "paintCount":2381,"paintsPerSec":59.5,"avgPaintUs":312.4,"maxPaintUs":1893,
   "paintMsPerSec":18.6,"appendsPerSec":124.9,"samplesPerSec":47980.1}]}
```

- `paintMsPerSec` — `avgPaintUs × paintsPerSec / 1000`; the headline number.
- `mode` uses the applet's UI names: `Scope` / `Envelope` / `History` / `Bands`.
- `floating` + `windowClass` — which top-level surface hosts the scope
  (`MainWindow` docked, `FloatingContainerWindow` popped out, or the strip).
- `nativeWindow`, `nativeAncestorsBlocked`, and `nativeAncestorCount` expose
  the same QWidget/native-surface topology as `get rhi`. On macOS, waveform
  scopes embedded in scroll areas are expected to remain composited
  (`false`, `false`, `0`) so their rendering follows QWidget resize, scroll,
  and clipping geometry.
- Counters accumulate from app start; a selector narrows to one scope
  (`get wavestats waveAppletScope`) and the pseudo-property `reset` zeroes
  the counters after the read (`get wavestats "" reset`) so successive reads
  measure disjoint intervals.
- Hidden scopes keep counting appends (the data feed stays live) but never
  paint — `paintsPerSec` 0 with a nonzero `appendsPerSec` is the expected
  hidden-widget signature, not a bug.

### `tune`
Set a slice's frequency in MHz — the most fundamental control the
custom-painted `VfoWidget` couldn't expose. RX/config only; despite the name it
does **not** key (cf. `atu tune`, which does). Honors the per-slice VFO lock.

Without a slice id the **active slice** is tuned (the original verb shape).
An optional second argument (bare line) / `id` field (JSON) targets a specific
slice by id, so scripts driving a non-active slice no longer need the racy
`slice select` → `tune` → re-select flap:

```json
→ {"cmd":"tune","value":"7.175"}
← {"ok":true,"tune":7.175,"sliceId":0,"letter":"A"}

→ {"cmd":"tune","value":"14.074","id":"1"}
← {"ok":true,"tune":14.074,"sliceId":1,"letter":"B"}
```

Bare-line form: `tune 14.074 1`.

Refused with `refused: slice A is VFO-locked` when the slice is locked, with
`no slice with id N` when the id names no slice, and with `refused: slice N
belongs to another client` when another client owns it (Multi-Flex). To
recenter the *pan* (band change) rather than move the slice within it, use
[`pan center`](#pan).

### `targettune`
Tune through the same absolute-target policy used by typed frequency entry and
other commanded jumps. Unlike `tune`, this can preselect a different band stack
before applying the final frequency, so it is the bridge path for testing
radio-authoritative band restores. RX/config only and honors VFO lock and SWR
sweep guards.

```json
→ {"cmd":"targettune","value":"146.520"}
← {"ok":true,"targetTune":146.52,"sliceId":0,"letter":"A"}
```

### `memory`
Recall a radio memory through `MainWindow::activateMemorySpot()`, including its
cross-band preselection and delayed reveal behavior. The optional `panId`
selects the target pan; omit it to use the active/preferred slice. RX/config
only.

```json
→ {"cmd":"memory","action":"activate","value":"12 0x40000000"}
← {"ok":true,"memory":"activate","index":12,"panId":"0x40000000"}
```

### `slice`
Slice lifecycle, mode, diversity, Center Lock, TX assignment, antennas, and
receive source. All actions are RX/config — none keys the transmitter.
`add`/`remove`/`tx`/`diversity` are async (radio-authoritative); re-poll
`get slices`.

```json
→ {"cmd":"slice","action":"add","value":"14.074"}
← {"ok":true,"slice":"add","freq":14.074,"requested":true,"sliceCount":2}

→ {"cmd":"slice","action":"tx","value":"1"}
← {"ok":true,"slice":"tx","id":1,"requested":true}

→ {"cmd":"slice","action":"mode","value":"DSTR"}
← {"ok":true,"slice":"mode","id":0,"mode":"DSTR","requested":true}
```

| `action` | `value` | effect |
|---|---|---|
| `add` | optional `<mhz>` | create a slice (radio-wide slot capacity is pre-checked; refused at the slice limit, naming any foreign occupant) |
| `remove` | `<sliceId>` | remove a slice (refuses the last one) |
| `select` | `<sliceId>` | make a slice the active slice (`slice set <id> active=1`) |
| `tx` | `<sliceId>` | make a slice the TX slice — the external-split transition; radio enforces single-TX |
| `mode` | `<name>` e.g. `DSTR` | set the active slice mode through `SliceModel`; validated against the radio-advertised mode list |
| `diversity` | `<sliceId> <on\|off>` | enable or disable diversity through the slice model; re-poll `get slices` for parent/child state |
| `centerlock` | `<sliceId> <on\|off>` | enable or disable Center Lock for that exact slice through the same per-pan path as the context menu; an explicit id permits testing either diversity member |
| `txant` / `rxant` | `<port>` e.g. `ANT2` | set the TX/RX antenna of the TX (else active) slice; validated against the slice's antenna list — establish the dummy-load antenna before any TX-safety gate, then read back with `get slice tx txAntenna` |
| `rxsource` (alias `source`) | see below | select the slice's receive source (Flex / virtual-Kiwi) |
| `fixture` | `<sliceId> [A-H]` | disconnected-only test fixture: synthesize an owned slice through the normal slice-status path, optionally with a single radio `index_letter`, so `dumpTree` can assert UI without a radio |
| `clearfixture` | `<sliceId>` | remove a slice created by `fixture`; when the final fixture is removed, restores the pre-fixture disconnected model/max-slice state |

### `gps`

Inject a disconnected-only GPS report through `RadioModel::applyGpsChanges`,
the same typed-delta path used by live `FlexBackend` status. This is safe for
deterministic dashboard and model testing: it is refused while connected and
never sends a radio command.

```json
→ {"cmd":"gps","action":"fixture","value":"6000"}
← {"ok":true,"gps":"fixture","profile":"6000","snapshot":{"status":"Locked","latitude":"N 34 13.464",…}}

→ {"cmd":"gps","action":"fixture","value":"8000"}
← {"ok":true,"gps":"fixture","profile":"8000","snapshot":{"status":"Locked","latitude":"34.224400000","ntpServerAddress":"192.0.2.80",…}}

→ {"cmd":"gps","action":"clearfixture"}
← {"ok":true,"gps":"clearfixture"}
```

Both profiles use the public Mount Wilson Observatory location so screenshots
are safe to share. The `6000` profile exercises the hemisphere/degrees/decimal-
minutes form seen from a FLEX-6700 GPSDO. The `8000` profile exercises decimal
degrees and the `track` (course-over-ground) field captured from FLEX-8600
firmware 4.2.18, plus the reserved TEST-NET-1 address `192.0.2.80` for testing
the 8000-series NTP tip. Use `get gps` or `dumpTree` after injecting to assert
model or widget state.

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

### `dragAt`
Synthesize a drag from an exact target-local point. Optional modifiers are a
comma- or plus-separated combination of `control`, `meta`, `shift`, and `alt`
(`cmd`/`command` and `option` are accepted aliases). This reaches custom-widget
gestures whose behavior depends on both position and a held modifier. As with
`clickAt`, the drag is refused when the target or an ancestor is marked as a
TX-keying control unless transmit automation is explicitly enabled.

```json
→ {"cmd":"dragAt","target":"SpectrumWidget","value":"1564 100 0 300 meta"}
← {"ok":true,"target":"SpectrumWidget","class":"SpectrumWidget",
   "x":1564,"y":100,"dx":0,"dy":300,"modifiers":268435456}
```

`drag` remains the backward-compatible one-shot form. It now enforces the same
disabled-control, TX-keying, and `AETHER_AUTOMATION_TX_MAX_POWER` pointer rails
as `clickAt`; it cannot be used to bypass those guards.

### `gesture`
Keep a real left-button gesture open across multiple main-loop requests. This is
the phaseful counterpart to atomic [`drag`](#drag-alias-mouse), intended for
testing behavior such as a delayed model/radio update arriving while
`QAbstractSlider::isSliderDown()` is genuinely true.

```json
→ {"cmd":"gesture","action":"begin","target":"RF power"}
← {"ok":true,"active":true,"sliderDown":true,"value":50,"leaseMs":60000}

→ {"cmd":"gesture","action":"move","value":"0 -30"}
← {"ok":true,"active":true,"sliderDown":true,"dx":0,"dy":-30}

→ {"cmd":"gesture","action":"end"}
← {"ok":true,"active":false,"target":"RF power","dx":0,"dy":-30}
```

Phases:

- `begin <target> [x y]` presses at the widget center, or at optional local
  coordinates. Only one phaseful gesture may exist in the app at once.
- `move <dx> <dy>` sends a move at fixed-base offsets from the original press.
- `status` is read-only and reports `active`, ownership, offsets, and
  `sliderDown`/`value` for a slider.
- `end [dx dy]` optionally moves to a final offset, then releases.
- `cancel` releases at the current offset.

The owning bridge connection must remain open between phases. The typed MCP
`gesture` tool does this automatically while ordinary tools keep using separate
connections, which is what lets an independent `invoke`, `dump_tree`, or delayed
app event interleave. Raw clients must reuse one socket themselves.

Safety is fail-closed: auth and observe-only checks apply to every phase; begin
uses the same disabled-control, TX-keying, and power-ceiling rails as pointer
clicks. End, cancel, malformed continuation, target destruction, client
disconnect, bridge stop, or 60 seconds without a move all synthesize the release
and clear ownership.

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

### `tooltip`
Force-show a widget's native Qt tooltip, using the widget's current
`toolTip()` text unless an override string is supplied. This is for screenshots
and assertions where injected hover should prove the target state but the
platform does not run Qt's built-in tooltip timer under automation.

```text
→ {"cmd":"tooltip","target":"E"}
← {"ok":true,"target":"E","class":"QToolButton",
   "text":"Slice E (global slot 5)","grabHint":"QTipLabel", ...}
→ tooltip E Screenshot override text
← {"ok":true,"target":"E","text":"Screenshot override text", ...}
→ {"cmd":"grab","target":"QTipLabel","path":"/tmp/slice-tooltip.png"}
← {"ok":true,"class":"QTipLabel","path":"/tmp/slice-tooltip.png", ...}
```

Use `{"cmd":"tooltip","target":"E","action":"hide"}` to dismiss it.

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

### `rightClick`
Trigger a widget path that handles right-clicks directly in `mousePressEvent`,
rather than through Qt's `contextMenuEvent`/`customContextMenuRequested` policy.
The panadapter's `SpectrumWidget` menu is the main use case: it is position
sensitive and built from a real right-button press, so [`contextMenu`](#contextmenu)
cannot reach it.

```json
→ {"cmd":"rightClick","target":"Panadapter spectrum display"}
← {"ok":true,"target":"Panadapter spectrum display","class":"SpectrumWidget",
   "x":939,"y":735,"deferred":true}

→ {"cmd":"rightClick","target":"Panadapter spectrum display","value":"940 730"}
← {"ok":true,"target":"Panadapter spectrum display","class":"SpectrumWidget",
   "x":940,"y":730,"deferred":true}
```

The verb posts a right-button `MouseButtonPress` onto the GUI event loop with the
owning window raised, then returns immediately. Follow with `dumpTree` to inspect
the visible `QMenu`, and `invoke <menu item text> trigger` to choose an action.

Section-title rows (a disabled `QWidgetAction` + `QLabel`, the app's idiom for
menu headers since `QMenu::addSection` text doesn't render under the app styling)
serialize with `"type":"header"` and the label's text, so titles are assertable
instead of blank rows.

### `hitTest`
Read-only Qt hit-test probe for overlay/input-mask regressions. The point is
target-local; omit `x y` to test the target center. `childAt` is the target's
child owner at that local point, while `widgetAt` is Qt's global topmost widget
owner at the same screen point.

```json
→ {"cmd":"hitTest","target":"SpectrumWidget","value":"80 80"}
← {"ok":true,"target":"SpectrumWidget","x":80,"y":80,
   "childAt":{"class":"VfoWidget",...},
   "widgetAt":{"class":"VfoWidget",...}}
```

### `clickAt`
Synthesize a real left-click (`press`→`release`) at a **point** rather than at a
named widget. This is the escape hatch for when `invoke`/name matching can't reach
the control you want — most commonly because several widgets share the same
`accessibleName` (every side-panel tile's close button is `containerClose`, its
float toggle `containerFloatToggle`, etc.), so `invoke` can only ever hit the
**first** match. `dumpTree` reports widget `geometry` in **global (screen)**
coordinates, so clicking the centre of a tile's dumpTree rect clicks exactly that
tile's control.

Two forms:
- **`clickAt <x> <y>`** — `x y` are **global** screen coordinates. The bridge
  clicks whatever `QApplication::widgetAt(x, y)` resolves (the topmost widget at
  that point).
- **`clickAt <target> <x> <y>`** — `x y` are **local** to `<target>` (like
  `hitTest`); the point must lie inside the target's rect, and the click is
  routed to the deepest `childAt` that point.

The click is **TX-guarded** like `invoke`, but stricter: the guard walks the
**whole ancestor chain** of the widget under the point (Qt propagates an
unaccepted press to parents, so a click on a passive child of a keying control
must be refused too) and is refused unless `AETHER_AUTOMATION_ALLOW_TX=1` — a
coordinate click is never a hole around the keying gate. Clicks on the RF/Tune
power sliders are refused while `AETHER_AUTOMATION_TX_MAX_POWER` is armed (a
groove click can't be clamped — use `invoke … setValue`, which is). A disabled
widget under the point is refused (`"disabled":true`), same as `invoke`.

Delivery is deferred one main-loop turn (`"deferred":true`), so any popup/dialog
it raises runs on a clean stack; follow with `dumpTree`/`grab` to read the
result. Because the reply is sent **before** delivery, anything already queued
(a relayout, a pan re-center) can change what that pixel means by the time the
press lands — re-`dumpTree` right before `clickAt` and don't interleave other
mutating verbs in between.

In the reply, `localX`/`localY` are local to the widget the click was **routed
to** (`clicked`) — the deepest child — not to the named `<target>`, so for the
target-local form they generally differ from the `x y` you sent.

```json
→ {"cmd":"clickAt","x":1420,"y":210}          // global point (or "value":"1420 210")
← {"ok":true,"clicked":{"class":"QPushButton","accessibleName":"containerClose",…},
   "globalX":1420,"globalY":210,"localX":7,"localY":6,"deferred":true}

→ {"cmd":"clickAt","target":"AppletPanel","value":"12 34"}   // point local to a widget
← {"ok":true,"clicked":{"class":"…"},"globalX":1318,"globalY":97,
   "localX":5,"localY":3,"deferred":true}     // local to `clicked`, not to AppletPanel
```

The JSON `x`/`y` fields must both be present and JSON-numeric; a missing or
string-typed coordinate is rejected rather than coerced to 0 (which would click
the screen edge).

Recipe — close a **specific** side-panel tile (not just the first `containerClose`):
read the target tile's `containerClose` rect from `dumpTree`, compute its centre in
global coordinates, and `clickAt` that point.

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

### `shortcut`
Fire a registered `ShortcutManager` action by id — the **exact** path a MIDI
controller mapping takes (`fireShortcut` → `action(id)->handler()` in
`MainWindow_Controllers.cpp`). Many actions carry no default key sequence **and**
no menu entry — Band Zoom, Segment Zoom, and every MIDI-only trigger — so they're
otherwise unreachable by `invoke` (no widget), a key event (no `QKeySequence`), or
`menu` (no menu item). This verb drives them directly. The id is the shortcut's
registration id (as in the Configure Shortcuts list / MIDI mapping short id), e.g.
`band_zoom`, `segment_zoom`, `split_toggle`, `filter_widen`.

```json
→ {"cmd":"shortcut","target":"band_zoom"}
← {"ok":true,"shortcut":"band_zoom","fired":true}
```

(JSON form: the id rides the `target` field; a `target` present alongside other
fields wins.) The handler runs synchronously on the GUI thread. An unknown id
replies `{"ok":false,"error":"unknown shortcut action id: …"}`.

**`fired:true` means the handler ran, not that anything happened.** Handlers
validate their own preconditions exactly like a physical MIDI press — no radio
connection, no active slice, or no resolvable pan is a silent no-op. Assert
effects through `get`/`dumpTree` (e.g. `get pans` for the zoom actions), not
from the reply.

**Momentary key actions can't be fired by id.** `ptt_hold` and the CW
straight-key/paddle ids appear in the Configure Shortcuts list but register
null handlers on purpose — they're driven by the app-level event filter, which
needs key **release** edges. The bridge replies with a distinct
`event-filter-driven` error for these, not `unknown id`.

**TX-safety:** actions registered as transmit-keying (`keysTx` at their
`registerAction` site — `mox_toggle`, `tune_toggle`, `two_tone_tune`,
`atu_start`, `ptt_hold`, and the CW key ids) are refused unless
`AETHER_AUTOMATION_ALLOW_TX=1`, mirroring the [`invoke`](#invoke) /
[`key`](#key) TX guard. The gate reads the registration flag — one source of
truth, no bridge-side id list to drift. RX-only actions (the zoom shortcuts
included) need no flag.

### `midi`
Inject one MIDI Control Change value through the same learned VFO Tune Knob
relative decoder used by physical controllers. This focused automation surface
does not create or persist a binding and is RX-only.

```json
→ {"cmd":"midi","action":"cc","value":"65"}
← {"ok":true,"midi":"cc","value":65,"paramId":"rx.tuneKnob","accepted":true}
```

Bare form: `midi cc 65`. Use `get slice active` before and after the injection
to assert that center-64 values 65 and 63 move exactly one configured tuning
step in opposite directions. The controller manager coalesces events for 20 ms,
so callers should wait briefly before reading the resulting slice frequency.

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

Floating and docking use the production pan title-bar control rather than a
radio lifecycle command. Target its stable object name through the pan scope;
the same toggle works in both states:

```json
→ {"cmd":"invoke","target":"pan 1/panFloatToggle","action":"click"}
← {"ok":true,"target":"pan 1/panFloatToggle","action":"click"}
```

Re-poll `layout get` and assert `floatingCount` / `dockedCount` after each
transition. The control's accessible name also changes between
`Pop out panadapter` and `Dock panadapter` for semantic snapshots.

### `layout`
Drive the panadapter **splitter layout** directly, decoupled from how many
panadapters the radio has granted.

```json
→ {"cmd":"layout","action":"rearrange","value":"2v"}
← {"ok":true,"layout":"rearrange","requested":"2v","applied":true,
   "fellBack":false,"effectiveLayout":"2v","settlesNextTurn":true,
   "panCount":2,"dockedCount":2,"floatingCount":0,"savedLayout":"1"}

→ {"cmd":"layout","action":"get"}
← {"ok":true,"layout":"get","requested":"","applied":false,
   "panCount":1,"dockedCount":1,"floatingCount":0,"savedLayout":"1"}
```

| `action` | `value` | effect |
|---|---|---|
| `rearrange` | layout id (`1`/`2v`/`2h`/`2h1`/`12h`/`3v`/`2x2`/`4v`/`3h2`/`2x3`/`4h3`/`2x4`) | rebuild the splitter for that id via the production `PanadapterStack::rearrangeLayout`, exercising the full teardown/reparent/GPU-surface path on whatever pans exist. |
| `get` | — | report the saved `PanadapterLayout` + live pan/docked/floating counts without changing anything. |

Why it exists: on a shared radio, MultiFlex caps how many panadapters a client
can open, so the add-2nd-pan resize path (the #4091 crash) can be unreachable
from the bridge. `layout rearrange` forces the splitter machinery to run
regardless. It is a **transient exerciser** — it does *not* persist
`PanadapterLayout`. RX/config only; never keys TX.

Honesty of the reply: an **unknown id is rejected** (`ok:false`) rather than
silently building the trivial fallback; an id needing **more applets than
exist** runs the production vertical-stack fallback and reports
`fellBack:true` + `effectiveLayout:"vstack"` — a test that meant to exercise a
*nested* layout (the #4091 reparent path) must assert `fellBack:false`, or the
green result proves nothing. Geometry **settles on the next event-loop turn**
(`settlesNextTurn:true` — ex-floating pans re-show and sizes equalize
deferred), so don't pipeline `get rhi`/`grab` in the same write; re-poll after.

### `scale`
Report — and optionally persist — the UI scale factor, so scale-dependent
rendering bugs (fractional `QT_SCALE_FACTOR`, e.g. #4091) are reproducible and
assertable.

```json
→ {"cmd":"scale"}
← {"ok":true,"scale":true,"qtScaleFactorEnv":"0.85",
   "uiScalePercentSaved":85,"primaryScreenDpr":2.0}

→ {"cmd":"scale","value":"85"}
← {"ok":true,"scale":true,"qtScaleFactorEnv":null,"uiScalePercentSaved":100,
   "primaryScreenDpr":2.0,"uiScalePercentSet":85,"appliesOnNextLaunch":true}
```

Bare `scale` reports only. `scale <pct>` (one of `75|85|100|110|125|150|175|200`,
matching the **View → UI Scale** menu) persists `UiScalePercent`. Because
`QT_SCALE_FACTOR` must be set before `QApplication` (see `main.cpp`), a scale
change **only applies on the next launch** — this verb never mutates the running
process. To actually run under a fractional scale in one shot, launch with the
env directly: `QT_SCALE_FACTOR=0.85 AETHER_AUTOMATION=1 …`. Pair with `get rhi`
to assert the resulting swapchain dimensions. Never keys TX.

Note: the running session's **View → UI Scale menu checkmark is built once at
startup and will not reflect a bridge write** — the persisted value is applied
(and the menu re-seeded) on the next launch.

### `panmessage`
Manual test hook for panadapter overlay popup messages. This is UI-only: it
does not send radio commands and never keys the transmitter.

```json
→ {"cmd":"panmessage","action":"add","target":"0","id":"kiwi",
   "title":"Waiting for KiwiSDR receiver slot",
   "detail":"Receiver channels are full; AetherSDR will reconnect automatically.",
   "timeoutMs":0}
← {"ok":true,"panmessage":"add","target":"0","id":"kiwi","accepted":true,
   "messages":[{"id":"kiwi","title":"Waiting for KiwiSDR receiver slot",...}]}

→ {"cmd":"panmessage","action":"add","target":"0","id":"tx","timeoutMs":2500,
   "tone":"warning",
   "title":"Transmit disabled",
   "detail":"Transmit is disabled because this panadapter is displaying a KiwiSDR receiver."}
← {"ok":true,"panmessage":"add","target":"0","id":"tx","accepted":true,...}

→ {"cmd":"panmessage","action":"list","target":"0"}
← {"ok":true,"panmessage":"list",
   "messages":[{"id":"tx","remainingMs":1840,"countdown":"2s","tone":"warning",...}]}

→ {"cmd":"panmessage","action":"remove","target":"0","id":"kiwi"}
← {"ok":true,"panmessage":"remove","removed":true,...}
```

Bare-line forms are accepted for quick screenshot setup. For `add`, an optional
`tone=info|warning` may follow `<timeoutMs>`, and the text after that is split
at the first `|` into title and detail:

```text
panmessage add 0 kiwi 0 Waiting for KiwiSDR receiver slot|Receiver channels are full; AetherSDR will reconnect automatically.
panmessage add 0 tx 2500 tone=warning Transmit disabled|Transmit is disabled because this panadapter is displaying a KiwiSDR receiver.
panmessage list 0
panmessage remove 0 kiwi
panmessage clear 0
```

Targets are a `panIndex` from `dumpTree`, `active`, a `SpectrumWidget`
`objectName`, or a radio pan id (`0x...`). Use `grab pan-visible <index>` to
capture the operator-visible stack, including the close buttons.

Messages with `timeoutMs > 0` render a small countdown badge on the card and
report the same value in the `countdown` snapshot field.

> ⚠️ **This verb shares the production overlay.** The same overlay carries
> owner-managed cards — the KiwiSDR `kiwi.connection` status card and the
> `interlock.active` "Transmit disabled" warning. Consequences for tests:
> - `panmessage clear` deletes **live** status cards too. Their producers only
>   re-post on the next state transition, so a quiet pan can be left with no
>   disconnected/interlock indicator until something changes. Prefer
>   `panmessage remove <id>` scoped to an id your test created.
> - `add` can upsert those production ids directly (e.g. forge a
>   `Transmit disabled` card, or overwrite `kiwi.connection`), indistinguishable
>   from the real path. Namespace injected ids (e.g. a `test.` prefix) so a
>   teardown `clear`/`remove` can't touch operator-facing status.

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

None of the `streams` actions keys the transmitter. In **Observe only** mode,
the default Layer-A inventory and `radio`/`inventory` reads remain available;
`reset`, `resync`, and `refresh` are blocked. `reset` changes the local orphan
tally, while `resync`/`refresh` send the `sub pan all` subscription command to
the radio.

### `memprofile`
Cross-platform process and subsystem memory profiling for long-running leak
investigations. An instant snapshot combines the operating system's native
process counters with explicitly sized application buffers and lightweight
QObject lifecycle inventories:

```json
→ {"cmd":"memprofile","action":"snapshot"}
← {"ok":true,"schemaVersion":1,
   "process":{"residentMetric":"physicalFootprint","residentBytes":412876800,
              "privateBytes":355205120,"allocatorInUseBytes":287309824,
              "trackedSubsystemBytes":94781440,"unattributedResidentBytes":318095360},
   "subsystems":{
     "panadapter":{"trackedBytes":89128960,"estimatedGpuBytes":7549747,
                   "objectCount":143,"classes":{"SpectrumWidget":1},"details":{"panCount":1}},
     "audio":{"trackedBytes":65536,"objectCount":42},
     "radioModels":{"trackedBytes":0,"objectCount":71},
     "gui":{"trackedBytes":0,"objectCount":2610},
     "automation":{"trackedBytes":5586944,"objectCount":12}}}
```

`residentMetric` deliberately follows the number operators see in the native
task monitor: Working Set on Windows, physical footprint on macOS, and VmRSS on
Linux. `privateBytes`, allocator counters, virtual size, thread count, and
handle count appear where that OS exposes them. They are useful within one OS;
they are not byte-for-byte comparable across operating systems.

The sampler is bounded and off until explicitly started:

```text
memprofile start 5000 10000   # sample every 5 s, retain at most 10,000 points
memprofile status             # current snapshot + compact trend report
memprofile sample             # force an extra point now
memprofile report             # compact report, no raw points
memprofile samples            # compact report + retained raw points
memprofile stop               # final point + compact report
memprofile reset              # stop and release the retained series
```

JSON requests use `action` and, for `start`, `value: "<intervalMs>
<maxSamples>"`. The interval is bounded to 250–3,600,000 ms and retention to
2–10,000 samples so the profiler cannot become the leak. Five-second sampling
holds about 13.8 hours; use 30–60 seconds for multi-day runs.

Each byte/count trend reports `first`, `last`, `delta`, `min`, `max`, a linear
per-hour slope, and `rSquared`. `sustainedGrowth` is a triage heuristic only: at
least six samples over at least one minute, ≥4 MiB net byte growth, positive
slope, and R² ≥0.5. `growthSuspects` collects metrics meeting that bar;
`classCountGrowth` shows retained or released QObject classes by subsystem.
Correlate the two:

- rising `subsystem.panadapter.trackedBytes` identifies retained waterfall/3DSS
  buffers;
- a growing `SpectrumWidget`/panadapter class count identifies lifecycle leaks;
- rising process resident/private bytes with flat tracked subsystems points to
  an uninstrumented native, allocator, Qt, driver, or GPU allocation;
- a positive slope with low R² is usually warm-up/cache/noise, not proof of a
  leak. Extend the run and reproduce the same slope before fixing anything.

Tracked subsystem bytes are deliberately honest lower bounds. They do not claim
to attribute every heap allocation, and GPU surface estimates are excluded from
resident attribution. GUI object counts include panadapter objects, so object
counts are scoped diagnostics and are not additive.

Use the companion driver for unattended runs; it writes the final report and
raw series atomically, records the bridge identity, and warns when the process
has the independent TX-automation rail armed (the soak never invokes TX):

```bash
python3 tools/memory_soak.py --duration 300 --interval 5 \
  --output /tmp/aethersdr-memory-5m.json
```

For a repeatable cross-band soak, the driver can also cycle the active slice
and panadapter through RX frequencies. The first frequency is applied
immediately, each change is marked in the bridge log, and the tune responses
are retained in the output JSON alongside the memory series:

```bash
python3 tools/memory_soak.py --duration 3600 --interval 5 \
  --tune-interval 600 \
  --tune-frequencies 3.573,7.074,14.074,21.074,28.074,50.313 \
  --output aethersdr-memory-1h.json
```

`tune` and `pan center` are RX/config-only bridge actions; this cycle never
keys the transmitter. A refused VFO lock or pan-center request is printed as a
failure and preserved in `tuneEvents` rather than being silently ignored.

The MCP server exposes the same surface as `memory_profile`.

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

### `get txtimer`
Read the status-bar transmit timer's state. The timer sits just left of the
**PC Audio** button and runs **only** for operator-driven phone/data transmits
— MOX, local/hardware PTT, footswitch, VOX — and deliberately **not** for
TCI-hardware or DAX transmits (external-app keying paths) **nor CW** (break-in/
QSK toggles the interlock per element, which would thrash a wall-clock timer).
All three exclusions are gated in `RadioModel::operatorTransmitChanged`. It is
hidden when idle; on unkey it holds the final elapsed reading for 15 s, then
fades out.

```json
→ {"cmd":"get","model":"txtimer"}
← {"ok":true,"model":"txtimer","visible":true,"running":true,"holding":false,
   "fading":false,"elapsedMs":4210,"text":"0:04","opacity":1.0}
```

Fields: `visible` (label shown at all), `running` (keyed, counting up),
`holding` (the 15 s post-unkey hold is armed), `fading` (fade-out animation in
flight), `elapsedMs` / `text` (live while running, frozen at unkey), `opacity`
(1.0 while shown/holding, ramps to 0 during fade). A trailing property narrows
it: `get txtimer running` → `{"value":true}`. Assertion shapes: after a 1 W
dummy-load MOX key, `running=true` + `elapsedMs` climbing; after unkey,
`running=false`, `holding=true`, `text` frozen; ~15 s later `fading=true` then
`visible=false`. A DAX, TCI, or CW transmit must leave `visible=false`
throughout.

### `tci`
In-process TCI **client** simulator. Connects to this app's own TCI server
over loopback and offers two profiles after draining the init burst through
`ready;`:

- Default/WSJT-X: sends `audio_samplerate:48000;` + `audio_start:0;` and counts
  binary RX-audio frames.
- SDC: sends `iq_samplerate:96000;`, `audio_samplerate:24000;`, and
  `iq_start:0;`, then separately counts type-0 IQ frames. This reproduces the
  CW-skimmer initialization path and proves the DAX IQ stream is live.

Removes the external-WebSocket dependency for TCI/DAX lifecycle testing.
Requires the TCI server to be running (toggle via `invoke tciEnable click` if
needed).

```json
→ {"cmd":"tci","action":"start"}            // optional value = port
← {"ok":true,"action":"start","profile":"wsjtx","port":50001}

→ {"cmd":"tci","action":"start","value":"sdc"} // optional: "sdc 50001"
← {"ok":true,"action":"start","profile":"sdc","port":50001}

→ {"cmd":"tci","action":"status"}
← {"ok":true,"running":true,"connected":true,"ready":true,"profile":"sdc",
   "iqStarted":true,"iqFrames":412,"binaryFrames":412,"binaryBytes":3375104,
   "textMessages":37,"msSinceLastFrame":18}

→ {"cmd":"tci","action":"stop","value":"abrupt"}   // omit value for graceful audio_stop + close
← {"ok":true,"action":"stop","abrupt":true,"binaryFrames":412, …}
```

`stop abrupt` closes the socket without `audio_stop` or `iq_stop`; graceful
stop sends the command matching the selected profile. This lets tests assert
the relevant DAX cleanup. For the SDC profile, `iqFrames` climbing is the
"skimmer IQ is actually flowing" assertion. For the default profile,
`binaryFrames` climbing at a steady rate (~47/s at 48 kHz) proves RX audio is
flowing. `msSinceLastFrame` spiking while the selected stream is started means
that stream went silent.

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

### `get clock`
Read the AetherClock time-signal decode snapshot (engine + voter state for the
WWV/WWVH/WWVB decoders). Served before the radio guard, so it answers even
while disconnected; until the GUI wires a model it replies
`"no clock model available"`.

```json
→ {"cmd":"get","model":"clock"}
← {"ok":true,"model":"clock","state":2,"stateName":"Locked",
   "station":1,"stationName":"WWV","decodedUtc":"2026-07-20T22:52:59.000Z",
   "offsetMs":-129.7,"lockQuality":75,"sliceId":0,"gpsTimeAvailable":false}
```

Useful fields:

| field | meaning |
|---|---|
| `state` / `stateName` | `NoSignal`, `Acquiring`, `Locked` — the authoritative currency signal |
| `station` / `stationName` | Auto-tagged station: WWV / WWVH / WWVB |
| `decodedUtc` | Last voted broadcast time (ISO-8601; empty until a first decode). Retained after demotion so age-since-decode stays computable — always read it beside `stateName` |
| `offsetMs` | decoded − host at the second edge; positive = host behind broadcast |
| `lockQuality` | Voter lock confidence 0–100 (weakest-voted-bit semantics) |
| `sliceId` | Bound slice while running, −1 when stopped |
| `gpsTimeAvailable` | Whether the connected radio reports GPS time (context for the offset) |

Acquisition telemetry (additive; mirrors the engine's ~1 Hz `ClockDiagnostics`
snapshot — every value is a real measurement or a real gate verdict, updated
while the engine runs):

| field | meaning |
|---|---|
| `toneSnrDb` | Stage 1 carrier readout: WWVB tone-search peak/median in dB; WWV/WWVH folded tick-band peak-to-mean in dB |
| `pwmContrast` | WWVB envelope p90/p10 contrast (0 when n/a); ≥ ~1.4 means a real AM drop exists |
| `toneDetected` | Carrier gate result (WWVB tone gate / WWV tick-fold lock) |
| `phaseLocked` | Second-edge timing sync (WWV tick lock / WWVB envelope phase) |
| `delayEstMs` | WWV tracked matched-filter delay estimate; `null` when the decoder has none |
| `anchored` | Minute frame anchored (marker sync) |
| `badFrameStreak` | Consecutive broken-marker-skeleton frames (WWV; 3 triggers resync) |
| `classifiedPct` | % of the last 60 s that classified into a symbol |
| `framesInWindow` / `windowSize` | Voter sliding-window occupancy |
| `voteQuality` | Raw voter lock confidence 0–1 (pre-scale; `lockQuality` is the 0–100 post-lock mirror) |
| `refusalReason` / `refusalName` | Which lock gate is currently refusing: `None`, `QualityFloor`, `Plausibility`, `Staleness`, `Contested` (`None` = locked or still collecting frames) |

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

### `dss`
Automation-only 3D stacked-trace / waterfall scrollback proof surface. It finds
a `SpectrumWidget` by `panIndex`, injects synthetic RX rows through the normal
SpectrumWidget waterfall paths, and returns compact counters/peak-bin snapshots.
It is RX-only: no radio commands and no transmit keying.

```json
→ {"cmd":"dss","action":"reset","target":"0","value":"native"}
← {"ok":true,"panIndex":0,"live":true,"waterfallRows":96,
   "centerMhz":14.1,"bandwidthMhz":0.192,"dssHistoryRows":0,...}

→ {"cmd":"dss","action":"inject","target":"0","value":"99 100 1 native"}
← {"ok":true,"dssHistoryRows":99,"waterfallHistoryRows":99,
   "maxHistoryOffsetRows":3,
   "dssHistoryRowsAdded":99,"waterfallHistoryRowsAdded":99,...}

→ {"cmd":"dss","action":"scrollback","target":"0","value":"1"}
← {"ok":true,"live":false,"historyOffsetRows":1,
   "maxHistoryOffsetRows":3,...}

→ {"cmd":"dss","action":"inject","target":"0","value":"3 420 0 native"}
← {"ok":true,"live":false,"dssHistoryRows":102,
   "waterfallHistoryRows":102,"historyOffsetRows":4,
   "dssHistoryRowsAdded":3,"waterfallHistoryRowsAdded":3,
   ...}

→ {"cmd":"dss","action":"scrollback","target":"0","value":"0"}
← {"ok":true,"live":false,"historyOffsetRows":0,"dssVisiblePeakBin":420,...}
```

This example assumes the reset response reports `waterfallRows:96`; for a
different widget height, inject at least `waterfallRows + 3` rows before asking
for `scrollback 1`.

Actions:

| action | value | effect |
|---|---|---|
| `snapshot` | optional pan target | Read `live`, current center/bandwidth MHz, waterfall/DSS history row counts, visible DSS row count, the current front-row peak bin/min/max/span, localized plateau metrics (`dssVisibleFrontMinValueBins`, `dssVisibleFrontLongestFlatRunBins`, and visible maxima), and flat/non-flat visible-row counts. |
| `reset` | `native` or `kiwi` | Clear the selected stream's current/history rows and make that stream active for subsequent injection. |
| `inject` | `<count> <firstPeakBin> <stepBin> [native\|kiwi [rowLowMhz rowHighMhz]]` | Add synthetic rows with one strong peak per row. `count` is rejected if it exceeds the retained waterfall history capacity. Native injection adds one fallback-style waterfall/DSS row per input row; Kiwi injection drives `updateKiwiSdrWaterfallRow()`. Kiwi frame arguments override the source row's frequency span, so tests can cover partial-overlap rows. |
| `scrollback` | `<offsetRows>` | Enter waterfall history mode and rebuild the 3D surface using the same offset. |
| `live` | none | Return to live mode. |

The paused/live-history assertion is: enter `scrollback`, inject more rows,
confirm both `waterfallHistoryRowsAdded` and `dssHistoryRowsAdded` match the
injected count while `historyOffsetRows` advances and `dssVisiblePeakBin` stays
on the same paused historical row, then set `scrollback 0` and confirm the newly
injected peak becomes visible. The total row counts are still returned, but the
`*RowsAdded` fields are the deterministic assertion surface if live data is also
arriving between bridge requests.

To reproduce a low-coverage Kiwi row, read `centerMhz` and `bandwidthMhz` from
`dss snapshot`, then inject a Kiwi source row whose span overlaps less than 5%
of the current view. For example, with `viewHigh = centerMhz + bandwidthMhz/2`,
`dss inject 3 120 0 kiwi <viewHigh - 0.03*bandwidthMhz> <viewHigh + 0.97*bandwidthMhz>`
keeps row counts aligned while proving the DSS history stores the partial row
content instead of a flat fallback row.

### `whoami`
Identify **this** bridge instance among concurrent bridges (each app process gets
its own per-pid socket + discovery entry).

```json
→ {"cmd":"whoami"}
← {"ok":true,"pid":34758,"name":"aethersdr-automation-34758",
   "socket":"/var/folders/…/aethersdr-automation-34758",
   "label":"","station":"Automation","agentName":"Automation",
   "txAllowed":false,"version":"26.6.5"}
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

In **Observe only** mode, `categories`, `get`, `tail`, `subscribe`, and
`unsubscribe` remain available. `set` and `reset` are blocked because they
change the app's logging state.

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

`grab` and `invoke` resolve a `target` string in this order. Within each match
class, visible/enabled widgets win over hidden duplicate controls; hidden
widgets are only a fallback when there is no visible candidate, and `invoke`
will refuse to drive a hidden widget.

0. **VFO shortcuts** — `"vfo slice 1"` or `"vfo:slice:1"` targets the VFO
   flag for slice 1. `"vfo 1"` or `"vfo:1"` targets the first VFO flag inside
   the `SpectrumWidget` whose `panIndex` is 1, mirroring `grab pan 1`.
   Prefer the slice form when a pan contains multiple VFOs.
1. **Pan-scoped `"pan <index>/<name>"`** — targets a control inside the
   `PanadapterApplet` whose `SpectrumWidget` has that `panIndex`, e.g.
   `"pan 0/Display"` or `"pan 0/displayAutoBlackBtn"`. This is the preferred
   form when multiple panadapters have the same side buttons or Display panel
   object names.
2. **Scoped `"<scope>/<name>"`** — disambiguates a control whose
   `accessibleName` appears in more than one applet (e.g. `"AF gain"` and
   `"Squelch threshold"` exist in **both** `RxApplet` and `PanadapterApplet`).
   `<scope>` matches an ancestor by objectName, class, or accessibleName;
   `<name>` is resolved within that subtree by objectName, class,
   accessibleName, then button text. Use `"RxApplet/AF gain"` vs
   `"PanadapterApplet/AF gain"`, or target an objectName such as
   `"PanadapterApplet/displayAutoBlackBtn"`. Falls through to flat matching if
   it doesn't resolve, so a literal `/` in a name still works.
3. **Exact `objectName`** — the most stable handle. Prefer this.
4. **Class name** — full (`AetherSDR::SpectrumWidget`) or short
   (`SpectrumWidget`). Handy when a widget has no objectName (the panadapter is
   targeted as `SpectrumWidget`).
5. **`accessibleName`** — e.g. `"Panadapter spectrum display"`,
   `"Master volume"`.
6. **Button text** — last resort, e.g. `"Send"`, `"Transmit"`. Lowest priority,
   so a real objectName/accessibleName always wins; first match in tree order.
7. **Visible popup-menu action** — exact `QAction` objectName, visible text,
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

---

## Verb registry (auto-generated)

The complete registry, generated from the `add(...)` table in `AutomationServer.cpp` by `tools/gen_bridge_docs.py`. CI fails if this drifts from the code.

<!-- BEGIN GENERATED VERB TABLE (tools/gen_bridge_docs.py) -->
<!-- Do not edit by hand — run tools/gen_bridge_docs.py. 53 verbs. -->

| Verb | Aliases | Description |
|---|---|---|
| `ping` | — | liveness check → app + version + whether a token is required |
| `verbs` | — | list every bridge verb with aliases and help (this table) |
| `dumpTree` | — | serialize the full widget tree as JSON |
| `floors` | — | per-pan measured noise + display floor (dBm) |
| `grab` | — | grab <target\|pan\|pan-visible [index]> [path] — PNG capture |
| `close` | — | close <target> — close the target's top-level window |
| `hover` | — | hover <target> [leave] — synthetic mouse hover |
| `tooltip` | — | tooltip <target> [hide\|text…] — force-show a native tooltip |
| `scrollTo` | `ensureVisible` | scrollTo <target> — scroll a widget into its scroll-area viewport |
| `drag` | `mouse` | drag <target> <dx> <dy> — synthesize press→move→release |
| `dragAt` | — | dragAt <target> <x> <y> <dx> <dy> [control\|meta\|shift\|alt,...] |
| `gesture` | — | gesture <begin\|move\|end\|cancel\|status> — phaseful pointer gesture |
| `showMenu` | `openMenu` | showMenu <target> — pop a button's drop-down menu |
| `contextMenu` | — | contextMenu <target> [x y] — Qt context-menu path |
| `rightClick` | — | rightClick <target> [x y] — mousePressEvent menu path |
| `hitTest` | `hittest` | hitTest <target> [x y] — read-only widget-owner probe |
| `clickAt` | `clickat` | clickAt <x> <y> \| clickAt <target> <x> <y> — TX-guarded coordinate click |
| `invoke` | — | invoke <target> <action> [value…] — drive a control (TX-guarded) |
| `get` | — | get <model> [selector] [property] — live model snapshot |
| `connect` | — | connect <list\|show\|hide\|local\|ip\|wait> [args] |
| `disconnect` | — | disconnect from the radio |
| `txtest` | — | txtest <twotone\|off> — TX-gated test signal |
| `atu` | — | atu <bypass\|start> — antenna tuner (start is TX-gated) |
| `slice` | — | slice <action> [args] — slice lifecycle/config (see doSlice) |
| `gps` | — | gps <fixture\|clearfixture> [6000\|8000] — disconnected GPS test data |
| `waveform` | — | waveform <start\|stop\|unregister\|resync> [args] — digital-voice service |
| `tune` | — | tune <mhz> [sliceId] — set a slice frequency (default: the active slice) |
| `targettune` | — | targettune <mhz> — absolute tune through band-stack preselection |
| `memory` | — | memory activate <index> [panId] — recall a radio memory |
| `cwx` | — | cwx <send\|speed\|stop> [args] — CWX keyer (send is TX-gated) |
| `record` | — | record <start\|stop\|status\|path\|dir> [args] |
| `testtone` | — | testtone <on\|off> [freqHz levelDb] |
| `pan` | — | pan <create\|add\|remove\|close\|center> [value] |
| `layout` | — | layout <rearrange <id>\|get> — splitter layout exerciser |
| `scale` | — | scale [pct] — report/persist the UI scale factor |
| `panmessage` | — | panmessage <add\|remove\|clear\|list> <pan> [id timeout [tone=…] title\|detail] |
| `dss` | — | dss <snapshot\|reset\|inject\|scrollback\|live> [pan] [args] |
| `streams` | — | streams [radio\|inventory\|resync\|refresh\|reset] — stream diagnostics |
| `memprofile` | — | memprofile <snapshot\|start\|sample\|status\|report\|samples\|stop\|reset> [intervalMs maxSamples] |
| `tci` | — | tci start\|status\|stop — in-process TCI client simulator (JSON form only) |
| `audioCapture` | — | audioCapture <start\|stop\|status\|read\|probeNr2Stereo\|probeDspStereo> [args] |
| `txwaterfall` | — | txwaterfall <on\|off> — show keyed TX in the waterfall |
| `key` | — | key <ptt on\|off \| mox> — semantic keying (TX-gated) |
| `station` | — | station <name> — set the GUI-client station name |
| `resize` | — | resize <w> <h> [target] — resize a window |
| `window` | — | window <maximize\|restore\|minimize\|fullscreen> [target] |
| `shortcut` | — | shortcut <id> — fire a ShortcutManager/MIDI action (TX-gated) |
| `midi` | — | midi cc <0-127> — inject a learned VFO Tune Knob CC event |
| `menu` | — | menu list \| open <name> — menu-bar menus |
| `whoami` | — | bridge instance info: pid, socket, label, station, txAllowed |
| `log` | — | log <categories\|get\|set\|reset\|tail\|subscribe\|unsubscribe> [args] |
| `mark` | — | mark <text> — timestamped annotation in the log ring |
| `qrz` | — | qrz <status\|cached\|lookup\|spottext> [args] |

<!-- END GENERATED VERB TABLE -->
