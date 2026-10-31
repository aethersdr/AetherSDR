---
title: "Firmware Update"
slug: "/firmware-update"
description: "AetherSDR can update a FlexRadio's firmware on Linux, macOS and Windows from the Updates window, downloading the SmartSDR installer from FlexRadio or using one you downloaded."
status: "Experimental"
applies_to: ["FlexRadio"]
---

:::info[Status]

**Status:** Experimental · **Applies to:** FlexRadio

:::

AetherSDR can update a FlexRadio's firmware on Linux, macOS and Windows from the Updates window (**File → Update...**), downloading the SmartSDR installer from FlexRadio or using one you downloaded.

> **Caution:** firmware update from AetherSDR is a highly experimental feature. Use it at your own risk. Updating with the SmartSDR for Windows application is still the recommended route.

## Requirements

- A connected FlexRadio. Firmware update is a FlexRadio feature: on any other radio family the Updates window's radio section says **Not updatable here** and its buttons are dimmed with the reason (see [Supported Radios](./supported-radios.md)).
- An internet connection to FlexRadio's website, or a SmartSDR installer or `.ssdr` firmware file you downloaded yourself.

## Setup

Open **File → Update...**. Below the AetherSDR section, the **YOUR RADIO** section names the connected radio and its firmware version, and a status badge says whether the radio is **Current** or **Behind** the newest release in its own line. When the status bar shows an out-of-date firmware version, clicking it opens the window at this section.

The radio's firmware version is also shown, read-only, as **FW Version** in **Settings → Radio Setup...** → **Radio** → **Radio Information**.

## Updating the Firmware

### Choose a release

The section lists every SmartSDR release FlexRadio publishes (AetherSDR fetches the list at most once a day), one row per major line (v4, v3, v2), oldest to newest. The release installed on the radio has a thicker outline; the newest release in the radio's own line is highlighted. When the radio is behind, that newest release is already selected. Click another release to choose it, and **Release notes** opens its notes.

### Install

1. Click **Install** *version*.
2. A confirmation names the release and the radio, and warns that the radio reboots after the update. Do not disconnect during the update.
3. AetherSDR downloads the SmartSDR installer from FlexRadio, checks it against FlexRadio's MD5, extracts the firmware for your radio's model family, and uploads it. If FlexRadio's MD5 can't be downloaded, AetherSDR stops before the upload and asks again, saying that the firmware's integrity was not checked. The steps **Prepare firmware**, **Upload to radio** and **Radio confirms** light in turn, the progress bar fills, and the status line says what is happening.

### Install from a file

1. Download the SmartSDR installer from [flexradio.com](https://www.flexradio.com), or have an extracted `.ssdr` file.
2. Click **Open Installer...** and choose one of:
   - the **`.msi`** installer (SmartSDR v4.2 and later);
   - the **`.exe`** installer (v4.1.x and earlier);
   - an already extracted **`.ssdr`** firmware file.
3. AetherSDR extracts the firmware for your radio's model family, then asks before uploading it.

For an **`.msi`** installer, AetherSDR identifies the firmware by the installer's
recorded filename and cabinet metadata. It checks that the extracted file's size
matches that record. A missing or ambiguous firmware entry stops staging.
The staged file keeps the installer's firmware filename, including its build
number when present; the upload confirmation and log use that same name.
The firmware log records the staged filename, byte count, and MD5 for diagnostics.
An already extracted **`.ssdr`** file whose name identifies the other firmware
family is rejected instead of being renamed for your radio.
Manually selected **`.ssdr`** files keep their original filename. If the name
contains no version, the staged version is reported as `unknown`.

Delete firmware files staged by earlier AetherSDR versions and select the
original installer again. Those cached **`.ssdr`** files can contain firmware
for a different family despite their filename; renaming them cannot fix it.

### The outcome

The upload ends in one of three outcomes:

| Outcome | What it means | What to do |
|---------|---------------|------------|
| **Succeeded** | The radio confirmed the update. | Wait for the radio to reboot, then reconnect. |
| **Unconfirmed** | The bytes were sent, but the radio did not confirm the installation. It is probably rebooting to apply the image. | Reconnect and check the firmware version. A retry is blocked until you reconnect. |
| **Failed** | The upload did not complete, or the radio rejected it. | Read the status line, then try again. |

While firmware is being prepared or written, the rest of the Updates window waits, and closing the window asks first. Closing it while preparing sends nothing to the radio; closing it during the upload stops the transfer; once the bytes have been sent, it does not undo the update. **Cancel** asks the same question.

## Reference

### Where to find `.ssdr` files

- Windows, after installing SmartSDR: `C:\ProgramData\FlexRadio Systems\SmartSDR\Updates\`
- Or let **Open Installer...** extract the file from the installer for you.

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
2. Select the installer with **Open Installer...**.
3. If staging still fails, use SmartSDR for Windows to extract the appropriate
   **`.ssdr`** file and select that file instead. In SmartSDR 4.2/4.3 MSI
   installers, FLEX-6000 radios use **`FLEX-6x00`**, while FLEX-8000 and Aurora
   radios use **`FLEX-9600`**. Renaming a file does not change its compatibility.

### The radio section says Not updatable here

You are connected to a radio that is not a FlexRadio.

1. Connect to a FlexRadio.
2. Open **File → Update...** again.

### Install stays unavailable after an Unconfirmed upload

A retry is blocked until you reconnect, because the radio is probably rebooting to apply the image.

1. Wait for the radio to come back, then reconnect.
2. Check the firmware version in the **YOUR RADIO** section, or **FW Version** in Radio Setup.

### The upload Failed

The upload did not complete, or the radio rejected it.

1. Read the status line for the reason.
2. Try the upload again.

## See also

- [FlexRadio](./flexradio.md)
- [Supported Radios](./supported-radios.md)
- [Updates](./updates.md)
- [Radio Setup](./radio-setup.md)
- [FlexRadio downloads](https://www.flexradio.com)
