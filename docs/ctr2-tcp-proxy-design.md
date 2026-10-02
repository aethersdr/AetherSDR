# CTR2 TCP proxy prototype design

Status: Draft implementation specification requested by Jeremy on 2026-10-02.
No proxy has been implemented or tested by this document.

Build an optional TCP proxy inside AetherSDR so an unmodified CTR2-Max can
connect to the PC's LAN IP on port 4992. AetherSDR opens a separate TCP
connection to the configured radio and forwards the complete byte stream in
both directions. This tests the relay approach using current hardware before
adding a USB transport to the CTR2 firmware.

This document is the authority for this prototype. The
[USB relay design](ctr2-usb-relay-design.md) builds on it, replacing the
controller-facing TCP connection with HID framing while keeping the same
opaque forwarding.

## Required behavior

```text
Stock CTR2-Max in Wi-Fi mode
    <--> TCP --> PC LAN address:4992
                    AetherSDR TCP proxy
    <--> separate TCP connection --> configured radio address:4992

AetherSDR's existing radio connection remains separate.
```

The operator sets the CTR2's radio IP to the AetherSDR PC's LAN address.
The upstream destination is the radio AetherSDR is connected to, at the
address AetherSDR reached it by, including a routed VPN or tailnet address;
the operator does not enter it. The PC must be reachable from
the CTR2. The proxy uses the operating system's routing; it does not establish
a VPN, implement SmartLink authentication, or create a network tunnel.

Despite the informal name “telnet proxy,” this is an opaque TCP stream relay.
Do not implement Telnet negotiation or interpret Telnet control bytes.

- Forward every application byte unchanged, in order, once per connection.
- Preserve CR, LF, NUL, arbitrary binary bytes, and unknown commands.
- Do not decode text, split into protocol lines, filter, normalize line
  endings, renumber commands, rewrite handles or addresses, synthesize replies,
  inject subscriptions, send greetings, or add keepalive payloads.
- Forward unsolicited radio output immediately, including output received
  before the CTR2 sends anything.
- Preserve the byte stream, not TCP packet boundaries or individual read sizes.
  Scheduling and latency necessarily differ from a direct connection.
- Never inject proxy errors into either stream. Report errors only in the UI
  and application log.
- Never replay data or transparently reconnect an upstream connection under
  an existing CTR2 connection.

## Scope and protocol consequences

The first prototype forwards all TCP traffic on the accepted connection,
including any CW traffic, without distinguishing commands. It requires no
firmware patch, USB interface, MIDI mapping, or controller-specific parser.

Each accepted CTR2 connection gets a fresh upstream TCP connection. Do not
use RadioConnection::sendCommand, an existing backend socket, or
RadioModel's command dispatch. Those paths own AetherSDR's protocol state.
The proxy neither registers itself as a GUI client nor substitutes
AetherSDR's identity. The radio sees a separate connection originating at the
PC. The CTR2 controls registration, binding, subscriptions, and targeting
through its unchanged traffic. Correct MultiFlex selection is a hardware
validation item, not something this proxy guarantees or repairs.

TCP-only forwarding does not forward UDP discovery or UDP streams. Manual
radio IP configuration is the intended entry point. Payloads that advertise
addresses or ports remain untouched, so any corresponding UDP traffic may
still take a direct path, fail, or arrive at the PC without a listener.
Do not claim working meters, spectrum, audio, discovery, or complete Wi-Fi
parity without observing those features. Classify limitations using public
FlexLib/documentation and wire observations. A later UDP design must specify
endpoints and routing explicitly; do not silently add payload rewriting.

## Operator controls

Add a small CTR2 Proxy applet using existing applet patterns:

| Control or status | Prototype behavior |
| --- | --- |
| Listen address | Explicit local IPv4 LAN address selected by operator |
| Listen port | 4992, editable while stopped |
| Radio | The radio AetherSDR is connected to (port 4992), captured at Start; unavailable when AetherSDR is disconnected, on SmartLink, or on a radio without multi-client sessions |
| Start and Stop | Start listening or close listener and current connection |
| State | Stopped, Listening, Connecting, Relaying, Closing, or Error |
| Endpoints | Actual listener, connected CTR2 peer, and upstream destination |
| Traffic | Directional byte counters and queued bytes |
| Error | Last local socket/configuration error, separate from payload |

