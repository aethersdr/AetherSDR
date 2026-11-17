---
title: "Tailscale Remote Access"
slug: "/tailscale-remote-access"
description: "Operate a FlexRadio from anywhere: a small service installed inside the radio, as a container or a waveform, joins your private Tailscale network."
---

Operate a FlexRadio from anywhere: a small service installed **inside the
radio** joins your private Tailscale network. FLEX-8000 series and Aurora
radios run it as a container; FLEX-6000 series radios run it as a legacy
waveform. It works
behind carrier-grade NAT (CGNAT), Starlink, cellular home internet or any
router whose ports you can't forward, and it needs no SmartLink account.
The radio needs firmware **4.2.18 or later**.

AetherSDR installs and sets it up for you from the **Waveforms**
window. After that you connect from anywhere with **Connect by IP**, as if the
radio were on your desk.

> ### 💲 It costs nothing
> Tailscale's **Personal plan is free forever**, for non-commercial use (which
> amateur radio is). It covers up to **6 users** and **unlimited devices**,
> and includes the **subnet routing** used here to share station
> accessories. A typical ham station (the radio plus your laptop, phone and
> tablet) fits easily, and you never need a paid plan for this. The
> container and AetherSDR are free and open source too.
>
> **Sign up with a personal login** (Gmail/Google, Apple, a personal
> Microsoft or GitHub account) to land on the free plan. A login on your own
> or your employer's domain is treated as a business and starts a business
> trial instead.

