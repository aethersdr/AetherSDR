# flex-tailnet-shim

Prototype. Not built by CMake, not shipped, and pending an RFC.

`flex-tailnet-shim` runs inside a FLEX-8000-series or Aurora radio as a Docker
waveform container. It joins a Tailscale tailnet with
[`tsnet`](https://pkg.go.dev/tailscale.com/tsnet) and relays AetherSDR's
SmartSDR session to the radio. Both ends make only outbound connections, so it
works when the radio and the operator are both behind CGNAT. In that case
traffic goes through Tailscale's DERP relays.

Remote connections need no new AetherSDR code: run Tailscale on the operator's
machine and use the existing manual/routed connect with the shim's tailnet IP.
AetherSDR's only addition is the Configure… dialog that provisions the
container over the LAN (see "Setup from AetherSDR").

```text
AetherSDR (tailnet peer)                       radio container (host network namespace)
  TCP  -> 100.x:4992  ----tailnet---->  shim  -> 172.30.1.1:4992   radio API
  UDP  -> 100.x:4991 (TX VITA-49) ---->  shim  -> host socket P -> 172.30.1.1:4991
  UDP <-  100.x:4993 (RX VITA-49) <----  shim  <- host socket P <- radio (from :4993)
```

## What it does

- **Control:** each tailnet connection to `:4992` gets a fresh TCP connection
  to the radio API. Bytes pass through unchanged, except two lines:
  - `client udpport N` is rewritten to the session's host UDP port P, so the
    radio sends VITA-49 to the shim. The shim remembers N, so it can forward to
    the client's real port.
  - `network_mtu=` is clamped to `-max-mtu` (default 1200), so VITA-49
    datagrams fit Tailscale's 1280-byte path MTU.
- **UDP:** VITA-49 from the radio is forwarded to the client's registered
  tailnet address. Client datagrams (TX audio, DAX TX, netcw, primes) leave
  from the same host socket P, so the radio sees one address per client, as on
  a LAN. Datagrams from unregistered senders are dropped.
  - Host socket P is bound to the address the shim's own radio connection
    comes from (normally 172.30.1.1), where the radio sends VITA-49, so it
    doesn't listen on the radio's LAN address. Linux still delivers a
    datagram a LAN host deliberately routes to 172.30.1.1.
  - So P also relays only datagrams from the radio: `SSDR_RADIO_ADDRESS` or
    another of the host's own addresses. Others are dropped and counted (see
    Link telemetry).
- **Identity:** every connection is checked with Tailscale `WhoIs`. `-allow`
  limits access to listed logins or tags, on the radio, station devices and
  side channels alike. Each announced side channel admits one connection,
  from the computer whose transfer opened it. Saving a narrower list ends the
  open sessions, transfers and device connections it no longer admits. The shim listens only on its own tailnet ports and on
  ports an app on the radio published (see Published ports), so the radio's
  other services, such as its SSH, are not exposed, and a shared route never
  splices into the radio's own addresses.
- **Teardown (Principle VI):** when either side ends, the shim closes the radio
  connection at once. The radio drops the GUI client and unkeys anything that
  client owned. The shim never keeps a radio session alive for a client it
  can't hear.

## Measured on a FLEX-8600, firmware 4.2.20 (2026-10-07)

| Check | Result |
|---|---|
| GUI client from inside the container | Accepted. `client udpport N` alone delivers VITA-49, sent from 172.30.1.1:4993. The radio creates a default pan and waterfall per GUI client and removes them on disconnect. |
| Container after `radio reboot` | Auto-started ~22 s after kernel boot, before the radio API was listening. The writable layer (tsnet state) persisted. The clock reads ~5 h wrong until NTP syncs, under 82 s after boot. |
| Tailnet join | ~6 s from container start, with a one-time ephemeral auth key |
| Path | Forced DERP (`TS_DEBUG_ALWAYS_USE_DERP=true`), Seattle relay |
| AetherSDR connect via `connect ip` | Connected in 0.64–0.74 s; pan, waterfall, meters and RX audio rendered (12.4 FFT frames/s, 6.2 waterfall rows/s) |
| API `ping` round trip | 56 / 67 / 84 ms (min / median / max) |
| Radio → client | ≈ 3.5 Mbit/s with uncompressed RX audio, ≈ 1.9 Mbit/s with Opus |
| Shim footprint on the radio | 19–23 % of one Cortex-A72 core, 34 MB RSS, 20.8 MB binary |
| Clean disconnect | The shim closed the radio connection in the same second |
| Client goes silent (process frozen) | The radio's keepalive dropped the client after ~15 s, and the shim closed in the same second |

