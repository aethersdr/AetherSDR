---
title: "Menu Reference"
slug: "/menu-reference"
description: "Every menu item in AetherSDR, where it lives, and what it opens, followed by a tour of the title bar that sits above the panadapters."
---

Every menu item in AetherSDR, where it lives, and what it opens, followed by a tour of the title bar that sits above the panadapters. Menu items that need a connected radio, or a feature your radio or build does not have, are **dimmed** rather than hidden, and hovering one shows the reason.

The top-level menus are **File · Settings · Profiles · Tools · View · Window · Help**.

- **Windows and Linux:** the menus sit behind the **☰** button at the far left of the title bar. Alt+letter mnemonics (Alt+F, Alt+S, …) still open each menu under the button, and every menu shortcut keeps working.
- **macOS:** the menus are in the system menu bar at the top of the screen. **Radio Setup…** appears as **AetherSDR → Preferences…**.

<img src="/img/screens/title-bar.png" width="1600" alt="The AetherSDR title bar. From left: the menu button, the AetherSDR logo, three radio tabs reading Hermes-Lite 2 available, FLX8600 FLEX-8600 connected KK7GWY (highlighted, with a green dot) and Simulator (not on the air) AetherSDR Demo available DEMO, and a plus button. On the right: a green PC Audio button, a speaker icon and slider at 100, a headphone icon and slider at 100, three panel layout buttons, and minimise, maximise and close buttons." />

*The title bar: the menu button, one tab per radio, PC Audio and volume controls, panel layout buttons and window controls.*

## File

| Item | What it does |
|------|-------------|
| **Connect to Radio...** | Opens the connection panel (LAN radios, SmartLink, manual IP). See [First Connection](./first-connection.md) and [Manual Connection](./manual-connection.md). |
| **Disconnect** | Ends the radio session without closing AetherSDR. |
| **Quit** | Closes the application. |

## Settings

| Item | What it does |
|------|-------------|
| **Radio Setup...** | The main multi-page configuration dialog. See [Radio Setup](./radio-setup.md). On macOS: AetherSDR → Preferences…. |
| **AetherControl...** | Opens the AetherControl controller window. |
| **FlexControl Knob & Buttons...** | Jumps to the FlexControl section of Radio Setup → Serial & Controllers. See [FlexControl Tuning Knob](./flexcontrol-tuning-knob.md). |
| **Receive Sync ▸** | Timing alignment between the radio and a KiwiSDR receiver: Sync Kiwi Audio & Display, **Manual Offset** / **Auto Assist**, Delay KiwiSDR 50 ms, Delay Flex 50 ms, Reset Offset, and **Latency ▸** Normal (360 ms) / More Stable (520 ms) / High Jitter (1000 ms). See [KiwiSDR and Web-888](./kiwisdr-and-web-888.md). |
| **MQTT...** | Broker, subscriptions and publish buttons. See [MQTT Station Automation](./mqtt-station-automation.md). |
| **MIDI Mapping...** | See [MIDI Controller Mapping](./midi-controller-mapping.md). |
| **Icom RC-28 Remote Encoder...** | See [USB Control Surfaces](./usb-control-surfaces.md). |
| **Ulanzi Dial Mapping...** | See [Ulanzi Dial](./ulanzi-dial.md). |
| **USB Cables...** | See [USB Cable Management](./usb-cable-management.md). |
| **Keyboard Shortcuts** | Checkable. Turns operating shortcuts on or off; **off by default**. See [Keyboard Shortcuts](./keyboard-shortcuts.md). |
| **Configure Shortcuts...** | The shortcut editor. |
| **SpotHub...** | See [SpotHub](./spothub.md). |
| **multiFLEX...** | The multi-client dashboard. See [Multi-Flex](./multi-flex.md). |
| **TX Band Settings...** | Per-band RF power, tune power and inhibit choices. |
| **Inhibit during TUNE ▸** | Picks which integrations are held while the radio tunes. |
| **AetherRX...** | The receive audio chain. See [Aetherial Audio](./aetherial-audio.md). |
| **Settings Browser...** | Browse and edit stored settings. See [Settings and Backups](./settings-and-backups.md). |
| **Autostart CAT with AetherSDR** / **Autostart TCI…** / **Autostart DAX…** | Start those services automatically. |
| **Reset Settings...** | Restores application settings after confirmation. A backup is written first. |

