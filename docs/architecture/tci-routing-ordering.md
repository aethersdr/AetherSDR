# TCI Routing and Ordering Contract

This document defines how AetherSDR projects TCI receivers and VFO channels
onto FlexRadio slices, panadapters, and the radio-global TX slice. It is the
implementation contract for [issue #4220](https://github.com/aethersdr/AetherSDR/issues/4220).

## Evidence

The contract reconciles four sources:

- TCI 2.0 command direction and receiver/channel grammar.
- The Thetis state-machine oracle at
  `/Users/patj/oracles/thetis-tci-oracle.md`.
- FlexRadio status-before-reply ordering from
  `/Users/patj/oracles/flexlib-oracle.md`.
- Client sequences verified in WSJT-X Improved 3.1.0, JTDX 2.2.159, and
  wfview 2.23/2.24 and recorded on issue #4220.

The protocol and client implementations are compatibility evidence. Flex
status and command replies remain authoritative for accepted radio state.

## Invariants

| Area | Invariant | Source / bug evidence | Implementation proof |
|---|---|---|---|
| Startup barrier | Identity, topology, both VFO channels, stream settings, then `ready;` are emitted in one deterministic order. | Thetis queue can reorder coalesced state around `ready`; WSJT-X seeds requested frequencies from the init burst. | `TciProtocol::generateInitBurst()` and `tci_protocol_test`. |
| Stable ownership | Routing state stores Flex slice IDs, never a retained contiguous TRX index. | #3715, #4160, lower-index slice churn. | `TciRoutingState`; TRX translation occurs at command/broadcast boundaries. |
| VFO A | `vfo:<trx>,0` targets the RX slice represented by that TRX. | TCI 2.0, WSJT-X startup tuning. | `TciServer::handleVfoRequest()`. |
| VFO B | `vfo:<trx>,1` targets a distinct TX slice when available. Resolution order is an external radio-selected TX slice, a previously tracked route, then a new slice on the RX pan. An arbitrary non-TX slice is never treated as spare because it may be an operator's independent receiver. | #1686, #1807, #2102, JTDX and WSJT-X Rig sequences. | `TciRoutingState::resolveVfoB()` and table-driven tests. |
| No-op tuning | A requested frequency already present in authoritative model state is acknowledged immediately. | WSJT-X waits 2 s for VFO A and 1 s for VFO B; a no-op produces no Flex status edge. | `TciServer::tuneSliceAndConfirm()`. |
| Changed tuning | A changed frequency is confirmed only after the Flex command succeeds. | Flex status precedes command reply; #3543 and startup tuning errors. | Command callback is the completion barrier; sender and observers receive the accepted coordinate. |
| Split state | `split_enable` is explicit shared TCI state, not inferred from transient TX-slice topology. A true request is confirmed only after a distinct external, previously tracked, or newly created TX route is selected. | Thetis exposes one global `VFOSplit`; #1686. | `TciRoutingState::setSplitRequested()` and ordered route continuations in `TciServer::handleSplitRequest()`. |
| WSJT-X compatibility | A steady `split_enable:false` never discards VFO B or moves TX. | WSJT-X Improved 3.1.0 always sends false before programming channel 1. | Edge-only reclaim in `TciServer::handleSplitRequest()`. |
| External split | Disabling TCI split never reclaims an externally selected TX slice. | Satellite/full-duplex #1807. | External route owner is preserved and tested. |
| TCI split teardown | A true-to-false edge reclaims the RX slice only for a TCI-owned route; a TCI-created slice is then removed. | #311, #4051, orphan-slice risk. | Route owner plus ordered `tx=1` then `slice remove`. |
| Topology serialization | VFO-B promotion/creation, split edges, and TRX route selection are serialized. PTT waits behind earlier route work; topology-changing commands wait until current TX is fully unkeyed. | Thetis selects `trx` before MOX and treats actual MOX as authoritative; Flex status/reply ordering. | Generation-tagged transition barrier and ordered deferred-command queue in `TciServer`. |
| PTT ordering | Resolve/select TX slice, prepare DAX when requested, send `xmit`, wait for interlock, then emit `trx:true` and start TX chrono. | Thetis PTT poller; WSJT-X/JTDX route-before-key sequence; #4090. | `TciServer::handleTrxRequest()` and `onRadioTransmittingChanged()`. |
| PTT rejection | No successful `trx:true` is emitted if the radio never enters `TRANSMITTING`. | Radio error `50000043`, #4090. | 1.25 s confirmation timeout emits actual false state and releases prepared audio. |
| PTT cancellation | A `trx:false` that cancels an unconfirmed key-up establishes a bounded fail-closed barrier. A late radio TX edge is unkeyed and is never exposed as a new external `trx:true` session. | Thetis treats actual MOX as authoritative; Flex command/status streams can cross during rapid key/unkey. | Cancellation generation plus deferred route/PTT drain in `TciServer`. |
| Deferred PTT cancellation | A client's `trx:false` removes its deferred `trx:true` and queued route commands before checking current PTT ownership. Route completion therefore cannot replay a stale key-up after the abort. | PR #4407 review sequence: split transition -> deferred key-up -> abort -> transition completion. | Client-scoped cancellation at the start of `TciServer::handleTrxRequest()`'s unkey path. |
| PTT ownership | Only the client that initiated a TCI PTT session may release it. Unowned `trx:false` reports actual state and never unkeys operator, VOX, or another client. | Thetis assigns one active TX owner before MOX; existing user communities may key outside TCI. | Owner checks in `TciServer::handleTrxRequest()`; teardown bypasses optional outro delays and fails closed. |
| PTT source | DAX/TCI audio uses `RadioModel::setTransmit(...Dax)`. Hardware-style TCI PTT uses the shared preflight/Quindar coordinator, with no second optimistic key path. | #2262 and the Thetis `trx`-before-MOX ordering. | Deferred parser plus one source-specific key path in `TciServer::handleTrxRequest()`. |
| Sender convergence | The initiating client receives the same accepted VFO, split, and TRX state as passive observers. | #4161; WSJT-X exact PTT wait. | Confirmations use `broadcast()` rather than excluding the sender. |
| `tx_enable` direction | `tx_enable` is notification-only and cannot select a TX slice when received from a client. | TCI 2.0 and Thetis command catalog. | Parser ignores inbound SET; test asserts no mutation/notification. |
| Power command shape | Every `drive` and `tune_drive` reply or notification contains `trx,power`; a bare power value is never emitted. | TCI 2.0; #4343; ESDR3-mode WSJT-X/JTDX index `args[1]` after matching `args[0]`. | Shared `txTrx()` fallback and exact wire-string tests. |
| Power read/write direction | `drive:<trx>` and `tune_drive:<trx>` are reads; only the two-argument `<trx>,<power>` form writes. Bundled Elgato and StreamController senders use the two-argument form. | TCI 2.0; hardware-observed silent power rewrite in #4345. | Strict parser cardinality/range checks plus coordinated plugin updates. |
| Disconnect safety | Loss of the PTT/TX-audio owner unkeys, stops chrono, clears DAX TX mode, and releases TCI-created routing on last-client exit. | Thetis ownership cleanup; #4084, #4144. | Idempotent disconnect and server-stop paths. |

## Wire Projection

TCI receiver indexes remain contiguous positions in AetherSDR's owned-slice
list. They are translated on every request and event. Internal routing state
uses the resulting stable Flex slice IDs.

| TCI coordinate | Flex projection |
|---|---|
| `(trx, channel 0)` | RX slice represented by `trx` |
| `(trx, channel 1)` | Resolved radio-global TX slice for that RX route |
| `split_enable:<trx>` | Shared TCI split-request state, echoed under the requested receiver |
| `trx:<trx>` | Explicit PTT intent for the route; acknowledgement is actual interlock state |
| `tx_frequency` | Frequency of the radio-authoritative TX slice |
| `tx_enable` | Radio-authoritative TX-slice availability/selection notification |

`channels_count:2` is advertised because both channels are addressable. A
physical TX slice may also appear as another contiguous receiver in the
current topology. In that case its frequency can legitimately be projected
both as its own channel 0 and as the owning RX route's channel 1.

## Required Sequences

### WSJT-X Startup Tune

```text
client: vfo:0,0,<rxHz>
server: dispatch Flex slice tune
radio:  status with accepted frequency
radio:  successful command reply
server: vfo:0,0,<acceptedHz> to sender and observers
```

If `<rxHz>` already equals current state, the final server message is emitted
without a radio write. This closes the no-status-edge timeout at startup.

### JTDX Rig Split

```text
split_enable:0,true
resolve/promote/create TX slice
split_enable:0,true
split_enable:0,true
vfo:0,1,<txHz>
vfo:0,1,<acceptedTxHz>
trx:0,true,tci
prepare DAX -> xmit 1 -> interlock TRANSMITTING
trx:0,true
TX_CHRONO
```

### WSJT-X Rig Compatibility

```text
split_enable:0,false
split_enable:0,false
vfo:0,1,<txHz>
resolve/promote/create TX slice without changing splitRequested
vfo:0,1,<acceptedTxHz>
trx:0,true,tci
```

The false split flag is not interpreted as permission to erase channel 1.

### Satellite / External TX

```text
external controller selects slice B as TX
TCI route for TRX 0 observes RX slice A and TX slice B
vfo:0,1,<uplinkHz> tunes B
trx:0,true keys B without selecting A
```

## Test Layers

| Layer | Current coverage | Next required coverage |
|---|---|---|
| Pure routing | Exact WSJT-X steady-false -> VFO B -> TRX sequences for single-slice creation, preservation of an independent second receiver by creating a separate route, and external satellite TX preservation; split edges and stable-ID removal | Multiple independent route requests and cross-pan receive-only endpoints |
| Parser | VFO B, invalid VFO channel, split, TRX source, `tx_enable` direction, exact DRIVE/TUNE_DRIVE read/write forms, init topology/order | Invalid numeric/range corpus for every remaining routing command |
| Radio sequencing | Completion-barrier implementation reviewed against Flex oracle | Fake delayed/rejected radio adapter with exact command transcript assertions |
| Automation / WebSocket | Fake server validates WSJT-X init negotiation, raw ordered `send`, bounded bidirectional `trace`, atomic export, read-only `routes`, and observe-only enforcement | Two-client sender/observer convergence and abrupt owner loss |
| Real clients | Sequence matched to WSJT-X/JTDX source | Automated WSJT-X/JTDX startup, Rig/Fake It, repeated FT8 cycles |
| Live radio | No RF required for frequency/split inspection | Dummy-load PTT confirmation, rejection injection, satellite full duplex |

No live-radio test may key RF without the normal automation-bridge TX safety
gate, explicit authorization, and a dummy load.
