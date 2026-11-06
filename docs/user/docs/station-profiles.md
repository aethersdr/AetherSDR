---
title: "Location and Operator Profiles"
slug: "/station-profiles"
description: "Organize saved radio connections by location and choose personal audio and layout preferences."
---

**Connect to a radio** connects to an individual radio. Locations such as Home, POTA, Cabin or Club organize the saved radios and connection paths you can choose from. Each location can contain several radio families. A location can share a physical radio with another location while keeping its own connection paths. Open it with **File → Connect to Radio...**.

An operator profile stores your microphone, audio output and panadapter arrangement. It is separate from SmartLink credentials and the radio's Global, TX and Mic profiles. Choosing a location previews its radios without changing the live session. Switching operators explicitly recalls that operator’s saved computer audio and layout preferences.

## Setup

A new setup starts with **Home**. With one location, its name and **Add location** appear above the radio list. Adding another location reveals the **Locations** column. The selected location appears in bold, and the blue radio-list heading names it.

**Add location** offers **POTA**, **Club**, **Cabin** and **RV** as name suggestions. Choose one or type your own name, including emoji.

<img src="/img/screens/location-suggestions.png" width="636" alt="Add Location dialog offering POTA, Club, Cabin and RV name suggestions." />

*Choose a suggested location name or type your own, including emoji.*

<img src="/img/screens/station-home.png" width="1094" alt="Connect to a radio with a compact Home header and saved radio list." />

*Home keeps a single-location setup compact. Add another location to reveal the Locations column.*

1. Choose **+ Add location**, enter a distinguishable name and save.
2. Add a discovered radio through **Nearby radios**, create a manual path with **+ Add radio by IP**, or save a remote radio through **SmartLink accounts…**.
3. Select the location and radio, then press **Connect**. A saved radio stays listed when offline.

Drag locations up or down to set their order; the order is saved automatically. Right-click a location for **Edit**, **Move**, **Rename** and **Remove**, or open **Location options…**. **Move** also changes its position in the list. **Remove** asks for confirmation and removes its saved paths while keeping physical radios and account credentials. Keep at least one location.

Press **Enter** or keypad **Enter** to save an entry in the location, saved-connection, operator and SmartLink account profile dialogs. The same keys confirm the destination in **Move radio** and **Copy connection to location**. Invalid entries stay open with a validation message.

> **Upgrading from an older version:** **Home** imports saved connections with a known radio family, routed source interfaces and the last radio. Older recent-address history is available through **Import address history…**: choose an address and radio type, review its settings, then save it. Merely reviewing an address does not connect. **Radio operator** imports your current audio and layout choices. Startup auto-connect keeps its existing setting. Rename these profiles whenever you are ready; no geographic names are assumed.

## Using saved connections

Radio names appear in bold, with model and availability beneath. Unnamed radios show their address as secondary information. Saved names take priority when several paths identify one radio.

The selected-radio card shows the model above the radio name, followed by separate **Connection**, **SmartLink** (for remote connections) and **Operator** rows. The connection and operator describe the next connection. Multiple paths for a verified radio identity appear under **Connection settings → Connect using**. Choose the desired path before connecting. Only the selected SmartLink account supplies the current remote list.

Right-click a radio, or open **Radio options…**, for **Edit**, **Move**, **Rename** and **Remove**. **Move** carries its saved paths to another location; **Remove** asks for confirmation. Expand **Connection settings** to save a nearby radio or **Copy to another location…**. Copying a path preserves its shared account reference. It does not copy a password or create another radio.

Connection forms retain the selected family's settings, including source interfaces, Icom ports and low bandwidth mode. Family support levels and receive-only limits remain those listed in [Supported Radios](./supported-radios.md). KiwiSDR and Web-888 remain auxiliary receive sources in Antennas.

**Startup and connection preferences** contains startup auto-connect, adaptive throttling, Icom wake behavior and the simulator toggle. The window expands to show all preference checkboxes. These app-wide preferences keep their existing defaults.

<img src="/img/screens/station-pota.png" width="1094" alt="POTA selected in the Locations list with its radios and connection summary." />

*Choose POTA or another location to see its saved connection paths. Browsing locations does not connect or disconnect a radio.*

## Using operator profiles

<img src="/img/screens/operator-profile-selection.png" width="732" alt="Operators library with saved profiles and Create, Edit, Duplicate, Remove and Use profile actions." />

*Operator profiles keep personal audio and layout preferences separate from locations and account credentials.*

<img src="/img/screens/operator-profile-creation.png" width="636" alt="Create Operator profile dialog with a name, source profile, microphone, audio output, pan arrangement and Create and use profile action." />

*Start from defaults or another profile, then choose microphone, output and pan arrangement. Enter saves the profile.*

1. Choose **Operators…** to open **Operators**.
2. Choose **Create**. Enter a unique name and start from app defaults, current preferences or another operator profile.
3. Choose the microphone, audio output and pan arrangement, then **Create and use profile**.
4. Use **Operating as** to switch operators, or select an entry in the library and **Use profile**. With one operator, its name appears as plain text; the dropdown appears when there are multiple operators.

**Edit** renames or changes preferences. **Duplicate** creates an independent copy. To remove the selected profile, choose a replacement first; keep at least one operator. Profiles never contain account credentials, frequency, mode, power or radio operating profiles.

Preferences are recalled only when you explicitly switch operators or create and use a new operator. Reconnecting or browsing locations keeps your current computer audio and layout choices. Editing a saved operator changes its saved preferences.

**System default** follows the computer’s default device. If an explicitly selected operator references an unavailable device, AetherSDR temporarily uses the system default and displays a warning. The saved device preference is kept, so it can be recalled when the device is available again.

On a touchscreen, editable connection fields offer a keyboard button. Tapping a field requests the platform’s onscreen keyboard. Email, host and password fields provide appropriate input hints; the platform input method supplies the keyboard.

## Troubleshooting

### A saved radio is offline

Saving a connection does not establish reachability.

1. Power on the radio and check its network or USB connection.
2. Use **Nearby radios → Retry Discovery** for supported discovery sources.
3. For a routed network, edit the saved host and source interface; see [Manual Connection](./manual-connection.md).
4. For SmartLink, check the account shown in the connection summary; see [SmartLink Setup](./smartlink-setup.md).

### The operator profile needs an unavailable device

1. Open **Operators…**, select the operator and choose **Edit**.
2. Reconnect the saved device, choose a replacement, or explicitly choose **System default**.
3. Save the updated operator preferences. Switching to this operator recalls them.

### Profiles could not be saved

The settings store refused the write, or the workspace belongs to a newer application schema. The prior document is retained.

1. Use the latest AetherSDR build that supports the saved configuration.
2. Check that the settings directory is writable; see [Settings and Backups](./settings-and-backups.md).

## See also

- [First Connection](./first-connection.md)
- [Manual Connection](./manual-connection.md)
- [SmartLink Setup](./smartlink-setup.md)
- [Audio Settings](./audio-settings.md)