Items that depend on optional build features (MQTT, MIDI, serial controllers) only appear in builds that include them.

## Profiles

| Item | What it does |
|------|-------------|
| **Profile Manager...** | The main profile dialog. |
| **Import/Export Profiles...** | SmartSDR-compatible `.ssdr_cfg` import and export. |
| *(profile names)* | Below the separator, the radio's global profiles; pick one to load it. |

See [Profile Management](./profile-management.md).

## Tools

<img src="/img/screens/menu-tools.png" width="285" alt="Tools menu: Add Panadapter..., AetherTX..., CW Keyer, Copy Assist, AetherModem..., Configure KiwiSDR...; Start SWR Scan..., Pre-tune ATU Bands..., Clear ATU Memories..., Calibrate AGC-T...; Callsign Lookup... (Ctrl+Shift+L), PSK Reporter..., FreeDV Reporter...; Net Scheduler..., Memory..., Waveforms..., Wideband Bandscope...; Radio Health..., GPS Dashboard..., Network Diagnostics... and Runtime Monitor...." />

*The Tools menu, connected to a FLEX-8600.*

| Item | What it does |
|------|-------------|
| **Add Panadapter...** | Opens the layout picker and adds a pan if the radio has capacity. |
| **AetherTX...** | Checkable. Shows the transmit audio chain. See [Aetherial Audio](./aetherial-audio.md). |
| **CW Keyer** | Checkable. Shows the CW keyer panel. See [CWX Panel](./cwx-panel.md). |
| **Copy Assist** | Checkable. Speech-to-text panel (builds with speech-to-text only). See [Copy Assist](./copy-assist.md). |
| **AetherModem...** | Packet and HF modem workspace. See [AetherModem Packet Radio](./aethermodem-packet-radio.md). |
| **Configure KiwiSDR...** | Opens Radio Setup → Antennas. |
| **Start SWR Scan...** | **Transmits.** Runs an SWR sweep. See [AetherSweep](./aethersweep.md). |
| **Pre-tune ATU Bands...** | **Transmits.** Steps the ATU through your bands. |
| **Clear ATU Memories...** | Clears the radio's ATU memories after confirmation. |
| **Calibrate AGC-T...** | Noise-floor AGC-T calibration for the active slice. It listens only; it does not transmit. The same calibration is on the AGC-T slider's right-click menu. |
| **Callsign Lookup...** | QRZ lookup (Ctrl+Shift+L). See [Callsign Lookup](./callsign-lookup.md). |
| **PSK Reporter...** | See [PSK Reporter Map](./psk-reporter-map.md). |
| **FreeDV Reporter...** | FreeDV Reporter station list. |
| **Net Scheduler...** | See [Net Scheduler](./net-scheduler.md). |
| **Memory...** | See [Memory Channels](./memory-channels.md). |
| **Waveforms...** | Install and manage radio waveforms (radios with installable waveforms only). |
| **Wideband Bandscope...** | The radio's converter view before tuning; dimmed on radios without one. See [Hermes-Lite 2](./hermes-lite-2.md). |
| **Radio Health...** | Reachability, other clients holding the radio, PA temperature — works even with no session. |
| **GPS Dashboard...** | See [AetherClock and GPS](./aetherclock-and-gps.md). |
| **Network Diagnostics...** | Link quality, trends and logs. See [Troubleshooting](./troubleshooting.md). |
| **Runtime Monitor...** | CPU, memory, threads and logs for the app itself. See [Runtime Monitor](./runtime-monitor.md). |

