# Configuring Data Modes

## Why Digital Operation Feels More Complicated

Voice operation can often be understood as "microphone in, speaker out." Digital operation adds more moving parts:

- a radio-control path (CAT or TCI)
- a receive-audio path (DAX)
- a transmit-audio path (DAX)
- sometimes an IQ path
- sometimes a single integrated protocol such as TCI that carries everything

The goal is not to memorize every acronym. The goal is to keep each path visible
and predictable so you can wire things up once and get back to operating.

---

## The Digital Signal Paths Inside AetherSDR

### Control path

This is how outside software reads frequency, mode, PTT, and other radio state.
AetherSDR offers three options:

| Method | Transport | Protocol | Best for |
|---|---|---|---|
| CAT over TCP | TCP socket | Hamlib rigctld | WSJT-X, fldigi, JS8Call |
| CAT over TTY/PTY | Virtual serial port | Hamlib rigctld | Older apps that require a COM/serial port |
| TCI | WebSocket | TCI v2.0 | WSJT-X 3.0 (TCI mode), apps with native TCI support |

### Receive audio path (DAX RX)

This is how digital software hears the radio. AetherSDR creates virtual audio
devices that appear as microphone inputs to other applications.

### Transmit audio path (DAX TX)

This is how digital software sends audio back into the radio. AetherSDR creates
a virtual audio output device that captures audio from your digital program.

### IQ path (DAX IQ)

For applications that need raw baseband IQ instead of demodulated audio.
AetherSDR supports four DAX IQ channels with selectable sample rates
(24k, 48k, 96k, 192k).

---

## Finding and Using the Data Mode Tiles

The data mode controls are split across four independent tiles in the applet
panel. Each tile can be shown, hidden, floated, docked, and drag-reordered
independently.

### Opening the tiles

1. Connect to your radio.
2. Look at the **right-hand applet panel** (the vertical strip of tiles on the
   right side of the main window).
3. In the **applet tray** (the button rows at the top of the panel), click the
   toggle button for the tile you need:

| Button | Tile | Purpose |
|--------|------|---------|
| **CAT** | CAT Control | rigctld TCP + virtual serial ports |
| **DAX** | DAX Audio | virtual audio channels (RX + TX) |
| **TCI** | TCI Server | TCI WebSocket server |
| **IQ** | DAX IQ | raw IQ streaming channels |

Each button toggles its tile on or off. You can have any combination of tiles
visible at the same time.

> **Tip:** Keep the relevant tiles visible during setup. They answer the
> question "What endpoints is AetherSDR offering to the rest of the station?"

> **Tile controls:** Each tile's title bar has a **float/dock** button that
> pops the tile out into its own window or docks it back into the panel.
> You can also **drag tiles** to reorder them within the applet stack.

### What each tile contains

#### CAT Control tile

- **Enable TCP** — starts four rigctld-compatible TCP servers (channels A-D)
- **Enable TTY** — creates four virtual serial ports (channels A-D)
- **Base port** — the starting TCP port number (default `4532`)
- **Channel rows (A-D)** — each row shows the TCP port number, connection count,
  and the virtual serial port path

#### TCI Server tile

- **Enable** — starts the TCI WebSocket server
- **Port** — the WebSocket port (default `50001`)
- **Status** — shows "(stopped)" or the port and connected client count

#### DAX Audio tile

- **DAX 1-4** — four receive channels, each with a combined gain slider and
  level meter (MeterSlider). When a slice is assigned to a DAX channel, the
  label shows which slice (e.g. "Slice A").
- **TX** — transmit channel with gain slider and level meter. Shows which slice
  owns transmit.
- **Enable** — starts the DAX virtual audio bridge

#### DAX IQ tile

- **IQ 1-4** — four IQ channels, each with a sample rate selector
  (24k/48k/96k/192k), level meter, and On/Off toggle

---

## How CAT Works — TCP and Serial Port Emulation

### CAT over TCP

When you click **Enable TCP** in the CAT Control tile, AetherSDR starts four
independent TCP servers that speak the **Hamlib rigctld protocol**. Any
application that can talk to rigctld can connect directly.

The four servers use consecutive ports starting from the base port:

| Channel | Default port | Controls |
|---|---|---|
| A | 4532 | Slice A |
| B | 4533 | Slice B |
| C | 4534 | Slice C |
| D | 4535 | Slice D |