> **Radios:** FLEX-8000 series and Aurora, which can run Docker waveform
> containers, on firmware **4.2.18 or later** (see
> [Firmware Update](./firmware-update.md)). It was developed and measured
> on a FLEX-8600 (firmware 4.2.20); the FLEX-8400 and Aurora have not been
> tested yet.
>
> **FLEX-6000 series** radios can't run containers, so they get the same
> service as a legacy waveform instead (see
> [On a FLEX-6000](#on-a-flex-6000)). It has run on a FLEX-6400; the
> FLEX-6300 and 6700 passed the compatibility probe but haven't run it yet.

## What Tailscale is

[Tailscale](https://tailscale.com) is a private network service built on
[WireGuard](https://www.wireguard.com), a modern encrypted VPN protocol.
Every device you add (your laptop, your phone, and here your radio) joins
one private network called a **tailnet** and gets a fixed address in the
`100.x.y.z` range. The address stays the same wherever the device is.

Some things to know:

- **No port forwarding.** Devices find each other through Tailscale's
  coordination servers and then connect **directly**, punching through NAT
  where they can. When no direct path exists (two strict NATs, for example),
  traffic goes through Tailscale's **DERP relay servers**. It still works,
  just with more delay.
- **End-to-end encrypted.** Traffic is encrypted between your devices with
  WireGuard. A relay passes encrypted packets along and can't read them.
- **Private by default.** Only devices signed in to *your* tailnet can reach
  the radio. Nothing is opened to the internet.
- **Free for personal use.** The **Personal** plan is free forever for
  non-commercial use: up to **6 users**, **unlimited devices**, and subnet
  routers (used here to share station devices) all included. Check
  [tailscale.com/pricing](https://tailscale.com/pricing) for the current
  terms.

### Tailscale or SmartLink?

| | Tailscale (this page) | [SmartLink Setup](./smartlink-setup.md) |
|---|---|---|
| Port forwarding at the station | Not needed; works behind CGNAT | Required (TCP 4994, UDP 4993) or UPnP |
| Account | A **free** Tailscale Personal account | A FlexRadio SmartLink account |
| Radios | FLEX-8000 series and Aurora (firmware 4.2.18 or later), FLEX-6000 series | All FlexRadio models with SmartLink |
| Station accessories (Antenna Genius, PGXL, TGXL) | Reachable remotely | Not reachable |
| What the remote computer needs | The Tailscale app | Nothing extra |

## Part 1 — Create a free Tailscale account

Tailscale has no passwords of its own. You sign in with an account you
already have. Sign in with a **personal** account and you're on the **free
Personal plan** automatically, with nothing to cancel later.

1. Go to [tailscale.com](https://tailscale.com) and choose **Get Started**
   (or **Sign up**).
2. Pick a sign-in method: **Google**, **Microsoft**, **GitHub**, **Apple**, a
   **passkey**, or another single sign-on provider (Okta, OneLogin, or a
   custom OpenID Connect provider). Email-and-password sign-up isn't
   offered. **Use a personal account** (a Gmail, Apple, personal Microsoft
   or personal GitHub login): Tailscale treats a login on a custom domain,
   such as `you@yourcall.com` or a work address, as a business and puts it
   on a trial rather than the free plan.
3. When asked, choose **personal** use. That's the free plan. This creates
   your tailnet, and you become its owner. Skip any business or trial
   offers; you don't need them.
4. Tailscale offers to install the app on your first device. Do that on the
   computer you'll operate from (next part).

The **admin console** at
[login.tailscale.com/admin](https://login.tailscale.com/admin) is where you
manage the tailnet: the **Machines** page lists every device, and
**Access controls** decides which devices and people may reach which.

## Part 2 — Put your operating computer on the tailnet

Install Tailscale on every computer, phone or tablet you'll run AetherSDR
from:

1. Download it from [tailscale.com/download](https://tailscale.com/download)
   (Windows, macOS, Linux, iOS and Android are all available).
2. Install it, open it and **log in** with the same account as Part 1.
3. The device appears on **Machines** in the admin console with its own
   `100.x.y.z` address.

**Linux:** to reach shared station devices (Part 5), also run
`sudo tailscale set --accept-routes` once. Windows, macOS, iOS and Android
accept shared routes automatically.

## Part 3 — Install the container on the radio

Do this **at the station**, with AetherSDR connected to the radio over its
local network.

<img src="/img/screens/waveforms-dialog.png" width="980" alt="Waveforms window. A Connected Radio card reads FLEX-8600 FLX8600 with its serial blacked out, beside WFP Support Supported, WFP Power ON, WFP Ready READY and WFP IP (blacked out). Radio Waveform Processor explains legacy and Docker waveform packages. Installed Waveforms has an Install... button and one entry, flex-tailnet-shim 0.4.0, with Docker, Configure..., Restart and Remove buttons." />

*The Waveforms window, listing the waveforms installed on the radio. The serial number and the waveform processor's address are blacked out.*

When the installed list is empty, **No radio waveforms installed** appears
with the hint **Use Install... to add waveforms to your radio.**

1. Open **Tools → Waveforms...**.
2. Click **Install...** and choose **Remote Access (Tailscale)**.
3. AetherSDR downloads the container image (about 8 MB) and checks its
   size and SHA-256 against the values built into AetherSDR. Any other file
   is refused, and nothing is installed.
4. The image uploads to the radio and appears in the installed list as
   **flex-tailnet-shim**, with a **Configure…** button.

The container starts with the radio from then on, about 20 seconds after
power-on.

**Without internet on the AetherSDR computer**, download
`flex-tailnet-shim-<version>.tar.gz` from the AetherSDR GitHub release
tagged `flex-tailnet-shim-v<version>`. Then use **Install... → Docker
Waveform Image...** and pick that file.

### On a FLEX-6000

The same **Install... → Remote Access (Tailscale)** works. On a radio with no
Waveform Processor, AetherSDR downloads the legacy waveform package instead
(`AetherTailnet-<version>-native.ssdr_waveform`, about 9 MB), checks it the
same way, and installs it. It appears in the installed list as
**AetherTailnet**, marked **Legacy**, with a **Configure…** button.
Everything after that, signing in, sharing station devices and connecting
remotely, is the same as on a FLEX-8000.

Things that differ on a FLEX-6000:

- **Space.** The waveform needs about 9 MB on the radio's waveform storage.
  A FLEX-6400 or 6700 has only 13–17 MB free there, so it may not fit
  alongside other legacy waveforms. If the install fails for space,
  uninstall a waveform you don't use and try again.
- **One CPU core.** The radio's processor is shared with SmartSDR, so the
  service runs at a lower priority and the radio always comes first.
- **Without internet on the AetherSDR computer**, download the
  `.ssdr_waveform` file from the same release and use **Install... → Legacy
  Waveform (.ssdr_waveform)...**.

The container can run alongside other waveform containers, such as FreeDV.
AetherSDR has its own RADE digital voice, so it doesn't need the FreeDV
container (see [RADE Digital Voice](./rade-digital-voice.md)).

Waveform installation stays with the radio connection that started it. If the
radio reconnects while you choose or prepare the package, start the install
again. Another firmware, profile or Radio DVK transfer must finish first.
Upload completion means the bytes were sent; the radio's installed-waveform
list confirms when the waveform becomes available.

## Part 4 — Sign the radio in to your tailnet

### Scan the code

1. In **Waveforms**, click **Configure…** on the **flex-tailnet-shim** row.
   The **Remote Access** window opens, and a **QR code** appears by itself
   within a few seconds.
2. Scan the code with your phone's camera, sign in to Tailscale with the
   account from Part 1, and approve the radio. No phone handy? **Copy Link**
   and open the link in a browser instead.
3. The window turns to **On your tailnet**. If your tailnet requires an
   admin to approve new devices, it first says **Waiting for approval**:
   approve the radio on **Machines**.

The code is shown only to the computer that started the sign-in. Don't share
it: whoever opens it first adds the radio to *their* tailnet. If no code
appears, click **Refresh**.

The radio joins under a name taken from its nickname (for example
`flx8600`). Rename it on **Machines** if you like.

<img src="/img/screens/tailnet-remote-access.png" width="600" alt="Remote Access window. The status line reads On your tailnet, container 0.5.0, 0 remote session(s), above an explanation of remote access over Tailscale. The Tailnet group shows the tailnet name and address (both blacked out) with a Copy button, Remote sessions 0, Container version 0.5.0 and Sign-in expires Thursday, April 29, 2027, with a note on turning off key expiry in the Tailscale admin console. Station devices lists a Tuner Genius XL and an Antenna Genius (addresses blacked out) with a Share the devices found here over the tailnet checkbox, an Other devices field, a note that their routes are offered but not yet confirmed, and Save Sharing. Refresh, Disconnect and Done close the window." />

*The Remote Access window with the radio on the tailnet. Tailnet names, addresses and station device addresses are blacked out.*

When the status reads **On your tailnet**, the **Tailnet** card shows:

| Field | Meaning |
|---|---|
| **TAILNET NAME** | The radio's name on your tailnet |
| **TAILNET ADDRESS** | The `100.x.y.z` address to connect to. **Copy** puts it on the clipboard. |
| **REMOTE SESSIONS** | Clients connected through the tailnet right now |
| **CONTAINER VERSION** | The container's version (**WAVEFORM VERSION** on a FLEX-6000) |
| **SIGN-IN EXPIRES** | When the radio's sign-in runs out, or **Never** |

Write the tailnet address down: it's what you connect to when you're away.

### Turn off key expiry

A device signed in this way has to sign in again every **180 days** by
default. Until it does, it drops off your tailnet, and it can only be signed
in again from the station. To keep the radio on for good, open **Machines**
in the admin console, click **⋯** on the radio's row and choose **Disable key
expiry**. **SIGN-IN EXPIRES** then reads **Never**.

If it does expire, open **Remote Access** at the station: a new code appears,
and scanning it brings the radio back.

### Advanced: join with an auth key

An **auth key** signs the radio in without a phone. A key with a **tag** also
makes a radio whose sign-in never expires and that your access rules can
address by tag.

1. In the admin console open **Settings → Keys**, then **Generate auth
   key**. Leave **Reusable** and **Ephemeral** off; turn on
   **Pre-approved** if your tailnet requires device approval; optionally
   add a **tag** (for example `tag:radio`).
2. Click **Generate key** and copy it. It starts `tskey-auth-` and is shown
   only once.
3. In **Remote Access**, open **▸ Advanced: use an auth key**. Check
   **MACHINE NAME** (lowercase letters, digits and dashes), paste the key
   into **AUTH KEY** and click **Join with Key**.

The key is sent to the radio once and kept nowhere: not in AetherSDR, not in
its settings, not in its logs.

A container older than 0.5.0 can only join with a key: the window opens
Advanced for you and suggests updating the container (remove it, then
install again from Part 3).

### The admin token

The first sign-in returns an **admin token** that AetherSDR stores in your
system keychain (Windows Credential Manager, macOS Keychain, or the Secret
Service / GNOME Keyring on Linux), keyed to this radio's serial number. From
then on the container accepts changes only from a computer that holds it:
device sharing, disconnecting, or joining again.

A rejoin can replace a legacy container's admin token. A delayed rejection of
the older token does not remove the replacement saved on this computer.

- The first setup is open to anyone on the radio's local network, the same
  trust the radio's own API uses there. So is a sign-in that was started but
  never finished.
- A computer that doesn't hold the token (a second computer, for example)
  shows **Reset** instead of **Disconnect** (Part 7).

## Part 5 — Share station devices (optional)

The container listens on the radio's network for **Antenna Genius**, **Power
Genius XL** and **Tuner Genius XL** units and offers them to your tailnet, so
their applets work remotely too.

1. In **Remote Access**, the **Station devices** card lists what it found.
   **Share the devices found here over the tailnet** is on by default.
2. Turning sharing off for a device, or removing it, ends any remote
   connection to it at once.
3. To share other devices on the station network (for example an amplifier
   with a web page), list their LAN addresses in **OTHER DEVICES**, separated
   by commas, then click **Save Sharing**.
4. **Approve the routes once** in the admin console: **Machines →** the
   radio **→ Subnets → Edit → Edit route settings**, tick the routes and
   **Save**. The window shows **Offered, not yet confirmed** until a remote
   connection actually travels through a route; after that it shows
   **Confirmed in use over the tailnet**.
5. Remotely, set each device's applet to its **LAN address** (for example
   `192.168.1.40`), exactly as at home. Your tailnet's access rules apply
   to the devices as well as the radio. See [Amplifiers](./amplifiers.md) and
   [TGXL Tuner Control](./tgxl-tuner-control.md).

Remember `--accept-routes` on Linux computers (Part 2).

## Part 6 — Connect from anywhere

1. Make sure Tailscale is running and logged in on the remote computer.
2. In AetherSDR open **File → Connect to Radio...** and choose **Connect by
   IP** (see [Manual Connection](./manual-connection.md)).
3. Set **Radio type:** to **FlexRadio**, enter the radio's **tailnet
   address**, and click **Connect by IP**.

The container passes the session through to the radio: control, audio,
panadapter, waterfall, meters, DAX, and the extra connections the radio opens
on demand (file transfers, for example). Each remote client gets its own
session, and the radio sees an ordinary client.

### Audio and bandwidth

- **Opus by default.** A radio reached at a tailnet address gets **Opus**
  audio compression unless you have chosen otherwise, as SmartLink does under
  **Auto**. **Radio Setup** shows which setting applies; an explicit choice
  always wins. See [Audio Settings](./audio-settings.md).
- **MTU.** The container sets the radio's network MTU to 1200 bytes for its
  sessions, so VITA-49 packets fit inside the WireGuard tunnel without
  fragmenting. AetherSDR asks for the same 1200 itself whenever this
  computer or the radio has a Tailscale address, including a subnet router
  in front of a radio that can't run the container.
- **Measured on a relayed path** (both ends forced through a DERP relay):
  connection in under a second, a median round trip of 67 ms, and about
  3.5 Mbit/s uncompressed or 1.9 Mbit/s with Opus. A direct path is faster.
  For slow links, see [Low Bandwidth Connections](./low-bandwidth-connections.md).

### Other clients

The container relays the standard SmartSDR protocol, so other clients that
can **connect by IP** from a computer running Tailscale also work through
it. SmartSDR for Windows has been used this way. A device that can't run
Tailscale itself can't reach the tailnet.

## Network Diagnostics

While you're connected over the tailnet, **Network Diagnostics** adds the
radio side's view of the link, reported by the container:

- **Overview → Latency** card: a second line such as *Tunnel 42 ms, DERP
  sea*.
- **Connection Details → Remote Link (Tailscale)**:
  - **Path**: **Direct** (with the address), **Peer relay**, or **Relayed
    (DERP *region*)**. Relayed is slower; allowing outbound UDP on both ends'
    firewalls often lets Tailscale find a direct path.
  - **On This Path**: how long the current path has held, and how many times
    it changed.
  - **Radio-Side RTT**: the container's own round trip to your computer.
  - **To / From This Client**: throughput through the tunnel.
  - **Breaks Before Tunnel**: VITA-49 sequence breaks that happened inside
    the radio.
  - **Breaks Added by Tunnel**: breaks between the radio and your computer.
    These two separate radio-side loss from internet loss.
  - **Send Failures**: packets the container couldn't hand to the tunnel.
  - **Container**: version, CPU, memory, and the MTU clamp.
- **Trends**: a **Tunnel RTT** trace on Latency & Jitter, and **Added by
  tunnel** on Packet Loss.

The container answers these requests only on the tailnet, and only with the
asking computer's own session.

## Part 7 — Update, disconnect, reset or remove

:::warning

**Don't do any of these while you're away from the radio.** Each one takes the radio off your tailnet, and signing it in again takes the radio's local network. Until then you can't reach it remotely.

:::

- **Update** (shown when a newer version is available): removes the container (on a FLEX-6000, uninstalls the
  waveform) and installs the new one, after asking. Open **Remote Access** again and scan the new code.
- **Waveforms → Environment… → Apply & Recreate** on the remote-access container: rebuilds it from its
  image, which may sign the radio out. AetherSDR asks first.
- **Disconnect** (shown once the radio is on the tailnet): ends remote
  sessions, signs the radio out and deletes its tailnet state and token. The
  next time you open **Remote Access**, a new code appears to sign it in
  again, to the same tailnet or another. If the container can't delete its
  saved settings, it changes nothing and says so, and AetherSDR keeps the
  token.
- **Reset** (shown instead of Disconnect when this computer doesn't hold the
  admin token): removes the container (on a FLEX-6000, uninstalls the
  waveform) and installs it again, after asking.
  The radio leaves your tailnet; open **Remote Access** again and scan the
  new code. Delete the old entry from **Machines** in the admin console.
- **Remove the container** (on a FLEX-6000, **Uninstall** the
  **AetherTailnet** waveform) from the **Waveforms** list. That also wipes
  its tailnet state. Then delete the radio from **Machines** in the
  admin console.

On a FLEX-6000, Reset and Uninstall rely on the radio deleting the
waveform's saved state with its files, which hasn't been confirmed on a
radio yet. If **Remote Access** still shows the radio on your tailnet after
a Reset, please report it.

Settings can only be changed **from the radio's local network**. The window
shows **Unavailable** when AetherSDR is connected to the radio over the
tailnet itself. Remotely you can operate, but not reconfigure, the
container.

## Security

- **Anyone your tailnet lets reach the radio can operate it, including
  transmitting.** By default that is every device in your tailnet. If you
  share the tailnet (family, club members), limit who may reach the radio
  with **Access controls** in the admin console; tag the radio with an auth
  key (Part 4) to write rules for it by tag.
- Containers before 0.5.0 kept their own **Who may connect** list. Version
  0.5.0 drops it when it starts, so after updating, set any restriction with
  your tailnet's access rules instead.
- The sign-in code is a link that adds the radio to whichever tailnet opens
  it first. AetherSDR shows it only to the computer that started the sign-in
  or holds the admin token; the container never logs it.
- A file transfer's side channel is open only to the computer that asked for
  the transfer.
- Shared devices never reach the radio itself. A route wide enough to
  include the radio's own address doesn't make the radio's ports reachable
  through it.
- A discovered device is shared at the address its announcement came from,
  not an address the announcement names. Discovery trusts the station LAN,
  as the radio itself does: a computer there can still announce fake
  devices, up to 16. If a real device is missing, list it under **OTHER
  DEVICES**.
- On the tailnet the container exposes only the radio's own ports, its
  forwarded side channels, shared station devices, and the diagnostics
  report. Its setup interface listens only on the radio's local network.
- The sign-in link, an auth key and the admin token travel over the station
  LAN unencrypted, to the radio's local address. A key is single-use and
  spent within seconds; the container stores only a hash of the token.
- Over the internet, everything travels inside WireGuard and is encrypted end
  to end.

## Ports and resources

| Where | Port | Use |
|---|---|---|
| Tailnet | TCP 4992 | Radio control (SmartSDR API) |
| Tailnet | UDP | VITA-49 audio, panadapter, waterfall, meters, DAX (per session) |
| Tailnet | TCP, as the radio opens them | Side channels such as file transfer |
| Tailnet | TCP 48993 | Network Diagnostics report (`GET /v1/session`) |
| Tailnet, via shared routes | UDP 9007 / 9008 / 9010 discovery, device TCP ports | Antenna Genius / PGXL / TGXL |
| Radio LAN only | TCP 48992 | Setup interface used by **Remote Access** |

The container runs on the radio's own processor. On a relayed link it used
about a fifth of one core and around 34 MB of memory.

## Troubleshooting



- **The container won't install or start**: check that the radio runs
  firmware 4.2.18 or later (see [Firmware Update](./firmware-update.md)).
- **Remote Access says "Unavailable"**: connect to the radio over its
  local network, not over the tailnet, to change settings.
- **No QR code appears**: click **Refresh**. A container older than 0.5.0
  can't show one; update it (Part 4).
- **"Waiting for approval"**: your tailnet requires an admin to approve new
  devices. Approve the radio on **Machines**.
- **The radio dropped off the tailnet after months**: its sign-in expired.
  Open **Remote Access** at the station and scan the new code, then turn off
  key expiry (Part 4).
- **"Doesn't look like a Tailscale auth key"**: paste the whole key,
  starting `tskey-auth-`.
- **"Could not join the tailnet"**: the key is used up, expired or
  revoked. Generate a new one. If your tailnet requires device approval,
  approve the radio on **Machines**, or use a **Pre-approved** key.
- **Disconnect is missing and the sharing settings are grayed out**: this
  computer doesn't hold the admin token. Use the computer that set the radio
  up, or **Reset** (Part 7).
- **Can't connect remotely**:
  - check that Tailscale is running on the remote computer, that the radio
    shows as connected on **Machines**, and that your tailnet's access rules
    let you reach it;
  - right after a radio reboot, give the container about 20 seconds.
- **Spectrum or audio stutters**:
  - check **Network Diagnostics → Remote Link** for a relayed path;
  - keep Opus on, and see [Low Bandwidth Connections](./low-bandwidth-connections.md).
- **Station device applets can't reach the device**:
  - approve its route in the admin console;
  - on Linux, run `sudo tailscale set --accept-routes`;
  - set the applet to the device's LAN address.
- Before reporting a problem, turn on the relevant log categories in **Help →
  Support & Diagnostics...** (see [Support and Logging](./support-and-logging.md)).

### AetherPD diagnostics in development builds

Development builds of the shim can report an on-radio AetherPD waveform's
state through authenticated diagnostics. A
`recalibration_required` result means correction has been bypassed after a
transport fault and is waiting for a validated model. `unavailable` means the
shim could not obtain a valid current report. Installing the released
Tailscale service does not install the experimental AetherPD waveform.

Diagnostics retain the latest alignment reset separately from the latest
alignment estimate, so reacquisition does not erase the reset evidence.
These records belong to the current AetherPD process; collect them before
stopping the session. They do not indicate that correction is active.

Development diagnostics can also retrieve a short raw DAX IQ snapshot from
the session already running on the radio. This does not transmit or change
the IQ stream. Snapshots expire and cannot be retrieved after the session is
stopped or replaced. The development **Measure linearity…** window uses this
raw DAX IQ path. It is not a released or hardware-qualified measurement feature.

In development builds with a prepared local AetherPD session, authenticated
stop ends the process owned by that shim. A stop request is acknowledged
before the independent guardian finishes restoring a safe radio mode. The
shim leaves separately started AetherPD processes alone.

The development service can accept an explicit start request bound to your
current radio connection. Starting never keys the radio or arms calibration.
After a reboot, it needs the new connection identity; it cannot reuse a saved
session handle. Automatic AetherPD startup and installation are still in
development. A missing or unrelated waveform is reported
as unavailable.

In development builds, open **Tools → Waveforms... → AetherPD…** to control a
prepared installation. The window loads this computer's saved Remote Access
credential for the connected radio. Opening it reads status only.

- **Feedback DAX IQ** lists pans owned by this connection with **IQ Ch 1**
  through **IQ Ch 4** assigned. Choose the feedback pan explicitly. Changing this
  choice sends no radio command and is not saved across connections. If the
  pan's channel or owner changes, choose it again after correcting the assignment.
- **Start AetherPD** requests that exact pan/channel for the current connection.
  The installed service must confirm the same binding. Older services that lack
  selected startup are refused; the desktop does not retry with channel 1. It does
  not transmit or arm calibration. A start receipt is followed by a status
  read; it is not confirmation that correction is running. Startup checks the
  idle state for your connected GUI and refuses to continue if its identity
  or fresh radio status cannot be confirmed. After status reports that a
  session ended, Start can request a new session without an extra Stop click.
  The old cleanup warning remains a warning; restarting does not prove that
  cleanup succeeded. The service retires the old control channel and checks
  ownership again before admitting the new session. It never restarts or
  rearms a source automatically.
- **Arm calibration** requires fresh, unkeyed status and radio APD off.
  **Cancel calibration** withdraws the calibration request.
- **Stop AetherPD** can interrupt startup. Closing the window leaves the
  session running; use Stop when you want it stopped. **Stopped — session
  cleanup confirmed** appears only after the service confirms the DSP and its
  cleanup workers exited cleanly. **Session ended — cleanup unconfirmed**
  means the service cannot confirm clean termination, including an abnormal
  DSP exit or a cleanup failure or timeout. **Stopped — receive recovery unconfirmed**
  means DSP and worker retirement completed, but the receive helper exited
  without confirming restored receive settings. It can refuse recovery to
  protect another slice's DAX assignment; check your receive settings. This
  status does not automatically retry restoration. Successful IQ delivery does
  not confirm receive cleanup; that requires its own acknowledgments and radio
  readback. A Stop receipt alone confirms
  neither shutdown nor unkey. These completion messages describe the owned
  session; check the radio's transmit indicator for its current RF state.
- **Restore receive** explicitly requests the checked receive recovery
  described below. It is never triggered by opening or closing the window.
- **Refresh** reads status or reloads a missing/rejected credential. After a
  radio reconnect, close and reopen the window for the new connection.

If a development measurement client arms the shared source, the window can
show **Measurement tones armed — waiting for your transmission**.
**Cancel calibration** also cancels that source. An arm receipt does not
confirm transmission or correction.

In the development AetherPD window, **Measure linearity…** opens the measurement
controls. This workflow remains under qualification; it does not establish
ADC headroom or calibrated output power.

1. Connect the dummy load and attenuated feedback path, select your TX slice,
   frequency and power, and leave FDX off. In the feedback panadapter's **DAX**
   panel, assign an available channel from **IQ Ch 1** through **IQ Ch 4**.
   In the AetherPD window, choose that pan under **Feedback DAX IQ**, then click
   **Start AetherPD**. Wait for **Service reports … binding confirmed** before
   proceeding, then select **PD** mode on your TX slice. The installed local-IQ session binds to the stream's reported
   feedback panadapter and verifies that this GUI owns it. Removing the pan or
   reassigning its channel or owner ends that binding; stop and start AetherPD
   explicitly after correcting the assignment. Another pan reporting the same
   channel cannot take its place. **Waiting for DAX IQ binding** means the
   service has not yet observed matching pan ownership. **DAX IQ binding failed —
   stop and start AetherPD again** means that binding was lost. Arming and
   correction controls remain dimmed while binding is invalid; correct the
   assignment, then explicitly stop and start the session. Confirm **Dummy load connected;
   feedback attenuation checked**.
   Before arming, you can choose **Radio power setting (W)** and click
   **Apply power**. This requests the nominal setting without transmitting;
   wait for **Radio reports … W setting** before continuing. Actual RF output
   may differ. The control requires radio-confirmed power and maximum-power
   readback and a fresh AetherPD service status. It locks while a source is
   armed, a transmission or measurement is active, or installation is pending.
   The analyzer does not remember or restore this radio setting.
2. Choose **Measured forward-power abort limit (W)**. This is a measured-power
   cutoff, not a command to change radio drive. The nominal radio power setting
   must be within the limit. The active band plan and license-class filter must
   cover both the tones and the wider alignment-noise band.
3. Choose **Tone spacing (Hz)** (200–10000 Hz, default 1190). The pair is
   centered 1305 Hz above the carrier; odd spacings produce half-Hz offsets.
   The display separates requested offsets from the shim's reported profile.
   Editing sends no command. The draft lasts for this window only.
   Older shims reset the draft to 1190 Hz, keep the selector disabled with a
   reason, and use 710/1900 Hz.
   Click **Arm measurement source**, then **Prepare receive check** once the
   arm is observed. These actions do not transmit. Preparation downloads a
   complete receive-only IQ frame.
4. Click **Transmit measurement** within two seconds of readiness. This is the
   explicit RF action. An independent cleanup helper must be ready before the
   original-session TX request is dispatched. The source emits four seconds of
   alignment noise followed by the confirmed tone pair, for at most eight seconds.
   Before preparing, choose **Analysis window**, **Averages** (one to four frames)
   and **Highest order** (IMD3, IMD5 or IMD7). **Transmit timeout (s)** defaults
   to ten seconds and can be shortened to three through nine seconds. It counts
   from **Transmit measurement**, including startup. Short limits may stop
   before alignment or capture completes; they never extend the source window.
   These client measurement preferences are remembered separately for each
   radio; they do not change its power, tones or IQ stream. Analysis defaults
   are Blackman–Harris 7, four frames and IMD7.
   **Arm measurement source** submits the chosen spacing when the shim
   supports it. A rejected configured request never falls back to fixed tones.
   Read-only status refreshes do not block this button. Wait for the source's
   armed readback before preparing; an arm receipt alone does not confirm it.
   The selector locks while arming, armed, transmitting, measuring or installing.
   Preparation uses the confirmed frequencies and requires
   matching metadata on every capture. A configured capture never falls back
   to a legacy endpoint. Arming and preparation refuse a highest IMD order outside the
   IQ bandwidth, a fundamental overlapping the analyzer's DC exclusion,
   or an IMD product overlapping DC leakage;
   use a narrower spacing or lower order as appropriate.
   The controls lock while a measurement is pending. More averages reduce
   estimate variance, not the physical noise floor; the four-frame ceiling
   reserves time for snapshot retrieval within the bounded source window.
   Window choice changes leakage and resolution, so no window guarantees a
   particular measurable IMD depth.
   A completed receive check shows the sampled DAX IQ stream ID when the shim
   reports it. Older shims show **DAX IQ stream identity not reported**. The ID
   does not identify a DAX channel number or antenna. The measurement rejects
   a changed stream, or a changed acquisition generation between averaged frames.
   Capture waits for the guarded tone interval and collects the selected frames
   consecutively on the radio. Once the complete batch is stored, the desktop
   requests unkey before downloading the samples. Downloads have a separate
   six-second limit and cannot extend transmission. Results require every
   requested frame and confirmed release; stopping or losing the original
   session discards unfinished downloads. This workflow requires the matching
   batch-capable AetherPD shim; older shims cannot perform the measurement.
   A cold
   correction fit can use most of the source window; capture waits within that
   eight-second window and reports a failure if enough qualified frames do not
   arrive. The independent helper initiates cleanup on its fixed timer;
   ownership verification and the radio's response can delay actual unkeying.
   A delayed startup reply cannot make the helper ready. Transmit authorization
   expires within the selected timeout, never more than ten seconds from
   **Transmit measurement**, and the helper schedules cleanup earlier. Expiry requests stop and
   ends capture; the panel waits for confirmed release. If the independent helper
   unkeys while the desktop is stalled, the desktop can confirm release after
   it resumes, including when ownership is still clearing. This requires the
   complete original release sequence, its own stop acknowledgment and complete
   idle readback after that acknowledgment.
   The interrupted measurement is not restarted; another transmission requires
   **Arm measurement source**, a new receive check and **Transmit measurement**.
   Missing release evidence keeps the measurement in recovery.
   Process scheduling or
   radio/API delays can postpone actual RF release, so a ten-second physical
   RF-off guarantee remains unqualified. Routine status
   polling keeps **Prepare receive check** available while the last status is
   fresh; a failed status request or pending source change disables it.
5. **Stop measurement** requests release of that measurement. Closing the
   window also requests stop; it does not stop AetherPD correction. After confirmed
   release, another measurement requires a fresh arm and receive check; it does
   not require reconnecting the desktop. A pending
   release remains visible on reopening. An unused source arm expires after
   30 seconds, or **Cancel source** withdraws it immediately.
   If a running measurement source ends or is canceled before unkeying completes,
   its output stays silent until the radio reports READY. It does not hand the
   still-keyed measurement over to microphone audio. This differs from voice
   calibration, which deliberately returns to the microphone.
6. Results appear only after confirmed release of the original transmission.
   Background helper connections do not complete that confirmation; AetherSDR
   waits for the original transmission's stop sequence from the radio.
   A radio ownership update while the key command is awaiting acknowledgment
   does not complete or cancel the measurement; the acknowledged transmit and
   stop transitions are still required.
   **Export result…** offers JSON, CSV and (for a valid spectrum) PNG. JSON contains the station settings, source and capture
   identities, timestamps, correction state, spectrum, noise limitations and
   reference conventions. Each completed capture includes its original IQ
   samples and a SHA-256 checksum for offline analysis. JSON and CSV also retain
   each capture's reported IQ stream ID and acquisition generation. Captures from
   the selected local-IQ service also retain the confirmed channel, feedback pan
   and GUI owner in JSON, CSV and the PNG description. These are recorded with
   the samples; later receiver changes do not relabel an exported capture.
   Unavailable identities remain explicitly unknown. All export formats include the selected
   transmit authorization timeout; it does not certify the measured RF duration. Exporting collects no
   new samples and never transmits. IMD is labeled both per tone and relative to
   fundamental-derived PEP; the separately measured sample-peak reference is
   retained in the export. Raw IQ levels are not calibrated dBFS or watts.
   The spectrum marks the two tones, IMD products, noise floor and shoulder/ACPR
   integration bands. The vertical scale is uncalibrated raw IQ; product labels
   use dBc relative to one tone. PNG saves this plot with capture settings and
   measurement limitations in its image description. Invalid captures clear the plot.

   CSV contains the measurement table, station settings, explicit reference
   units, noise floor, shoulder and ACPR bands. Unknown values and noise-limited
   products have blank numeric cells with a stated status. CSV excludes the raw
   IQ and spectrum arrays; use JSON to retain those for independent analysis.

For a matched comparison, use **Bypass correction** in the AetherPD window
before arming the uncorrected measurement. Then use **Enable correction** and
prepare a separate corrected measurement with the same frequency, power and
feedback settings. These controls require a supporting installed service,
fresh idle status and no armed source. They never key or arm the radio.
A change invalidates the previous fit and measurement preparation; wait for
observed status before continuing. The selection applies to the current
AetherPD session; a newly started session enables correction by default.

An uncorrected measurement waits for fresh, aligned feedback IQ and settled
tones; it does not require a correction model. A corrected measurement also
waits for a usable correction model. Stale IQ or a changed TX stream prevents
either capture from being accepted.

No source arm or transmission is retried automatically. A changed source,
frequency, power, session, band-plan selection or revoked original automation
input cancels the preparation or run. A new receive check is needed after
readiness expires. Changing correction state requires a separately prepared
run; compare exported runs made with matching station settings.

During a measurement, PA temperature and supply voltage samples expire after
three seconds, accommodating the radio's slower updates. Forward-power and
SWR samples retain a two-second limit. Missing or expired required telemetry
stops the measurement; rereading a cached value does not extend its lifetime.

If a measurement stops for SWR, power, temperature, voltage or stale telemetry,
the application log includes `TX linearity safety abort` with the exact readings,
their sample ages and the limits used for that decision. Use this record when a
brief spike is no longer visible on the meters. Missing readings remain marked
as missing; the record reports the radio's telemetry and does not independently
verify the load or confirm that transmission has stopped.

If the source request is refused, check **PD** mode, the pan and IQ channel
selected under **Feedback DAX IQ**, fresh DAX IQ and the transmit interlock
before trying again.
The failure notice remains visible across status refreshes. If an arm or
cancellation is unconfirmed, check the service status; **Cancel source**
withdraws an arm and **Stop AetherPD** ends the owned source.
If transmission is refused, the window shows the admission or telemetry
reason. Resolve it and prepare a fresh receive check before another attempt.

**Status unknown** means there is no fresh observation. The window does not
retry Start, calibration or Stop automatically after a lost reply. The released
Remote Access package alone does not supply the AetherPD installation required
by these controls.

With a managed AetherPD installation, **Remote Access** keeps the installation's
local owner credential while joining or rejoining a tailnet. A pending QR
sign-in remains owned; another computer cannot restart it without that
credential. If a sign-in reply is lost, reopen **Remote Access** on the same
computer to retrieve the current sign-in state and code using its saved token.
Enrollment stays disabled when a managed runtime does not confirm support for
preserving local ownership; update the managed service before signing in.
**Disconnect** removes the tailnet identity and stops the owned AetherPD
session, then returns to **Local access ready** with the same installation ID
and owner credential. A failed auth-key join also retains local control.
If the managed service rejects the saved owner credential, **Remote Access**
reports the rejection and keeps that credential; it does not clear it as a
revoked legacy sign-in token.
Losing that saved owner credential is a separate recovery case; token-free
takeover and reinstalling the legacy remote-access waveform are not recovery
procedures for this managed installation.

Open **Tools → Waveforms... → AetherPD… → Remote Access…** to configure that
managed service. Stop AetherPD and finish any service update first. The entry
also lets the known owner resume sign-in when the old IQ control channel is
unavailable. **Cancel sign-in** ends a pending managed sign-in and retains local
access. A Remote Access window belongs to the connection it opened on; after
that connection ends, reopen it from the current radio's AetherPD panel.

The **AetherPD service installation** section shows a prepared update's target radio,
firmware, planned changes and authenticated package identifiers. Qualified package
selection is not yet available. Builds explicitly configured with a development
package show its version and **not qualified for production**. A prepared plan
labels its selected installation type. **Prepare update**
verifies that package against the hashes and signing keys compiled into the app
and prepares a plan without changing the radio or loading credentials. This
button is dimmed when no package is compiled for the connected firmware.
**Approve update** stays disabled until a verified plan has been prepared for
the current connection.

- **Approve update** approves exactly the displayed plan, then loads the saved
  radio credential and starts installation. Approval is unavailable while the
  radio is transmitting. An update does not transmit or arm calibration.
- **Cancel update** cancels download/preparation or requests installer cleanup.
  Canceling after installation has started does not guarantee rollback.
- Closing the window leaves the update owned by the current radio connection.
  Reopening shows its current progress. Keep AetherSDR running until the outcome
  and temporary-installer cleanup are reported; recovery after application exit
  is not yet qualified.

An update requires confirmed AetherPD cleanup. A **Stop AetherPD** acknowledgment
alone is insufficient: wait until the service confirms it has stopped. An uncertain
cleanup result keeps the session owned and prevents the update from starting.

The fixed helpers clear obsolete failed AetherPD session records after checking
that they are empty. Running sessions and uncertain cleanup remain protected.
This housekeeping is included in the installation plan shown before approval.

**Prepare first installation** is a separate development-build action for a
radio without a managed AetherPD installation. It is dimmed unless the build
contains a first-install package for the connected firmware, and while a saved
installation needs checking. Preparation verifies that package and creates a
plan without writing credentials or uploading. The plan identifies itself as
**First installation** and lists the temporary host-root installation step, new
local identity and fixed helpers that remain installed. **Approve first
installation** saves the new credential persistently in the system keychain
before the first upload. If that save fails or times out,
installation does not start. An existing radio credential is retained until the
radio confirms the exact new installation. If saving the active credential then
fails, the window reports that failure separately from the confirmed radio
installation; the attempt credential remains in the keychain. The approved
public plan is saved locally before upload, alongside a separate keychain entry
for the attempt. An unresolved record blocks another first
installation. **Check saved installation** checks that exact plan using read-only
radio status and restores the active credential only after confirmation. The
control is dimmed unless the build trusts the saved attempt's publisher and the
record matches the connected radio and firmware. It does not repeat the
installation or establish that temporary helpers were removed.

If an attempt is still pending, **Prepare saved attempt** loads its existing
keychain credential and rebuilds the exact saved plan from the trusted package.
It preserves the installation identity and package names. **Approve saved
attempt** explicitly authorizes another delivery of that plan. Preparation alone
makes no radio changes, and a completed or changed local record cannot be used
for this action. An unconfirmed status does not prove that the previous attempt
stopped; an existing carrier prevents delivery. The radio accepts only the
matching transaction. Retrying a terminal failed transaction also requires a
verified package with explicit retry support. Older packages may require separate
recovery. This control does not replace the identity or erase the installed store.

After that check confirms the exact committed installation, **Remove temporary
installer packages** rechecks the same commit and removes only the two package
names shown in the plan. It preserves the installed service, identity and fixed
boot/session helpers. It requires an idle transmitter and does not upload,
reinstall or transmit. Cancellation stops local tracking; a removal already
requested may still finish. A rejected or unmatched commit cannot authorize
package removal.

These controls remain experimental. The development workflow supports a fresh
local identity, reboot startup and checking a saved installation after restarting
the Linux desktop. Recovery after desktop termination following delivery, with
explicit temporary-package cleanup, has been exercised on the FLEX-8600.
A pending attempt interrupted between the native and carrier uploads can be
prepared and approved again; this path has also passed reboot and receive-only
DAX IQ checks on the FLEX-8600. Other interruption points, power loss, other desktop credential stores and
FLEX-6000 installation still require qualification.

A confirmed update and confirmed cleanup are separate results. If cleanup is
unconfirmed, the window says so even when the radio confirmed the new payload.
Cleanup still requests removal of an installer that appeared and then disappeared
from the waveform list: a stopped installer can leave files on the radio. The
window waits for the removal acknowledgment and the installer's absence; a rejected
or missing acknowledgment leaves cleanup unconfirmed.
A connection change requires reopening the window; an old window cannot approve
a plan for the replacement connection. Fixed boot/session helpers remain installed
for reboot and local IQ setup; temporary delivery and upgrade workers are retired.

The native development service keeps Tailscale's log and cache files in RAM.
Its saved configuration and Tailscale identity each have a 64 KiB limit, matching what
the installer can preserve during an update. If a file exceeds that limit,
the service refuses the load or write and preserves the saved file for
recovery. It does not erase the identity or silently sign the radio out. An unreadable
saved configuration blocks enrollment and control; it does not make the radio
available for a new owner to claim.
This bounds those identity files, not total radio storage; FLEX-6300 disk and
power-loss qualification remain required.

Signing out or replacing the admin token also stops the shim's owned AetherPD
session and cancels calibration. Signing in again does not restart correction
or rearm calibration. A rejected sign-out keeps the existing session. A credential
change stops AetherPD once the new credential is saved; a later sign-in failure
does not restart the old session. Delayed sign-in callbacks cannot recreate a signed-out
credential or change a replacement identity. Sharing updates preserve the
current sign-in ownership, and a login URL still being read when its credential
is revoked is refused.

When the receive slice survives a stopped, journaled session, the development
interface's authenticated `POST /v1/aetherpd/receive/restore` can reapply the
captured modeled receive settings, including filter, AGC and noise-blanker
level. It accepts no parameters or body, requires the DSP and unkey helper to
have stopped, and verifies ownership and radio readback. Reassigned slices are
refused. Missing slices require the additional API-loss checks described below.
The development window exposes this request as **Restore receive**.

A managed development session also requires an independent unkey helper.
Startup waits for that helper before launching the DSP. It can stop software
PTT owned by the prepared GUI client if the DSP exits; it cannot start a
transmission. The shim can also own an independent lease guardian that preserves the radio
API connection until it can safely restore the original receive mode. Receive
restoration waits for that guardian to finish and checks the original client
and panadapter ownership. Installing the shim alone does not provision this
session. A separate unprivileged receive guardian keeps the modeled receive
snapshot in memory so it can restore settings after the shim process exits.
It verifies the original receiver and radio state before writing, and cannot
recreate a missing slice or start a transmission. The snapshot does not survive
a reboot. Before **Start AetherPD**, select USB on the active receiver and wait
for the radio's readback. Start is dimmed with that reason in other modes;
changing to USB does not start the service. Managed startup currently supports
USB only and also checks the receive baseline before launching the DSP.

The development calibration interface can arm a bounded source for the next
AetherSDR software PTT after PD mode and RF settings are selected on the
prepared GUI's slice. Arming alone emits
no signal and expires after 30 seconds. It checks a wall-clock expiry and emits at most eight seconds of calibration
samples before returning to microphone input. Unkey, configuration changes or
a fault cancel it; recovery never rearms it. Saved settings for other bands
do not cancel an arm; changing live transmit power does. Completing the source
sequence does not by itself prove successful correction. Cancel also cancels
an arm request still being prepared. If an arm request loses its reply, the shim
cancels it separately; if cancellation cannot be confirmed, it stops the owned
DSP process. Use PTT to unkey the radio.
These controls remain in development and are not included in the released
Tailscale service.
Loss of the waveform's radio API connection can remove its active PD receive
slice, even while unkeyed. The development restore request can recreate a
receiver after confirmed API loss for a basic single-receiver USB/LSB session,
provided the original GUI and pan survive and the radio reports free resources.
It checks allocation and ownership before restoring receive settings. A confirmed conflict with another receiver using the original DAX channel
makes the automatic receive guardian refuse restoration promptly and report the
reason in its log. It leaves that receiver untouched. Advanced
receive profiles and ambiguous allocation replies are refused; recreate the
receive slice in AetherSDR in those cases. Recovery requires an explicit request
and never transmits or rearms calibration. Automatic lifecycle/UI recovery and
FLEX-6000 recovery qualification remain unfinished.
For a process started by the shim, status comes from that process's private
connection. After it exits, status reports `unavailable` even if another
waveform is serving diagnostic status elsewhere.

The authenticated development status includes `input_limited_samples`. A
nonzero count means samples clipped before correction during the current or
last transmission. Reduce **Microphone gain** before the next test; the count
resets when transmission starts. An active correction flag alone does not
establish clean RF output.

The AetherPD development session resumes a recent correction model after a
brief transmit scheduling stall only after fresh DAX IQ, alignment and a
matching linear-path fit validate it. Correction stays bypassed during those
checks. The candidate must be less than 30 seconds old; changed configuration,
stale IQ or a send error discards it. This assumes the amplifier's unexcited
nonlinear response has not changed. Recovery never transmits or starts a new
calibration signal. This behavior remains under bench qualification.


## Licensing

The container (`tools/flex-tailnet-shim` in the AetherSDR repository) is part
of AetherSDR and licensed under the GNU GPL v3. It includes Tailscale's
open-source client library and other Go modules under their own BSD, MIT,
ISC and Apache-2.0 licenses, listed in the `LICENSES` file inside the image
and in `THIRD_PARTY_LICENSES`. The image is built reproducibly, so anyone can
rebuild it from the source at its release tag and get the exact bytes
AetherSDR checks for.

Tailscale is a trademark of Tailscale Inc. AetherSDR is not affiliated with
Tailscale.