## Setup from AetherSDR

The image is generic and contains no secret.

1. **Install:** in AetherSDR, open File → Waveforms → Install… and pick
   `flex-tailnet-shim-<version>.tar.gz`. The radio starts the container. It
   stays unprovisioned until it is signed in to a tailnet.
2. **Provision:** the container's row in the Waveforms list has a
   **Configure…** button. Choose a machine name and, optionally, who may
   connect, then sign in with a QR code: scan it with a phone, sign in to
   Tailscale and approve the radio, and the dialog turns to connected. Under
   Advanced, a single-use Tailscale auth key works instead; AetherSDR sends it
   to the container over the LAN and does not keep it. Either way the
   container returns an admin token, which AetherSDR stores in the OS
   keychain, one entry per radio.
3. **Connect remotely:** from anywhere on the tailnet, use Connect → Manual
   with the address shown in the dialog.

The node's tailnet state survives radio reboots, so the sign-in is needed
only once. Removing or reinstalling the container wipes that state.

**Sign-in by URL** (version 0.5.0): without an auth key, tsnet starts an
interactive login and the control plane sends a sign-in URL, which AetherSDR
draws as a QR code. Whoever opens that URL first claims the radio for their
tailnet, so it goes only to the caller that started the join or holds the
admin token: the provision reply and `GET /v1/login`, never `/v1/status` or a
log. The join then waits for the sign-in without a timeout, since starting
over would mint a new URL under the code being scanned. A node signed in this
way belongs to the user, not a tag, and its key expires (180 days by default)
unless key expiry is turned off for it in the admin console; `key_expiry` in
the status says when. Once expired, the shim asks the control plane for a new
URL, and the dialog shows it as a new QR code (from the LAN only, since the
expired node is off the tailnet). A tagged auth key, under Advanced, makes a
node that never expires.

**Access** (0.5.0): who may connect is decided by the tailnet's own access
rules. Versions before 0.5.0 kept a Who may connect list and enforced it;
0.5.0 drops a saved list at start-up (and logs it), ignores one sent with a
provision, and refuses to set one.

## Provisioning API

The API is plain HTTP on the radio's LAN address, TCP 48992. It is never on
the tailnet: tsnet's listeners are in their own network stack. Callers on the
radio's internal container network (172.30.0.0/16) and non-private addresses
are refused.

| Method | Path | Token needed | Does |
|---|---|---|---|
| GET | `/v1/status` | no | State (`needs_login` and `needs_approval` while waiting on a person), tailnet IP and DNS name, allowlist, session count, version, key expiry, last error. Never a secret. |
| POST | `/v1/provision` | only once provisioned | `{auth_key, hostname, allow[]}`: joins (or, with the token, re-joins) the tailnet and returns `{admin_token, status}`. Without `auth_key` it signs in by URL and also returns `auth_url` once the control plane sends it (within 30 s); the join goes on in the background |
| GET | `/v1/login` | yes | `{state, auth_url, key_expiry}`: the sign-in URL while one is wanted (a new join, or an expired key) |
| PUT | `/v1/allow` | yes | `{allow: []}` clears a list an older version kept; from 0.5.0 a non-empty list is refused (400): who may connect is the tailnet's access rules |
| POST | `/v1/signout` | yes | Logs out, deletes the node state and forgets the token |

### AetherPD status (development)

`GET /v1/aetherpd/status` requires the existing admin token and the same LAN
caller policy as provisioning. An owned DSP is queried through a private
inherited Unix datagram socket, with a one-second deadline, 16 KiB response
limit and one request at a time. Replies must echo the request counter; late
replies cannot satisfy a newer query. The channel accepts only a fixed status
request and has no control operations. The owned DSP opens no TCP status port.

Without an owned session the public status endpoint reports unavailable. It
never falls back to a separately running diagnostic waveform. The HTTP endpoint
accepts no parameters or request body.