**How to use it:**

1. In the CAT Control tile, click **Enable TCP**.
2. Note the base port (default `4532`). Change it if another application
   already uses that port.
3. In your digital program, set the rig control type to **rigctld** or
   **Hamlib NET rigctl** and point it at `localhost` on the port for the
   channel that matches your slice.

CAT over TCP works identically on **Linux, macOS, and Windows**.

### CAT over TTY/PTY (virtual serial ports)

When you click **Enable TTY**, AetherSDR creates four virtual serial ports
using Unix pseudo-terminals (PTYs). These look like real serial ports to
other applications.

| Channel | Symlink path |
|---|---|
| A | `<runtime>/aethersdr/cat-A` |
| B | `<runtime>/aethersdr/cat-B` |
| C | `<runtime>/aethersdr/cat-C` |
| D | `<runtime>/aethersdr/cat-D` |

`<runtime>` resolves to a per-user directory so two users on the same
machine don't collide:

- **Linux (systemd):** `$XDG_RUNTIME_DIR` (typically `/run/user/<uid>`)
- **Linux (fallback):** `~/.cache/aethersdr/`
- **macOS:** `~/Library/Caches/AetherSDR/`

The CAT Control tile shows the actual fully-resolved path for each
channel — copy from there if you're not sure which case applies on your
system.

**How to use it:**

1. In the CAT Control tile, click **Enable TTY**.
2. The channel rows will show the actual PTY device paths (and the
   resolved symlink path for the current user).
3. In your digital program, select that symlink path as the serial port.

> **Platform note:** Virtual serial ports via PTY are available on **Linux and
> macOS** only. On Windows, use CAT over TCP instead — most modern digital
> applications support TCP/rigctld natively.

### What commands does CAT support?

AetherSDR's CAT interface implements the Hamlib rigctld command set, including:

- **Frequency:** get/set VFO frequency
- **Mode:** get/set operating mode and passband
- **PTT:** get/set push-to-talk
- **VFO:** VFO selection and split operation
- **CW:** CW keying via the `b` (send Morse) command

Mode mapping between AetherSDR and Hamlib:

| AetherSDR mode | Hamlib mode |
|---|---|
| USB | USB |
| LSB | LSB |
| DIGU | PKTUSB |
| DIGL | PKTLSB |
| CW | CW |
| AM | AM |
| FM | FM |
| RTTY | RTTY |

---

## How DAX Audio Devices Work

DAX (Digital Audio Exchange) creates virtual audio devices on your system so
that digital programs can receive audio from the radio and send audio back.

### How it works by platform

#### Linux (PipeWire / PulseAudio)

AetherSDR uses PulseAudio pipe modules (compatible with both PulseAudio and
PipeWire via `pipewire-pulse`) to create virtual audio devices:

- **RX devices** appear as audio *sources* (microphone inputs):
  `AetherSDR DAX 1` through `AetherSDR DAX 4`
- **TX device** appears as an audio *sink* (speaker output):
  `AetherSDR TX`

Audio format: **24 kHz, mono, 16-bit integer**.

**Requirements:** PipeWire with `pipewire-pulse`, or PulseAudio.

**Finding the devices:**

```bash
# List all audio sources (look for AetherSDR DAX)
pactl list sources short | grep -i aether

# List all audio sinks (look for AetherSDR TX)
pactl list sinks short | grep -i aether
```

In most applications, DAX devices will appear in the audio input/output
dropdown menus once DAX is enabled and the radio is connected.

#### macOS (Core Audio HAL plugin)

AetherSDR uses a Core Audio HAL plugin with shared memory to create virtual
audio devices:

- **RX devices** appear as audio inputs in System Preferences and app settings
- **TX device** appears as an audio output

Audio format: **24 kHz, stereo, 32-bit float** (shared memory ring buffer).

**Finding the devices:**

1. Open **System Settings > Sound** (or **System Preferences > Sound** on older macOS).
2. Look for devices named `AetherSDR DAX 1` through `AetherSDR DAX 4` under Input.
3. Look for `AetherSDR TX` under Output.
4. In your digital program's audio settings, select these devices.

#### Windows

