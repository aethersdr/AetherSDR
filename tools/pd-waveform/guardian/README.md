# Unkey-only guardian candidate

This development helper is included by the shim builder’s `--with-aetherpd`
option and started by Go before the DSP for a prepared managed session. It
is a separate process from the DSP and Go owner. It accepts no
network requests and never keys the transmitter.

A trusted local session provider must prepare:

- A dedicated TCP connection to `127.0.0.1:4992`, with the operator GUI
  binding request sent as sequence910000, but its reply and initial status
  still unread. The provider may consume only the greeting through the API
  handle. No other process may read the connection after handoff.
- A read-only lifetime pipe whose writers disappear when the DSP session ends.
- That connection's API handle and the GUI client's handle, freshly obtained
  from radio status. These are trusted handoff values, not HTTP parameters.

Invocation is `pd-unkey-guard <api-fd> <lifetime-fd> <0xAPIhandle> <0xGUIhandle>`.
The process requires a non-root UID, no effective/permitted/inheritable
capabilities, and a pipe for its event output. It opens no sockets or files.
It ignores SIGPIPE so loss of the Go diagnostics reader cannot interrupt
unkey or acknowledgment handling. It disables dumps and requests `no_new_privs`; Linux2.6's `EINVAL` is accepted
because the helper never executes another program or gains capabilities.
Actual Linux2.6 hardware execution remains untested.

Startup requires the binding acknowledgment, an acknowledged transmit
subscription and the complete initial idle interlock tuple. It continuously
tracks updates. At lifetime hangup, it drains queued updates and requires a new
subscription acknowledgment barrier before using that tracked state. SmartSDR
4.3.16 sends the initial tuple at connection time and does not replay it on
repeated subscriptions; consuming it in the provider breaks this contract. It sends exactly one `xmit 0` only for
software PTT owned by the prepared GUI client. Other GUI clients, hardware PTT,
malformed/incomplete status, rejected queries, and lost API connections do not
authorize a command. An existing unkey request needs no second command.

The only encoded commands are two fixed `sub tx all` subscriptions and one
fixed `xmit 0`. FlexLib's `Radio.Mox` setter is the command authority. Radio
status establishes interlock state and ownership. The trusted provider is
responsible for sending the binding request for the same GUI identity;
the helper validates its acknowledgment but does not discover or bind clients
itself.

`unkey_acknowledged` proves command acceptance, not RF cessation. The independent
lease guardian must continue to retain the waveform API lease until the full
READY tuple and mode restoration are observed. A session provider must monitor
helper failure; this helper alone is not the full fail-safe session system.
No automatic restart, rekey, calibration source, or configuration command is
implemented here.

## Bounded measurement mode (development)

An optional fifth argument, `cleanup-after-ms`, selects one-shot bounded mode.
It is a canonical decimal integer from 1 through 15000; zero, leading zeros,
signs, whitespace and larger values are refused. The caller supplies the same
prepared descriptors and GUI binding as the lifetime-only mode. No extra
socket, file, privilege or command channel is introduced.

After initial idle admission it emits `bounded_ready` with `cleanup_due_ms`
as a decimal string in the radio's CLOCK_MONOTONIC domain. The deadline starts
before publishing readiness, is fixed once, and cannot be extended. At that
deadline, or earlier lifetime-pipe hangup, it starts the same ownership-refresh
and unkey path. An open lifetime writer, queued pipe bytes, repeated TX status
and loss of the diagnostics reader do not renew it.

This timestamp is **cleanup initiation, not an RF-cessation deadline**. The
refresh write, acknowledgment wait and unkey write have nominal budgets of
500 ms, 2 seconds and 500 ms, plus scheduling delays. Missing/ambiguous ownership
or API failure can prevent an unkey command altogether. The production adapter
must account for those bounds, monitor helper loss, fence queued positive keying,
and retain the existing qualified stop-evidence path. A successful unkey reply
is still only command acceptance.

