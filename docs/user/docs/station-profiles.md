---
title: "Location and Operator Profiles"
slug: "/station-profiles"
description: "Organize saved radio connections by location and choose personal audio and layout preferences."
---

**Connect to a radio** connects to an individual radio. Locations such as Home, POTA, Cabin or Club organize the saved radios and connection paths you can choose from. Each location can contain several radio families. A location can share a physical radio with another location while keeping its own connection paths. Open it with **File → Connect to Radio...**.

An operator profile stores your microphone, audio output and panadapter arrangement. It is separate from SmartLink credentials and the radio's Global, TX and Mic profiles. Choosing a location or operator previews the next connection; it does not disconnect a live radio, change its operating state or reroute live audio.

## Setup

The selected location appears in bold, and the blue **Radios at Home** heading updates to show the location you chose.

Home, POTA, Club and RV are supplied as starter locations. Rename or remove any of them, or add your own. Starter locations are created once; removing one keeps it removed.

<img src="/img/screens/station-home.png" width="912" alt="Connect to a radio with Home selected in bold and a blue Radios at Home heading above the radio list." />

*Locations organize your saved radios. The selected location is bold; the blue heading names its radio list.*

1. Choose **+ Add location**, enter a distinguishable name and save.
2. Add a discovered radio through **Nearby radios**, create a manual path with **+ Add radio by IP**, or save a remote radio through **SmartLink accounts…**.
3. Select the location and radio, then press **Connect**. A saved radio stays listed when offline.

Drag locations up or down to set their order; the order is saved automatically. Right-click a location for **Edit**, **Move**, **Rename** and **Remove**, or open **Location options…**. **Move** also changes its position in the list. **Remove** asks for confirmation and removes its saved paths while keeping physical radios and account credentials. Keep at least one location.

Press **Enter** or keypad **Enter** to save an entry in the location, saved-connection, operator and SmartLink account profile dialogs. The same keys confirm the destination in **Move radio** and **Copy connection to location**. Invalid entries stay open with a validation message.

> **Upgrading from an older version:** **Home** imports your saved addresses, routed source interfaces and last radio. **Radio operator** imports your current audio and layout choices. Startup auto-connect keeps its existing setting. Rename these profiles whenever you are ready; no geographic names are assumed.

## Using saved connections

Radio names appear in bold, with model and availability beneath. Unnamed radios show their address as secondary information. Saved names take priority when several paths identify one radio.

The selected-radio summary shows its saved access path and the operator for the next connection. Multiple paths for a verified radio identity appear in **Saved connection path**. Choose the desired path before connecting. Only the selected SmartLink account supplies the current remote list.

Right-click a radio, or open **Radio options…**, for **Edit**, **Move**, **Rename** and **Remove**. **Move** carries its saved paths to another location; **Remove** asks for confirmation. Expand **Connection settings** to save a nearby radio or **Copy to another location…**. Copying a path preserves its shared account reference. It does not copy a password or create another radio.

Connection forms retain the selected family's settings, including source interfaces, Icom ports and low bandwidth mode. Family support levels and receive-only limits remain those listed in [Supported Radios](./supported-radios.md). KiwiSDR and Web-888 remain auxiliary receive sources in Antennas.

**Startup and connection preferences** contains startup auto-connect, adaptive throttling, Icom wake behavior and the simulator toggle. The window expands to show all preference checkboxes. These app-wide preferences keep their existing defaults.

<img src="/img/screens/station-pota.png" width="912" alt="POTA selected in the Locations list with its radios and connection summary." />

*Choose POTA or another location to see its saved connection paths. Browsing locations does not connect or disconnect a radio.*

## Using workspace radio tabs

Saved radios appear as persistent tabs in the main window, including saved IP connections and radios that are currently unavailable. A radio's saved paths share a tab within its Location. The second line shows its connection state and Location. Discovery updates availability without removing saved tabs or changing their order.

Drag a tab left or right to reorder it, like browser tabs. Release to save the order across restarts; **Esc** cancels the drag. Drag near an edge to scroll through an overflowing strip. The mouse wheel and dragging empty strip space also scroll it. Reordering never connects or disconnects a radio. Hidden tabs retain their saved position, newly added radios appear at the end, and the active radio stays where you placed it.

Click a disconnected radio tab to connect through its preferred saved path with the operator selected in **Operating as**. Clicking the connected tab keeps the current operating view. When another radio is connected, clicking a destination tab opens **Switch radio**. Choose **Switch** to disconnect the current radio and connect the destination, or **Cancel** to keep operating. Switching is unavailable during transmit. The active highlight changes only after the destination connects.

Right-click a tab, or focus it and press **Shift+F10**, for these actions:

<img src="/img/screens/radio-tab-switch-confirmation.png" width="552" alt="Switch radio prompt naming the source and destination Locations, explaining that the current radio will disconnect, with Cancel and Switch buttons." />

*Confirm a replacement while the current radio remains connected. Location names distinguish similarly named radios.*

| Action | Behavior |
|---|---|
| **Connect** / **Switch to this radio…** | Connect using the saved path, confirming replacement of a live connection. |
| **Cancel connection** | Cancel a pending connection or sign-in continuation. |
| **Disconnect** | Disconnect this client's live radio and keep its tab. |
| **Rename…** | Change the saved connection label in both tabs and Locations. The simulator retains its safety label. |
| **Connection settings…** | Open the matching Location, radio and saved path in the connection window. |
| **Remove from tabs** | Hide a disconnected tab while preserving its saved paths and radio settings. |

To restore a hidden tab, open **+**, select the saved radio and choose **Radio options… → Add to tabs**. **Remove** in the saved radio menu deletes saved entries after confirmation; it is separate from **Remove from tabs**.

The **+** button opens **Connect to a radio** at the browsed Location. The dialog remembers the radio selected within each Location and the preferred path chosen for each radio. Browsing another Location or operator while connected preserves the live connection and audio. A pending connection keeps the Location, path and operator captured when it was requested, even if browsing changes afterward.

If credentials need attention, the connection window opens at the relevant saved connection. A SmartLink sign-in continues the requested connection after authentication. Switching SmartLink accounts can require ending the existing remote session before sign-in. If a switch fails after disconnecting, **Retry** attempts the destination again and **Reconnect to [previous radio]** offers an explicit return.

## Startup and recovery

With auto-connect enabled and a last radio saved, startup opens the operating window and attempts that radio. With auto-connect disabled, or no saved last radio, startup opens Locations. A manual connection choice cancels startup auto-connect for that launch; late discovery, probe and credential results cannot override it.

If a saved IP path fails at startup, **Connect by IP** shows the failure with the saved connection name. **‹ Back to Locations** returns to that saved radio's Location and selection so another path or radio can be chosen.

<img src="/img/screens/radio-ip-startup-recovery.png" width="912" alt="Connect by IP shows a saved connection name, a documentation IP address, the failed connection reason and a Back to Locations button." />

*An unavailable saved IP path keeps its connection name and offers Back to Locations.*

## Using operator profiles

<img src="/img/screens/operator-profile-selection.png" width="732" alt="Operator profiles library with saved profiles and Create, Edit, Duplicate, Remove and Use profile actions." />

*Operator profiles keep personal audio and layout preferences separate from locations and account credentials.*

<img src="/img/screens/operator-profile-creation.png" width="636" alt="Create Operator profile dialog with a name, source profile, microphone, audio output, pan arrangement and Create and use profile action." />

*Start from defaults or another profile, then choose microphone, output and pan arrangement. Enter saves the profile.*

1. Choose **Profiles…** to open **Operator profiles**.
2. Choose **Create**. Enter a unique name and start from app defaults, current preferences or another operator profile.
3. Choose the microphone, audio output and pan arrangement, then **Create and use profile**.
4. Use **Operating as** to choose an operator for the next connection, or select an entry in the library and **Use profile**.

**Edit** renames or changes preferences. **Duplicate** creates an independent copy. To remove the selected profile, choose a replacement first; keep at least one operator. Profiles never contain account credentials, frequency, mode, power or radio operating profiles.

Preferences apply when you connect. **System default** explicitly follows the computer's default device. A missing saved device is displayed as unavailable and blocks the connection until you choose a replacement or System default; AetherSDR does not silently choose another microphone.

## Troubleshooting

### A saved radio is offline

Saving a connection does not establish reachability.

1. Power on the radio and check its network or USB connection.
2. Use **Nearby radios → Retry Discovery** for supported discovery sources.
3. For a routed network, edit the saved host and source interface; see [Manual Connection](./manual-connection.md).
4. For SmartLink, check the account shown in the connection summary; see [SmartLink Setup](./smartlink-setup.md).

### The operator profile needs an unavailable device

1. Open **Profiles…**, select the operator and choose **Edit**.
2. Reconnect the saved device, choose a replacement, or explicitly choose **System default**.
3. Save and retry the connection.

### Profiles could not be saved

The settings store refused the write, or the workspace belongs to a newer application schema. The prior document is retained.

1. Use the latest AetherSDR build that supports the saved configuration.
2. Check that the settings directory is writable; see [Settings and Backups](./settings-and-backups.md).

## See also

- [First Connection](./first-connection.md)
- [Manual Connection](./manual-connection.md)
- [SmartLink Setup](./smartlink-setup.md)
- [Audio Settings](./audio-settings.md)