DAX virtual audio devices require **macOS or Linux with PipeWire**. On Windows,
use **TCI** instead — TCI carries both control and audio over a single WebSocket
connection, eliminating the need for virtual audio devices entirely.

### DAX gain staging

The DAX Audio tile provides gain sliders for each DAX channel:

- **RX gain (DAX 1-4):** Controls the level of audio sent from the radio to
  your digital program. Start at 50% and adjust if decodes are poor or the
  program shows clipping.
- **TX gain:** Controls the level of audio sent from your digital program back
  to the radio. Keep this moderate to avoid an overdriven transmit signal.

> **Tip:** Digital software is much less tolerant of overdriven audio than
> voice. If decode quality is poor or your transmitted signal looks dirty,
> reduce DAX levels before changing anything else.

---

## Enabling Auto-Start

By default, CAT, TCI, and DAX services must be started manually each session.
You can configure them to start automatically whenever AetherSDR connects to
the radio.

### Where to find auto-start settings

1. Open the **Settings** menu in the menu bar.
2. You will see three checkable options:
   - **Autostart CAT with AetherSDR** — auto-starts the four virtual serial
     ports (TTY/PTY) on connection. *(Linux and macOS only.)*
   - **Autostart TCI with AetherSDR** — auto-starts the TCI WebSocket server
     on connection.
   - **Autostart DAX with AetherSDR** — auto-starts the DAX virtual audio
     bridge on connection. *(Linux with PipeWire, or macOS only.)*

3. Check the options you want. The setting is saved immediately and persists
   across restarts.

> **Note:** The **Enable TCP** button in the CAT Control tile is separate from
> auto-start. TCP CAT servers are toggled directly from the CAT Control tile or
> will start if you had them enabled when you last used the application.
> The "Autostart CAT" menu item specifically controls the TTY/PTY virtual
> serial ports.

### What happens on auto-start

When you connect to the radio with auto-start enabled:

- **CAT:** The four PTY symlinks at `<runtime>/aethersdr/cat-A` through
  `<runtime>/aethersdr/cat-D` are created and begin accepting connections
  (the CAT Control tile shows the resolved per-user path).
- **TCI:** The WebSocket server starts on the configured port (default `50001`).
- **DAX:** The virtual audio devices are created after a short delay (about
  3 seconds, to allow the radio to finish session setup). The DAX Enable
  button in the DAX Audio tile will light up automatically.

---

## Application Walkthroughs

### WSJT-X 3.0

WSJT-X is the most popular application for FT8, FT4, JT65, and other weak-signal
digital modes. WSJT-X 3.0 supports both traditional CAT+DAX and native TCI.

#### Option 1: CAT over TCP + DAX audio

This method works on **Linux and macOS**.

**Step 1 — Prepare AetherSDR**

1. Connect to the radio.
2. Select or create a slice and set the mode to **DIGU** (for FT8/FT4) or the
   appropriate digital mode.
3. Confirm the slice owns transmit (TX indicator visible on the slice).
4. Assign the slice to **DAX channel 1** (click the DAX channel selector in
   the slice bar or panadapter DAX overlay).
5. Open the **CAT Control** tile (click **CAT** in the applet tray).
6. Click **Enable TCP**. Note the base port (default `4532`).
7. Open the **DAX Audio** tile (click **DAX** in the applet tray).
8. Click **Enable**. Verify the DAX 1 row shows your slice (e.g. "Slice A").

**Step 2 — Configure WSJT-X**

1. Open WSJT-X and go to **File > Settings** (or **Preferences** on macOS).
2. Go to the **Radio** tab:
   - **Rig:** select `Hamlib NET rigctl`
   - **Network Server:** `localhost:4532`
     (use `4533` for channel B, `4534` for C, `4535` for D)
   - **PTT Method:** `CAT`
   - **Mode:** `None` or `Data/Pkt`
   - Click **Test CAT** — the button should turn green.
   - Click **Test PTT** — the radio should briefly key up.
3. Go to the **Audio** tab:
   - **Input (Soundcard):** select `AetherSDR DAX 1`
     - On Linux: may appear as `AetherSDR DAX 1` or the PulseAudio source name
     - On macOS: appears as `AetherSDR DAX 1` in the dropdown
   - **Output (Soundcard):** select `AetherSDR TX`
4. Click **OK** to save settings.

