# Configuring MasterSDR Controls

MasterSDR can be operated in several different ways. You can click and drag with a mouse, tune with a trackpad, use the built-in keyboard shortcuts, and add external control hardware such as FlexControl, MIDI controllers, USB tuning knobs, Stream Deck buttons, and serial PTT or CW accessories.

The good news is that you do **not** need to learn everything at once. Most operators start with the mouse and a few shortcuts, then add one external controller later when they know what they want to do more quickly.

This guide explains the control options MasterSDR supports today, how the built-in shortcuts work, how to customize them, how to use the panadapter and VFO with a trackpad, and what to expect from each supported USB knob or control surface.

## A few ideas that make every control method easier

Before you add any controller, it helps to know four basic ideas:

- **The active slice is the one that moves.** If a knob or shortcut changes the wrong slice, click the slice you want first.
- **The current step size matters.** A one-step tune command moves by the step size shown in the receiver controls.
- **Tune Lock stops frequency changes.** If tuning seems dead, check whether the active slice is locked.
- **Most controls need a live radio connection.** If MasterSDR is not connected, many control actions will not do anything yet.

## The control systems MasterSDR supports

MasterSDR currently supports these control paths:

- **Mouse and trackpad control** for the panadapter, waterfall, and on-screen VFO controls.
- **Built-in keyboard shortcuts** for tuning, audio, transmit, and other common actions.
- **Custom keyboard bindings** through the shortcut editor.
- **FlexControl USB tuning knob** through the serial-control path.
- **MIDI controllers** with learn mode, profiles, and relative-encoder support.
- **USB HID encoder devices** including:
  - Icom RC-28
  - Griffin PowerMate
  - Contour ShuttleXpress
  - Contour ShuttlePro v2
- **Stream Deck integration** through external plugins that talk to MasterSDR's TCI server.
- **Serial PTT and CW accessories** such as foot switches, straight keys, paddles, and amplifier keying lines through USB-serial adapters.

Some of these features only appear when the build includes the needed support. If a menu item is missing, your build may not include that feature.

## Keyboard shortcuts

### Important: the shortcut system is off until you enable it

MasterSDR separates **operating shortcuts** from ordinary menu shortcuts.

To turn on operating shortcuts, use:

`View > Keyboard Shortcuts`

This setting is **off by default**. If the shortcuts below do nothing, this is the first place to check.

### Built-in default operating shortcuts

These are the default shortcut bindings that ship with MasterSDR:

- `Right Arrow` — tune up 1 step
- `Left Arrow` — tune down 1 step
- `Shift + Right Arrow` — tune up 10 steps
- `Shift + Left Arrow` — tune down 10 steps
- `G` — go to frequency entry on the active slice
- `T` — toggle MOX
- `Space` — hold PTT while the key is held down
- `Up Arrow` — AF gain up
- `Down Arrow` — AF gain down
- `M` — mute toggle
- `]` — step size up
- `[` — step size down
- `L` — tune lock toggle

### Other shortcut actions you can assign

The shortcut editor also includes many actions that ship **unassigned** until you bind them yourself. Examples include:

- Band jumps for `160m`, `80m`, `60m`, `40m`, `30m`, `20m`, `17m`, `15m`, `12m`, `10m`, `6m`, and `2m`
- Mode changes such as `USB`, `LSB`, `CW`, `CWL`, `AM`, `SAM`, `FM`, `NFM`, `DFM`, `DIGU`, `DIGL`, and `RTTY`
- `TUNE` toggle
- `Two-Tone Tune`
- Next or previous slice
- Split toggle
- Filter widen and narrow
- DSP actions such as `NB`, `NR`, and `ANF`
- AGC mode cycle
- Band zoom and segment zoom
- `RIT` and `XIT` toggles

### Other built-in application shortcuts

MasterSDR also has a few ordinary menu shortcuts that are separate from the operating-shortcut system:

- `Ctrl+M` — toggle Minimal Mode
- `Ctrl+=` — increase UI scale
- `Ctrl+-` — decrease UI scale
- `Ctrl+0` — reset UI scale to 100%

The exact key labels shown in the menus may vary slightly by platform.

### Helpful shortcut behavior

A few shortcut details are easy to miss:

- `Space` only acts as press-to-talk when **Keyboard Shortcuts** is enabled.
- While you are typing in a text field, MasterSDR backs off so your typing does not accidentally trigger radio actions.
- If the radio is disconnected, transmit and tuning shortcuts do not perform radio actions.
- If the active slice is locked, tune shortcuts will not move it.

## Customizing keyboard shortcuts

To customize shortcuts, open:

`View > Configure Shortcuts...`

The shortcut editor is designed to be beginner friendly:

1. Click a key on the on-screen keyboard map.
2. Pick the action you want for that key.
3. If the key is already in use, MasterSDR will ask before reassigning it.
4. Use **Clear** to remove the selected key's action.
5. Use **Reset to Default** to put one shortcut back.
6. Use **Reset All to Defaults** if you want to start over.

### Good first customizations

If you are new to keyboard control, these are good first shortcuts to add:

- A favorite band jump such as `20m` or `40m`
- `Next Slice` and `Previous Slice`
- `TUNE` toggle
- `RIT` toggle
- `NB` or `NR` toggle

### Current editor limitation

The current shortcut editor is centered on selecting keys from the on-screen keyboard map, so it is easiest to think of it as a **one-key-at-a-time** editor. The built-in shifted arrow shortcuts still work, but the editing experience is currently most natural for simple single-key bindings.

## Using a mouse or trackpad on the panadapter

You can do a great deal of operating without any external hardware at all.

### Panadapter and waterfall actions

On the main spectrum display:

- **Drag in the spectrum or waterfall** to pan left or right.
- **Double-click in the spectrum or waterfall** to tune to that frequency.
- **Single-click to tune** is available if you turn on `View > Single-Click to Tune`.
- **Drag the frequency scale bar** to change the displayed bandwidth.
- **Drag the divider between the spectrum and waterfall** to change how much height each one uses.
- **Scroll vertically** over the spectrum or waterfall to tune the active slice by the current step size.
- **Drag the dBm scale on the right edge** to pan the reference level up or down.
- **Ctrl-drag the dBm scale** (⌘-drag on macOS) to zoom the visible dB span; the bottom of the scale stays anchored.
- **Click the ▲ / ▼ arrows at the top of the dBm scale** to step the reference level by 10 dB.

### Trackpad notes

Trackpads usually work very well in MasterSDR:

- Vertical scrolling tunes the active slice.
- Horizontal-dominant swipe gestures are ignored for tuning so accidental side-swipes do not move the radio.
- On systems that provide a native zoom gesture, **pinch to zoom** changes panadapter bandwidth.

In practice, pinch-to-zoom is most commonly reliable on macOS trackpads. On Windows and Linux, it depends on whether the operating system, desktop environment, and Qt input stack deliver a native zoom gesture to the application.

### Off-screen slice indicators

If a slice is currently outside the visible spectrum window:

- **Single-click** its edge indicator to make that slice active.
- **Double-click** its edge indicator to recenter the panadapter on that slice.

## Using the built-in VFO and native controls

The on-screen VFO controls are also part of the control system.

### Frequency entry and wheel tuning

On the VFO display:

- **Scroll over the frequency display** to tune by the current step size.
- **Double-click the frequency display** to open direct frequency entry.
- Press `G` to open direct frequency entry from the keyboard.
- **Right-click the frequency display** to add a spot at the current slice frequency.

### Expanded and collapsed VFO behavior

MasterSDR has slightly different behavior depending on whether the VFO is expanded or collapsed:

- In **expanded** mode, scrolling only tunes when the pointer is over the frequency display. This helps avoid accidental changes.
- In **collapsed** mode, scrolling anywhere over the collapsed VFO tunes by the current step size.
- Clicking the **slice badge** collapses the VFO.
- Clicking the **collapsed VFO** expands it again.
- Clicking the **collapsed TX badge** toggles TX assignment for that slice.

### Audio and quick controls

A few useful native controls are easy to miss:

- Right-click the speaker tab to toggle mute quickly.
- Many tabs in the VFO open mode-specific controls such as AF, SQL, AGC, RIT, XIT, digital offsets, and FM options.
- If tuning does not respond, check whether Tune Lock is enabled for that slice.

## FlexControl USB tuning knob

For many operators, FlexControl is the easiest external tuning knob to start with.

