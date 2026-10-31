---
title: "Windows"
slug: "/windows"
description: "AetherSDR runs natively on 64-bit Windows, as an installer or as a portable ZIP."
---

AetherSDR runs natively on 64-bit Windows, as an installer or as a portable ZIP. This page collects everything that is specific to Windows: installing, the features that differ from Linux and macOS, where your files live, and fixes for Windows-only problems. Everything else in these docs applies to Windows as written.

## Requirements

| Package | File | Notes |
|---|---|---|
| **Installer** | `AetherSDR-*-Windows-x64-setup.exe` | Setup wizard with a Start Menu shortcut and an uninstaller |
| **Portable** | `AetherSDR-*-Windows-x64-portable.zip` | No install. Extract anywhere and run `AetherSDR.exe`. |

- Both packages carry the Microsoft Visual C++ runtime with the application. **No Visual C++ Redistributable install is needed**, and repairing or removing the system redistributable does not affect AetherSDR.
- The panadapter renders with **Direct3D 11**.
- For digital-mode programs, plan on the [TCI Server](./tci-server.md): AetherSDR ships no DAX audio driver on Windows (see below).

## Setup

### Installing

1. Download the installer or the portable ZIP from [GitHub Releases](https://github.com/aethersdr/AetherSDR/releases/latest).
2. Run the installer, or extract the ZIP and run `AetherSDR.exe`.
3. The Windows builds are Authenticode-signed (publisher: Jeremy Fielder) and GPG-signed. **Windows SmartScreen** may still warn about a new release until it has built download reputation. Verifying the signature confirms the download is genuine; see [Installation](./installation.md) and [docs/VERIFYING-RELEASES.md](https://github.com/aethersdr/AetherSDR/blob/main/docs/VERIFYING-RELEASES.md).
4. The **Connect to Radio** window opens. Continue with [First Connection](./first-connection.md), or try [Your First Session](./your-first-session.md) with no radio.

Both packages keep your settings in the same place, `%LOCALAPPDATA%\AetherSDR\`, so you can switch between them.

### Windows Firewall

The installer adds Windows Defender Firewall rules for `AetherSDR.exe`: incoming UDP (radio discovery and the radio's streams) on every network, and incoming TCP (TCI, CAT) on private and domain networks only. It first removes the program's old incoming rules, including a block rule left behind by a dismissed firewall prompt. Outgoing rules are never changed. Uninstalling removes the rules it added.

The portable ZIP adds no rules. To check either package, open **Tools → Network Diagnostics...** and choose **Windows Firewall** under **STATUS**. It reports whether the firewall allows AetherSDR on every active network, lists the rules behind that verdict, and names any other security product that manages the firewall. **Fix…** asks Windows for permission and replaces AetherSDR's incoming rules with the ones above; **Check again** reads them once more. Fix is not offered for a rule that blocks outgoing connections; ask whoever manages the PC to remove it.

### Connecting digital-mode programs

There is no built-in DAX audio driver on Windows. The DAX applet says so: "No built-in DAX driver on Windows. Use TCI, or SmartSDR DAX."

- **TCI** carries audio and CAT over one connection and is the simplest route for WSJT-X. See [TCI Server](./tci-server.md) and [WSJT-X Integration](./wsjt-x-integration.md).
- **FlexRadio's own SmartSDR DAX** also works with a FlexRadio.
- **CAT** has no virtual serial ports on Windows. Point your program at one of the CAT applet's TCP ports instead. See [CAT Control](./cat-control.md).

## Using AetherSDR on Windows

### What is different on Windows

- **Menus** are behind the **☰** button at the left of the title bar. Alt+letter mnemonics (Alt+F, Alt+S, …) still open each menu. F11 toggles full screen. See [Menu Reference](./menu-reference.md).
- **Radio Setup** is **Settings → Radio Setup...**.
- **Window frame:** a Qt-drawn frame with the system shadow and resize borders, with AetherSDR's own minimize, maximize and close buttons.
- **GPU choice on hybrid laptops:** the discrete GPU is used by default. The integrated GPU is listed in **Display → SYSTEM → GPU:** but grayed out ("disabled (#1921)"), because it crashes when a panadapter is popped out or docked. See [GPU Rendering](./gpu-rendering.md).
- **Noise reduction:** **BNR** (NVIDIA Maxine, on an NVIDIA RTX/GeForce GPU) is available on Windows; its runtime comes as a single self-contained download. **MNR** is macOS only and is grayed out. See [BNR GPU Noise Removal](./bnr-gpu-noise-removal.md) and [DSP Noise Mitigation](./dsp-noise-mitigation.md).
- **Copy Assist** can use a **Vulkan** GPU (NVIDIA, AMD or Intel). See [Copy Assist](./copy-assist.md).
- **Credentials** are kept in Windows Credential Manager, never in the settings database.
- **Power throttling:** Windows does not throttle AetherSDR (no EcoQoS), so audio, DAX and TCI keep streaming while the window is minimized or covered. While a radio is connected, AetherSDR also keeps the computer from idling into sleep. See [Audio Settings](./audio-settings.md#prevent-system-sleep).
- **USB devices** appear as `COMn` ports. A FlexControl knob that is unplugged and replugged may come back on a different COM port. The ThumbDV helper opens its port exclusively, so close any other program using it. See [FlexControl Tuning Knob](./flexcontrol-tuning-knob.md) and [D-STAR (ThumbDV)](./d-star-thumbdv.md).
- **Ulanzi Dial:** Windows cannot give AetherSDR exclusive control of the dial, so each key press also reaches Windows (for example as a media key). See [Ulanzi Dial](./ulanzi-dial.md).
- **Runtime Monitor:** Windows does not report per-thread state, so the State column shows a dash. See [Runtime Monitor](./runtime-monitor.md).
- **Screen readers:** NVDA, Narrator and JAWS. See [Accessibility](./accessibility.md).

## Reference

### Where files live

| What | Location |
|---|---|
| Settings database (`AetherSDR.db`), backups, quarantine | `%LOCALAPPDATA%\AetherSDR\` |
| Application logs and support bundles | `%LOCALAPPDATA%\AetherSDR\logs\` (newest log also linked as an `aethersdr.log` shortcut) |
| NR2 FFTW wisdom | `%APPDATA%\AetherSDR\aethersdr_fftw_wisdom` |
| BNR NVIDIA runtime | `%LOCALAPPDATA%\AetherSDR\AetherSDR\nvidia-afx\current\` |
| Crash dumps (once enabled, below) | `%LOCALAPPDATA%\CrashDumps\` |
| SmartSDR firmware files, after installing SmartSDR | `C:\ProgramData\FlexRadio Systems\SmartSDR\Updates\` (see [Firmware Update](./firmware-update.md)) |

See [Settings and Backups](./settings-and-backups.md) and [Support and Logging](./support-and-logging.md).

### Environment variables

Set these in **System Properties → Environment Variables**, or in a Command Prompt before starting `AetherSDR.exe` from the same window.

| Variable | Effect |
|---|---|
| `AETHER_NO_GPU=1` or `QT_OPENGL=software` | Software OpenGL for every panadapter, instead of Direct3D 11 |
| `AETHER_SETTINGS_DIR=<folder>` | Use a different configuration folder |

## Known issues

- Hovering over the maximize button does not show Snap Layouts ([#6224](https://github.com/aethersdr/AetherSDR/issues/6224)).
- The BNR runtime download can stall with an empty `.staging` folder under `nvidia-afx` and leave BNR unavailable ([#4132](https://github.com/aethersdr/AetherSDR/issues/4132)).
- A Ulanzi D100H paired over Bluetooth can stay "Disconnected" in the mapper ([#3485](https://github.com/aethersdr/AetherSDR/issues/3485)).
- Popping out a panadapter has crashed AetherSDR with an older AMD Radeon HD 7000 series graphics card ([#5990](https://github.com/aethersdr/AetherSDR/issues/5990)).

## Troubleshooting

### Windows SmartScreen warns about AetherSDR

The Windows builds are Authenticode-signed (publisher: Jeremy Fielder), but SmartScreen can still warn about a new release until it has built download reputation.

1. Check the publisher: right-click the setup `.exe` or `AetherSDR.exe`, choose **Properties → Digital Signatures**, and confirm the signer is Jeremy Fielder. You can also verify the download's GPG signature, following [docs/VERIFYING-RELEASES.md](https://github.com/aethersdr/AetherSDR/blob/main/docs/VERIFYING-RELEASES.md).
2. If the signature is good, run AetherSDR anyway from the SmartScreen prompt.

### Discovery, QRZ, maps or the update check stop working after an update

A firewall rule is blocking AetherSDR. A dismissed firewall prompt can leave a block rule, and another security product that manages the firewall can block AetherSDR as well.

1. Open **Tools → Network Diagnostics...** and choose **Windows Firewall**.
2. If it reports a blocking or missing rule, click **Fix…** and accept the Windows permission prompt.
3. If it names another security product, allow AetherSDR in that product.
4. A failed request to an external service is logged once per service, host, error and HTTP status under **External Services** (`aether.network`), with the error. See [Support and Logging](./support-and-logging.md).

### WSJT-X or another digital-mode program gets no audio

There is no DAX audio driver on Windows.

1. Connect the program through the [TCI Server](./tci-server.md), which carries audio as well as CAT. See [WSJT-X Integration](./wsjt-x-integration.md).
2. Or, with a FlexRadio, use FlexRadio's SmartSDR DAX.

### A logging or digital-mode program cannot find AetherSDR's serial port

Windows has no CAT virtual serial ports.

1. In the **CAT** applet, enable a port with the **Rigctld**, **TS-2000** or **Flex** dialect.
2. Point the program at that TCP port on this computer. See [CAT Control](./cat-control.md).

### The panadapter draws wrongly or shows "Spectrum renderer unavailable"

The Direct3D 11 renderer failed or your driver draws incorrectly.

1. Set `AETHER_NO_GPU=1` (or `QT_OPENGL=software`) and start AetherSDR again to use software OpenGL.
2. **Help → About AetherSDR** shows which renderer is in use. See [GPU Rendering](./gpu-rendering.md).

### A USB knob, encoder or Stream Deck+ does nothing

HID encoders are off by default.

1. Open **Settings → Radio Setup... → Serial & Controllers**.
2. Turn on **Enable HID encoders / StreamDeck+ (RC-28, PowerMate, ShuttleXpress, …)**.
3. Unplug the device and plug it back in. See [USB Control Surfaces](./usb-control-surfaces.md).

### Reporting a crash

Windows keeps a crash dump only if local dumps are switched on.

1. In an administrator PowerShell, run:

   ```powershell
   $k = 'HKLM:\SOFTWARE\Microsoft\Windows\Windows Error Reporting\LocalDumps\AetherSDR.exe'
   New-Item -Force $k | Out-Null
   New-ItemProperty -Force $k -Name DumpType -PropertyType DWord -Value 1 | Out-Null
   ```

2. After the next crash, attach `AetherSDR.exe.<pid>.dmp` from `%LOCALAPPDATA%\CrashDumps\` to your report from **Help → File an Issue...**, and say whether you ran the installer or the portable ZIP.

See [Troubleshooting](./troubleshooting.md) and [docs/debugging-crashes.md](https://github.com/aethersdr/AetherSDR/blob/main/docs/debugging-crashes.md).

## See also

- [Installation](./installation.md)
- [TCI Server](./tci-server.md)
- [WSJT-X Integration](./wsjt-x-integration.md)
- [GPU Rendering](./gpu-rendering.md)
- [Settings and Backups](./settings-and-backups.md)
- [Troubleshooting](./troubleshooting.md)