The response includes `available`, `state`, `registered`, `keyed`,
`correction_enabled`, `correcting`, `calibration_required`, and transport fault
counters. The shim projects these fields rather than forwarding native debug
metadata. Native historical fit counts do not establish current correction:
a stalled, bypassed process reports `recalibration_required` even if it had
successful fits earlier. `calibration_required=false` alone does not establish
that a usable correction model exists. A missing, malformed or unresponsive
native service returns HTTP 503 with `available=false` and `state=unavailable`.

This is status integration for an experimental waveform.
The status report grants no capture or transmitter authority. A trusted launcher
can supply a prepared session for managed startup as described below. IQ
provisioning and its production installer remain integration work.

### AetherPD IQ snapshots (development)

An authenticated `POST /v1/aetherpd/capture` collects 16,384 samples from the
owned DSP's existing raw DAX IQ stream. It returns a capture manifest; fetch
its 32 bounded chunks with `GET /v1/aetherpd/capture/{capture_id}/{chunk}`.
Captures expire, stay bound to their original session and are discarded by
Stop or replacement. There is no DAX audio fallback, stream reconfiguration
or keying operation. The full wire contract and test scope are in
[`IQ-MEASUREMENT.md`](../pd-waveform/IQ-MEASUREMENT.md).

The native reader, authenticated relay and desktop capture adapter have local
integration tests. Capture cancellation does not stop correction; its bounded
private reply is drained. The analyzer controller/UI and live-radio
qualification remain unfinished. These endpoints are not available in the
released shim package.

### Bounded AetherPD test cleanup (development)

`POST /v1/aetherpd/test-unkey` accepts exactly `{"cleanup_after_ms":15000}`
with an integer from 1 through 15000. It starts a separate unprivileged cleanup
helper bound to the original GUI session and returns a random test ID and fixed
deadline. `GET /v1/aetherpd/test-unkey/{test_id}` observes it;
`POST /v1/aetherpd/test-unkey/{test_id}/stop` requests early cleanup. All require
the current admin token. A stale test ID cannot stop a later helper, and polling
does not extend its deadline. These operations never key RF or arm calibration.