Start is disabled until configuration is valid. Freeze endpoints while
running; stop before editing them. The listener starts only on explicit
operator action and is off on every application launch. Do not silently
fall back to another port or bind all interfaces when a selected address is
unavailable. If multiple windows attempt port 4992, report the bind conflict.

Use a single active CTR2 connection for the prototype. Close additional
accepted connections without sending a banner and without disturbing the
active connection. Validate port ranges and reject destinations resolving to
the proxy's own local listener, including another local interface with the
same listening endpoint. IPv6 and hostnames can remain future scope.

No configuration persistence is needed for this prototype. If later added,
use one feature-owned AppSettings document; never persist active connections,
buffered commands, or an enabled-on-launch state.

## Implementation structure

Use C++20, Qt6 Network, and asynchronous sockets on the owning event loop.
No new dependency, worker thread, blocking waits, or external proxy process
is needed for the proposed prototype.

Suggested responsibilities, with names adjustable to repository conventions:

- `src/core/TcpByteProxy.{h,cpp}`: generic opaque relay, listener, socket pair,
  bounded queues, connection generation, counters, and shutdown state machine.
  Compile into libaethercore. It has no Flex syntax or widget dependencies.
- `src/models/Ctr2ProxyModel.{h,cpp}`: owns the relay, validates configuration,
  and exposes controls/status to the GUI. Keep the model independent of radio
  command dispatch and backend types.
- `src/gui/Ctr2ProxyApplet.{h,cpp}`: widgets bound to the model. Use
  ThemeManager and existing accessibility/app registration conventions.
- Wire lifetime in the appropriate existing MainWindow sibling, keeping
  new feature behavior out of MainWindow.cpp. The model stops before its
  owning window is destroyed. Hiding the applet does not imply Stop.

The destination is captured from AetherSDR's connected radio when the
operator presses Start and stays fixed until Stop. Changing the selected
AetherSDR radio must not retarget a live proxy. The model never reads
RadioModel: MainWindow pushes the radio in, and withholds it (with a reason)
when AetherSDR is disconnected, on SmartLink, or on a radio without
multi-client sessions. No RadioSession ownership
change or new IRadioBackend API is required by this design. Existing slice 0
RX, GUI session state, and audio/spectrum paths must remain unaffected.

## Buffering and failure handling

Use two independent pumps, one in each direction. Consume only what fits in
bounded buffers. Account for application queues, Qt socket read buffers, and
bytes queued for socket writes; do not treat write() acceptance as delivery
to the peer. Handle partial and zero writes without dropping or duplicating
bytes. Resume through socket progress signals without a busy loop.

Proposed prototype constants are a 256 KiB read-buffer limit on each socket,
a 256 KiB maximum application-plus-pending-write budget per direction, and
at most 64 KiB transferred per event-loop pump before yielding. These are
engineering defaults, not protocol message limits. Apply backpressure when
full rather than growing a queue or discarding selected bytes. Bound OS
buffers separately as platform-managed resources; these limits are not a
claim about total process or kernel memory.

Use a 10-second upstream connect timeout and a 10-second no-forward-progress
timeout only while bytes are pending. An idle, empty connection has no
application heartbeat or idle expiry. Timeout constants should be injectable
for tests. Start/Stop, an error, or a new connection generation invalidates
old callbacks and queues. Unsent data never enters another connection.

On orderly EOF, stop new reads from the closing side, preserve already-read
bytes, and attempt a bounded drain to the other side before closing the pair.
The prototype may close the entire pair after this drain; it need not support
long-lived TCP half-close semantics. Describe that limitation in test results.
On hard error, timeout, or explicit Stop, close the pair and discard remaining
queues with a visible reason and pending-byte counts. Do not claim delivery
of bytes lost at a broken connection. A subsequent CTR2 reconnect creates a
new pair; the listener may remain active after a peer failure.

## Transmit and review boundary

