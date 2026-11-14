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
2. Add a discovered radio through **Nearby radios**, create a manual path with **+ Add radio by IP**, or save a remote radio through **Accounts…**.
3. Select the location and radio, then press **Connect**. A saved radio stays listed when offline.

Drag locations up or down to set their order; the order is saved automatically. Right-click a location for **Edit**, **Move**, **Rename** and **Remove**, or open **Location options…**. **Move** also changes its position in the list. **Remove** asks for confirmation and removes its saved paths while keeping physical radios and account credentials. Keep at least one location.

Press **Enter** or keypad **Enter** to save an entry in the location, saved-connection, operator and SmartLink account profile dialogs. The same keys confirm the destination in **Move radio** and **Copy connection to location**. Invalid entries stay open with a validation message.

> **Upgrading from an older version:** **Home** imports saved connections with a known radio family, routed source interfaces and the last radio. Older recent-address history is available through **Import address history…**: choose an address and radio type, review its settings, then save it. Merely reviewing an address does not connect. **Radio operator** imports your current audio and layout choices. Startup auto-connect keeps its existing setting. Rename these profiles whenever you are ready; no geographic names are assumed.

## Using saved connections

Radio names appear in bold, with model and availability beneath. Unnamed radios show their address as secondary information. Saved names take priority when several paths identify one radio.

The selected-radio card shows the model above the radio name, followed by separate **Connection**, **SmartLink** (for remote connections) and **Operator** rows. The connection and operator describe the next connection. Multiple paths for a verified radio identity appear under **Connection settings → Connect using**. Choose the desired path before connecting. Only the selected SmartLink account supplies the current remote list.

Right-click a radio, or open **Radio options…**, for **Edit**, **Move**, **Rename** and **Remove**. **Move** carries its saved paths to another location; **Remove** asks for confirmation. Connecting to a nearby radio does not save it; expand **Connection settings** to save it or **Copy to another location…**. Copying a path preserves its shared account reference. It does not copy a password or create another radio.

Connection forms retain the selected family's settings, including source interfaces, Icom ports and low bandwidth mode. Family support levels and receive-only limits remain those listed in [Supported Radios](./supported-radios.md). KiwiSDR and Web-888 remain auxiliary receive sources in Antennas.

**Startup and connection preferences** contains startup auto-connect, adaptive throttling, Icom wake behavior and the simulator toggle. The window expands to show all preference checkboxes. These app-wide preferences keep their existing defaults.

<img src="/img/screens/station-pota.png" width="1094" alt="POTA selected in the Locations list with its radios and connection summary." />

*Choose POTA or another location to see its saved connection paths. Browsing locations does not connect or disconnect a radio.*

## Using workspace radio tabs

Two recent radio shortcuts appear in the main window, including saved IP connections and radios that are currently unavailable. A radio’s saved paths share a shortcut within its Location. The second line shows its connection state and Location. Discovery updates availability; the drawer keeps the full saved radio list.

Drag a tab left or right to reorder it, like browser tabs. Release to save the order across restarts; **Esc** cancels the drag. Drag near an edge to scroll through an overflowing strip. The mouse wheel and dragging empty strip space also scroll it. Reordering never connects or disconnects a radio. Hidden shortcuts retain their saved position. Recent choices determine which two radios are shown, while dragged order controls their left-to-right positions.

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
| **Move left** / **Move right** | Swap the tab with its neighbor; the order is saved, as it is after dragging. |
| **Remove from tabs** | Hide a disconnected tab while preserving its saved paths and radio settings. |

To restore a hidden shortcut, open **⋯ More radios** and choose the saved radio, or use **Actions → Add to tabs** in the drawer. **Remove** in the saved radio menu deletes saved entries after confirmation; it is separate from **Remove from tabs**.

The title bar shows up to **two recent radio shortcuts**. The active radio stays visible, and a pending target temporarily uses the other slot. Recent choices and dragged shortcut order are saved across restarts. Other saved and nearby radios remain available in **⋯ More radios**, searchable by name, Location, model, address or status. **Locations and connections…** opens the full connection window.

The dialog remembers the radio selected within each Location and the preferred path chosen for each radio. Browsing Locations preserves the live connection and audio; explicitly switching operators recalls local preferences. A pending connection keeps the Location and path captured when it was requested.

If credentials need attention, the connection window opens at the relevant saved connection. A SmartLink sign-in continues the requested connection after authentication. Switching SmartLink accounts can require ending the existing remote session before sign-in. If a switch fails after disconnecting, **Retry** attempts the destination again and **Reconnect to [previous radio]** offers an explicit return.

## Startup and recovery

With auto-connect enabled and a last radio saved, startup opens the operating window and attempts that radio. With auto-connect disabled, or no saved last radio, startup opens Locations. A manual connection choice cancels startup auto-connect for that launch; late discovery, probe and credential results cannot override it.

If a saved IP path fails at startup, **Connect by IP** shows the failure with the saved connection name. **‹ Back to Locations** returns to that saved radio's Location and selection so another path or radio can be chosen.

<img src="/img/screens/radio-ip-startup-recovery.png" width="912" alt="Connect by IP shows a saved connection name, a documentation IP address, the failed connection reason and a Back to Locations button." />

*A failed saved IP path retains its name. Back to Locations returns to its saved radio.*

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

Preferences are recalled only when you explicitly switch operators or create and use a new operator. Reconnecting or browsing locations keeps your current computer audio and layout choices. Editing a saved operator changes its saved preferences without switching to it. Operators cannot be switched while transmitting, because the switch can change the microphone.

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
