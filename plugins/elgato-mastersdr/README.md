# MasterSDR Elgato Stream Deck Plugin

Controls FlexRadio via MasterSDR's TCI WebSocket server. 43 actions for TX, bands, modes, DSP, audio, and DVK.

Works with the **official Elgato Stream Deck app** on macOS and Windows.

## Installation

1. Enable TCI in MasterSDR: **Settings > Autostart TCI with MasterSDR**
2. Download `com.mastersdr.radio.streamDeckPlugin` from the [latest release](https://github.com/MasterSDR/MasterSDR/releases/latest)
3. Double-click the file — the Elgato app installs it automatically
4. Drag MasterSDR actions onto your Stream Deck buttons

No build step, no npm, no command line required.

## Building from Source

If you want to rebuild the distributable:

```bash
cd plugins/elgato-MasterSDR/com.mastersdr.radio.sdPlugin
npm install
cd ..
zip -r com.mastersdr.radio.streamDeckPlugin com.mastersdr.radio.sdPlugin/
```

## 43 Available Actions

**TX:** PTT, MOX Toggle, TUNE Toggle, RF Power, Tune Power
**Bands:** 160m, 80m, 60m, 40m, 30m, 20m, 17m, 15m, 12m, 10m, 6m, Band Up, Band Down
**Frequency:** Tune Up, Tune Down
**Modes:** USB, LSB, CW, AM, FM, DIGU, DIGL, FT8
**Audio:** Mute Toggle, Volume Up, Volume Down
**DSP:** NB, NR, ANF, APF, SQL toggles
**Slice:** Split, Lock, RIT, XIT toggles
**DVK:** Play, Record