This opaque relay cannot inspect operator intent or apply AetherSDR's
per-command TxCoordinator admission without abandoning the requested design.
Treat the CTR2 as an independent external radio client. Do not claim that
AetherSDR's TX arbitration protects this connection. The proxy never creates
a key-down, retries a command, or sends an invented key-up on disconnect.
Closing a TCP socket is not proof that the transmitter is idle.

Forwarding is unchanged by design, including whatever the CTR2 sends; do not
add a command filter, and do not bypass any guard in AetherSDR's own command
path. RFC #6091 (approved) settles how this fits Principle VI: the relay
carries traffic only after the operator explicitly enables it, it is off on
every launch, and AetherSDR's existing transmit indicator shows when the radio
is on the air. No separate relay TX indicator, confirm-on-Start notice, or
LAN-only restriction is required. CW/PTT tests through the relay still need an
operator-controlled test setup and explicit transmit authorization.

## Validation and acceptance

1. Inject transports into the pump/state-machine tests: fragmented reads,
   coalesced data, partial/zero writes, backpressure, bounds, timeout, Stop,
   stale callbacks, and queue isolation across reconnects.
2. Compare complete streams in both directions, including all byte values,
   mixed line endings, large transfers, and an unsolicited upstream greeting.
   Equality and ordering must survive different chunk boundaries.
3. Exercise our proxy server on loopback with generic byte endpoints, not a
   fake Flex radio. Use ephemeral test ports. Disclose socket-owning tests
   under the tests/ guide, including bind failures and their skip policy.
   Test busy-client rejection, unavailable upstream, bind conflicts, normal
   close, and cleanup. Register in tests/tests.cmake, not the frozen CI gate.
4. Build AetherSDR and run the relevant tests/static checks. Record which
   platforms were actually built; Qt portability is not cross-platform proof.
5. Start AetherSDR's ordinary radio session. Start the proxy on the PC LAN IP
   and port 4992, pointing upstream at the actual radio. Configure the stock
   CTR2 to that PC IP. Observe startup, display updates, tuning, mode changes,
   and binding to the intended MultiFlex session. Confirm existing RX remains
   intact. Record unsupported UDP-dependent features separately.
6. Stop/restart the proxy and disconnect/reconnect the CTR2. Confirm no stale
   command replay and clear local error reporting. Measure receive/control
   behavior over the intended VPN separately from same-LAN behavior.

Acceptance is unchanged bidirectional TCP delivery, bounded resource use,
usable manual startup/shutdown, and demonstrated stock-hardware TCP control.
Automated byte tests alone do not establish hardware compatibility. No live
RF transmission is part of automated acceptance.

## Alternatives and provenance

A standalone TCP forwarder would be a quick transport experiment but would
not deliver the requested in-application controls. Reusing AetherSDR's
command connection would require mediation and violate opaque forwarding.
A complete UDP/USB relay remains later work; a TCP prototype does not replace
that objective.

Sources for this design are the operator's explicit requirements, general TCP
relay behavior, and the repository's architecture and contribution guides.
No proprietary firmware disassembly, addresses, inferred routines, or
binary-derived protocol fixtures are used. Implementers must not consult the
firmware investigation when building this host feature.

Local references: [governance](../GOVERNANCE.md),
[constitution](../CONSTITUTION.md), [backend guide](agents/backends.md),
[GUI guide](agents/gui.md), [test guide](agents/tests-ci.md), and
[RadioSession](../src/models/RadioSession.h).

## Prototype implementation record