Bounded mode tracks the first observed software-TX interval. Once it sees
unkeying, loss of ownership, an incomplete ownership observation, or the end of
that interval, it emits `bounded_observation_ended` and retires without adopting
a later cycle. This is sticky across multiple statuses in one TCP read and
across the final subscription barrier. That event and exit zero do not prove
RF release. The supervisor must withdraw the run permit whenever this helper
ends, and independent TX admission must prevent another producer sharing the
measurement operation. Both modes wait for complete, drained status input at
the acknowledgment barrier, including any partial trailing ownership update.

This bounded mode has development public/private session operations, but is not
deployed on the radio. It is launched as a per-test helper and does not replace the
existing session-lifetime guardian. Reusing or restarting it cannot grant a new
transmit intent. The prepared socket and process must retire after that test.
Host tests cover its timer, lifetime and ownership rules; ARMv7/ARM64 compilation
does not qualify Linux 2.6, scheduling, network timing or RF behavior.

The Go shim has a separate per-test launcher with an owned lifetime pipe and
context cancellation. It accepts only the exact bounded readiness schema and
a canonical, still-future CLOCK_MONOTONIC deadline no farther away than the
requested duration. Cancellation before readiness aborts startup; cancellation
after readiness closes the lifetime writer so the helper can perform cleanup.
Helper termination or an expired deadline permanently retires its observation.
The launcher does not create the prepared API connection or authorize keying.
The caller must close its parent API descriptor, retain the original operation
and enforce exclusive admission. If startup returns a nonnil helper with an
error, termination is still pending: retain and reap that helper before any
replacement test. No automatic retry or new transmit authority is implied.

Local Go tests exercise parser refusals, cancellation before/after readiness,
explicit close, helper exit and deadline retirement using fixture processes.
The private-session process now owns an exclusive test-helper slot. Stop closes
that slot permanently and cancels pending startup; its shutdown wait includes
test-helper termination. A failed startup retains the slot until its child is
reaped. A stale run's stop cannot affect a replacement. The production start
method uses only the captured GUI identity, fixed loopback TCP4992 and sibling
`pd-unkey-guard` executable. It verifies the returned GUI handle before leaving
a second bind ACK unread for the native helper, matching the existing native
session provider. Parsing stops at the exact line boundary, preserving subsequent
status bytes. Binding has a three-second deadline and cancellation closes TCP.

The test run's context represents its original lifetime; a short-lived HTTP
request context must not replace that lifetime after startup. Authenticated
begin/status/stop operations now carry random per-test identities through the
versioned private channel; see [the API contract](../IQ-MEASUREMENT.md#bounded-test-cleanup-api-development).
The desktop TX lease still needs to compose this supervisor with original
operator intent and qualified stop evidence. A helper's availability
check is not a remote keying permit or an RF release certificate. The supervisor
is tested with injected bind input and local launcher fixtures; the new connection
factory and complete production path have not run on a radio.

Host checks:

```sh
cc -O2 -std=c11 -Wall -Wextra -Werror unkey_guard.c -o /tmp/pd-unkey-guard
cc -O2 -std=c11 -Wall -Wextra -Werror unkey_guard_test.c -o /tmp/unkey-guard-test
/tmp/unkey-guard-test
python3 unkey_guard_protocol_test.py /tmp/pd-unkey-guard
```

Run from this directory. The protocol test reserves only local TCP4992 for a
fake radio and never contacts real hardware. It clears capabilities in its own
child to reproduce the launcher contract. Assertions remain enabled in the C
test even with a compiler configuration that defines `NDEBUG`.

Cross-build examples use Zig `-target aarch64-linux-musl` or
`-target arm-linux-musleabihf -mcpu=cortex_a8`, plus `-static`. Compilation is
not qualification of radio firmware, GUI binding, Linux2.6, or RF behavior.
