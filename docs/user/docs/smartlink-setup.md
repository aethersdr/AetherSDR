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

Can't forward ports at the location (CGNAT, Starlink, cellular)? On a FLEX-8000 series or Aurora radio with firmware 4.2.18 or later, see [Tailscale Remote Access](./tailscale-remote-access.md). It needs no port forwarding and runs on Tailscale's free plan.

## Requirements

- A FlexRadio registered with SmartLink
- Port forwarding on the router at the radio's location (TCP 4994, UDP 4993), or UPnP

## Setup

At the location, using SmartSDR for Windows:

1. Open SmartSDR for Windows → **Settings** → **SmartLink Setup**.
2. Log in with your FlexRadio account.
3. Click **Register** to register the radio with SmartLink.
4. Under **Network Settings**, configure port forwarding:
   - **Forward TCP Port:** 4994 (external) → 4994 (internal)
   - **Forward UDP Port:** 4993 (external) → 4993 (internal)
   - Target: the radio's LAN IP address
5. Click **Test** — both indicators should be green.

## Connecting with SmartLink

<img src="/img/screens/connect-smartlink-card.png" width="912" alt="Connect to a radio SmartLink page with saved account selection, Add, Rename and Remove actions, sign-in fields and a remote radio list." />

*Saved SmartLink accounts and sign-in, before authentication.*

1. Open **File → Connect to Radio...** and select the location that will contain the remote radio.
2. Choose **SmartLink accounts…**. Use **Add** to name an account and enter its email address.
3. Enter that account's password and click **Sign In**.
4. Select a remote radio and click **Save selected radio to location**. Return with **Back to location**, select the radio and press **Connect**.

### Switching accounts

Each saved remote connection remembers its account. Pressing **Connect** selects that identity and looks up its current radio list. A saved credential lets the connection continue automatically. If sign-in is required, authenticate that account and retry **Connect**.

The account selector supports **Add**, **Rename** and **Remove**. Signing out or removing an account forgets only that account's credential. Connections that reference a removed account remain saved and explain that the account must be added again. Disconnect a live SmartLink radio before switching, signing out or removing an account. Local radios remain available independently of SmartLink authentication.

Account discovery uses one account at a time. Switching clears the previous account's list; an offline saved radio is not presented as a freshly discovered radio. Account labels and email addresses survive restart. Refresh tokens survive restart only in builds with a working platform keychain; other builds display a session-only notice.

On a slow or metered connection, tick **Use low bandwidth mode** under *Connection options for slower links* before connecting, and consider Opus audio compression — see [Low Bandwidth Connections](./low-bandwidth-connections.md).

## Certificate pinning

AetherSDR pins each SmartLink radio's TLS certificate on first use, to protect you from a man-in-the-middle.

- The first time you connect to a radio over SmartLink, AetherSDR stores the SHA-256 fingerprint of its certificate.
- On later connections the certificate must match. If it does not, the connection **pauses before authentication** and a warning shows the expected and presented fingerprints with two choices:
  - **Accept new certificate** — only if you know the certificate changed (for example after a firmware update or a radio replacement);
  - **Reject and disconnect** — the default.
- Nothing is sent that would authenticate you until you accept, so an impostor never gets a working session.

### Managing pins

**Settings → Radio Setup... → SmartLink → Pinned SmartLink Certificates** lists each pinned radio with its **Host**, **SHA-256 Fingerprint** and the date it was **Pinned**.

- **Forget Selected** removes one pin.
- **Forget All** clears every pin after confirmation. Each radio is pinned again, silently, on its next connection.

Forget a pin only when you have deliberately changed a radio's certificate.

## Reference

### Security

- Sign-in uses FlexRadio's identity provider, and all SmartLink connections use TLS with certificate pinning (above).
- Your password is not stored. The SmartLink **refresh token** is saved in the operating system's keychain, so AetherSDR signs you in automatically on the next launch. Builds without keychain support keep it for the current session only. See [Settings and Backups](./settings-and-backups.md).
- Logs redact email addresses, tokens, IP addresses and serial numbers — see [Support and Logging](./support-and-logging.md).

### UDP session maintenance

AetherSDR registers the SmartLink UDP endpoint every 50 ms until it receives a packet with a complete Flex VITA-49 preamble, including compressed audio. It then sends a registration keepalive every five seconds for the rest of the connection to maintain the NAT mapping. Disconnecting stops these keepalives; reconnecting registers the new session. This happens automatically and needs no client-side setting.

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

1. At the location, open SmartSDR for Windows → **Settings** → **SmartLink Setup**.
2. Check that the radio is registered, and click **Test**: both indicators should be green.

### The connection drops after about 10 seconds

Port forwarding at the location is probably not set up correctly.

1. Check that TCP 4994 and UDP 4993 are forwarded to the radio's LAN IP address.
2. Run the SmartLink **Test** in SmartSDR again.

### The radio is listed, but you cannot connect

TCP 4994 is probably not forwarded to the radio.

1. Check the TCP 4994 forward on the location's router.

### A certificate mismatch warning appears

The radio presented a different certificate from the one AetherSDR pinned. See [Certificate pinning](#certificate-pinning).

1. If you did not change anything at the location, choose **Reject and disconnect**.
2. If you know the certificate changed (for example after a firmware update or a radio replacement), choose **Accept new certificate**.

## See also

- [Tailscale Remote Access](./tailscale-remote-access.md)
- [Low Bandwidth Connections](./low-bandwidth-connections.md)
- [Manual Connection](./manual-connection.md)
- [Support and Logging](./support-and-logging.md)
- [FlexRadio](./flexradio.md)
