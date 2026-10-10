---
title: "PSK Reporter Map"
slug: "/psk-reporter-map"
description: "The PSK Reporter map shows who is hearing your signal, and who you are hearing, using live reports from PSK Reporter."
---

The PSK Reporter map shows who is hearing your signal, and who you are hearing, using live reports from [PSK Reporter](https://pskreporter.info/). Reports are drawn on an OpenStreetMap world map or a 3D globe, coloured by mode, with optional great-circle paths, a day/night terminator, NASA night lights and weather radar. The same window also has a one-shot **WSPR beacon**.

## Setup

Open the map from **Tools → PSK Reporter…** Type your callsign in **Call:** to see who is hearing you and who you are hearing.

## Using the map

### Choosing reports

The **Reports** group at the top of the left sidebar chooses what is shown:

| Control | What it does |
|---|---|
| **Call:** | A callsign whose sent and received reports you want to see (normally your own) |
| **All Callsigns** | Show every PSK Reporter report instead of one callsign |
| **Band:** | All, or one band |
| **Mode:** | All, FT8, FT4, WSPR, JS8, CW, PSK, RTTY, SSB or Other |
| **Lookback:** | 15 min, 30 min, 1 hour, 2 hours, 4 hours or 8 hours |
| **Active monitors** | Also show stations that are monitoring and reporting right now |
| **Globe** | Switch between the flat map and an interactive 3D globe |
| **Paths** | Draw great-circle paths between the stations in each report |
| **Day/night** | Shade the part of the world that is in darkness |

Reports arrive from PSK Reporter's live feed, backed up by periodic polling, and are cached so the map is not empty when you reopen it. Markers are coloured by mode; the legend is in the status bar at the bottom. Hover or click a marker for a card with the station's details.

When a band-conditions forecast is available, a **Band conditions** group appears below Reports.

### Moving around the map

- **Drag** to pan. The flat map wraps continuously across the date line, so you can keep panning east or west; markers and paths repeat in every copy of the world.
- **Mouse wheel or trackpad scroll** zooms; equal steps in and out return to the same scale.
- **Pinch** to zoom on a trackpad.

The **Map** group has **Dark map** (a dark-tinted basemap) and **Map brightness**.

### City lights

**City lights** overlays NASA's VIIRS night-lights image (a historical 2016 composite, not live data) on the map or globe. With **Day/night** on, the lights fade in between sunset and the end of civil twilight; with Day/night off they are shown worldwide.

| Control | Default | Effect |
|---|---|---|
| **Brightness** | 70 % | Overall intensity |
| **Faint lights** | 50 % | Lifts dim settlements while keeping the background black. 0 restores the original image. |
| **Warmth** | 0 | 0 keeps NASA's grayscale; higher values tint the lights gold |

Some light sources are not cities. A status line at the bottom of the map reports loading and retries.

### Weather

Tick **Weather overlay** to show precipitation on the map or globe. It is off by default and needs no account or API key. The **Weather precipitation** group chooses the sources:

| Source | Coverage |
|---|---|
| **Global primary · LibreWXR** | Worldwide precipitation. This is a mix of ground radar, satellite estimates and model data, not worldwide radar. |
| **US backup · NOAA** | NOAA/NWS radar, United States |
| **Canada backup · ECCC** | Environment and Climate Change Canada radar |
| **Europe backup · OPERA** | EUMETNET OPERA radar, Europe |

LibreWXR is the primary source. Each regional backup has its own checkbox and is used when LibreWXR is turned off or unavailable; turn LibreWXR off to see the regional radar feeds directly.

- **Radar coverage** shades the radar sites and their nominal range.
- **Intensity legend** shows the colour scale, with **Position at top** to move it.
- **Sources & licenses** explains each source and its terms.

#### Playback

Press the play button to loop through past observations.

- **History:** 1 h, 2 h or 4 h, limited to what the provider still holds (LibreWXR currently offers about two hours).
- **Speed:** 0.25× to 5×.
- Each frame shows its observation time in local time. In live mode the age shows as **Age unknown**, because live images carry no verified scan time.
- Playback shows original images only; nothing is interpolated between frames.

Radar can be delayed or missing. **This overlay is not a substitute for official weather warnings.**

If a weather or NASA server fails, AetherSDR backs off (60 seconds, then longer) and honours the server's `Retry-After`, so a busy map does not hammer a public service.

## Using the WSPR beacon

The **WSPR beacon** group sends a single WSPR transmission.

| Field | Meaning |
|---|---|
| **TX call:** | Callsign to encode |
| **Grid:** | Four-character Maidenhead locator, for example `CN85` |
| **Band:** | 160 m to 6 m; default 20 m. Tunes the TX slice to the standard WSPR dial frequency in DIGU with a 1200–1800 Hz filter. |
| **Reported:** | Power to report, 0–60 dBm, default 30. **This only goes into the message; it does not change your RF power.** |
| **Offset:** | Audio tone, 1400–1600 Hz, default 1500 |
| **Level:** | Generated audio level, −60 to −3 dBFS |

**Transmit once** arms one transmission for the next even UTC minute; it does not repeat. The button becomes **Cancel** while armed. The status line shows progress, then **Complete**.

The beacon refuses to start when RADE is active, the radio cannot transmit, the transmitter is already in use, there is no TX slice or the TX slice is locked. It unkeys if the audio stream is lost, you close the window or you press Cancel.

## Reference

### Saved settings

All map choices (callsign, filters, layers, weather sources, playback history and speed, light settings, beacon fields) are saved with the PSK Reporter preferences and restored next time.

## Troubleshooting

### The WSPR beacon won't start

The beacon refuses to start when RADE is active, the radio cannot transmit, the transmitter is already in use, there is no TX slice or the TX slice is locked.

1. Turn RADE off if it is active.
2. Make sure a slice holds TX and is not locked.
3. Wait until nothing else is transmitting, then press **Transmit once** again.

### The weather overlay is missing or out of date

Radar can be delayed or missing, and a failing server is retried with a growing delay.

1. Check the status line at the bottom of the map for loading and retry messages.
2. To use a regional radar feed directly, tick its backup source and turn **Global primary · LibreWXR** off.

## See also

- [SpotHub](./spothub.md)
- [WSJT-X Integration](./wsjt-x-integration.md)
- [AetherClock and GPS](./aetherclock-and-gps.md)
- [PSK Reporter](https://pskreporter.info/)
- Developer notes: [weather radar](https://github.com/aethersdr/AetherSDR/blob/main/docs/psk-reporter-weather-radar.md), [regional radar and LibreWXR](https://github.com/aethersdr/AetherSDR/blob/main/docs/weather-radar-regions.md), [city lights](https://github.com/aethersdr/AetherSDR/blob/main/docs/psk-reporter-city-lights.md), [provider retries](https://github.com/aethersdr/AetherSDR/blob/main/docs/map-provider-retries.md)
