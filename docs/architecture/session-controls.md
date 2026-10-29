# Session initialization and client management

`SessionControlRequest` is the optional backend contract for desktop session
initialization. `FlexBackend` advertises its supported fields and owns command
encoding and query decoding in `FlexSessionControls`. Other backends leave the
record absent and do not receive Flex registration or subscription requests.
This contract is not exposed as a daemon RPC or a transmit authorization.

`RadioModel` retains connection orchestration, operator-selected client
disconnections, GUI identity policy, `GuiClientRegistrationState`, settings
ownership and session restoration. Program identification and optional low
bandwidth precede GUI registration. The slice enumeration window opens before
registration, because restored statuses may precede its reply. Subscriptions
remain ordered batches without per-topic acknowledgement waits. LAN conflict
inspection, WAN validation, UDP registration and the one port-collision retry
retain their existing order. Optional subscription refusals do not invalidate
registration. Station and MTU initialization reuse the setup encoder without
calling the settings-edit adapter.

Each request captures the model session and a thread-safe cancellation token.
The transport pointer and generation are pinned when the session starts. LAN
writes check both the generation and token on the connection worker immediately
before writing. WAN writes check the same invariants on their owning thread.
Retirement cancels pending completions; a request completes at most once and
expires after 30 seconds. Required GUI registration or pre-registration client
disconnection timeouts close the connection through the existing reconnect
path. Delayed continuations, stream checks and ping callbacks retain the old
session fence, so they cannot act on a replacement session.

Query results are typed and bounded. A failed or malformed slice inventory is
distinct from a successful empty inventory and cannot create a default slice.
Info fields are applied only when present; IARU region retains its existing
snapshot semantics (unknown when absent or invalid). Identity is installed
before notifying observers. Persistent GUI identity is updated only by an
accepted registration reply under the existing transient-identity policy.

`session_control_test` uses production encoders, parsers and write gates with
recording sinks and inert transports. It checks ordering, settings isolation,
reconnect races, cancellation, timeout cleanup and reentrant signal handlers
without connecting or binding a socket. Real-radio validation is separate:
connect, receive, inspect state, disconnect and reconnect via the automation
bridge with TX automation disabled. SmartLink WAN end-to-end validation remains
distinct from socket-free WAN write-gate tests.

Media stream creation, audio/DAX ownership, NetCW/CWX, file transfers and daemon
audio ownership remain separate contracts.
