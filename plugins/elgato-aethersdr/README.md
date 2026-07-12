# AetherSDR Elgato Stream Deck Plugin

Controls FlexRadio via AetherSDR's TCI WebSocket server. 46 actions for TX, bands, modes, DSP, audio, and DVK.

Works with the **official Elgato Stream Deck app** on macOS and Windows.

## Installation

1. Enable TCI in AetherSDR: **Settings > Autostart TCI with AetherSDR**
2. Download `com.aethersdr.radio.streamDeckPlugin` from the [latest release](https://github.com/aethersdr/AetherSDR/releases/latest)
3. Double-click the file — the Elgato app installs it automatically
4. Drag AetherSDR actions onto your Stream Deck buttons

No build step, no npm, no command line required.

## Building from Source

If you want to rebuild the distributable:

```bash
cd plugins/elgato-aethersdr/com.aethersdr.radio.sdPlugin
npm install
cd ..
zip -r com.aethersdr.radio.streamDeckPlugin com.aethersdr.radio.sdPlugin/
```

## 46 Available Actions

**TX:** PTT, MOX Toggle, TUNE Toggle, RF Power, Tune Power, RF Power Dial (Stream Deck+ dial — rotate to adjust, press to cycle step size 1W/5W)
**Bands:** 160m, 80m, 60m, 40m, 30m, 20m, 17m, 15m, 12m, 10m, 6m, Band Up, Band Down
**Frequency:** Tune Up, Tune Down, VFO Tuning (Stream Deck+ dial — rotate to tune, press to cycle step size 10 Hz–1 MHz)
**Modes:** USB, LSB, CW, AM, FM, DIGU, DIGL, FT8
**Audio:** Mute Toggle, Volume Up, Volume Down, Volume Dial (Stream Deck+ dial — rotate to adjust, press to toggle mute)
**DSP:** NB, NR, ANF, APF, SQL toggles
**Slice:** Split, Lock, RIT, XIT toggles
**DVK:** Play, Record