**Step 3 — Verify**

1. You should see the WSJT-X waterfall filling with signals.
2. The CAT Control tile should show `1 client` on channel A's TCP row.
3. The DAX 1 level meter in the DAX Audio tile should show activity.
4. To test transmit: click **Tune** in WSJT-X. The radio should key up and
   the TX meter in the DAX Audio tile should show level. Keep the TX gain moderate.

#### Option 2: TCI (control + audio over one connection)

This method works on **Linux, macOS, and Windows** — no virtual audio devices
needed.

**Step 1 — Prepare AetherSDR**

1. Connect to the radio.
2. Select or create a slice and set the mode to **DIGU**.
3. Confirm the slice owns transmit.
4. Open the **TCI Server** tile (click **TCI** in the applet tray).
5. Click **Enable**. Note the port (default `50001`).
6. You do **not** need to enable DAX — TCI carries audio internally.

**Step 2 — Configure WSJT-X 3.0**

1. Open WSJT-X and go to **File > Settings**.
2. Go to the **Radio** tab:
   - **Rig:** select `TCI Server`
   - **Network Server:** `localhost:50001`
   - **PTT Method:** `CAT`
   - Click **Test CAT** — should turn green.
3. Go to the **Audio** tab:
   - **Input:** select `TCI Audio` (or leave at default — TCI handles routing)
   - **Output:** select `TCI Audio`
4. Click **OK**.

**Step 3 — Verify**

1. The TCI Server tile should show `1 client`.
2. The WSJT-X waterfall should fill with signals.
3. Test transmit with **Tune** as above.

> **Windows users:** TCI is the recommended method on Windows since DAX virtual
> audio devices are not available. WSJT-X 3.0's TCI support gives you full
> control and audio through a single connection.

---

### Winlink with VARA

Winlink uses VARA (or VARA FM) as the modem, and VARA needs both CAT control
and audio routing. This walkthrough covers the CAT+DAX method.

> **Platform note:** VARA is a Windows application. On Linux, VARA can be run
> under Wine. On macOS, use a Windows VM. The CAT+DAX configuration is the same
> regardless of how VARA is running.

**Step 1 — Prepare AetherSDR**

1. Connect to the radio.
2. Select or create a slice and set the mode to **DIGU** (for VARA HF) or
   **USB** depending on your VARA configuration.
3. Confirm the slice owns transmit.
4. Assign the slice to **DAX channel 1**.
5. Open the **CAT Control** tile (click **CAT** in the applet tray).
6. Click **Enable TCP**. Note the base port (default `4532`).
7. Open the **DAX Audio** tile (click **DAX** in the applet tray).
8. Click **Enable**.

**Step 2 — Configure VARA**

1. Open VARA HF (or VARA FM).
2. Go to **Settings > Soundcard**:
   - **Input:** select `AetherSDR DAX 1`
   - **Output:** select `AetherSDR TX`
   - On Linux under Wine: you may need to configure the Wine audio to use
     PulseAudio and the devices will appear with their PulseAudio names.
3. Go to **Settings > PTT** (or **CAT Control** depending on VARA version):
   - VARA itself may not need CAT — Winlink handles rig control.
   - If VARA asks for PTT, set it to **VOX** or configure it through Winlink's
     CAT connection.

**Step 3 — Configure Winlink Express**

1. Open Winlink Express.
2. Select **VARA HF Winlink** as the session type.
3. Go to **Settings** (gear icon):
   - Under **Radio Setup / Rig Control**:
     - **Rig type:** `Hamlib NET rigctl` or `Kenwood TS-2000` (rigctld is
       compatible with this selection in many Winlink builds)
     - If using TCP: **Host:** `localhost`, **Port:** `4532`
     - If using serial: **Port:** copy the channel-A path shown in the
       CAT Control tile (e.g. `/run/user/1000/aethersdr/cat-A` on Linux)
   - Verify that Winlink can read the frequency from the radio.
4. Click **Start** to begin a Winlink session.

**Step 4 — Verify**

1. The CAT Control tile should show a connected client on channel A.
2. When VARA transmits, the TX level meter in the DAX Audio tile should show
   activity.
3. When receiving, the DAX 1 meter should show activity and VARA's waterfall
   should display signals.

---

### fldigi with Hamlib