## View

| Item | What it does |
|------|-------------|
| **Workspace Canvas ▸** | The experimental canvas shell. See [Workspace Canvas](./workspace-canvas.md). |
| **Band Plan ▸** | Size (Off / Small / Medium / Large / Huge), **Show Spots**, and the region plan. See [Panadapter Controls](./panadapter-controls.md). |
| **Theme ▸** | Pick a theme. See [Themes and Theme Editor](./themes-and-theme-editor.md). |
| **Theme Editor…** | Edit or create a theme. |
| **VFO Marker Size ▸** | Default VFO marker: Off, 1 px or 3 px (default 3 px). See [VFO Widget](./vfo-widget.md). |
| **VFO Filter Edge ▸** | Default filter-edge lines: Show (default) or Hide. |
| **Single-Click to Tune** | Tune with one click instead of a double-click. Off by default. |
| **Pan Follows VFO** | Keep the active slice in view while tuning. On by default. |
| **UI Scale ▸** | 75 % to 200 %, plus Zoom In (Ctrl+=), Zoom Out (Ctrl+-) and Reset (100 %) (Ctrl+0). Takes effect after a restart. |
| **Reset Applet Order** | Restores the default applet order. |
| **Minimal Mode** | Ctrl+Shift+M. Collapses the window to a narrow applet strip. See [Keyboard Shortcuts](./keyboard-shortcuts.md). |
| **Frameless Window** | Ctrl+Shift+F. On by default. Turn it off to return the main window to the operating system's own title bar and borders. |
| **Propagation Conditions** | Shows the propagation forecast on the panadapters. Off by default. |
| **Smart Spot Filtering** | Dims SSB spots with no detected voice signal nearby. Off by default; needs SpotHub Signal History. |
| **FPS Meters** | Ctrl+F. Shows spectrum and waterfall frame rates on each pan. Off by default. |
| **Blink Status Indicator** | Pulses the radio-link dot on the radio tab. On by default. |

## Window

Lists every open AetherSDR top-level window (pick one to bring it forward), plus:

| Item | What it does |
|------|-------------|
| **Minimize** | Cmd+M on macOS; no default key elsewhere. |
| **Zoom** (macOS) / **Maximize** or **Restore** (Windows, Linux) | Toggles the window's maximized state. |
| **Enter Full Screen** / **Exit Full Screen** | F11 on Windows/Linux, Cmd+Ctrl+F on macOS. |
| **Bring All to Front** | Raises every AetherSDR window. |

The Minimize and Full Screen keys can be rebound in **Settings → Configure Shortcuts...**, and they work even with Keyboard Shortcuts turned off.

> **Upgrading from an older version:** Minimal Mode is **Ctrl+Shift+M on every platform** (it was Ctrl+M), so Cmd+M can be Minimize on macOS.

## Help

<img src="/img/screens/menu-help.png" width="281" alt="Help menu: Getting Started..., AetherSDR Help..., What's New...; Understanding Noise Cancellation..., Configuring AetherSDR Controls..., Configuring Data Modes...; AetherSDR Website, Donate to AetherSDR, Submit your Idea... (with a light-bulb icon), File an Issue..., Contributing to AetherSDR...; Support &amp; Diagnostics..., Slice Troubleshooting..., Check for Updates...; and About AetherSDR." />

*The Help menu.*

