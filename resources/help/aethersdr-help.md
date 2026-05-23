# AetherSDR Help

## The Main Window at a Glance

If you only remember one mental map of AetherSDR, make it this:

- The title bar is the station-status strip.
- The center is the live radio workspace.
- The right side is the deep control stack.
- The status bar is the quick sanity check row.

Everything else in the program supports those four ideas.

## Title Bar and Top Controls

The title bar combines identity, status, and quick client controls in one narrow strip.

### What you can read there quickly

- Discovery heartbeat
- Application identity
- multiFLEX presence
- Whether another client currently owns transmit
- Whether `PC Audio` is enabled
- Speaker and headphone levels

### What you can do there quickly

- Toggle `PC Audio`
- Mute or unmute line out
- Mute or unmute headphones
- Change local output levels
- Enter `Minimal Mode`
- Open the feature request and AI-assisted reporting flow

This is the best place to look when you want to answer, "Is this a station problem, an audio problem, or simply the wrong client state?"

## Menu Reference

### `File`

- `Quit` closes the application.

### `Settings`

This is the operational configuration menu. It contains most dialogs that affect station behavior, radio setup, control surfaces, and external integrations.

- `Radio Setup...`: the main multi-tab radio configuration dialog.
- `Connect to Radio...`: opens the radio connection dialog.
- `FlexControl...`: jumps directly into the serial and FlexControl setup area when serial support is available.
- `Network...`: opens network diagnostics.
- `Memory...`: opens the memory channel manager.
- `USB Cables...`: opens cable definitions and USB cable behavior.
- `MIDI Mapping...`: opens controller mapping when MIDI support is available.
- `StreamDeck...`: opens Stream Deck integration when HID support is available.
- `SpotHub...`: opens the unified spots and spotting workflow dialog.
- `multiFLEX...`: opens the multi-operator dashboard.
- `TX Band Settings...`: opens band-specific transmit settings such as RF power, tune power, and inhibit or interlock choices.
- Autostart items for rigctld, CAT, TCI, and DAX let you decide which services should come up automatically.
- `Low-Latency DAX (FreeDV)` affects digital voice and low-latency routing behavior.

### `Profiles`

This menu manages operating profiles.

- `Profile Manager...` is the main profile dialog.
- `Import/Export Profiles...` creates and restores SmartSDR-compatible `.ssdr_cfg` radio database packages. Export defaults to Global, TX, and MIC profiles, with optional Memories, Preferences, TNF, XVTR, and USB Cables. Import can replace same-name profiles, including defaults; export a backup first before restoring a package.
- `.ssdr_cfg` packages are firmware-sensitive. Avoid importing packages from newer firmware into older radio firmware. Packages that include Preferences may close or reopen slices, panadapters, and other persisted radio resources.
- Profile database transfer requires a direct LAN connection in this build; SmartLink/WAN import/export is disabled with an explanatory message.
- Below the separator, global profiles are listed dynamically and can be loaded directly.

### `View`

This menu changes how the operator workspace is presented.

- `Applet Panel` shows or hides the right-side panel.
- `Band Plan` controls band-plan overlays and region selection.
- `Single-Click to Tune` changes tuning behavior on the spectrum.
- `UI Scale` changes the overall application scale and requires a restart.
- `Reset Applet Order` restores the default applet arrangement.
- `Minimal Mode` removes visual clutter for a compact operating view.
- `Keyboard Shortcuts` enables or disables shortcut handling.
- `Configure Shortcuts...` opens the shortcut editor.

### `Help`

This menu gives you both offline guidance and troubleshooting tools.

- `Getting Started...`
- `AetherSDR Help...`
- `Understanding Noise Cancellation...`
- `Configuring AetherSDR Controls...`
- `Configuring Data Modes...`
- `Contributing to AetherSDR...`
- `Support...`
- `Slice Troubleshooting...`
- `What's New...`
- `About AetherSDR`

The bundled help guides are intentionally separate windows so you can keep one open while continuing to operate.

## Center Workspace

### Panadapter stack

The center column is a vertical stack of panadapters. AetherSDR supports multiple panadapters, so this area is meant to be watched, not hidden behind dialogs.

Each panadapter contains:

- FFT spectrum
- Waterfall
- Slice markers and VFO overlays
- Tracking notch filters
- Optional CW decode panel when CW decoding is in use

### Spectrum overlay menu