fldigi supports a wide range of digital modes (PSK31, RTTY, Olivia, etc.)
and integrates with Hamlib for rig control.

**Step 1 — Prepare AetherSDR**

1. Connect to the radio.
2. Select or create a slice and set the mode to **DIGU** (for most digital
   modes) or **DIGL** / **RTTY** as appropriate.
3. Confirm the slice owns transmit.
4. Assign the slice to **DAX channel 1**.
5. Open the **CAT Control** tile (click **CAT** in the applet tray).
6. Click **Enable TCP**. Note the base port (default `4532`).
7. Open the **DAX Audio** tile (click **DAX** in the applet tray).
8. Click **Enable**.

**Step 2 — Configure fldigi**

1. Open fldigi. If this is the first launch, the configuration wizard will run.
2. Go to **Configure > Config Dialog** (or the wizard will guide you).
3. Under **Rig Control > Hamlib**:
   - Check **Use Hamlib**.
   - **Rig:** select `NET rigctl` (Hamlib model 2, or search for "rigctl").
   - **Device:** `localhost:4532`
     - On some fldigi versions, you set the host and port separately:
       - **Address:** `localhost`
       - **Port:** `4532`
   - Click **Initialize** or **Connect**. The frequency display in fldigi
     should sync with the radio.

   **Alternative — using a serial port (Linux/macOS):**
   - Under **Rig Control > Hamlib**:
     - **Rig:** select `NET rigctl`
     - Or under **Rig Control > RigCAT** or **Rig Control > Hardware PTT**:
       - **Device:** copy the channel-A path from the CAT Control tile
         (e.g. `/run/user/1000/aethersdr/cat-A` on Linux,
         `~/Library/Caches/AetherSDR/cat-A` on macOS)
       - **Baud rate:** does not matter for virtual serial ports, but
         set it to `9600` if the field is required.

4. Under **Audio > Devices** (or **Soundcard**):
   - **Capture (Input):** select `AetherSDR DAX 1`
     - On Linux with PulseAudio/PipeWire: select **PulseAudio** as the audio
       system, then choose `AetherSDR DAX 1` from the capture device list.
     - On macOS: select `AetherSDR DAX 1` from the PortAudio device list.
   - **Playback (Output):** select `AetherSDR TX`
5. Click **Save** and **Close**.

**Step 3 — Verify**

1. fldigi's waterfall should show signals from the radio.
2. The frequency display in fldigi should match the radio's frequency.
3. Changing frequency in fldigi should move the radio, and vice versa.
4. The CAT Control tile should show a connected CAT client.
5. To test transmit: type some text in the transmit pane and press the TX
   button (or Ctrl+T). The radio should key up and the DAX Audio tile TX meter
   should show level.

---

## Important Terms

### DIGU and DIGL

Digital sideband modes. They give digital software a predictable transmit and
receive environment without the voice-processing assumptions of SSB phone modes.
Use **DIGU** for most digital modes (FT8, PSK31, etc.). Use **DIGL** for
modes that conventionally use lower sideband.

### DAX

Digital Audio Exchange — the software patch cable between the radio session and
your digital program. Creates virtual audio devices on your system.

### CAT

Computer Aided Transceiver — rig control. A digital program uses CAT to read
and set frequency, mode, and PTT.

### TTY / PTY

The virtual serial port version of CAT. Some older or more rigid software
expects a serial port instead of a TCP socket. AetherSDR creates Unix
pseudo-terminals that behave like real serial ports.

### TCI

Transceiver Control Interface — a more integrated network protocol that carries
control, audio, IQ, CW, and spot data through one WebSocket connection. When
the client application supports TCI, it eliminates the need for separate CAT
and audio routing.

### DAX IQ

Raw IQ streaming for applications that need baseband data rather than
demodulated audio (e.g. SDR receivers, digital mode research tools).

---

## How AetherSDR Maps Channels

The data mode tiles are organized around four channels (A, B, C, D) that align with
the four-slice workflow:

| Channel | CAT TCP port (default) | TTY path | DAX audio |
|---|---|---|---|
| A | 4532 | `<runtime>/aethersdr/cat-A` | DAX 1 |
| B | 4533 | `<runtime>/aethersdr/cat-B` | DAX 2 |
| C | 4534 | `<runtime>/aethersdr/cat-C` | DAX 3 |
| D | 4535 | `<runtime>/aethersdr/cat-D` | DAX 4 |

