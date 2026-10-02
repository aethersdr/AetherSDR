# Agent guide — SmartSDR protocol, multi-pan, multi-client

Part of [`AGENTS.md`](../../AGENTS.md). Read this before touching the Flex wire
path (`src/core/backends/flex/`), slice/pan/status handling, Multi-Flex
behavior, or the model↔radio command flow. FlexLib is the protocol authority
(Constitution Principle I).

## SmartSDR Protocol (v1.4.0.0)

### Message Types

| Prefix | Dir | Meaning |
|--------|-----|---------|
| `V` | Radio→Client | Firmware version |
| `H` | Radio→Client | Hex client handle |
| `C` | Client→Radio | Command: `C<seq>\|<cmd>\n` |
| `R` | Radio→Client | Response: `R<seq>\|<hex_code>\|<body>` |
| `S` | Radio→Client | Status: `S<handle>\|<object> key=val ...` |
| `M` | Radio→Client | Informational message |

Status object names are **multi-word** (`slice 0`, `display pan 0x40000000`,
`interlock band 9`). The parser finds the split between object name and
key=value pairs by locating the last space before the first `=` sign.

### Connection Sequence

1. TCP connect → radio sends `V<version>` then `H<handle>`
2. `sub <topic> all` for each of: `slice`, `pan`, `tx`, `amplifier`, `atu`,
   `meter`, `audio`, `gps`, `apd`, `client`, `xvtr`
3. `client gui` + `client program AetherSDR` + `client station AetherSDR`
4. Bind UDP socket, send `\x00` to radio:4992 (port registration)
5. `client udpport <port>` (returns error 0x50001000 on v1.4.0.0 — expected)
6. `slice list` → if empty, create default slice (14.225 MHz USB ANT1)
7. `stream create type=remote_audio_rx compression=none` → radio starts sending
   VITA-49 audio to our UDP port

### Protocol / Firmware Quirks (v1.4.0.0 protocol on fw 4.x)

- `client set udpport` returns `0x50001000` — use the one-byte UDP packet method
- `client set enforce_local_ptt=1` returns `0x50001000` — correct command is `client set local_ptt=1`; the radio echoes a full `connected` status to ALL clients updating their `local_ptt` field when ownership changes
- Slice frequency is `RF_frequency` (not `freq`) in status messages
- Streams are discriminated by **PacketClassCode** (PCC), NOT by packet type
- `audio_level` is the status key for AF gain (not `audio_gain`)
- The radio **never sends `mox=` in transmit status messages**. Use
  `isTransmitting()` (interlock state machine), NOT `isMox()`
- Three separate tune command paths all need interlock inhibit:
  `transmit tune 1`, `tgxl autotune`, `atu start`
- `cw key immediate` not supported — use netcw UDP stream for CW keying
- `transmit set break_in=1` wrong — correct: `cw break_in 1`

VITA-49 packet format, PCC codes, FFT bin conversion, waterfall tile format,
audio payload, meter data — see `docs/architecture/vita49-format.md`.

## GUI↔Radio Sync (No Feedback Loops)

> **The `commandReady` plane is FROZEN and is being removed** (#5262 M4).
> It is described here because most of the tree still uses it, not as the
> pattern for new work: the wire text is Flex-only and silently dropped on
> HL2/Icom/ANAN/RTL. A new control emits a **typed intent** through
> `IRadioBackend` instead (see [backends.md](backends.md), command-plane
> freeze).

- Model setters emit `commandReady(cmd)` → `RadioModel` sends to radio
- Radio status pushes update models via `applyStatus(kvs)`
- Use `m_updatingFromModel` guard or `QSignalBlocker` to prevent echo loops

## Auto-Reconnect

`RadioModel` has a 3-second `m_reconnectTimer` for unexpected disconnects.
Disabled by `m_intentionalDisconnect` flag on user-initiated disconnect.

## Optimistic Updates Policy

Some radio commands lack status echo (e.g. `tnf remove`). Update the local
model optimistically. **File a GitHub issue** tagged `protocol` + `upstream`
for each missing status echo — optimistic updates break Multi-Flex.

## Multi-Panadapter Support

**Architecture:** PanadapterModel (per-pan state), PanadapterStream (VITA-49
routing by stream ID), PanadapterStack (QSplitter), wirePanadapter() (per-pan
signal wiring), spectrumForSlice() (overlay routing).

**Key protocol facts:**
- Click-to-tune: `slice m <freq> pan=<panId>` — NOT `slice tune`
- Never send `slice set <id> active=1` — managed client-side only
- Push `xpixels`/`ypixels` on pan creation (radio defaults to 50×20)
- FFT stream ID = pan ID (0x40xx), waterfall stream ID = waterfall ID (0x42xx)

See `docs/architecture/multi-pan-pitfalls.md` for the numbered pitfalls.

## Multi-Client (Multi-Flex) Support

Filter all status and VITA-49 packets by `client_handle` — three layers:
1. **Slice ownership**: track `m_ownedSliceIds` from `client_handle` field
2. **Panadapter status**: only claim `display pan`/`display waterfall` matching our handle
3. **VITA-49 UDP**: `setOwnedStreamIds(panId, wfId)` drops non-matching packets

Early status messages arrive WITHOUT `client_handle`. Create SliceModels for
all initially, remove other clients' when handle arrives.

## KiwiSDR / Web-888 receivers

The KiwiSDR browser is a clean-room, API-policy-aware public-receiver directory
(#3679), independent of the FlexRadio protocol path. Kiwi panadapters are
receive-only (TX is inhibited). See `docs/kiwisdr-public-directory.md` and
`docs/kiwisdr-cleanroom-design.md` (Principle IV). The Kiwi path also serves
the Web-888 (a KiwiSDR server fork): profiles carry a receiver type, and the
client applies the small wire deltas — see `docs/web888-cleanroom-design.md`.
