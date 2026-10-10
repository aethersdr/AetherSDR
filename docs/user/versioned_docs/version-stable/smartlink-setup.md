---
title: "SmartLink Setup"
slug: "/smartlink-setup"
description: "SmartLink lets you operate your FlexRadio over the internet."
status: "Supported"
applies_to: ["FlexRadio"]
---

:::info[Status]

**Status:** Supported · **Applies to:** FlexRadio

:::

SmartLink lets you operate your FlexRadio over the internet. AetherSDR supports the full SmartLink sign-in and connection flow, including audio, panadapter and waterfall streaming.

Can't forward ports at the station (CGNAT, Starlink, cellular)? On a FLEX-8000 series or Aurora radio, see [Tailscale Remote Access](./tailscale-remote-access.md). It needs no port forwarding and runs on Tailscale's free plan.

## Requirements

- A FlexRadio registered with SmartLink
- Port forwarding on the router at the radio's location (TCP 4994, UDP 4993), or UPnP

## Setup

At the station, using SmartSDR for Windows:

1. Open SmartSDR for Windows → **Settings** → **SmartLink Setup**.
2. Log in with your FlexRadio account.
3. Click **Register** to register the radio with SmartLink.
4. Under **Network Settings**, configure port forwarding:
   - **Forward TCP Port:** 4994 (external) → 4994 (internal)
   - **Forward UDP Port:** 4993 (external) → 4993 (internal)
   - Target: the radio's LAN IP address
5. Click **Test** — both indicators should be green.

## Connecting with SmartLink

1. Open the connect panel (**File → Connect to Radio...**, or the **+** on the title bar's radio tabs).
2. Choose **Remote with SmartLink**.
3. In **SmartLink account**, enter your FlexRadio account email and password and click **Sign In**.
4. Your radio appears in the list of remote radios. Select it and click **Connect Remote Radio**.

You stay signed in across restarts (see [Security](#security)).

On a slow or metered connection, tick **Use low bandwidth mode** under *Connection options for slower links* before connecting, and consider Opus audio compression — see [Low Bandwidth Connections](./low-bandwidth-connections.md).

## Certificate pinning

AetherSDR pins each SmartLink radio's TLS certificate on first use, to protect you from a man-in-the-middle.

- The first time you connect to a radio over SmartLink, AetherSDR stores the SHA-256 fingerprint of its certificate.
- On later connections the certificate must match. If it does not, the connection **pauses before authentication** and a warning shows the expected and presented fingerprints with two choices:
  - **Accept new certificate** — only if you know the certificate changed (for example after a firmware update or a radio replacement);
  - **Reject and disconnect** — the default.
- Nothing is sent that would authenticate you until you accept, so an impostor never gets a working session.

### Managing pins

**Settings → Radio Setup... → SmartLink → Pinned SmartLink Certificates** lists each pinned radio with its **Host**, **SHA-256 fingerprint** and the date it was **Pinned**.

- **Forget selected** removes one pin.
- **Forget all** clears every pin after confirmation. Each radio is pinned again, silently, on its next connection.

Forget a pin only when you have deliberately changed a radio's certificate.

## Reference

### Security

- Sign-in uses FlexRadio's identity provider, and all SmartLink connections use TLS with certificate pinning (above).
- Your password is not stored. The SmartLink **refresh token** is saved in the operating system's keychain, so AetherSDR signs you in automatically on the next launch. Builds without keychain support keep it for the current session only. See [Settings and Backups](./settings-and-backups.md).
- Logs redact email addresses, tokens, IP addresses and serial numbers — see [Support and Logging](./support-and-logging.md).

### Limitations

- Port forwarding (or working UPnP) is required at the radio's location.
- Profile import/export (**Profiles → Import/Export Profiles...**) is disabled over SmartLink; use a direct LAN connection for it.

## Known issues

- **TUNE** may not work on a remote radio over SmartLink while AetherSDR is connected to Genius peripherals (Antenna Genius, PGXL, TGXL) ([#6001](https://github.com/aethersdr/AetherSDR/issues/6001)).
- On a remote link, receive audio can drop out briefly several times a minute when packets arrive out of order ([#6051](https://github.com/aethersdr/AetherSDR/issues/6051)).

## Troubleshooting

Before reporting a SmartLink problem, turn on the **SmartLink**, **Audio** and **VITA-49** log categories in **Help → Support & Diagnostics...**.

### The radio is not listed after you sign in

The radio is not registered with SmartLink, or its port test does not pass.

1. At the station, open SmartSDR for Windows → **Settings** → **SmartLink Setup**.
2. Check that the radio is registered, and click **Test**: both indicators should be green.

### The connection drops after about 10 seconds

Port forwarding at the station is probably not set up correctly.

1. Check that TCP 4994 and UDP 4993 are forwarded to the radio's LAN IP address.
2. Run the SmartLink **Test** in SmartSDR again.

### The radio is listed, but you cannot connect

TCP 4994 is probably not forwarded to the radio.

1. Check the TCP 4994 forward on the station's router.

### A certificate mismatch warning appears

The radio presented a different certificate from the one AetherSDR pinned. See [Certificate pinning](#certificate-pinning).

1. If you did not change anything at the station, choose **Reject and disconnect**.
2. If you know the certificate changed (for example after a firmware update or a radio replacement), choose **Accept new certificate**.

## See also

- [Tailscale Remote Access](./tailscale-remote-access.md)
- [Low Bandwidth Connections](./low-bandwidth-connections.md)
- [Manual Connection](./manual-connection.md)
- [Support and Logging](./support-and-logging.md)
- [FlexRadio](./flexradio.md)
