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
window. After that the radio appears in your radio list wherever you are, as
if it were on your desk, and **Connect by IP** works too.

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
2. In AetherSDR open **File → Connect to Radio...** → **On This Network**.
   The radio is listed at its tailnet address; select it and click
   **Connect Selected Radio**.
3. If it isn't listed, choose **Connect by IP** instead (see
   [Manual Connection](./manual-connection.md)): set **Radio type:** to
   **FlexRadio**, enter the radio's **tailnet address**, and click
   **Connect by IP**.

The container passes the session through to the radio: control, audio,
panadapter, waterfall, meters, DAX, and the extra connections the radio opens
on demand (file transfers, for example). Each remote client gets its own
session, and the radio sees an ordinary client.

### Who the radio is announced to

On its own network the radio announces itself about once a second, and that
is how clients list it. A tailnet carries no such broadcasts, so the
container sends a copy of each announcement to your tailnet devices, naming
the radio's tailnet address. The **Radio announcement** card in **Remote
Access** chooses which devices get it:

- **Every device away from the station** (the default)
- **Only the devices ticked below**
- **Off**

Click **Save Announcement** to apply a change. Signing the radio in again
(Part 7) starts it back at **Every device away from the station**, so choose
again afterwards if you had picked another. Each device in the list shows
what happens to it:

| Status | Meaning |
|---|---|
| **Announced** | It is sent the announcement. |
| **At the station** | It is on the radio's own network and hears the radio's broadcast, so it isn't sent a second one. |
| **Via its discovery helper** | A discovery helper on that computer receives the announcement and broadcasts it there. |
| **Not ticked** | **Only the devices ticked below** is chosen and it isn't ticked. |
| **Offline** | It isn't connected to the tailnet now. |
| **Not on the tailnet now** | It is ticked but no longer in your tailnet. Untick it and save. |
| **Not sent: over the limit of 32 devices** | At most 32 devices are sent the announcement. |
| **No IPv4 address** | It has no tailnet IPv4 address to send to. |

Below the list, **Last heard from the radio at** shows that the container
hears the radio's own broadcast. If it says the announcement hasn't been
heard, nothing is sent.

An older container can't announce the radio: the card says so and its
controls are dimmed. Update it (Part 4).

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

The container relays the standard SmartSDR protocol, so other clients on a
computer running Tailscale also work through it:

- **SmartSDR for Mac** lists the radio from its announcement, and can also
  connect by IP: set it to **Fixed IP** with the radio's tailnet IPv4
  address (the `100.x.y.z` one, not the `fd7a:` IPv6 one).
- **SmartSDR for Windows** lists the radio from its announcement. Only one
  program on a computer receives each announcement, so SmartSDR, DAX and CAT
  can't all see the radio at once unless a discovery helper runs on that
  computer. With the helper running, SmartSDR, DAX and CAT all work over the
  tailnet, and AetherSDR on the same computer lists the radio too.

A device that can't run Tailscale itself can't reach the tailnet.

### The discovery helper

SmartSDR for Windows lists the radio without help when it is the only Smart
program running. When SmartSDR DAX or SmartSDR CAT runs as well, only one of
them gets each announcement. The discovery helper receives the announcement
from the radio and broadcasts it on the computer, where every program gets it.

1. Download `discovery-helper-<version>.zip` from the AetherSDR GitHub
   release tagged `flex-tailnet-shim-v<version>`, the same release as the
   container, and unzip it anywhere.
