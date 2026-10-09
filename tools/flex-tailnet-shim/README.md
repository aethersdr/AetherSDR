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
  open sessions, transfers and device connections it no longer admits. The shim listens only on its own tailnet ports, so the
  radio's other services, such as its SSH, are not exposed, and a shared
  route never splices into the radio's own addresses.
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
   stays unprovisioned until it receives an auth key.
2. **Provision:** the container's row in the Waveforms list has a
   **Configure…** button. Paste a single-use Tailscale auth key, choose a
   machine name and, optionally, who may connect, then press **Join Tailnet**.
   AetherSDR sends the key to the container over the LAN and does not keep it.
   The container joins the tailnet and returns an admin token, which AetherSDR
   stores in the OS keychain, one entry per radio.
3. **Connect remotely:** from anywhere on the tailnet, use Connect → Manual
   with the address shown in the dialog.

The node's tailnet state survives radio reboots, so the key is needed only
once. Removing or reinstalling the container wipes that state.

## Provisioning API

The API is plain HTTP on the radio's LAN address, TCP 48992. It is never on
the tailnet: tsnet's listeners are in their own network stack. Callers on the
radio's internal container network (172.30.0.0/16) and non-private addresses
are refused.

| Method | Path | Token needed | Does |
|---|---|---|---|
| GET | `/v1/status` | no | State, tailnet IP and DNS name, allowlist, session count, version, last error. Never a secret. |
| POST | `/v1/provision` | only once provisioned | `{auth_key, hostname, allow[]}`: joins (or, with the token, re-joins) the tailnet and returns `{admin_token, status}` |
| PUT | `/v1/allow` | yes | `{allow[]}`: changes who may connect without leaving the tailnet |
| POST | `/v1/signout` | yes | Logs out, deletes the node state and forgets the token |

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
