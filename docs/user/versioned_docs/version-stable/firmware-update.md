---
title: "Firmware Update"
slug: "/firmware-update"
description: "AetherSDR can upload new firmware to a FlexRadio on Linux, macOS and Windows, using the SmartSDR installer you download from FlexRadio."
status: "Experimental"
applies_to: ["FlexRadio"]
---

:::info[Status]

**Status:** Experimental · **Applies to:** FlexRadio

:::

AetherSDR can upload new firmware to a FlexRadio on Linux, macOS and Windows, using the SmartSDR installer you download from FlexRadio.

> **Caution:** firmware update from AetherSDR is a highly experimental feature. Use it at your own risk. Updating with the SmartSDR for Windows application is still the recommended route.

## Requirements

- A connected FlexRadio. Firmware update is a FlexRadio feature: the group is hidden while you are connected to any other radio family (see [Supported Radios](./supported-radios.md)).
- The SmartSDR installer from [flexradio.com](https://www.flexradio.com), or an extracted `.ssdr` firmware file.

## Setup

Open **Settings → Radio Setup...** (on macOS: **AetherSDR → Preferences...**), choose the **Radio** page, and find the **Firmware Update** group. It shows the radio's current **FW Version** and three buttons: **Check for Update**, **Select Installer...** and **Upload Firmware**.

<img src="/img/screens/firmware-update-group.png" width="643" alt="Firmware Update group showing FW Version: 4.2.20.41343 with a copy button, and three buttons: Check for Update, Select Installer... and a dimmed Upload Firmware." />

*The Firmware Update group on the Radio page.*

## Updating the Firmware

### Check for updates

1. Click **Check for Update**.
2. AetherSDR asks the FlexRadio website for the latest version and compares it with the radio's firmware.
3. The status line reads either "Firmware is up to date (vX.Y.Z)" or "Update available: vX.Y.Z", with a reminder to download the SmartSDR installer from flexradio.com.

AetherSDR does not download the installer for you.

### Stage the firmware

1. Download the SmartSDR installer from [flexradio.com](https://www.flexradio.com).
2. Click **Select Installer...** and choose one of:
   - the **`.msi`** installer (SmartSDR v4.2 and later);
   - the **`.exe`** installer (v4.1.x and earlier);
   - an already extracted **`.ssdr`** firmware file.
3. AetherSDR extracts the firmware for your radio's model family and shows its progress. When staging finishes, **Upload Firmware** becomes available.

For an **`.msi`** installer, AetherSDR identifies the firmware by the installer's
recorded filename and cabinet metadata. It checks that the extracted file's size
matches that record. A missing or ambiguous firmware entry stops staging.
The staged file keeps the installer's firmware filename, including its build
number when present; the upload confirmation and log use that same name.
The firmware log records the staged filename, byte count, and MD5 for diagnostics.
For FLEX-8000 and Aurora radios, a note below the staged filename explains:
"FLEX-8000 and Aurora radios use FLEX-9600 firmware."
An already extracted **`.ssdr`** file whose name identifies the other firmware
family is rejected instead of being renamed for your radio.
Manually selected **`.ssdr`** files keep their original filename. If the name
contains no version, the staged version is reported as `unknown`.

Delete firmware files staged by earlier AetherSDR versions and select the
original installer again. Those cached **`.ssdr`** files can contain firmware
for a different family despite their filename; renaming them cannot fix it.

### Upload

1. Click **Upload Firmware**.
2. A confirmation names the file and the radio, and warns that the radio reboots after the update. Do not disconnect during the upload.
3. The progress bar and status line follow the transfer.
4. The upload ends in one of three outcomes:

| Outcome | What it means | What to do |
|---------|---------------|------------|
| **Succeeded** (green) | The radio confirmed the update. | Wait for the radio to reboot, then reconnect. |
| **Unconfirmed** (amber) | The bytes were sent, but the radio did not confirm the installation. It is probably rebooting to apply the image. | Reconnect and check the firmware version. A retry is blocked until you reconnect. |
| **Failed** (red) | The upload did not complete, or the radio rejected it. | Read the status line, then try again. |

Closing Radio Setup during an upload asks for confirmation first. Closing it stops the transfer; once the bytes have been sent, it does not undo the update.

## Reference

### Where to find `.ssdr` files

- Windows, after installing SmartSDR: `C:\ProgramData\FlexRadio Systems\SmartSDR\Updates\`
- Or let **Select Installer...** extract the file from the installer for you.

### Firmware files

| File | Models |
|------|--------|
| `FLEX-6x00_vX.Y.Z[.build].ssdr` | FLEX-6000 series (Microburst and DeepEddy) |
| `FLEX-9600_vX.Y.Z[.build].ssdr` | FLEX-8000 and Aurora series (BigBend), in SmartSDR 4.2/4.3 MSI installers |

The MSI's `FLEX-9600` filename also identifies the firmware used by FLEX-8000
and Aurora radios. Choose firmware for the radio's hardware platform; neither
file size nor a renamed filename establishes compatibility. FlexRadio's
[update instructions](https://helpdesk.flexradio.com/hc/en-us/articles/50209943129371-Upgrading-your-FLEX-6000-8000-Aurora-Radio-using-SmartSDR-for-Mac)
distinguish FLEX-6000 and FLEX-8000 firmware.

Automatic staging stops for an unknown model or an unverified firmware family,
including RT-2122 (DragonFire). For commercial radios, obtain the appropriate
software from FlexRadio; its public SmartSDR download is for amateur models.

### Firmware compatibility

The active test target is FLEX-8600 firmware 4.2.18 (SmartSDR protocol v1.4.0.0). Earlier 4.x firmware works; v3.x is unsupported.

## Known issues

- The transfer reaches 100%, but the radio does not apply the update or reboot, and can report that the installation failed ([#5572](https://github.com/aethersdr/AetherSDR/issues/5572)).

## Troubleshooting

### The installer cannot be staged

The installer may be incomplete, use unsupported metadata, or lack an unambiguous
firmware entry for your radio.

1. Download the original SmartSDR installer from FlexRadio again.
2. Select the installer with **Select Installer...**.
3. If staging still fails, use SmartSDR for Windows to extract the appropriate
   **`.ssdr`** file and select that file instead. In SmartSDR 4.2/4.3 MSI
   installers, FLEX-6000 radios use **`FLEX-6x00`**, while FLEX-8000 and Aurora
   radios use **`FLEX-9600`**. Renaming a file does not change its compatibility.

### The Firmware Update group is missing

You are connected to a radio that is not a FlexRadio, so the group is hidden.

1. Connect to a FlexRadio.
2. Open **Settings → Radio Setup...** → **Radio** again.

### Upload Firmware stays unavailable after an Unconfirmed upload

A retry is blocked until you reconnect, because the radio is probably rebooting to apply the image.

1. Wait for the radio to come back, then reconnect.
2. Check **FW Version** in the **Firmware Update** group.

### The upload Failed

The upload did not complete, or the radio rejected it.

1. Read the status line for the reason.
2. Try the upload again.

## See also

- [FlexRadio](./flexradio.md)
- [Supported Radios](./supported-radios.md)
- [Radio Setup](./radio-setup.md)
- [FlexRadio downloads](https://www.flexradio.com)