### What MasterSDR uses it for

MasterSDR treats FlexControl as a serial device and uses it for:

- Rotary tuning with acceleration
- Automatic device detection
- Optional inverted tuning direction
- Configurable button actions

### Where to configure it

Open:

`Settings > FlexControl...`

This jumps to the **Serial** tab of the Radio Setup dialog.

### How to set it up

1. Plug in the FlexControl.
2. Open `Settings > FlexControl...`.
3. In the **FlexControl Tuning Knob** section, click **Detect**.
4. If the device is found, MasterSDR stores the port and marks it to open.
5. Turn on **Auto-detect on startup** if you want MasterSDR to find it automatically next time.
6. Turn on **Invert tuning direction** if the knob feels backwards for your operating style.

### Default button actions

The standard setup page exposes **Tap** and **Double** actions for each of the three FlexControl buttons.

The defaults are:

- **Button 1**
  - Tap — `StepUp`
  - Double — `StepDown`
- **Button 2**
  - Tap — `ToggleMox`
  - Double — `ToggleTune`
- **Button 3**
  - Tap — `ToggleMute`
  - Double — `ToggleLock`

### Actions you can choose

Each FlexControl button action can be set to one of these choices:

- `None`
- `StepUp`
- `StepDown`
- `ToggleMox`
- `ToggleTune`
- `ToggleMute`
- `ToggleLock`
- `NextSlice`
- `PrevSlice`
- `ToggleAgc`
- `VolumeUp`
- `VolumeDown`

### Current limitation

The underlying FlexControl protocol supports long-press style events, but the current standard setup page only exposes **Tap** and **Double** choices in the UI.

### Platform notes

- **Windows:** if the FlexControl is not detected, make sure Windows sees it as a serial COM port first. FlexRadio documents a Windows FlexControl driver path for SmartSDR systems, so this is the first thing to check if the knob never appears.
- **Linux and macOS:** the MasterSDR repository does not document a separate vendor driver for FlexControl. The main requirement is that the operating system presents the device as a serial port.

### If FlexControl tunes nothing

If the knob turns but frequency does not move, check these items in order:

- Is MasterSDR connected to a radio?
- Is the correct slice active?
- Is Tune Lock turned on for that slice?
- Did the device detect correctly?
- On Windows, does the FlexControl appear as a COM port in Device Manager?

## USB tuning knobs and HID control devices

MasterSDR also supports several USB HID tuning devices.

### Currently supported HID devices

These devices are explicitly recognized:

- **Icom RC-28**
- **Griffin PowerMate**
- **Contour ShuttleXpress**
- **Contour ShuttlePro v2**

### How MasterSDR uses them today

When one of these devices is recognized:

- Turning the control tunes the active slice.
- Tuning uses the current step size.
- If the active slice is locked, tuning will not move it.
- The device is auto-detected when HID support is present in the build.
- Hot-plugging is watched automatically, so reconnecting the device should be picked up.

### Device-by-device notes

#### Icom RC-28

MasterSDR recognizes:

- The main rotary encoder
- The encoder push button
- One secondary button

#### Griffin PowerMate

MasterSDR recognizes:

- Rotary movement
- The main push button

This makes the PowerMate a simple, compact tune-and-press controller.

#### Contour ShuttleXpress

MasterSDR recognizes:

- The jog wheel
- Five buttons

At the moment, MasterSDR uses the **jog wheel** for tuning. The spring-loaded shuttle ring itself is not currently part of the tuning path.

#### Contour ShuttlePro v2

MasterSDR recognizes:

- The jog wheel
- Fifteen buttons

As with ShuttleXpress, MasterSDR currently uses the **jog wheel** for tuning, not the shuttle ring.

### Current limitation for HID knobs

MasterSDR's HID encoder support is currently more automatic than the FlexControl or MIDI paths. In plain language, that means:

- The supported HID knobs are detected automatically.
- Rotation works when the device is recognized.
- There is **not currently a dedicated polished in-app setup page** just for these HID encoder devices.
- If you want easy, visible, per-control mapping, **MIDI** or **Stream Deck** is usually the easier path today.

## MIDI controllers

MIDI support is the most flexible way to build a custom control surface.

### Where to configure it

Open:

`Settings > MIDI Mapping...`

If this menu item is missing, the build you are using may not include MIDI support.

### What the MIDI mapping dialog does

The MIDI dialog lets you:

- Choose a MIDI port
- Connect and disconnect
- Refresh the port list
- Turn on **Auto-connect on startup**
- Use **Learn** mode to bind a control by moving it
- Save named profiles
- Load saved profiles
- Clear all bindings if you want to start over

### What kinds of parameters you can control

MasterSDR groups MIDI targets into these categories:

- **RX**
- **TX**
- **Phone/CW**
- **EQ**
- **Global**

Examples include:

- AF gain
- Squelch
- AGC threshold
- Audio pan
- Noise blanker and noise reduction toggles
- Tune lock
- RIT and XIT
- RF power and tune power
- MOX and TUNE
- Mic level and monitor level
- VOX
- CW speed
- TX and RX EQ bands
- Master and headphone volume
- Next and previous slice

### The best way to map knobs

For endless rotary encoders, turn on the **Relative** checkbox for that binding. This is especially useful for the `VFO Tune Knob` target.

When you use **Learn** on `VFO Tune Knob` with a MIDI CC control, MasterSDR enables relative mode automatically. Older bindings still work if they were saved before this behavior was added.

For `VFO Tune Knob`, each relative encoder pulse maps to one selected radio step. Use the radio step-size control for coarse or fine tuning.

For slider and knob targets, Learn ignores touch-only NoteOn messages and waits for movement data.

For sliders, leave **Relative** off and use normal absolute mode.

Use **Invert** if the control moves the opposite way from what feels natural.

### CW and PTT from MIDI

MIDI can do more than knobs and sliders. It can also be used for:

- Straight-key style CW gating
- Separate **dit** and **dah** paddle inputs
- Hold-style PTT

This makes MIDI a practical option for custom operating panels, not just audio mixers.

### MIDI profiles

Profiles are useful when you want one controller for different jobs. For example:

- A contest profile
- A casual ragchew profile
- A digital-mode profile
- A CW profile

Save the profile after you finish a layout so you can return to it quickly later.

## Stream Deck integration

MasterSDR's current Stream Deck support is based on **external plugins** that talk to MasterSDR's **TCI WebSocket server**.

That is important because the current setup is **not** a built-in “Stream Deck tab” inside the main MasterSDR window.

### Before you install any Stream Deck plugin

Start with this step inside MasterSDR:

`Settings > Autostart TCI with MasterSDR`

That gives the Stream Deck plugin a control connection to the radio. If you prefer not to autostart it, make sure the TCI server is running before you try to use the plugin.

### macOS and Windows: official Elgato Stream Deck plugin

MasterSDR includes an official plugin for the **Elgato Stream Deck desktop application**.

#### What it requires

To use the official plugin path, the repository currently documents these requirements:

- **Stream Deck software 6.4 or later**
- **macOS 13 or later**, or
- **Windows 10 or later**

#### How to install it

1. In MasterSDR, enable `Settings > Autostart TCI with MasterSDR`.
2. Download `com.mastersdr.radio.streamDeckPlugin` from the latest MasterSDR release.
3. Double-click the plugin file.
4. The Elgato app installs it automatically.
5. Drag MasterSDR actions onto your Stream Deck buttons.

No build step, npm, or command line is required just to use the plugin.

#### What it controls

The official Elgato plugin currently provides **43 actions** across areas such as:

- TX
- Bands
- Frequency step actions
- Modes
- Audio
- DSP
- Slice controls
- DVK record and playback

#### Important note about dials

The current official Elgato plugin is **button-oriented**. Its manifest advertises keypad-style actions, not dial/controller actions. If you want Stream Deck dial support, the Linux StreamController path below is currently the better fit.

### Linux: StreamController plugin

On Linux, MasterSDR includes a separate plugin for **StreamController**, which is a Linux alternative to the Windows/macOS Elgato software.

#### Why Linux uses a different plugin

StreamController does **not** use the same plugin format as the official Elgato Stream Deck software. That is why MasterSDR ships a separate StreamController plugin.

#### What it requires

The included MasterSDR StreamController plugin currently declares:

- **minimum app version 1.0.0**
- plugin version `1.0.0`
- a default TCI connection to `ws://localhost:50001`