The floating left-side overlay is a fast operator menu for the currently focused panadapter. Its buttons are:

- `+RX`: add a new receive slice on that panadapter
- `+TNF`: add a tracking notch filter
- `Band`: jump by band and open XVTR setup
- `ANT`: receive antenna, RF gain, and WNB controls
- `DSP`: per-slice DSP toggles and levels
- `Display`: FFT, waterfall, color, averaging, and background presentation
- `DAX`: DAX channel and IQ channel choices for that panadapter or slice context

This overlay is important because it keeps the most common "I need to adjust the picture or slice quickly" controls next to the spectrum instead of burying them in a large dialog.

## VFO and Slice Controls

Each slice has a VFO overlay that acts as a compact operating head.

### What the VFO area shows

- Slice letter
- Frequency
- Mode
- Filter width
- RX antenna
- TX antenna
- TX assignment
- split status
- signal level

### What the VFO area lets you do

- Tune directly on the frequency display
- Change antennas
- Lock or unlock tuning
- Close a slice
- Work with AF, SQL, AGC, diversity, ESC, APF, digital offsets, FM options, RIT, XIT, DAX, and mode-specific functions

The exact controls change with mode. For example:

- FM adds offset and simplex or reverse controls.
- DIGU and DIGL expose digital offset controls.
- CW adds APF and CW-specific handling.
- diversity-capable contexts expose ESC controls.

That is why it is better to think of the VFO as a live mode-sensitive operating surface, not just a frequency label.

## The Applet Panel

The applet panel is a persistent right-side control column. It has two always-visible toggle rows, a separate S-meter section, and a reorderable vertical stack of applets below.

Default applet order is:

- `RX`
- `TUN`
- `AMP`
- `TX`
- `PHN`
- `P/CW`
- `EQ`
- `DIGI`
- `MTR`
- `AG`

### `VU`

The meter section is separate from the main stack and is useful for constant visibility. It can show receive and transmit meter selections without forcing the rest of the applet panel to remain expanded.

### `RX`

The RX applet is the slice-centric receive control surface. It repeats the most important slice identity information at the top, then exposes receive controls such as step size, filter, mute, pan, and offset-related functions. Use this when you want more detail than the compact VFO overlay offers.

### `TUN`

This applet appears when tuner hardware or tuner support is relevant. Use it to manage tuning state, watch SWR and power behavior, and confirm that the RF path is behaving as expected before staying on the air.

### `AMP`

This applet is for amplifier integration when available. It is part of the station-status side of the app rather than the slice side, so always confirm whether you are making a station-wide change or a single-slice change.

### `TX`

The TX applet is the main transmit command surface. It includes:

- forward power and SWR gauges
- RF power
- tune power
- TX profile selection
- TUNE
- MOX
- ATU
- MEM
- APD state

Before transmitting, this is the applet that should match your intention.

### `PHN`

The Phone applet is focused on voice-mode shaping and behavior:

- AM carrier level
- VOX
- VOX level
- VOX delay
- DEXP
- TX low-cut and high-cut filters

Use it when you want to shape the transmit voice path without opening the larger setup dialog.

### `P/CW`

This applet is mode-sensitive. In phone-oriented modes it exposes microphone-oriented controls such as mic source, level, processing, monitor, and DAX. In CW modes it switches to keyer-oriented controls such as delay, speed, sidetone, break-in, iambic mode, and pitch.

### `EQ`

The EQ applet is for transmit and receive equalization. Small moves are best. Shape the audio, then listen and measure before making another large adjustment.

### `CAT`, `DAX`, `TCI`, `IQ` (data mode tiles)

The bridge between AetherSDR and external software is split across four
independent, drag-reorderable tiles. Each one is toggled from its own button
in the applet tray:

- **`CAT`** — CAT Control tile: rigctld TCP servers + virtual TTY/PTY ports
  for any application that speaks Hamlib rigctld
- **`DAX`** — DAX Audio tile: virtual audio devices for the four RX channels
  and the TX channel, with per-channel gain and level meters
- **`TCI`** — TCI Server tile: WebSocket server speaking the TCI v2.0
  protocol (CAT + audio + IQ + CW + spots in one connection)
- **`IQ`** — DAX IQ tile: raw I/Q streams at 24/48/96/192 kHz for SDR
  applications