| Item | What it does |
|------|-------------|
| **AetherSDR Documentation** | Opens this documentation site in your browser. |
| **Printable Manual (PDF)** | Opens the [printable manual](pathname:///AetherSDR-Manual.pdf), all of these pages in one PDF. |
| **Log Analyzer** | Opens the [Log Analyzer](/log-analyzer), which checks a log or support bundle for known problems in your browser. |
| **Getting Started...**, **AetherSDR Help...**, **What's New...** | Built-in guides, each in its own window. They work offline, and each guide links its page on this site. |
| **Understanding Noise Cancellation...**, **Configuring AetherSDR Controls...**, **Configuring Data Modes...** | Topic guides, also offline. |
| **AetherSDR Website**, **Donate to AetherSDR** | Open the web pages. |
| **Submit your Idea... 💡** | The AI-assisted feature-request helper. |
| **File an Issue...** | Opens a GitHub issue pre-filled with a redacted log tail. |
| **Contributing to AetherSDR...** | See [Contributing Guide](./contributing-guide.md). |
| **Support & Diagnostics...** | Logging categories and support bundles. See [Support and Logging](./support-and-logging.md). |
| **Slice Troubleshooting...** | A diagnostics window for slice, audio and display state. |
| **Check for Updates...** | Checks for a newer release now. |
| **About AetherSDR** | The About window (below). |

## The Title Bar

The main window has one 52 px title bar. From left to right:

| Part | What it is |
|------|-----------|
| **☰** | The application menu (Windows and Linux only). |
| **AetherSDR** brand mark | Drag the bar here to move the window. |
| **Radio tabs** | One tab per known radio (LAN, SmartLink or routed). See below. |
| **EXPERIMENTAL** badge | Shown when the connected radio family's support is experimental. |
| **multiFLEX** | Appears when other clients share the radio; hover lists them, click opens the dashboard. |
| **TX** *station* | Red badge when another client is transmitting. |
| **Transmit timer** | Elapsed key-down time (below). |
| **PC Audio** | Routes receive audio and PC-mic transmit through this computer. Hover shows the input and output devices. |
| **Speaker + master slider** | Mute and level for the audio path you are hearing (below). |
| **Headphone + slider** | Headphone mute and level. Dimmed, with the reason, on radios that have no headphone output. |
| **Dock left / Dock right / Pop out** | Move the applet panel to the left or right of the panadapters (click the active side again to hide it), or float it in its own window. **Ctrl+Shift+S** also pops it out. |
| **Window controls** | Minimize, maximize and close (see "Per platform"). |

### Radio tabs

<img src="/img/screens/radio-list-popup.png" width="380" alt="Discovered radios popup with a Search name, address, or status field. Three rows, each with an Actions button: FLX8600, connected; Hermes-Lite 2, available; and Simulator (not on the air), 127.0.0.1, available, with the first two addresses blacked out. Connect manually... and Rescan radios sit at the bottom." />

*The radio list, opened from the plus button beside the radio tabs. Addresses are blacked out.*

- Line one is the radio's name; line two is its state in words (for example "link lost"), so colour is never the only signal.
- The dot on the active tab is the **radio-link indicator**. It pulses once per discovery heartbeat, turns amber while discovering and red after three missed heartbeats (blinking, or solid if **View → Blink Status Indicator** is off). The alarm stays on the radio that dropped until you disconnect on purpose, start a new session or remove the tab. A deliberate disconnect never raises it. Right-click the tabs to toggle blinking.
- When the tabs no longer fit, scroll them with the mouse wheel or drag them sideways.
- **"+"** opens the radio list: search by name, model, address or status; a per-radio **Actions** menu (Disconnect, Rename…, Radio setup…, Remove from tabs / Add to tabs); **Connect manually…**; **Rescan radios**. Removing a tab only hides it; it does not forget the radio or its credentials. Selecting a radio opens the connection panel on it; nothing connects without your confirmation.

### Transmit timer

A green outlined `M:SS` readout (`H:MM:SS` past an hour) just left of **PC Audio**. It appears when you key up, starts from 0:00 for each transmission, holds the final time for 15 seconds after you unkey, then fades out. It counts operator transmissions only (MOX, PTT, footswitch, VOX, CW, tune) and never TCI or DAX transmissions from other programs.

### Speaker and master slider

The speaker icon and master slider show the path you are actually hearing:

| PC Audio | Speaker icon shows | Master slider shows |
|----------|-------------------|---------------------|
| On | PC mute | PC playback level |
| Off | The radio's Line Out mute | The radio's Line Out level |

With PC Audio off, a Line Out change made by another client (or in Radio Setup) shows up here too. See [Audio Settings](./audio-settings.md).

### Per platform

| Platform | Window frame and controls |
|----------|---------------------------|
| macOS | Native window with the standard traffic-light buttons, tiling menu, Stage Manager and native full screen. |
| Windows | Qt-drawn frame with the system shadow and resize borders; AetherSDR draws its own minimize / maximize / close buttons. Snap Layouts on hover over maximize does not appear yet. |
| Linux | Frameless window with a 6 px resize band on the sides and bottom (no resize along the top edge under the bar). |

If your window manager or compositor misbehaves, turn off **View → Frameless Window** to go back to the system title bar on any platform.

In **Minimal Mode** the bar shrinks to the active radio tab and status badges; the ☰ button, audio controls and dock icons are hidden, and the maximize button leaves Minimal Mode.

## About AetherSDR

**Help → About AetherSDR** opens a rounded, borderless window showing:

<img src="/img/screens/about-window.png" width="400" alt="About AetherSDR window. Below the AetherSDR logo it shows the version v26.10.1 and commit, the tagline Cross-platform SmartSDR-compatible client for FlexRadio transceivers, and a box listing Built with Qt 6.12.0 and C++20, the compile date and the GPU QRhi (OpenGL) renderer. A Contributor Logbook button, the copyright and GPLv3 licence, the GitHub link, trademark notices and an OK button follow." />

*The About AetherSDR window: version, build and renderer details.*

- the version and commit;
- a **build details** card (Qt version, compile date, and the spectrum renderer in use). The text is selectable, so you can paste it into a bug report;
- a **Contributor Logbook ↗** button that opens [contributors.aethersdr.com](https://contributors.aethersdr.com/#all-time).

Drag anywhere in the window to move it. **Esc** or **Ctrl+W** (Cmd+W) closes it, and choosing About again raises the open window. Its animations stop when the operating system asks for reduced motion.

## Known issues

- On Windows 10 with **Frameless Window** on, the **☰** menu and the panadapter's right-click menu flash and close straight away, and click-to-tune does nothing ([#6272](https://github.com/aethersdr/AetherSDR/issues/6272)).
- On Windows 10, a thin white strip shows along the left, right and bottom edges of the main window ([#6266](https://github.com/aethersdr/AetherSDR/issues/6266)).
- **View → Pan Follows VFO** has no visible effect for most tuning; it does not keep the slice centred the way Center Lock does ([#5622](https://github.com/aethersdr/AetherSDR/issues/5622)).

## Troubleshooting

### A menu item is dimmed

The item needs a connected radio, or a feature your radio or build does not have.

1. Hover over the item to read the reason.
2. If it says no radio is connected, connect with **File → Connect to Radio...** and open the menu again.

### The ☰ menu or the window frame misbehaves

Your window manager or compositor does not get on with AetherSDR's own frame.

1. Turn off **View → Frameless Window** (Ctrl+Shift+F). The main window goes back to the operating system's title bar and borders.
2. If the ☰ menu itself won't stay open, press **Ctrl+Shift+F** instead of using the menu.

### The window is a narrow strip with no panadapters

Minimal Mode is on.

1. Press **Ctrl+Shift+M**, or click the title bar's maximize button, to leave Minimal Mode.

## See also

- [Keyboard Shortcuts](./keyboard-shortcuts.md)
- [Radio Setup](./radio-setup.md)
- [Panadapter Controls](./panadapter-controls.md)
- [Workspace Canvas](./workspace-canvas.md)
- [Support and Logging](./support-and-logging.md)