(`<runtime>` is the per-user runtime/cache dir — see *CAT over TTY/PTY*
above for how it resolves on each platform.)

Keep your channel numbering consistent with your slice usage so you do not have
to rediscover the routing every session.

---

## First Digital Setup Checklist

Use this sequence the first time you integrate a new digital application:

1. Connect to the radio.
2. Create or select the slice you want for digital work.
3. Set the slice to the correct digital mode (usually **DIGU** or **DIGL**).
4. Confirm that the correct slice owns transmit.
5. Assign the slice to a DAX channel (1-4).
6. Open the relevant data mode tiles and keep them visible.
7. Enable **TCP**, **TTY**, or **TCI** depending on what your application expects.
8. Enable **DAX** if your workflow needs virtual audio devices.
9. Configure your digital application to point at the correct CAT endpoint
   and audio devices.
10. **Test receive first** — verify decodes or waterfall before touching transmit.
11. **Test transmit** only after receive, frequency tracking, and slice
    assignment all make sense.

---

## Gain Staging

Bad gain staging creates many fake "protocol" problems. If decode quality is
poor or your transmitted signal looks dirty:

1. Reduce the problem to one slice and one digital application.
2. Set DAX RX and TX gain sliders to 50% as a starting point.
3. Avoid clipping in the digital program's audio meter.
4. Avoid excessive transmit processing in the voice path.
5. Test with a short transmission and check your signal on a monitor.

---

## Keep the Window Readable During Digital Sessions

A good digital layout keeps these items on screen:

- the active slice and its mode
- the data mode tiles (CAT Control, DAX Audio, TCI Server, DAX IQ as needed)
- the TX applet
- the status bar network and TX indicators
- at least one visible panadapter

If you collapse too much into Minimal Mode too early, you may hide the clues
you need to diagnose a routing problem.

---

## Common Mistakes

### The software connects, but the wrong slice moves

**Cause:** The external program is attached to a different CAT channel than the
slice you are watching.

**Fix:** Match the CAT channel letter to your slice letter. Channel A (port
4532) controls slice A, channel B (port 4533) controls slice B, and so on.

### Receive works, but transmit goes nowhere

**Cause:** Wrong transmit slice, wrong DAX transmit path, or wrong audio device
selected in the external program.

**Fix:** Confirm TX ownership first (check which slice has the TX indicator),
then confirm the application's transmit audio is routed to `AetherSDR TX`.

### CAT works, but there is no decode audio

**Cause:** The radio-control path is correct but the DAX receive audio path
is not.

**Fix:** Leave CAT alone. Check that DAX is enabled in the DAX Audio tile and
that the application's audio input is set to the correct `AetherSDR DAX`
channel.

### Audio is present, but the application does not tune the radio

**Cause:** DAX is right, but CAT or TCI is wrong or not connected.

**Fix:** Leave audio alone. Check the CAT Control tile for a connected CAT client.
Verify the port number and host in your application's rig control settings.

### No DAX devices appear in the audio settings

**Cause:** DAX is not enabled, the radio is not connected, or (on Linux) the
PulseAudio/PipeWire service is not running.

**Fix:**
- Verify the radio is connected.
- Click **Enable** in the DAX Audio tile.
- On Linux: run `pactl list sources short` to check if the devices exist.
- On macOS: check **System Settings > Sound > Input** for AetherSDR devices.
- On Windows: use TCI instead — DAX virtual audio is not available on Windows.

### Everything works locally, but remote digital operation is choppy

**Cause:** WAN latency, audio compression, or too much visual and streaming
load.

**Fix:** Simplify the session, reduce extra panadapters, and check network
quality before changing digital settings.

---

## A Safe Troubleshooting Order

When a digital setup fails, isolate one layer at a time:

1. Verify the slice and mode.
2. Verify CAT, TTY, or TCI control (can the app read the frequency?).
3. Verify receive audio (does the app's waterfall show signals?).
4. Verify transmit audio (does the radio key up and show TX power?).
5. Verify transmit ownership (is the correct slice assigned TX?).
6. Verify IQ only if the workflow actually needs it.

This order is faster than changing ports, channels, modes, and audio devices
all at once.
