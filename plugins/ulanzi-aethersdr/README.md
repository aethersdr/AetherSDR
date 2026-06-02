# AetherSDR Ulanzi Studio Plugin

Controls FlexRadio via AetherSDR's TCI WebSocket server from any Ulanzi-Studio-compatible macro keypad / dial. Covers TX (MOX / TUNE / RIT), VFO tuning, modes, bands, and gain — as both keypad buttons and rotary (Encoder) knobs (AF / RF / Mic / Filter width / Squelch).

Works with **[Ulanzi Studio](https://www.ulanzi.com/)** on macOS and Windows. Smoke-tested on the **Ulanzi D100H / KEHWIN Dial_Lite** (6 keys + 1 dial, BLE HOGP); the wider D200H (14 keys) and D200X (14 keys + 3 knobs) profile work is in progress.

The plugin is a sibling to [`elgato-aethersdr`](../elgato-aethersdr) (Stream Deck) and [`streamcontroller-aethersdr`](../streamcontroller-aethersdr) (open-source Stream Deck alternative) — same TCI command vocabulary, different host application.

## Installation

1. Enable TCI in AetherSDR: **Settings → Autostart TCI with AetherSDR**.
2. Download the packaged plugin from the [latest AetherSDR release](https://github.com/aethersdr/AetherSDR/releases/latest) (when available).
3. Copy / link into Ulanzi Studio's plugin directory:
   - **Windows:** `%APPDATA%\Ulanzi\UlanziDeck\Plugins\com.g0jkn.aethersdr.ulanziPlugin`
   - **macOS:** `~/Library/Application Support/Ulanzi/UlanziDeck/Plugins/com.g0jkn.aethersdr.ulanziPlugin`
4. Restart Ulanzi Studio fully (system tray → Quit, relaunch).
5. Drag AetherSDR actions onto your Ulanzi device's keys.

A ready-made D100H layout is in [`profiles/aethersdr-d100h-default.ulanziDeckProfile`](profiles/aethersdr-d100h-default.ulanziDeckProfile) — Studio → Profile menu → Import.

## Building from Source

```bash
cd plugins/ulanzi-aethersdr/com.g0jkn.aethersdr.ulanziPlugin
npm install
```

That installs the `ws` WebSocket library used by `plugin/app.js`. The bundled Ulanzi SDK (`libs/common-node/`, `libs/common-html/`) is vendored — no separate fetch required.

To regenerate the PNG icons after editing the action list:

```powershell
pwsh scripts/Generate-Icons.ps1
```

Outputs 196×196 PNGs into `com.g0jkn.aethersdr.ulanziPlugin/assets/icons/` and `…/assets/launchers/` — matching Ulanzi Studio's marketplace icon convention so text + glyphs render crisp on the device LCD. Pure PowerShell + System.Drawing; no npm / Node deps. Currently Windows-only — cross-platform port (ImageMagick / SkiaSharp) is future work.

## Available Actions

**Encoder (dial / knob):** VFO Tune, plus rotary AF Gain, RF Power, Mic Gain, Filter Width, Squelch Level (rotate to adjust; press+rotate = coarse). VFO dial press toggles MOX; level-knob press is a deliberate no-op so a knob can't key TX.
**TX:** MOX Toggle, TUNE / ATU, RIT Toggle
**Modes:** Mode Cycle, plus direct USB / LSB / CW / DIGU
**Bands:** Band Up, Band Down
**Gain (keypad):** AF Up/Down, RF Up/Down, Mic Up/Down (±5 per press)

Each action's property inspector lets you set the AetherSDR TCI URL.

## Companion shack-app launcher tiles

For pairing with Studio's built-in `System → Open` action so a Ulanzi key launches another shack app, `assets/launchers/` ships 5 coordinated launcher SVGs: AetherSDR, TCI Monitor, ShackLog, IQ Capture, aether-pad. Dark backdrop + accent-coloured wordmark + `↗` glyph — visually distinct from the radio-control action keys.

## Public-facing repository

Independent releases, marketplace submission, and operator-side documentation live at **[github.com/nigelfenton/aethersdr-ulanzi-plugin](https://github.com/nigelfenton/aethersdr-ulanzi-plugin)**. This in-repo copy is bundled context for AetherSDR developers; downstream consumers should follow the standalone repo for the latest release artifacts.

## License

GPL-3.0-or-later — matching the parent AetherSDR project (this plugin lives in-tree under `plugins/`). The vendored Ulanzi SDK in `libs/common-node/` and `libs/common-html/` keeps its own license (see the `LICENSE.txt` files there).

## Author

Nigel Fenton (G0JKN/W3).