If you use computer-driven digital modes, the relevant tiles deserve a
permanent place in your operating layout. See **Configuring Data Modes** in
the help topics for the full setup walkthrough.

### `MTR`

The meter applet provides additional visibility into operating state when the dedicated VU area is not enough.

### `AG`

The Antenna Genius applet appears when that station accessory is present. Use it to confirm band and port routing at a glance.

### `MQTT`

The MQTT applet connects AetherSDR to your station automation system via MQTT,
the lightweight messaging protocol used by Node-RED, Home Assistant, and many
ham shack automation tools.

#### Quick Start

1. Click the **MQTT** button in the applet toggle row to open the applet.
2. Enter your MQTT broker's **Host** and **Port** (default: `localhost:1883`).
3. Enter comma-separated **Topics** to subscribe to.
   Prefix a topic with `*` to display its value on the panadapter overlay.
   Example: `*rotator/pos, *ant/selected, station/log`
4. Click **On** to connect.

#### Publish Buttons

You can create custom buttons that publish MQTT messages when clicked:

1. Click **Edit** in the Publish section.
2. Click the **+** button to add a new button.
3. Enter a **Label** (what the button shows), **Topic** (where to publish),
   and **Payload** (the message body).
4. Click **Done** to exit edit mode.
5. Click any button to send its message to the broker.

Right-click a button in edit mode to remove it. Buttons are saved across restarts.

#### Panadapter Overlay

Topics prefixed with `*` display their last received value in the top-right
corner of the spectrum, below the propagation and WNB indicators. This lets
you see beam heading, selected antenna, or other station status at a glance
without leaving the panadapter view.

#### Antenna Display Names

AetherSDR can update its local antenna display names from MQTT. Subscribe to
the topics you want in the MQTT applet while a radio is connected. Retained
broker messages work well when they arrive after AetherSDR has identified the
connected radio.

- Per-port topic: `aethersdr/antenna/name/ANT1` with payload `80m Dipole`
- Bulk topic: `aethersdr/antenna/names` with payload
  `{"ANT1":"80m Dipole","ANT2":"Hexbeam","RX_A":"Beverage NE"}`
- Empty per-port payload clears that port's local name.
- In bulk JSON, an empty string or `null` clears that port's local name.

These are display names only. AetherSDR still sends canonical radio antenna
tokens such as `ANT1`, `ANT2`, `RX_A`, and `XVTA` to the radio.

#### Use Case: Rotator Control via Node-RED

Many operators use Node-RED to control rotators via serial, USB, or IP.
A typical setup:

- Node-RED publishes `rotator/pos` with the current heading (e.g., `240`)
- Node-RED subscribes to `rotator/cmd` for control commands
- In AetherSDR, subscribe to `*rotator/pos` (displays heading on panadapter)
- Create publish buttons: **CW** (topic `rotator/cmd`, payload `CW`),
  **CCW** (payload `CCW`), **Stop** (payload `STOP`)

Now you can see your beam heading on the spectrum and send rotator commands
without switching windows.

#### Use Case: Antenna Switching

If you use an MQTT-connected antenna switch (via Node-RED, ESP32, etc.):

- Subscribe to `*ant/selected` to see the active antenna on the panadapter
- Subscribe to `aethersdr/antenna/name/+` or `aethersdr/antenna/names` if
  your automation publishes the current local antenna display names
- Create buttons for each antenna: **Hexbeam** (topic `ant/select`, payload `1`),
  **Vertical** (payload `2`), **Wire** (payload `3`)

#### Use Case: SteppIR Controller

For SteppIR antennas controlled via MQTT:

- Subscribe to `*steppir/band` and `*steppir/direction`
- Create buttons: **Normal** (topic `steppir/cmd`, payload `normal`),
  **180°** (payload `reverse`), **Bi-Dir** (payload `bidir`)

#### Notes

- MQTT uses QoS 0 (fire-and-forget) for both subscribe and publish.
- The broker connection auto-reconnects with exponential backoff (5s–60s).
- Username and password are optional; leave blank for unauthenticated brokers.
- Enable the `TLS` checkbox to encrypt the connection. The port will switch automatically to 8883. Leave `CA cert` blank to use the system certificate bundle, or enter a path to a custom CA file for self-signed broker certificates.
- Up to 12 publish buttons can be configured (4 rows × 3 columns).

## Status Bar

The status bar is easy to underestimate. It carries both fast actions and live telemetry.