Status (2026-10-02): implemented on branch `ctr2-tcp-proxy-prototype` (PR
#6090) under approved RFC #6091; not merged. Jeremy reports the relay working
with a stock CTR2 (recorded in [the USB relay design](ctr2-usb-relay-design.md));
that hardware result was not observed by this branch's validation.

Architectural distinction for review:

- The relay is an independent external radio client path. It opens its own
  upstream TCP connection, never uses `RadioConnection`, `RadioModel`, a
  backend socket or `IRadioBackend`, and never registers as a GUI client.
- Bytes the CTR2 sends, including any CW/PTT/transmit commands, reach the radio
  without passing `TxCoordinator` or any AetherSDR transmit guard. AetherSDR's
  own transmit paths are unchanged and not bypassed. The proxy never keys,
  retries, replays or sends an invented key-up; closing a socket is not proof
  the transmitter is idle. RFC #6091 approved this under Principle VI on
  the basis of explicit operator enable plus the existing TX indicator.
- TCP only. UDP discovery, meters, panadapter/audio streams and SmartLink are
  not forwarded; MultiFlex binding is whatever the CTR2's own traffic does.

Code map:

| Piece | File |
| --- | --- |
| Transport-agnostic two-pump relay, budgets, timeouts, drain | `src/core/ByteRelay.{h,cpp}` |
| Listener, single client, per-connection upstream, generations | `src/core/TcpByteProxy.{h,cpp}` |
| Config validation, frozen-while-running, throttled stats | `src/models/Ctr2ProxyModel.{h,cpp}` |
| Applet (`CTR2`, Integration, off by default, not in default order) | `src/gui/Ctr2ProxyApplet.{h,cpp}` |
| Lifetime wiring; stop on close | `MainWindow_Controllers.cpp` `setupCtr2Proxy()`, `MainWindow::closeEvent` |

Behavior as built: 256 KiB socket read buffers; 256 KiB per-direction budget
(relay pending + destination unsent); 64 KiB per pump before a queued
continuation; 10 s upstream connect timeout; 10 s no-progress timeout armed
only while bytes are queued; 10 s drain after orderly EOF. On EOF the closing
side's buffered bytes are forwarded, the reverse direction is dropped and
counted (no long-lived half-close), and both sockets close gracefully. A CTR2
that disconnects before the upstream connect completes has its buffered bytes
discarded, not forwarded. Additional clients are closed without a byte. Errors
and discard counts go to the applet and the `aether.devices` log only.

### Validation actually run (Linux only, 2026-10-02)

- Built: `AetherSDR`, `byte_relay_test`, `tcp_byte_proxy_test` on Arch Linux,
  local Qt 6.12.0, RelWithDebInfo. macOS and Windows were not built.
- `byte_relay_test` (socket-free, injected transports): all byte values and
  CR/LF/NUL both ways, unsolicited upstream-first output, randomized
  fragmented/coalesced reads and flushes, partial and zero writes, budget
  bound and source backpressure, 64 KiB slice yield, progress timeout only
  while pending, Stop discard counts and late-callback isolation, generation
  isolation, orderly EOF tail drain, drain timeout with reverse-direction
  discard, write error. Pass.
- `tcp_byte_proxy_test` (binds 127.0.0.1 ephemeral TCP only; generic byte
  peers; exit 77 on bind refusal): 2 MiB each way with an upstream greeting,
  additional-client rejection without disturbing the active pair, refused
  upstream, deterministic connect timeout, bind conflict without fallback,
  self-proxy validation, client- and upstream-initiated EOF tail delivery,
  reconnect isolation, Stop. Pass, 20/20 repeated runs.
- Mutation checks (each restored afterwards): dropping a partial-write
  remainder, ignoring the budget, closing before the tail drains, removing
  the slice yield, accepting a second client, and aborting instead of a
  graceful close each turned the suite red.
- Static checks run locally: test registration, engine boundary, QSettings,
  network timeouts, CI test gate, colour ratchet (+0), a11y (0 findings).
- GUI: launched with a scratch config (no radio connected) and the automation
  bridge; the applet renders, the proxy is Stopped at launch, Start is dimmed
  with a stated reason, and invalid address/port/self-proxy entries are
  refused. The listener was never started.

Not verified: any stock CTR2-Max hardware behavior, MultiFlex binding, VPN
paths, UDP-dependent features, behavior on macOS/Windows, sanitizer lanes.
Half-close is unsupported: after one side's orderly EOF the reverse direction
is dropped and counted.
The no-progress timer is shared by both directions, so progress in one
direction masks a stall in the other. A Wi-Fi drop without a FIN leaves the
pair Relaying until queued bytes stall for 10 s; Stop clears it immediately.
