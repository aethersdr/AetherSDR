# Getting Started

## Welcome to MasterSDR

MasterSDR is a native SmartSDR-compatible desktop client for FlexRadio transceivers. It is built to let you discover a radio, create slices and panadapters, manage receive and transmit paths, and run daily operating tasks without relying on a browser or an always-online station computer.

If this is your first day with software defined radio, do not try to learn every control at once. Your first goal is much simpler:

1. Connect to the radio.
2. Identify the active slice and transmit path.
3. Learn what each major area of the main window is responsible for.
4. Make small, deliberate changes while watching the display and meters.

## Before You Start

Make sure:

1. The radio is powered on.
2. Your computer and radio are on the same LAN for local operation, or SmartLink is already configured for remote use.
3. Speakers, headphones, or your normal station audio path are ready.
4. You know whether you want local LAN operation, SmartLink remote operation, or multiFLEX shared operation.

## Connect to a Radio

### Local LAN connection

1. Start MasterSDR.
2. Wait for the `Connect to Radio` dialog to show radios on the `On This Network` page.
3. If the dialog is not visible, open `Settings -> Connect to Radio...`.
4. Select the radio you want.
5. Click `Connect`.

If nothing appears in the discovered list:

1. Confirm the radio and computer are on the same network segment.
2. Check that guest Wi-Fi isolation, VPN software, or firewall rules are not blocking discovery.
3. Try `Connect by IP` if you know the radio's address or are reaching it through a VPN.
4. Open `Settings -> Network...` for diagnostics if discovery still fails.

### SmartLink remote connection

1. Open `Settings -> Connect to Radio...`.
2. Switch to `Remote with SmartLink`.
3. Sign in to SmartLink.
4. Select the remote radio you want to use.
5. Turn on `Low Bandwidth` first if you are on a slower internet path.
6. Click `Connect`.
7. Give the audio and panadapter streams a few extra seconds to settle, especially on slower links.

Remote operation depends more heavily on Internet quality, latency, and firewall setup than LAN operation. If the display or audio feels rough, simplify the session first by using fewer panadapters and avoiding unnecessary visual load.

## Learn the Main Window

The MasterSDR main window has four areas that matter immediately.

### Title bar

The title bar is more than a cosmetic header. It holds:

- The menu bar
- The heartbeat indicator for discovery activity
- The `multiFLEX` indicator when another station is involved
- The other-client transmit warning
- The `PC Audio` toggle
- Master speaker volume
- Headphone volume
- The `Minimal Mode` button
- The lightbulb button for feature requests and AI-assisted reporting

This area answers an important question quickly: "Am I hearing the radio locally, and who owns transmit right now?"

### Center workspace

The center of the window is where operating actually happens:

- The `PanadapterStack` shows one or more panadapters vertically.
- Each panadapter contains a spectrum and waterfall display.
- Each active slice has a VFO overlay with frequency, mode, filter width, and quick controls.
- The left-side spectrum overlay opens fast controls for `Band`, `ANT`, `DSP`, `Display`, and `DAX`.

If you are watching signals, tuning, or comparing activity across a band, this is your primary work area.

### Right-side applet panel

The right side is the applet panel. Think of applets as focused control modules rather than separate operating modes. The panel can be shown or hidden from `View -> Applet Panel`, and the applets can be reordered to match your workflow.

Common applets include:

- `VU`: analog-style receive and transmit metering
- `RX`: slice-focused receive controls
- `TUN`: tuner controls when supported hardware is present
- `AMP`: amplifier controls when supported hardware is present
- `TX`: transmit power, profiles, ATU, TUNE, and MOX
- `PHN`: voice-specific transmit shaping
- `P/CW`: microphone controls in phone modes and keyer controls in CW modes
- `EQ`: transmit and receive equalization
- `DIGI`: CAT, DAX, TTY, and TCI controls for software integration
- `MTR`: additional metering
- `AG`: Antenna Genius controls when present

### Status bar

The bottom status bar is the fast "station sanity check" row. It includes quick actions and live status such as:

- Add panadapter
- Show or hide the applet panel
- TNF, CWX, DVK, and FDX controls
- The station label in the center
- GPS, temperature, voltage, network quality, tuner or amplifier status, transmit state, grid, date, and time on the right

When something feels wrong, the status bar is often the quickest place to confirm whether the issue is connection health, accessory state, or transmit ownership.

## How the Window Works During Real Operation

The layout makes more sense when you read it as a workflow:

1. The Connection panel gets you attached to a radio.
2. The center workspace lets you see signals and choose where to operate.
3. The VFO and slice overlays tell you exactly which slice is active and whether it is the transmit slice.
4. The applet panel exposes deeper controls without covering the panadapter.
5. The title bar and status bar keep the global station state visible while you work.

That is why the help guides open in separate non-modal windows: you can keep them beside the radio view while continuing to operate.

## First Safe Operating Session

Use this checklist for the first session on a new station:

1. Connect to the radio.
2. Verify that `PC Audio` is in the state you expect.
3. Turn speaker and headphone levels down before enabling monitor audio.
4. Identify the active slice letter and current mode.
5. Confirm whether the `TX` badge is on the slice you intend to transmit on.
6. Check the transmit antenna, RF power, tune power, and tuner state before keying.
7. Make one change at a time and watch the panadapter, meters, and status bar for the result.

## Global Controls vs Slice Controls

One of the biggest early learning curves in Flex-style software is understanding which settings affect everything and which settings only affect one slice.

### App-wide and station-wide controls

These usually change the client, the station workflow, or the presentation of the UI:

- `View -> UI Scale`
- `View -> Band Plan`
- `View -> Applet Panel`
- `View -> Minimal Mode`
- Keyboard shortcut enablement and shortcut configuration
- `PC Audio` and local output device choices
- SmartLink login state
- Station name
- Autostart settings for rigctld, CAT, TCI, and DAX

### Radio-wide controls

These affect the connected radio or the whole client session more broadly:

- `Settings -> Radio Setup...`
- `Settings -> TX Band Settings...`
- `Settings -> USB Cables...`
- multiFLEX enablement
- network and remote-operation behavior

### Slice-specific controls

These belong to one receive or transmit slice:

- Frequency
- Mode
- Filter width and passband
- RX and TX antenna selection
- AGC and many DSP choices
- RIT and XIT
- DAX channel assignment
- diversity and related receive features

If a change follows only slice `A`, `B`, `C`, or `D`, it is probably slice-specific. If the whole client or radio behaves differently, it is probably global or radio-wide.

## A Good Next Step

Once you are comfortable getting connected and identifying the main window areas:

- Open `Help -> MasterSDR Help...` for a deeper window-by-window reference.
- Open `Help -> Configuring Data Modes...` before setting up WSJT-X, Winlink, fldigi, JS8Call, VARA, or IQ software.
- Open `Help -> Contributing to MasterSDR...` if you want to report confusing behavior, request workflow improvements, or help improve the documentation itself.