### Left side

- Add panadapter
- Applet panel toggle
- TNF
- CWX
- DVK
- FDX
- radio and station context

### Center

- station label or station name

### Right side

- GPS state
- PA temperature
- supply voltage
- network quality
- TGXL or PGXL accessory state
- transmit indicator
- grid
- date and time

If you are operating quickly, this row helps prevent "silent failures" where the radio is connected but not in the state you assume.

## Connection and Station Management Windows

### Connection panel

The `Connect to Radio` dialog is the gateway to the radio. It includes:

- `On This Network` for beginner-friendly LAN discovery
- `Remote with SmartLink` for internet-connected stations
- `Connect by IP` for VPN or routed access when you already know the radio IP
- contextual `Low Bandwidth` mode for SmartLink or VPN links
- diagnostics and recovery actions when discovery does not find a radio

The dialog is meant to keep the novice path obvious while still exposing manual routing controls only when a station actually needs them.

### multiFLEX dashboard

The multiFLEX dialog shows connected stations and highlights local PTT control. Use it whenever more than one client may be affecting the shared radio environment.

### Memory dialog

The Memory dialog is an editable table for storing and recalling operating setups. It includes columns for frequency, mode, offsets, tones, filters, and digital details. It is useful for repeaters, nets, digital working frequencies, and recurring field-operation setups.

### Profile manager

The Profile Manager is where operating profiles become reusable station setups. Use it when you want repeatable combinations of settings instead of rebuilding a session manually.

### SpotHub

SpotHub brings spot sources together in one place so you can compare cluster information with the live panadapter. It is not just a list window; it is part of the tune-and-find workflow.

## `Radio Setup...` Tab Guide

`Settings -> Radio Setup...` is the main radio-configuration dialog. It currently contains these tabs:

- `Radio`: radio information, identification, firmware update, remote-on, multiFLEX, and station identity
- `Network`: network parameters, advanced options, and IP configuration
- `GPS`: GPS-related status and configuration
- `Audio`: radio outputs, SmartLink audio compression, local PC audio devices, and optional NVIDIA BNR
- `TX`: timings, interlocks, max power, tune behavior, and TX-follow rules
- `Phone/CW`: microphone, CW, and digital-specific setup
- `RX`: frequency offset, 10 MHz reference, and receive-related global settings
- `Filters`: filter behavior and low-latency digital choices
- `XVTR`: transverter definitions and management
- `USB Cables`: CAT, BCD, bit, and passthrough cable definitions
- `Serial`: serial port behavior, pin assignments, and FlexControl tuning knob setup when serial support is built in

This dialog affects radio-wide behavior more often than slice-local behavior, so change settings carefully and intentionally.

## Operating Advice by Area

### Before transmitting

Check these three places in order:

1. Slice or VFO `TX` assignment
2. TX applet power and antenna context
3. status bar and title bar warnings

### When receive audio sounds wrong

Work outward from the slice:

1. confirm the active slice and mode
2. confirm AF, mute, and AGC behavior
3. check `PC Audio`
4. check local output device selection in `Radio Setup -> Audio`
5. if using Opus compression (SmartLink/WAN), disable client-side spectral
   noise reduction (NR2). NR2's noise estimator cannot distinguish Opus
   codec artifacts from real noise, which causes rasping distortion.
   Use RNNoise (RN2), NR4 (specbleach), MNR, BNR, or DFNR instead —
   these neural/spectral filters handle lossy audio gracefully.

### When the display feels cluttered

Use:

- `View -> Minimal Mode`
- `View -> Applet Panel`
- reordered applets
- fewer panadapters
- the spectrum overlay instead of opening larger dialogs

### When tuning becomes confusing

Focus on:

- which slice is active
- which slice owns transmit
- whether single-click tuning is enabled
- whether you are acting on the VFO, the RX applet, or the spectrum overlay

## Keyboard and External Controls

Keyboard shortcuts exist for tuning, mode changes, TX actions, filter control, display work, and more. Enable them from `View -> Keyboard Shortcuts`, then use `Configure Shortcuts...` to tailor the bindings.

External control surfaces are also supported:

- FlexControl-style tuning knobs
- MIDI controllers
- Stream Deck layouts
- serial PTT or CW devices

These devices are powerful once they are set up, but the simplest approach is still best: map one function, test it immediately, then add the next one.