2. Install Python 3.8 or newer from [python.org](https://www.python.org/downloads/).
   Not the Microsoft Store's Python: under it the helper connects, but DAX and
   CAT never hear its broadcast.
3. The helper looks for the radio by its tailnet name, `flex-radio` unless
   you tell it otherwise. If yours differs, run `setx TAILNET_SHIM <name>` once
   in a Command Prompt.
4. Run `run-discovery-helper.bat` and leave its window open. Close the window
   to stop it.

On macOS or Linux, run `./run-discovery-helper.sh` instead, with
`export TAILNET_SHIM=<name>` if the name differs.

The helper connects to the radio over the tailnet, on TCP 48996, and only to a
tailnet address. The radio serves it only when the **Radio announcement**
setting includes that computer. It refuses the helper when the setting is
**Off**, when the computer isn't ticked under **Only the devices ticked
below**, or when the computer is at the station, and the helper's window
shows why. The helper tries again every 30 seconds, so it starts working by
itself once the setting includes the computer. While it is served, the
computer shows **Via its discovery helper** in the card.

On Windows the helper broadcasts on the computer's loopback network, so the
announcement stays on that computer. On macOS and Linux it broadcasts to
255.255.255.255, which also goes out on that computer's local network: other
devices there see the announcement, though they can't reach the tailnet
address it names.

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
- **Limiting access also needs a rule for the announcement.** The radio
  sends its announcement to your devices without being asked, so it needs
  permission in that direction: UDP 4992 from the radio to your devices.
  With the radio tagged `tag:radio` and your operators in `group:hams`, add
  this to **grants**:
  `{"src": ["tag:radio"], "dst": ["group:hams"], "ip": ["udp:4992"]}`.
  Without it the radio isn't listed, but **Connect by IP** still works. A
  discovery helper connects to the radio on TCP 48996, so a rule that lists
  ports must include it. The default rules (everyone reaches everything)
  need neither.
- The announcement carries what the radio broadcasts on its own network:
  model, serial number, nickname, callsign, firmware version, and the names
  and LAN addresses of connected clients. It goes only to the devices chosen
  under **Radio announcement**, and a discovery helper's broadcast on macOS
  and Linux also reaches that computer's local network (see
  [The discovery helper](#the-discovery-helper)).
- The **Radio announcement** card lists your tailnet's devices only on the
  computer that holds the admin token. The container leaves the device names
  out of the status it gives any other computer on the station network.
- A discovery helper connects only to a tailnet address, where the radio
  checks which device is connecting, and is served only when the
  announcement setting includes that device.
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
| Tailnet, radio to your devices | UDP 4992 | The radio's announcement, which lists it |
| Tailnet | TCP 48996 | Discovery helper |
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
- **Disconnect is missing, the sharing and announcement settings are grayed
  out, and the announcement card lists no devices**: this computer doesn't
  hold the admin token. Use the computer that set the radio
  up, or **Reset** (Part 7).
- **Can't connect remotely**:
  - check that Tailscale is running on the remote computer, that the radio
    shows as connected on **Machines**, and that your tailnet's access rules
    let you reach it;
  - right after a radio reboot, give the container about 20 seconds.
- **The radio isn't listed remotely**:
  - in **Remote Access** at the station, check the **Radio announcement**
    card: the radio must have been heard, and your device must show
    **Announced**;
  - if your access rules limit who may reach the radio, add the rule under
    [Security](#security);
  - meanwhile, use **Connect by IP**.
- **SmartSDR doesn't list the radio while AetherSDR is open on the same
  computer**: only one program on a computer receives each copy of the
  announcement. AetherSDR gives it up once it is connected to the radio over
  the tailnet; until then, start SmartSDR first, or run the
  [discovery helper](#the-discovery-helper), whose broadcast every program
  receives.
- **A computer at the station lists the radio twice**, at its LAN address and
  at its tailnet address: the container takes a device to be at the station
  when Tailscale's direct connection to it runs over the radio's network. A
  computer that is idle on the tailnet, or reached through a relay, has no
  direct connection, so it counts as away and is sent the announcement. It
  corrects itself once Tailscale connects directly. Meanwhile, untick it
  under **Only the devices ticked below**, or pick the LAN entry.
- **The discovery helper says it was refused**: its window gives the reason.
  - **The radio's announcement is off** or **this device isn't ticked**:
    change the **Radio announcement** setting, and the helper connects within
    30 seconds.
  - **This device is at the station**: the computer is on the radio's own
    network and already hears the radio's broadcast, so it needs no helper.
  - **Not a tailnet address**: `TAILNET_SHIM` or `--shim` names something
    outside the tailnet. Use the radio's tailnet name.
- **Spectrum or audio stutters**:
  - check **Network Diagnostics → Remote Link** for a relayed path;
  - keep Opus on, and see [Low Bandwidth Connections](./low-bandwidth-connections.md).
- **Station device applets can't reach the device**:
  - approve its route in the admin console;
  - on Linux, run `sudo tailscale set --accept-routes`;
  - set the applet to the device's LAN address.
- Before reporting a problem, turn on the relevant log categories in **Help →
  Support & Diagnostics...** (see [Support and Logging](./support-and-logging.md)).

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
