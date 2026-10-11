---
title: "Callsign Lookup"
slug: "/callsign-lookup"
description: "AetherSDR can look up a station's name, location, grid square, licence class and photo on QRZ.com."
---

AetherSDR can look up a station's name, location, grid square, licence class and photo on [QRZ.com](https://www.qrz.com/). Lookups appear in two places: the **Callsign Lookup** window, and a contact card that pops up in the CW decoder when it copies a station signing `DE <call>`.

Without a QRZ.com account, lookups still work at country level, using the prefix data AetherSDR ships for DXCC spot colouring.

## Requirements

- For full lookups, a QRZ.com account. An XML Logbook Data subscription on QRZ.com returns full details; a free account returns limited fields.
- Without an account, you get country-level prefix data only.

## Setup

Open **Radio Setup → QRZ & Callsigns** (in the **ONLINE & APPEARANCE** group).

### QRZ.com Account

- **Enable QRZ callsign lookups**
- **Username (callsign):** and **Password:**
- **Test Login** checks the credentials and reports "Login OK" or why it failed.

The password is stored in your operating system's keychain, never in the settings file. On a build without keychain support it is kept for the current session only.

### Lookup Cache

Looked-up callsigns are cached for **7 days**, together with station photos, so a busy net doesn't ask QRZ twice for the same station. The group shows how many entries are cached, and **Clear Cache** empties it.

## Using Callsign Lookup

### The Callsign Lookup window

Open it from **Tools → Callsign Lookup…** or press **Ctrl+Shift+L**.

Type a callsign and press **Lookup**. The card shows the callsign, licence class, name, location, grid, county, QSL methods (LoTW, eQSL, QSL) and the distance and bearing from your station, plus the station photo. Click the callsign to open its QRZ.com page.

The status line says where the answer came from: "Fetched from QRZ.com", or "From cache — fetched *n* day(s) ago". **Refresh** fetches fresh data from QRZ.com even if the callsign is cached.

If QRZ.com is not configured or doesn't answer, the card shows **prefix** data instead (country, continent and CQ zone) and says so.

### Contact card in the CW decoder

When the [CW Decoder](./cw-decoder.md) copies `DE <call>`, a compact contact card for that station appears beside the decoded text. It shows the name, location, grid and distance; click the callsign to open its QRZ.com page, and **✕** hides the card.

### Looking up a spot

Right-click any spot on the panadapter and choose **Lookup on QRZ** to open that station's qrz.com page in your browser. See [SpotHub](./spothub.md).

## Reference

### Distance and bearing

Distance and bearing are measured from your own position: the radio's GPS fix when it has one, otherwise a grid square (the radio's, or the one on your own QRZ.com record). See [AetherClock and GPS](./aetherclock-and-gps.md).

## Known issues

- The CW decoder's contact card has no setting to turn it off; **✕** only hides the current card ([#5454](https://github.com/aethersdr/AetherSDR/issues/5454)).

## Troubleshooting

### The card shows only country, continent and CQ zone

QRZ.com is not configured, or it didn't answer, so AetherSDR fell back to prefix data.

1. Open **Radio Setup → QRZ & Callsigns**.
2. Tick **Enable QRZ callsign lookups** and enter your **Username (callsign):** and **Password:**.
3. Press **Test Login** and read the result.

### The card is missing some details

A free QRZ.com account returns limited fields.

1. Full details need an XML Logbook Data subscription on QRZ.com.

### A station's details are out of date

The answer came from the 7-day cache.

1. Press **Refresh** in the Callsign Lookup window to fetch fresh data for that callsign.
2. To empty the whole cache, press **Clear Cache** in **Radio Setup → QRZ & Callsigns**.

### You have to enter the QRZ.com password every session

This build has no keychain support, so the password is kept for the current session only. Use a build with keychain support to have it remembered.

## See also

- [CW Decoder](./cw-decoder.md)
- [SpotHub](./spothub.md)
- [Radio Setup](./radio-setup.md)
- [AetherClock and GPS](./aetherclock-and-gps.md)
- [QRZ.com](https://www.qrz.com/)
