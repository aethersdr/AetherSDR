---
title: "Profile Management"
slug: "/profile-management"
description: "Profiles save and restore FlexRadio state."
status: "Supported"
applies_to: ["FlexRadio"]
---

:::info[Status]

**Status:** Supported · **Applies to:** FlexRadio

:::

Profiles save and restore FlexRadio state. AetherSDR supports all three SmartSDR profile types, and profiles are stored **on the radio**, so they are shared with SmartSDR and other clients.

AetherSDR's own preferences (layout, display, controllers) are separate and stay on your computer. See [Settings and Backups](./settings-and-backups.md).

## Requirements

- A FlexRadio must be connected. The Profiles menu, Profile Manager and PROF applet are hidden when another kind of radio is connected.
- Import and export also need a direct LAN connection (not SmartLink), and are not available while transmitting (MOX, TUNE, PTT).

## Profile types

### Global profiles

Save the whole radio state: panadapters, slices, frequencies, modes, filters, DSP, antennas and so on. Loading a global profile changes everything at once.

### Transmit profiles

TX settings: power, EQ, compressor, mic settings. Keep several for different operating (contest, ragchew, digital).

### Microphone profiles

Mic input settings: source, gain, bias, boost and compander level. Useful when you switch microphones.

## Using the Profiles menu

**Profiles** in the menu bar:

- **Profile Manager...**: open the management dialog
- **Import/Export Profiles...**: back up or restore the radio's profile database as a SmartSDR `.ssdr_cfg` package
- the list of global profiles: click one to load it; the check mark follows the radio's active global profile

## Using the Profile Manager

**Profiles → Profile Manager...** opens a non-modal window that remembers its size and position. Tabs: **Global**, **Transmit**, **Microphone**, **Auto-Save**.

On each profile tab:

<img src="/img/screens/profile-manager.png" width="460" alt="Profile Manager window with tabs Global, Transmit, Microphone and Auto-Save, on Global. A New Profile Name field sits above Load, Save and Delete buttons, and a list of the radio's global profiles: 20m CW, 20m Phone, 40m Phone, 80m Phone, BCSO, macOS_default_Profile, NIRG, SO2RDefault and VHF. A Close button sits at the bottom." />

*The Profile Manager, listing the global profiles stored in the radio.*

- **New Profile Name**: type a name for a new profile
- **Load** (or double-click): apply the selected profile
- **Save** (Global tab): save the current state to the selected profile, or to a new one using the name field
- **Create** (Transmit and Microphone tabs): create a new profile. The radio cannot overwrite an existing TX or Mic profile directly; changes to the active one are captured by Auto-Save. If you try to create a name that already exists, AetherSDR tells you so and, if Auto-Save is off, offers **Enable Auto-Save**.
- **Delete**: remove the selected profile, after confirmation

A status line reports what the radio did: `Saving "<name>"…`, `Saved profile "<name>".`, "Radio refused save of …", "Not connected — cannot save …", or "No response from the radio …". A blank name field never overwrites the highlighted profile.

### Auto-Save

With **Auto-save profile changes** enabled on the Auto-Save tab, changes you make to TX and Mic settings are saved to the active TX and Mic profiles as you go.

## Using the PROF applet (Profile Switcher)

The **PROF** applet ("Profile Switcher", in the Station group of the applet button bar, closed by default) has three drop-downs: **Global**, **TX** and **Mic**. Picking an entry loads that profile immediately. It follows the radio's active profiles, including changes made by other clients.

## Importing and exporting profiles (`.ssdr_cfg`)

**Profiles → Import/Export Profiles...** moves profile libraries between radios, and between SmartSDR for Windows and AetherSDR, using SmartSDR-compatible `.ssdr_cfg` backup packages. **Both directions are carried out by the radio**; AetherSDR transfers the package but never edits its contents.

<img src="/img/screens/profile-import-export.png" width="560" alt="Import/Export Profiles window on the Export tab. A note explains that export is radio-driven. Radio Database Categories has Select All and checkboxes for Global Profiles, TX Profiles and MIC Profiles (ticked), and Memories, Preferences, TNF, XVTR and USB Cables. The Destination field is blacked out beside Browse…, with an Export button below, a status line, a 0% progress bar and Cancel and Close buttons." />

*The Import/Export Profiles window, on the Export tab. The destination path is blacked out.*

### Export

1. On the **Export** tab, tick the **Radio Database Categories** to include: Global Profiles, TX Profiles and MIC Profiles (ticked by default), Memories, Preferences, TNF, XVTR and USB Cables. **Select All** ticks everything.
2. Choose a **Destination** (default name `ASDR_Config_<timestamp>_v<version>.ssdr_cfg`).
3. Click **Export**. The radio builds the package and AetherSDR saves it.

### Import

1. On the **Import** tab, choose the **Configuration Package** and click **Import**.
2. A confirmation shows the package's firmware version against the radio's, and warns that:
   - imported profiles, memories and other settings can replace matching items on the radio, including defaults;
   - packages from newer firmware may fail or leave the radio database in an unexpected state;
   - packages that include Preferences can close or reopen slices and panadapters.
3. **Export Backup First** saves a backup of the radio before you go ahead; **Import** uploads the package and the radio applies it. Profile lists refresh when it finishes.

## Workspaces and profiles

If you use the experimental [Workspace Canvas](./workspace-canvas.md), a workspace can be bound to a global profile (**View → Workspace Canvas → Workspaces → Bind to radio profile**), so recalling that profile also switches the screen layout.

## Multi-Flex

While a global profile is being restored, AetherSDR holds back its own slice, panadapter and waterfall changes until the radio has finished, so another client's profile load is not disturbed. See [Multi-Flex](./multi-flex.md).

## Reference

Profile operations are carried out by the radio with these commands:

- `profile global save "name"` / `load` / `delete`
- `profile transmit create "name"` / `save` / `delete`
- `profile mic create "name"` / `save` / `delete`

## Known issues

- An export that is interrupted or invalid can replace an existing backup file and still report success ([#5620](https://github.com/aethersdr/AetherSDR/issues/5620)).
- After repeated global profile changes, panadapter **FFT Auto** can stop adjusting the noise floor while still showing as on, until you turn it off and on again ([#3186](https://github.com/aethersdr/AetherSDR/issues/3186)).
- DFNR can come back on after you turn it off, and saving a profile does not keep it off, because it runs in AetherSDR and is not stored in radio profiles ([#4018](https://github.com/aethersdr/AetherSDR/issues/4018)).

## Troubleshooting

### The Profiles menu or PROF applet is missing

Profiles are a FlexRadio feature, and they are hidden when another kind of radio is connected.

1. Connect to a FlexRadio. The menu and applet appear.

### Import/Export Profiles is unavailable

Import and export need a FlexRadio on a direct LAN connection, with nothing transmitting.

1. Connect over the LAN rather than SmartLink.
2. Turn off MOX, TUNE and PTT.
3. Open **Profiles → Import/Export Profiles...** again.

### "Create" says a TX or Mic profile name already exists

The radio cannot overwrite an existing TX or Mic profile. Changes to the active one are captured by Auto-Save instead.

1. Click **Enable Auto-Save** when AetherSDR offers it, or tick **Auto-save profile changes** on the **Auto-Save** tab.
2. Load the profile you want to change, then make your changes. They are saved to it as you go.

## See also

- [Memory Channels](./memory-channels.md)
- [Settings and Backups](./settings-and-backups.md): AetherSDR's own settings on this computer
- [Multi-Flex](./multi-flex.md)
- [Workspace Canvas](./workspace-canvas.md)
- [FlexRadio](./flexradio.md)