The deadline means cleanup initiation; helper exit and command acceptance do
not establish RF cessation. Production TX ownership, release confirmation and
desktop integration remain required. The [full contract and limitations](../pd-waveform/IQ-MEASUREMENT.md#bounded-test-cleanup-api-development)
describe the versioned private protocol and local test scope. These endpoints
are not in the released package and have not been qualified on the radio.

### Authenticated AetherPD start (development)

`POST /v1/aetherpd/start` requires the existing admin token and a JSON object
containing exactly `gui_client_id` (UUID) and `gui_handle` (nonzero uint32).
Duplicate, unknown, null, oversized and trailing fields are rejected. The
current GUI identity is mandatory: a radio reboot changes its numeric handle.
There are no executable, command, address, descriptor, PID, scheduling, power
or RF parameters. HTTP202 means `start_requested`, not registered or correcting.
A concurrent start returns409; no start is retried automatically.

The ordinary shim connects only to `/run/aetherpd/start.sock`. A protected
socket-activated `pd-start-session` admits only UID78666/GID20 peers executing
the installed protected shim inode. The48-byte request is bounded to2s. The
fixed constructor prepares local DAX IQ and the existing guardian graph,
then drops authority before executing the private Go session role. That role
has no discovery, Tailscale node, provisioning API or public listener. All
runtime controls use the inherited connection, with bounded frames and up to
eight outstanding requests. Closing it stops its owned DSP;15s without a
request also expires the channel. Cancel can proceed while arm is pending.

The ordinary shim proxies status, calibration, stop and receive restoration
only to its owned private session. Sign-out, token rotation, failed startup
or a canceled in-flight request close that lifetime; late results cannot
reattach. Stop retains the private journal for explicit receive restoration.
A new explicit start retires a stopped session only when it has no pending
operation. No path keys the transmitter or automatically rearms calibration.

The socket and immutable payload still need authenticated installation,
platform-specific boot delivery and qualification. This development endpoint
is not an installer or a claim of automatic
AetherPD boot recovery. The FLEX-6300 service manager remains unqualified.

### AetherPD upgrade requests in RAM (development)

`POST /v1/aetherpd/install-request` accepts an authenticated, fixed 120-byte
upgrade request configuration. It keeps the credential in unlinked tmpfs and
launches only the installed `pd-primary-launch --install-request` entry as the
unprivileged waveform user. Its 202 reply means the input listener is ready;
it is not an installation-success response. The request blocks overlapping
AetherPD startup and expires after 400 seconds. Token revocation cancels it.

Authenticated `GET` and `DELETE` on
`/v1/aetherpd/install-request/<request_id>` report or cancel the owned request.
The carrier's RAM credential receiver, ordinary desktop installer and live
qualification remain unfinished. This path requires an already-installed shim
and cannot establish first-install trust. It uses the existing trusted-LAN HTTP
transport. See [`request/README.md`](../pd-waveform/request/README.md) for the
descriptor contract and why request credentials cannot be packaged in native
waveform archives.

### Managed AetherPD lifetime (development)

The desktop-side `AetherPDModel` wraps the fixed HTTP client as a volatile
connection model. Explicit attachment starts status polling at one-second
intervals. Observations expire after three seconds using monotonic time,
including when the Qt event loop has not delivered the expiry timer. Only a
fresh eligible observation admits an explicit calibration-arm request; the
shim remains the final admission authority. Acknowledgments are separate
receipts and never become optimistic `armed`, `correcting` or stopped state.

Control requests are never retried automatically. Cancel and stop retain the
transport's preemption rules; lost replies clear the observation without
claiming the remote operation was canceled. Unauthorized replies retire the
credential and monitoring. Replacing or clearing the session cancels its local
request, removes observations and receipts, and fences late replies even at
the same address. `RadioModel` owns this model and admits attachment only for
the caller's current registered GUI session. Each request and reply rechecks
the connection generation, transport, GUI handle/identity, radio serial and
address. Session retirement clears the model before notifying other consumers;
an old dialog cannot attach to or detach a replacement connection. Replacing
the supplied credential starts a new local model session. Credentials in this
model are memory-only.
Clearing the local model is not confirmation of remote stop or RF unkey.

The model and its `RadioModel` integration are tested with injected connection
state and HTTP transport. The development Waveforms window opens an AetherPD
panel using the existing per-radio token store. It exposes explicit Start,
Stop, arm/cancel, receive restoration and status refresh. Start sends only the
current GUI UUID and numeric handle, and Stop can preempt a pending Start.
Opening, closing or reconnecting never starts the service or arms calibration.
Close invalidates outstanding credential callbacks while the radio-owned model
continues monitoring an already attached session. Reopen after reconnect to
attach the new identity. Socket-free widget tests cover these lifetimes;
live desktop lifecycle qualification remains integration work. It has no keying,
privilege provisioning, automatic calibration or automatic receive-restore
operation.

A trusted local launcher must supply six already-open descriptors for a
managed session:
`SHIM_PD_IQ_FD` (private unnamed Unix datagram pair), `SHIM_PD_API_FD`
(TCP lease to `127.0.0.1:4992`), `SHIM_PD_GUARD_FD` (write end of the API guardian's lifetime pipe), `SHIM_PD_SETUP_FD` (write end
of the setup lifetime pipe), `SHIM_PD_UNKEY_API_FD` (dedicated local TCP control
connection), and `SHIM_PD_GUARD_READ_FD` (the read end of the same guardian
pipe). It also supplies `SHIM_PD_UNKEY_CLIENT_HANDLE` and `SHIM_PD_GUI_HANDLE`
as nonzero eight-digit hexadecimal handles with a `0x` prefix. The control
handle must differ from the GUI handle. Partial or malformed handoffs fail
startup, including the previous four-descriptor contract.
These are an internal launcher contract, not manual container settings.

The shim first launches the sibling `pd-unkey-guard` with fixed descriptor
arguments and the validated handles, then waits for its bounded readiness
message before starting `aether-pd`. The control connection preserves its
initial interlock snapshot: the launcher consumes only the greeting through
its API handle and sends the GUI binding as sequence910000 without reading
its reply. The helper checks that binding acknowledgment and tracks updates;
SmartSDR does not replay the initial tuple on repeated subscriptions. See
`tools/pd-waveform/guardian/README.md` for the handoff contract.

A prepared launcher may set `SHIM_PD_OWN_LEASE_GUARD=1` to let Go start the
sibling `pd-lease-guard` itself. Only `0` and `1` are accepted; the default
retains the external guardian contract. With ownership enabled, the launcher
must not start another API lease guardian. Go captures the original receive
identity, starts the unprivileged guardian with fixed descriptors and that
slice/GUI/pan/mode tuple, and waits for bounded readiness before starting DSP.
The helper retains the lease after Go dies and waits for a fresh, idle,
matching receiver before restoring its mode. Go reaps it during normal
shutdown; an unexpected guardian exit stops DSP. Explicit receive restoration
also waits for this owned guardian to exit. This flag grants no IQ setup or
transmit authority and requires the complete prepared descriptor handoff.
See `tools/pd-waveform/session/README.md` for the fixed helper contract.

With lease ownership enabled, the shim also starts a receive guardian using
its own executable's fixed internal mode and two inherited private pipes.
It retains the bounded original receive snapshot in memory and does not start
Tailscale, discovery, an HTTP server or a log service. It runs as the ordinary
waveform user with zero capability masks and waits for lifetime hangup without
reading payload. After the independent native guardian returns the original
mode, it reuses the shim's checked receive-restoration adapter and verifies
radio readback. Fresh owner/pan/frequency/antenna/DAX and idle checks precede
writes. It cannot allocate a missing receiver, key RF or resume calibration.
An unexpected exit stops DSP; normal stop waits for it before explicit recovery.
This protects modeled settings when the main Go process dies. Both copies use
the same packaged executable, adding no persistent binary or journal file.
A reboot discards this volatile snapshot and never triggers restoration or RF.


Managed startup currently requires the captured original mode to be USB. Other
modes are rejected before DSP launch; broader receive-recovery profiles do not
extend the native modulator's RF support.

The DSP receives fixed arguments and environment. It rejects root or retained
capabilities. A
separate owner pipe stops it when the shim exits, including SIGKILL; it does
not depend on Go's creating thread surviving. The setup pipe closes after the
DSP exits during normal shutdown. The independent API guardian must preserve
its lease until the radio can safely return to USB. The unkey helper survives
Go-owner death, observes the DSP lifetime pipe, and can send only an owned
software-PTT unkey through the prepared GUI connection. Go reaps it during
normal shutdown and stops its DSP if the helper exits. A helper unkey
acknowledgment does not by itself establish idle state or safe lease release.

The shim creates its child's private status socket and disables process dumps
for a prepared session. The launcher supplies the six descriptors
above and the two handles; it supplies neither a status destination nor a
process ID. The pipe read and write ends must identify the same kernel pipe.
`SHIM_PD_TRANSPORT_CACHE=1` opts a prepared session into the experimental
transport-recovery gate described in `tools/pd-waveform/README.md`. Only `0`
and `1` are accepted; the default is off. It cannot be enabled through the
HTTP status/stop interface, and enabling it without a prepared session fails.

`POST /v1/aetherpd/stop`, with the admin token and no parameters or body,
closes only this shim's DSP owner pipe. HTTP 202 means `stop_requested`, not
confirmation that the independent guardian has released its lease. Without
an owned session, it returns 409. Separately started processes are left alone;
repeated stop requests for an owned session are idempotent. The shim never
restarts the DSP automatically. Ordinary startup without descriptor handoff
is unchanged.

Successful sign-out or an admin-token rotation also stops the owned session
and permanently closes that session's calibration gate. The old token cannot
arm or restore it, and acquiring a new token does not restart it. Rejected
requests and failed configuration writes leave the existing session intact.
Sign-out revokes local authority before waiting for tailnet logout.

For a FLEX-6000 development package, `native/build.py --with-aetherpd` includes
the compressed ARMv7 runtime, unkey helper, lease guardian and licenses; the launcher unpacks
them into RAM only when a descriptor handoff is present. This does not install the
privileged IQ setup helper or enable correction by itself. The actual 6300 still requires kernel, scheduling and RF qualification.

### Bounded calibration control (development)

An owned managed session exposes `POST /v1/aetherpd/calibration/arm` and
`POST /v1/aetherpd/calibration/cancel` with the admin token and no query or body.
The shim creates a second private Unix datagram pair for these two fixed
commands; status remains read-only on its original socket. There are no caller
parameters for tones, gain, duration, frequency, power, PID or executable.

Select PD on the prepared GUI's slice and finish RF configuration before arming. Arming requires idle state,
correction enabled, native radio APD inactive and fresh local IQ. It expires
without PTT after30seconds. Only that GUI's subsequent software PTT starts the fixed
8second calibration sequence (4second noise preamble, then710/1900Hz tones),
followed by normal microphone input. Generated samples and a monotonic deadline
bound it; processing checks the deadline before each TX packet. This is source
control, not a hardware RF deadline or a substitute for independent unkey.
Unkey, configuration changes and faults cancel it; it never rearms on recovery.
Cancel removes the source; use PTT to unkey.

Only one arm request may be pending. Cancel invalidates an arm that is still
collecting its receive snapshot, so it cannot arm after cancellation completes.
If an arm loses its reply or its HTTP caller disconnects, the shim sends a
separate bounded cancel. If cancellation cannot be confirmed, it stops the
owned DSP process. Neither operation keys the radio.

Private requests carry monotonic counters. Duplicate requests return their
original result without extending an arm. Native status reports `calibration_control`,
`calibration_state` and `calibration_samples_left`. A `completed` source sequence
does not establish that a valid correction model was fitted. Check `correcting`
and RF evidence separately. `input_limited_samples` reports samples clipped
before correction during the current or last key. Reduce the existing
Microphone gain if it is nonzero; the counter resets at the next transmit.
Algorithmic correction can remain active while its input is clipped. Receive-only authenticated arm/cancel is verified on the8600. RF
qualification of this control path remains; the last frozen package predates it.

### Receive recovery policy (development)

The development session captures receive state before PD and journals a fresh
owned-PD observation when calibration arming succeeds. Native exit code 7
identifies API-lease loss; normal stops and generic failures remain separate.
The firmware can remove the PD receiver when its API lease fails.

Authenticated `POST /v1/aetherpd/receive/restore` takes no query or body. It
requires the owned DSP and unkey guardian to have stopped. For a surviving
receiver, it restores modeled filter and receive settings after fresh idle,
GUI/pan/slice ownership and DAX-conflict checks, then confirms radio readback.
Cancellation or token revocation stops further writes.

For a missing receiver, allocation additionally requires a journaled API-lease
failure, the original GUI and pan, fresh reported free slice resources, no
surviving receiver belonging to that GUI, and the measured basic USB/LSB
profile. The profile requires ANT1 transmit routing, an unlocked slice,
manual AGC threshold, and inactive advanced NR, diversity, recording, playback,
XIT, loop and related features with their measured defaults. Other profiles
remain available for ordinary managed operation but allocation recovery refuses
them rather than silently replacing settings it cannot reproduce.

One ordinary GUI-bound API connection creates the captured frequency, receive
antenna and mode on the surviving pan. The returned ID must equal the original
ID; fresh ownership, mode, frequency and DAX readback must agree before any
settings write. The captured tuning step is restored as well as the modeled
receive settings. A lost or rejected create reply consumes the one allocation
attempt; an HTTP retry cannot allocate a duplicate or target an unconfirmed ID.
A mismatched allocation is never removed automatically. Allocation admission
expires after five seconds; the full HTTP operation is bounded to six seconds.

The normal restore guard requires fresh unkeyed READY with transmit permission.
Only allocation into an empty receiver session also admits the observed
`RECEIVE`, `tx_allowed=0` tuple, with empty reason/source and zero TX owner.
This does not broaden calibration admission. Neither path selects TX, keys,
rearms calibration, removes another receiver or accepts caller-supplied settings.

The observer uses a fresh local API connection and four fixed subscriptions,
reply barriers, bounded delta merging, GUI identity, pan/slice ownership and
reported remaining slice resources. On firmware 4.3.16 the live interlock tuple
can arrive after subscription replies; both must arrive within two seconds.
No state or resource capacity is inferred when its field is missing. The
original receive profile remains sealed in session memory, not persisted.

The FLEX-8600 receive-only proof confirms removal after real API loss and
explicit restoration with matching GUI receive and transmit snapshots. Unit
checks cover ownership and ID mismatches, unknown profiles, no capacity,
revocation, cancellation and ambiguous allocation replies. This does not qualify
FLEX-6000 firmware or advanced profile recovery. Automatic lifecycle/UI recovery
and the narrow shipping session provider remain integration work. The
parameterless restore currently requires an explicit development-client request.

### Published ports

An app running on the radio itself, such as another container or a legacy
waveform, can make one of its TCP ports reachable on the tailnet:
`POST /v1/local/expose {"port": 48995, "name": "AetherWSPR"}` on TCP 48992.
This is the one path open to callers the table above refuses, and only to
them: loopback and the radio's own addresses. An 8000-series container
shares the radio's network, so it calls from these too. Anyone else,
including other 172.30.x hosts, gets 403.

- Only a port bound where the LAN could reach it can be published (400
  otherwise), read from `/proc/net/tcp`: the wildcard, or a LAN address. On
  an 8000-series radio that excludes 172.30.0.0/16, its internal container
  network; on a FLEX-6000 that range is plain LAN and any address but
  loopback qualifies. The shim reads binds, not firewall rules, so a wildcard
  listener a firewall keeps from the LAN qualifies too.
- This is checked again for each connection, against the socket the app
  registered. If the service goes away, or anything else takes the port, new
  callers are refused until the app that owns the name renews, and a renewal
  that finds nothing listening withdraws the publication.
- The shim listens on that port on its own tailnet addresses, IPv4 and IPv6,
  and splices each caller the tailnet admits (the same `WhoIs` check as the
  radio) to the app's listener, at most 32 at a time per port. A wildcard
  listener is dialed at the radio's LAN address, so callers reach the app as
  a LAN caller does, never as loopback. The reply's `listening` is false while the tailnet
  node is down; the port opens when it comes up. If the shim can't listen
  (for example, a radio file transfer holds the port), the reply is 503.
  Each port has one owner: a transfer the radio announces on a published
  port fails the same way, until the publication ends.
- The registration is a 10-minute lease (`expires_in_s`), renewed by posting
  again. The app's `name` owns the port: the same name renews it, another
  gets 409. When the lease lapses, the port and its open connections close.
- Ports 1024-65535 only, never 22, 4991-4995, 42607, 48992 or 48993, and at
  most 8. `/v1/status` lists them as `exposed_ports`. They survive the
  tailnet node restarting, but not the shim.
- Anything running on the radio is trusted to publish: names are
  self-asserted, and any wildcard listener can be published. That matches a
  radio that lets any LAN host install containers.

**Trust model:** the first provisioning is open to the LAN. That matches the
radio itself: its API on 4992 and its image upload on 42607 are
unauthenticated on the LAN, so any LAN host could already install or remove
containers. After the first use, every change needs the admin token issued
then. Only its SHA-256 is stored in the container. A lost token means removing
and reinstalling the container, which is visible in the Waveforms list. The
auth key crosses the LAN in the clear, but it is single-use and spent by the
tailnet login within seconds; it is never stored or logged.

Keys are changed in-process (logout, then a new tsnet server with the same
state directory). Nothing is known to restart a container whose process exits.

## FLEX-6000: the native build (experimental)

FLEX-6000 radios have no container feature, so `native/` packages the same
shim as a legacy waveform, `AetherTailnet.ssdr_waveform`, for their ARMv7
CPU. `-run-dir` / `SHIM_RUN_DIR` keeps tsnet's logs and caches on volatile
storage while the node's identity stays in the state directory: the
FLEX-6300 we measured persists only to a 60 MB flash
partition (19 MB on a FLEX-6700), so the shim ships gzipped and a small
unpacker (`native/unpack`) puts it in `/tmp` at each start. `native/TESTING.md` is the test protocol, with a prompt for a
tester's AI assistant; `native/tool.py` installs, provisions and watches it
from the LAN.

## Build

```sh
./build.sh          # static linux/arm64 binary, LICENSES, and flex-tailnet-shim-<version>.tar.gz
go test -race .
```

`build.sh` assembles the OCI image itself (`mkimage.py`), byte-for-byte
reproducibly: no Docker daemon, no base image, fixed timestamps, owners and
file order, and a binary built with `-trimpath -buildvcs=false`, so it records
neither the build path nor the commit. AetherSDR pins the image's SHA-256
(`src/core/TailnetShimRelease.h`, RFC #6271 ruling D2); running `build.sh` at
the `flex-tailnet-shim-v<version>` tag reproduces that hash, given the same
toolchain: gzip output differs between zlib builds, so the pinned hash was made
with Go 1.27.1 (from `go.mod`) and CPython 3.14 with zlib 1.3.2. The image's
`diff_id`, the SHA-256 of the uncompressed layer, does not depend on zlib.

The image is `FROM scratch`: the static binary, the `LICENSES` bundle and the
two radio labels (`com.flexradio.waveform.name`,
`com.flexradio.waveform.version`). It has no shell and no BusyBox, so it
carries no GPLv2 source obligation, and it has no CA bundle, because the
Mozilla roots are compiled in. It is about 8 MB compressed.

## Link telemetry

Each AetherSDR client reached over the tailnet can ask the shim how its link
looks from the radio side: `GET /v1/session` on the shim's tailnet address,
TCP 48993. This listener is on the tailnet only, applies the same allowlist
as the relay (403 otherwise), and answers each caller with only its own
sessions. AetherSDR polls it every 2 s for the Network Diagnostics window.

| Field | Meaning |
|---|---|
| `path`, `endpoint`, `relay` | `direct` (with the client's address), `peer-relay`, `relay` (with the DERP region) or `unknown` |
| `path_changes`, `path_since_s` | How often the path has changed, and how long it has held |
| `rtt_ms`, `rtt_via`, `rtt_age_s` | The shim's own disco ping to the client, every 4 s; `-1` until one succeeds |
| `to_client_kbps`, `from_client_kbps`, `last_handshake_s` | WireGuard throughput and handshake age for this peer |
| `shim_cpu_pct`, `shim_rss_kb`, `mtu_clamp` | The container's own load, and the `network_mtu` it injects |
| `sessions[].streams[]` | Per VITA-49 stream, counted as datagrams leave the radio, before the tunnel: `packets`, `gaps` (missed packets) and `breaks` (sequence discontinuities, the unit AetherSDR's own stream counters use) |
| `sessions[].to_client_failures` | Datagrams the shim could not send into the tunnel |
| `sessions[].from_other_sources` | Datagrams that reached the session's radio-side socket from anything but the radio, dropped. Nonzero on a session with no VITA-49 means the radio's own source address went unrecognised. AetherSDR doesn't display it; read it from this report or `/v1/status`, which on the LAN also names a recent sender (`last_rejected_source`) |

AetherSDR subtracts the radio-side break rate from the rate it sees itself,
so the remainder is what the tunnel added.

## Configuration

| Flag / environment | Default | Meaning |
|---|---|---|
| `-state` / `SHIM_STATE_DIR` | `/var/lib/flex-tailnet-shim` | Settings, token hash and tsnet state; survives reboots, lost on reinstall |
| `-api` / `SHIM_API_ADDR` | `:48992` | Provisioning API listen address |
| `-max-mtu` | 1200 | Clamp for `network_mtu=` |
| `SSDR_RADIO_ADDRESS` | set by the radio | Radio API address, normally 172.30.1.1 |
| `SHIM_LOG_URL` | unset | Development only: POST recent log lines to a listener every 15 s (the radio gives containers no log channel) |

## Open items

- Behaviour with two simultaneous AetherSDR clients, which uses both of the
  radio's GUI slots.
- FLEX-8400 and Aurora have not been tested.

## Vendored modules

Every Go module the shim links is vendored in `vendor/` (RFC #6271 ruling D1),
so the image builds from this tree with no module downloads; only the Go
toolchain (≥ 1.27.1, Tailscale's minimum) is needed. Go requires the vendor
tree beside `go.mod`, which is why it is here rather than under `third_party/`.
To update a dependency: `go get <module>@<version> && go mod tidy && go mod vendor`,
then refresh section 27 of `THIRD_PARTY_LICENSES`.

## Proving a remote path: `cmd/tailnet-probe`

`go run ./cmd/tailnet-probe -authkey-file key.txt 100.x.y.z:4992 192.168.50.103:9007`
joins the tailnet as an ephemeral node with its own userspace network stack,
accepts subnet routes, reports which peer routes each LAN target, and dials
every target. Its traffic can only cross the tailnet, so it gives an honest
answer even when run on the radio's own LAN.
