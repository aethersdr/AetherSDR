# TCI Peripheral Discovery — mDNS Service Schema

_Proposed in [#2502](https://github.com/ten9876/AetherSDR/issues/2502) as
a follow-up to [#2456](https://github.com/ten9876/AetherSDR/pull/2456).
This document defines the discovery contract; the Qt-side browse
implementation lives in a separate change._

## Scope

AetherSDR speaks TCI to a growing set of LAN peripherals — keyboards
(`aether_pad`), antenna switches (`ShackSwitch`), and over time
rotators, amplifiers, and other TCI-capable devices.  Each of these
needs to be discoverable on the local network without the user having
to type an IP address.

Rather than host a peripheral-specific UDP responder inside AetherSDR
per device (the pattern `AntennaGeniusModel` follows on port 9007), new
peripherals advertise themselves via mDNS / DNS-SD.  AetherSDR performs
a single generic browse when the Peripherals settings panel is opened.

This document is the **contract** that peripheral implementers and
AetherSDR's browse code both target.  It is intentionally narrow:
service type + TXT record keys + values.  Anything else (the Qt
browser, the settings UI, the library choice) is out of scope and
covered separately.

## Service type

```
_tci._tcp.local.
```

Rationale:

- TCI is the protocol AetherSDR speaks to peripherals already
  (`src/core/TciServer.h`, `src/core/TciProtocol.cpp`).  Naming the
  service after the protocol — not the AetherSDR brand — keeps the
  registration usable by any TCI-aware client (SDConsole, ExpertSDR,
  third-party tools) and matches how `_http._tcp`, `_ipp._tcp`, or
  `_airplay._tcp` are scoped.
- `_tcp` because TCI runs over WebSocket-over-TCP.
- Not registered with IANA.  AetherSDR treats this as a de-facto
  convention.  If a future formal registration uses a different name,
  AetherSDR will browse both during a transition period.

### What advertises

The **peripheral** advertises.  AetherSDR is the **browser**.  AetherSDR
does not register a `_tci._tcp.local` service of its own — the radio
side of TCI is reachable through the existing FLEX `_flex._tcp.local`
discovery already published by the radio firmware, and AetherSDR's TCI
server (port 50001) is for inbound clients (WSJT-X, JTDX), not LAN
discovery.

### Port

The SRV record carries the peripheral's TCI listening port.  There is
no fixed port — peripherals pick what suits them.  AetherSDR uses the
SRV port verbatim when initiating the TCI WebSocket connect.

### Instance name

The DNS-SD instance name (the user-visible label, e.g. `ShackSwitch in
the shack._tci._tcp.local.`) is the peripheral's choice and is shown
verbatim in the AetherSDR Peripherals list.  Peripherals SHOULD pick a
name that is meaningful to the operator (room, callsign, hardware
serial suffix) rather than a generic model string — the `model` and
`class` TXT keys below are for machine-readable identification.

## TXT record schema

All keys are lowercase ASCII.  Values are UTF-8.  Unknown keys MUST be
ignored by AetherSDR; peripherals MAY add vendor-specific keys
(prefixed `x-`) for their own purposes.

| Key           | Required | Value                                                                                               | Example                  |
| ------------- | -------- | --------------------------------------------------------------------------------------------------- | ------------------------ |
| `model`       | yes      | Stable machine-readable model identifier.  Lowercase, hyphen-separated.  Not a marketing name.      | `shackswitch-v2`, `aether-pad` |
| `class`       | yes      | Device class.  One of the values in the table below.                                                | `antenna-switch`         |
| `tci-version` | yes      | Highest TCI protocol version the peripheral speaks, as `MAJOR.MINOR`.                               | `1.9`                    |
| `txtvers`     | yes      | TXT record schema version.  Currently `1`.  Increment if a future revision changes key semantics.    | `1`                      |
| `x-*`         | no       | Vendor-specific keys.  AetherSDR ignores these but may surface them in a "details" view in future. | `x-serial=AB-12-34`      |

### `class` values

The class is what AetherSDR uses to route a discovered peripheral to
the right settings sub-panel and applet.  The set is closed — adding a
new class is a docs PR, not a peripheral-side decision.

| Value             | Meaning                                                            |
| ----------------- | ------------------------------------------------------------------ |
| `controller`      | Keyboard / panel / generic operator interface (e.g. `aether_pad`). |
| `antenna-switch`  | Antenna selector / matrix (e.g. `ShackSwitch`).                    |
| `rotator`         | Antenna rotator.                                                   |
| `amplifier`       | RF amplifier.                                                      |
| `tuner`           | Antenna tuner.                                                     |
| `other`           | Anything that doesn't fit the above; AetherSDR shows it under "Other peripherals". |

If `class` is missing or unrecognised, AetherSDR treats it as `other`.

### Required vs optional, in practice

- A peripheral missing `model`, `class`, `tci-version`, or `txtvers`
  MAY still appear in the discovery list (under "Unknown peripheral")
  so operators can see that *something* is responding, but AetherSDR
  will not auto-route it to a class-specific applet.
- AetherSDR MUST NOT crash, refuse to populate the list, or hide other
  valid peripherals because one entry is malformed.

## Example advertisement

A `ShackSwitch` v2 unit on the shack network with TCI listening on
port 4532 advertises:

```
Instance:  Shack rotor wall._tci._tcp.local.
SRV:       _tci._tcp.local. → shackswitch-3a.local. : 4532
TXT:
  txtvers=1
  model=shackswitch-v2
  class=antenna-switch
  tci-version=1.9
  x-serial=SS2-0007
```

AetherSDR's browse will pick this up, show `Shack rotor wall` in the
Peripherals → Antenna Switch list, and on connect open a TCI WebSocket
to `shackswitch-3a.local:4532`.

## Coexistence with existing UDP discovery

AetherSDR currently discovers `Antenna Genius` via a UDP listener on
port 9007 (`src/models/AntennaGeniusModel.cpp`).  That path is **not
removed** by this proposal:

- The AG firmware broadcasts on 9007; AetherSDR continues to listen
  for it.  Changing the AG side is out of AetherSDR's control.
- The mDNS path is **additive** for peripherals that have not shipped
  yet (`aether_pad`) or that choose to advertise both ways
  (`ShackSwitch`).
- A peripheral that advertises via mDNS AND replies to AG-style
  broadcasts will appear once in the list — AetherSDR deduplicates by
  resolved IP+port, mDNS taking precedence for display name.

Manual IP entry (the per-row fields in `RadioSetupDialog`'s Peripherals
tab) remains available and is the supported fallback for segmented
networks, guest VLANs, or other environments where mDNS does not
propagate.

## Versioning this contract

Schema changes are tracked by the `txtvers` TXT key.

- **`txtvers=1`** — this document.
- A future `txtvers=2` would mean key semantics have changed
  incompatibly.  AetherSDR will continue to parse `txtvers=1`
  advertisements during a deprecation window of at least one full
  release cycle.

Adding a new optional key, a new `class` value, or a new `x-*`
extension does **not** require bumping `txtvers`.  Only removing,
renaming, or changing the meaning of an existing required key does.

## Out of scope

- The Qt-side mDNS browser implementation (library choice, threading,
  cache lifetime).  Tracked separately in the implementation PR.
- The Peripherals settings UI redesign.
- Authentication / authorisation between AetherSDR and the peripheral.
- Discovery of the radio itself — that remains the FLEX-side
  `_flex._tcp.local` registration handled by the radio firmware.