#### What it controls

The StreamController plugin exposes **40+ actions** for:

- TX
- Band changes
- Frequency actions
- Mode changes
- Audio
- DSP
- Slice actions
- DVK

#### Dial support on Linux

The current StreamController plugin supports **dial input** for:

- **RF Power**
- **Tune Power**

The rest of the actions are primarily button or key style actions.

### Stream Deck troubleshooting

If a Stream Deck button does nothing, check these items:

- Is MasterSDR connected to a radio?
- Is TCI running?
- Does the plugin expect the same TCI port MasterSDR is using?
- On Linux, are you using the **StreamController** plugin rather than the Elgato-format plugin?
- On macOS or Windows, did the plugin install into the official Elgato app successfully?

## Serial PTT, CW keying, foot switches, and paddles

Not every control device is a knob. MasterSDR also supports practical station-control wiring through a USB-serial adapter.

### Where to configure serial control

Open:

`Settings > FlexControl...`

This opens the **Serial** tab, which includes the general serial-port controls in addition to FlexControl.

### Port configuration

The Serial tab lets you configure:

- Port selection from the detected list
- A `Custom...` entry for manual port paths
- Baud rate
- Data bits
- Parity
- Stop bits
- Auto-open on startup

This is useful when your device does not appear in the automatic list or when you need a specific serial path.

### Pin assignments

MasterSDR can drive and read the serial handshake pins for station control.

#### Output pins

You can assign **DTR** and **RTS** as:

- `PTT`
- `CW Key`
- `CW PTT`
- `None`

This is useful for:

- Foot-switch transmit control
- Amplifier keying
- External sequencers
- Simple CW keying interfaces

#### Input pins

You can assign **CTS** and **DSR** as:

- `PTT Input`
- `CW Key Input`
- `CW Dit Input`
- `CW Dah Input`
- `None`

This is useful for:

- External PTT switches
- Straight keys
- Iambic paddles

There is also a **Paddle Swap** option if your dit and dah wiring feels reversed.

### Platform notes for serial devices

- On **Windows**, serial devices usually appear as `COM` ports such as `COM3` or `COM8`.
- On **Linux**, they often appear as `/dev/ttyUSB*` or `/dev/ttyACM*`.
- On **macOS**, they often appear as `/dev/cu.*` device names.

If the automatic list does not show your device, use the `Custom...` option and enter the path manually.

## Build requirements and feature availability

If you are using an official prebuilt MasterSDR release, you usually do not need to think about build dependencies.

If you are **building MasterSDR from source**, the repository currently calls out these control-related dependencies:

- **Qt SerialPort** support enables:
  - FlexControl
  - serial PTT and CW
  - MIDI controllers
- **hidapi** support enables:
  - USB HID encoder support in the main application
  - other HID-based control paths that are compiled into that build

For ordinary Stream Deck use through the external TCI plugins described earlier, the more important requirement is that **TCI is available and running**.

If these pieces were not present when the program was built, the related menus or controller paths may be missing.

## Quick troubleshooting checklist

When a control does not behave the way you expect, use this order:

### Keyboard problem

- Turn on `View > Keyboard Shortcuts`
- Make sure you are not typing into a text field
- Make sure the radio is connected

### Tuning problem

- Click the slice you want to tune
- Check Tune Lock
- Check the current step size
- Try the mouse wheel on the panadapter to confirm the slice itself can tune

### External knob problem

- Unplug and reconnect the device
- Restart MasterSDR
- Confirm the operating system sees the device
- Confirm your build includes the needed support
- On Windows FlexControl, verify the COM port and driver status first

### Stream Deck problem

- Confirm TCI is running
- Confirm the plugin is installed in the correct host app
- Confirm the TCI port matches
- On Linux, use the StreamController plugin, not the Elgato-format plugin

## A good beginner path

If you are just getting started, this sequence usually feels easiest:

1. Learn mouse tuning and direct frequency entry first.
2. Turn on the built-in keyboard shortcuts and use only a few of them.
3. Add either FlexControl **or** MIDI next, not both at the same time.
4. Add Stream Deck later if you want large labeled buttons for repeatable operating tasks.

That approach keeps the station simple while you learn what each control method is best at.
