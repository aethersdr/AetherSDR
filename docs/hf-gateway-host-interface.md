# HF Gateway: host interface provenance

RFC #7296. This note records where AetherSDR's knowledge of the VARA-style TCP
host interface comes from, under Constitution Principle IV. It covers
`src/core/hfgateway/ModemHostProtocol.{h,cpp}` and everything built on it.

If a token isn't listed here, the code doesn't use it.

## Sources

| Id | Source | Version |
|---|---|---|
| D2G | Data2G host layer, vendored: `third_party/data2g/core/host/host.hpp` (the header comment lists the command and notification vocabulary) and `host.cpp` | v0.5.0, as vendored (#7283) |
| MER | Mercury `docs/TNC.md`, "Mercury TNC Command Reference", github.com/Rhizomatica/mercury | commit `762bb75bbc541ac55a0be98fcd6c66e72fef1773` |
| EA5HVK | The VARA specification published by its author, §3.6 ("VARA uses a TCP link to connect with others applications") | public PDF |

Both open-source sources describe an interface their modems implement so that
existing host applications work unchanged. AetherSDR implements that published
interface. No VARA binary, and no third-party VARA-compatible implementation's
source, informed this code.

## Transport

| Fact | Source |
|---|---|
| Two TCP ports: command on the base port, data on base + 1; default 8300/8301 | MER (table, "base_port"), D2G (header comment: command and data ports), EA5HVK §3.6 |
| Command lines are ASCII, each terminated by CR (`\r`) | MER ("All control commands and responses are CR-terminated ASCII") |
| Data port is an unframed byte stream, payload only while connected | MER ("Data Port") |
| One client per port at a time | MER ("One client at a time") |
| Replies `OK` / `WRONG` are positional: they answer the oldest outstanding command | D2G (`Replies OK / WRONG`), MER (each command's **Response**) |

LF is not part of the framing in either source. `LineSplitter` ignores it so that a CRLF sender still frames cleanly. That's a robustness choice, not something either interface requires.

## Modem → host notifications parsed (`parseNotification`)

| Token | Fields | Source |
|---|---|---|
| `OK`, `WRONG` | — | D2G, MER |
| `IAMALIVE` | — | D2G (`ALIVE_S`), MER |
| `PENDING`, `CANCELPENDING` | — | D2G (`host.cpp`, event pass-through), MER |
| `CONNECTED src dst bw` | `src` = initiator, `dst` = station called | D2G (`host.cpp`, master first), MER ("`<sourcecall>` is always the station that initiated") |
| `DISCONNECTED [reason]` | reason ignored | MER; D2G (`DISCONNECTED ABORT` with `DATA2G ON`) |
| `PTT ON` / `PTT OFF` | — | D2G, MER |
| `BUSY ON` / `BUSY OFF` | — | D2G, MER |
| `BUFFER n` | unacknowledged application bytes; 0 = all acknowledged | MER ("When it reaches 0, all data has been acknowledged"), D2G (`BUFFER_REPEAT_S`) |
| `SN x` | dB, one decimal | D2G (`SN %.1f`), MER |
| `BITRATE (code) bps BPS` | code, bps | MER ("BITRATE") |
| `REGISTERED call` | one line per callsign after `MYCALL`'s `OK` | D2G (`host.cpp`), MER ("REGISTERED") |
| `VERSION text` | free text | D2G, MER |
| `CQFRAME src bw` | — | D2G, MER |

Bandwidth tokens accepted: 500, 1200, 2300, 2750 (D2G `arq/modes.hpp` `bw_name`; MER "BW").

## Host → modem commands

### Sent by the gateway to a modem

| Command | Source |
|---|---|
| `MYCALL call [call...]` (1 + up to 4) | D2G, MER ("up to `CALLSIGN_MAX_SECONDARY` (currently 4)") |
| `LISTEN ON` / `LISTEN OFF` | D2G, MER |
| `DISCONNECT` | D2G, MER |
| `ABORT` | D2G, MER |
| `VERSION`, `BUFFER`, `SN` | D2G, MER |

Two behaviours the gateway's design relies on, both from MER:

- **`LISTEN OFF` cancels an incoming connect in progress.** It sends `CANCELPENDING` ("the host sent DISCONNECT or LISTEN OFF").
- **`ABORT` can key a final frame** ("it keys out of turn"). So the gateway quiets a modem that loses the channel with `LISTEN OFF`, never `ABORT`.

### Recognised from a back end (`parseCommand`)

Every command in the table above, plus the following. They're recognised so they can be answered and logged; the gateway's server decides the reply.

| Command | Source |
|---|---|
| `LISTEN CQ` | D2G, MER (treated as `LISTEN ON`) |
| `CONNECT from to` | D2G, MER |
| `CQFRAME call bw` | D2G, MER |
| `BW500` / `BW1200` / `BW2300` / `BW2750` | D2G (`bw_cap`), MER ("BW") |
| `BITRATE` | MER |
| `TUNE` | MER |
| `ENCRYPTION` | D2G (`IGNORED` list) |
| `CHAT` | D2G, MER |
| `PUBLIC` | D2G (`IGNORED`), MER |
| `COMPRESSION` | D2G (`IGNORED`), MER |
| `CLEANTXBUFFER` | D2G |

## Validation applied (Principle VII)

These limits are ours. They're set well above anything the sources produce.

- **Lines:** at most 1024 bytes, printable ASCII only. A violating line is discarded and counted.
- **Tokens:** matched exactly. `PTT ON` is two tokens, `PTT` then `ON`, and nothing else.
- **Callsigns:** 1-7 upper-case letters and digits, with at least one of each, plus an optional `-SSID` of 0-15.
- **`BUFFER`:** 0 to 16 MiB.
- **`SN`:** -50 to +60 dB, plain decimal only.
- **Bandwidths:** only the four listed above.
- **Builders:** a builder given invalid input returns an empty array, and the caller sends nothing.
