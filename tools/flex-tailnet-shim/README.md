# flex-tailnet-shim

Prototype. Not built by CMake, not shipped, and pending an RFC.

`flex-tailnet-shim` runs inside a FLEX-8000-series or Aurora radio as a Docker
waveform container. It joins a Tailscale tailnet with
[`tsnet`](https://pkg.go.dev/tailscale.com/tsnet) and relays AetherSDR's
SmartSDR session to the radio. Both ends make only outbound connections, so it
works when the radio and the operator are both behind CGNAT. In that case
traffic goes through Tailscale's DERP relays.

AetherSDR needs no changes. Run Tailscale on the operator's machine and use the
existing manual/routed connect with the shim's tailnet IP.

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
- **Identity:** every connection is checked with Tailscale `WhoIs`. `-allow`
  limits access to listed logins or tags. The shim listens only on its own
  tailnet ports, so the radio's other services, such as its SSH, are not
  exposed.
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

## Build

```sh
CGO_ENABLED=0 GOOS=linux GOARCH=arm64 go build -trimpath -ldflags='-s -w' -o flex-tailnet-shim .
go test -race .
```

The container image is a standard OCI image for `linux/arm64`. It needs the
labels `com.flexradio.waveform.name` and `com.flexradio.waveform.version`, and
an entrypoint that runs the binary. It needs no base-image CA bundle, because
the Mozilla roots are compiled in.

## Configuration

| Flag / environment | Default | Meaning |
|---|---|---|
| `-authkey-file` / `SHIM_AUTHKEY_FILE`, or `TS_AUTHKEY` | `/etc/flex-tailnet-shim/authkey` | Tailscale auth key, needed only on first start |
| `-hostname` / `SHIM_HOSTNAME` | `flex-radio` | Tailnet node name |
| `-state` / `SHIM_STATE_DIR` | `/var/lib/flex-tailnet-shim` | tsnet state; survives reboots, lost on reinstall |
| `-allow` / `SHIM_ALLOW` | any tailnet peer | Comma-separated logins or tags |
| `-max-mtu` | 1200 | Clamp for `network_mtu=` |
| `SSDR_RADIO_ADDRESS` | set by the radio | Radio API address, normally 172.30.1.1 |
| `SHIM_LOG_URL` | unset | Development only: POST recent log lines to a listener every 15 s (the radio gives containers no log channel) |

## Open items

- Auth-key delivery for real users. The image must not carry a secret when
  published, so the installer would add a per-user config layer at install
  time.
- Bandwidth controls for the relay path: Opus by default when the radio is
  reached over a tailnet, `low_bw_connect`, and a lower FFT rate.
- Behaviour with two simultaneous AetherSDR clients, which uses both of the
  radio's GUI slots.
- FLEX-8400 and Aurora have not been tested.
