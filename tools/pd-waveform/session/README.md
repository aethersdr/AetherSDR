# Fixed session helpers

Development components for the [session-provider proposal](../SESSION-PROVIDER.md).
Building them creates ordinary files only: no SUID bit, capability, service,
boot change or privileged installation is applied.

`pd-fixed-arp` is the fixed ARP keeper used by the FLEX-8600 bench. A trusted
launcher starts it with one inherited read-only lifetime pipe and an event
output pipe. It accepts no network address, interface, command, path or PID.
It refuses a different interface/address inventory or a conflicting neighbor.
An existing matching permanent neighbor without a complete ownership record
is borrowed and left untouched.

Before adding an entry it drops root, all bounding capabilities and every
effective/permitted capability except NET_ADMIN, retained only when cleanup
will be needed. It becomes UID65534, removes supplementary groups and disables
dumps. Pipe-writer hangup or SIGTERM triggers cleanup of its own unchanged
entry, then retirement of the remaining capability. Lifetime payload is never
read, so a full pipe neither spins the helper nor prevents hangup cleanup.

The abstract Unix socket is a cooperating-session lock, not a request server.
Check/delete is not atomic against an unrelated administrator changing ARP.
Changed interfaces or neighbor identities fail cleanup rather than deleting
someone else's state. Before mutation, the helper forks a cleanup watcher
that inherits the fixed inventory, ownership decision, lock and NET_ADMIN.
The watcher waits for EOF on a private pipe whose sole writer belongs to the
helper. Helper death closes that pipe, allowing cleanup even after SIGKILL.
The helper monitors its own watcher and retires the session if it dies first.
Normal teardown waits up to two seconds for the watcher; a stalled watcher
keeps the lock and reports failure rather than being killed.

The watcher ignores SIGTERM/SIGINT and accepts no requests. It exits after
helper death or normal teardown. Both processes have UID65534, empty groups
and bounding/inheritable capabilities; Go and DSP remain capability-free.
Simultaneous loss of both cleanup owners cannot run immediate userspace cleanup.
A fixed RAM-only record at `/run/.aetherpd-fixed-arp.8600` lets the next trusted
start reclaim an owned orphan. The root-owned, mode0600, single-link record
binds the boot ID, interface index and MAC to the compiled address/interface.
The helper requires `/run` to be tmpfs, opens only this fixed file before
dropping root, and publishes the complete64-byte intent before adding ARP.
An inherited descriptor allows only that record to be cleared after verified
cleanup. Failed cleanup retains the record; foreign state is never deleted.
Partial records are repairable only when the ARP entry is absent. Successful
cleanup leaves an empty inert file; reboot removes both record and ARP state.
This does not distinguish an unrelated administrator rewriting an identical
entry, or provide cleanup while all owners remain dead.

Local tests cover complete and partial records, metadata and identity refusal,
borrowed-entry preservation, and fresh-process recovery after both owners die.
A standalone ARM64 helper on the8600 recovered after both owners received
SIGKILL, then removed the entry on lifetime closure in143ms including restart.
The probe removed its empty record and temporary files; it did not update the
installed package. Integrated-package recovery, rollback reconciliation and
actual FLEX6000 init/orphan behavior remain qualification gates.
The host-only `arp-watch-test` uses a bench subreaper to
collect a deliberately orphaned watcher; the production helper does not use
subreapers or pidfds.

```sh
python3 tools/pd-waveform/session/build.py --output build/pd-session --arch host --test
build/pd-session/fixed-arp-test-host
build/pd-session/arp-journal-test-host
build/pd-session/arp-watch-test-host
python3 tools/pd-waveform/session/build.py --output build/pd-session --arch arm64
python3 tools/pd-waveform/session/build.py --output build/pd-session --arch armv7
```

The host test is socket-free and changes no network state. It checks fixed
request encoding, neighbor ownership, foreign/dynamic/proxy refusal, strict
descriptor parsing and lifetime event handling. The compiled profile is
8600-only (`eth0.10`, `172.30.2.1`); ARMv7 compilation does not qualify it on
a 6000. The native constructor and independently owned API lease are
implemented below; supported provisioning and client lifecycle integration
are still required.

## Native resource constructor

`pd-radio-launch` takes no command-line arguments. Its trusted launcher
supplies only the GUI UUID and handle; it creates the ARP keeper, ordinary
IQ source, capture supervisor and their private lifetime channels. It then
uses the API and prepared-DSP constructors and replaces itself with the
unprivileged Go shim. Go inherits only internally generated child identities.
Caller-provided child PIDs, paths, addresses and scheduling settings do not
select the privileged operations.

The fixed sibling executable tree must be root-owned and unreplaceable by
the waveform user. Native construction does not supply that initial
installation authority. The current addresses, capture profile and management
port are bench-specific, not a shipping discovery/configuration contract.
See the [provider proposal](../SESSION-PROVIDER.md) for the authority split
and remaining installation requirements.

Finite 8600 startup-failure probes replace one temporary protected ELF with
an unusable file after provenance checks remain applicable. Shim exec failure
returns exit6 and retires the native graph; DSP or capture exec failures
return exit7 after the bounded readiness timeout. The probes verify the
recorded graph and ARP state before bench cleanup, and verify that receive
state stayed unchanged without keying. This covers those three failure
points, not every partial initialization or simultaneous failure. The bench
has a subreaper; actual 6000 init behavior remains a separate qualification.

## Fixed service-manager entry

`pd-service-launch` accepts no command-line arguments and requires real and
effective UID0 from its trusted service manager. It validates the GUI UUID
and handle, opens only the root-protected sibling `pd-radio-launch`, and
supplies null input and private output/error pipes to that constructor. The
child receives only a fixed PATH and validated GUI identity. Both processes
close unrelated descriptors. The log collector drops to UID78666/GID20 with
zero capabilities before reading child output; the constructor follows its
existing privilege-retirement sequence.

The collector forwards SIGTERM/SIGINT only to its own unreaped child. Log
sinks are nonblocking: slow journal consumption may drop diagnostic bytes,
but cannot stop draining the waveform pipes. It propagates the child's exit
status and bounds post-exit pipe draining. The trusted service manager must
own the whole process group and enforce startup/stop deadlines, including
cleanup if the collector is killed or a constructor fails before dropping UID.
This component is not a root request server, installer, SUID entry or reboot
provisioning mechanism.

`service-launch-test` exercises simultaneous output/error draining against a
full log sink, exit propagation and owned-child signal/reaping behavior. The
component test needs no radio, root, or network listener. The 8600 service
lifetime probes use temporary systemd units with independent deadlines;
actual 6000 service-manager integration remains unqualified.

## Owned API lease guardian

`pd-lease-guard` runs without root or any effective, permitted or inheritable
capability. Its trusted arguments are a prepared loopback API lease, read-only
lifetime pipe, original slice ID, USB/LSB mode, GUI handle and pan ID. It
does not read the shared API connection while the DSP owns it. Writer hangup
triggers fresh ordinary API observations. A full lifetime pipe does not delay
hangup detection or authorize an action.

After the complete idle/inventory snapshot, restoring PD to the captured mode
requires the original slice's GUI owner and pan to match. Removal clears that
identity; a foreign or incompletely identified replacement cannot authorize a
write or safe release. Another PD slice, malformed status, rejected replies or
ambiguous ownership retains the API lease. Command writes have a two-second
deadline; uncertainty never authorizes keying or receiver allocation. Lost API
leases are reported separately and handled by the shim's explicit journaled
receive recovery. Provider supervision remains necessary. The shim can own this guardian with
`SHIM_PD_OWN_LEASE_GUARD=1` on a complete trusted descriptor handoff. It starts
the helper before DSP registration and reaps it on normal shutdown; no second
external lease guardian may share that API connection.

`lease-guard-test-host` checks the actual parser/admission functions with
socket-free inputs, including foreign/reused IDs, missing identities, changed
pans, duplicate fields, partial writes and permanently blocked output.

## Read-only kernel probe

The report includes `contended_recursive_pi_mutex`: a child creates the same
recursive priority-inheritance mutex type as the DSP, holds both recursive
acquisitions while a second thread times out waiting, then releases it and
checks that the waiter can acquire, recurse and release. Unsupported protocol,
unexpected uncontended success, lock errors and stuck threads fail the probe.
The child has a two-second parent watchdog. This tests the contended kernel
path without scheduling privileges or RF; it does not measure real-time
priority donation under CPU load. An actual FLEX-6300 result remains required.

`pd-kernel-probe` reports the executing user's capability masks, inherited
seccomp mode, strict-seccomp entry in a child, monotonic clock and timerfd tick.
It tests `no_new_privs` separately in another child. Parent restrictions are
unchanged. An inherited seccomp filter can prevent transition to strict mode;
that result is not proof the underlying kernel lacks strict seccomp. The
capture worker's requirement is actual strict-mode entry in its launch context.

The probe opens no raw socket, configures no interface or ARP entry, and sends
no radio commands. It does not qualify packet-ring setup or grant privileges.
The optional `--serve` mode serves one immutable startup JSON report and closes
each connection on TCP48998. It reads no requests and exposes no control verbs.
That report is unauthenticated and contains only kernel/self-process feature
information, so run the diagnostic briefly and uninstall it afterwards.

Build a tiny native waveform containing both ARM architectures:

```sh
python3 tools/pd-waveform/session/kernel_probe_package.py --output build/pd-kernel-probe
```

The builder prints its SHA256. On the teammate's LAN computer, from this
source checkout with Python3, substitute that reviewed digest and radio IP:

```sh
python3 tools/pd-waveform/session/kernel_probe_tool.py --host RADIO install build/pd-kernel-probe/AetherPDKernelProbe.ssdr_waveform --sha256 DIGEST
python3 tools/pd-waveform/session/kernel_probe_tool.py --host RADIO status > kernel-features.json
python3 tools/pd-waveform/session/kernel_probe_tool.py --host RADIO uninstall
```

Installation uses only the fixed ordinary waveform upload API, with no setup
commands. Removal targets only this diagnostic's name. No Go runtime, SSH,
root bridge, reboot, RF or privileged installation is required. Check that
TCP48998 closes after uninstall. A missing empty legacy inventory status is
not evidence that the firmware deleted every installed file.

Return `kernel-features.json` with model, firmware and the package digest.
In particular, preserve failures and capability masks rather than translating
an unsupported syscall or filtered launch context into a successful test.
The FLEX-8600 ordinary-launch proof reports UID78666/GID20, zero capability
masks, strict seccomp and timerfd working. Those facts do not qualify a 6300.

Socket-free Python boundary checks:

```sh
python3 -m unittest discover -s tools/pd-waveform/session -p 'test_*.py' -v
```

## Fixed local IQ capture component

`pd-capture-bootstrap` and its linked `pd_ring_worker.c` are the measured
FLEX-8600 capture component. They accept already-open UDP reservation,
private unnamed datagram and lifetime descriptors, plus the trusted launcher's
observed stream ID, session generation, supported sample rate and optional
capture duration (zero selects lifetime control; otherwise at most 120 seconds). They accept no interface, address, BPF program,
command, executable or external PID. A supported parent must establish that
stream identity through the ordinary radio API; this is not a public request
interface or a complete session provider.

The constructor validates the fixed `eth0.10` / `172.30.2.1` profile and the
reservation's address, exclusive port and descriptor types. It drops root,
supplementary groups and bounding capabilities, retaining NET_RAW only to
open one exact-flow filtered packet socket. It then retires that capability.
The supervisor and workers run as UID65534 with zero capability masks.
Before parsing IQ, the worker closes the packet descriptor and enters strict
seccomp. Its clock child closes the packet descriptor before the receive-ring
mapping exists. Go and DSP receive only the private datagram endpoint.

Lifetime hangup stops and reaps the worker; event output is nonblocking.
This component neither changes ARP, creates the API stream, schedules DSP,
launches Go, installs privileges nor keys the radio. Those responsibilities
still require the supported session provider. ARMv7 compilation does not
qualify the fixed 8600 network profile or strict seccomp on a 6000.

`capture-test-host` is socket-free. It executes the actual generated classic
BPF policy against all four rates and rejects foreign addresses, ports, stream
IDs, classes, non-UDP, fragments, short frames and malformed numeric arguments.
The repository builds reproduce both existing bench ARM64 and ARMv7 helper
binaries byte-for-byte. This import changes no capture behavior.

## Fixed shim handoff

`pd-shim-launch` is a development handoff component for the FLEX-8600 bench.
A trusted root parent prepares six descriptors: private unnamed IQ datagram,
loopback API lease, guard writer, setup writer, independent loopback unkey API,
and guard reader. The launcher validates their types and distinct roles,
including the matching guard pipe ends. It accepts no command, executable
path, PID, interface, capture filter or scheduling parameters.

The executable and its fixed siblings must be root-owned ELF files under a
root-owned directory tree that ordinary users cannot modify. Symlinks,
multiple hard links, group/other write access and SUID/SGID programs are
rejected. Private `state` and `run` directories belong to UID78666/GID20 with
mode0700. Standard input must be `/dev/null`; output and error are write-only
pipes. Unrelated descriptors are closed before execution.

The launcher drops supplementary groups, all capability bounding bits, root
UID/GID and all effective/permitted/inheritable capabilities. The shared drop
also enables `no_new_privs` for the primary shim, private session shim and
ordinary session relays. Only `EINVAL` (unsupported on Linux 2.6) is tolerated;
other failures refuse launch. It executes only
its fixed sibling `flex-tailnet-shim`, using a whitelist of inherited session
variables and fixed arguments. The managed DSP selects **local DAX IQ**.
Caller environment cannot select DAX audio or inject loader/configuration
settings. The two validated GUI/unkey client handles remain session-specific.

The current API address is the fixed **bench LAN address
192.168.50.100:48997**, independent of the capture component's internal
172.30.2.1 destination. This is a development profile, not a portable network
configuration or a shipping provisioning listener. The normal shim API remains
separate. Authentication and bounded lifecycle verbs still apply.

This component does not create streams, configure ARP, open packet capture,
schedule DSP or provide initial root authority. Those operations remain in the
trusted bench fixture. Building it installs no privileges and does not supply
the complete session provider. ARMv7 compilation does not qualify execution
on a FLEX-6000. `shim-launch-test-host` checks descriptor/handle grammar,
protected ownership/modes and data-directory policy without sockets or root.
It injects unsupported/error responses and checks `no_new_privs` across exec
in a separate child on the modern Linux test host. This does not qualify the
Linux 2.6 fallback on actual FLEX-6000 hardware.

Receive recovery preserves the captured slice's DAX assignment only while that
channel remains available. A second GUI assigning the same channel makes
restoration refuse rather than change the foreign receiver. The receive
helper's bounded refusal is distinct from DSP/unkey/API-lease child retirement;
qualification records both outcomes. This check concerns restoring preexisting
receive settings, not the DAX IQ feedback transport.

## Ordinary DAX IQ stream owner

`pd-iq-stream` moves the fixed stream transaction out of the root bench script.
It requires UID78666 and zero effective, permitted and inheritable capabilities.
Its three arguments are inherited descriptors: a fresh internal radio API
connection, an exclusively bound UDP reservation, and a read-only lifetime
pipe. The fixed8600 profile requires172.30.2.1 for the API peer and reservation.
It accepts no address, port, interface, PID, executable, shell command, IQ rate
or channel argument. The reservation supplies its already-bound port.

The trusted provider supplies only the sealed GUI UUID and handle through
`SHIM_PD_GUI_CLIENT_ID` and `SHIM_PD_GUI_HANDLE`. The provider must first verify
the intended GUI, receiver, panadapter and channel assignment. The helper
binds to that GUI, requests exactly one channel1 DAX IQ stream and sets48000
samples/s. Readiness requires successful replies to issued commands and
matching status for the created stream ID, client, GUI, channel, rate and IP.
Unrelated radio status does not confer ownership. The S-prefix handle is not
used as a resource-owner claim, consistent with FlexLib's status routing.

On firmware4.3.16, bind replies contain the bound GUI handle and the rate
setter returns `DAXIQ rate set succesfully`. These measured forms have parser
coverage. Unknown reply forms fail rather than weakening identity validation.
A second subscription obtains complete stream readback after setting the rate.

Lifetime closure or SIGTERM ends the session. Normal cleanup requests removal
of the owned stream and waits for its reply. A failed parser, uncertain setup
or an observed removal instead closes only the helper's API lease; it does not
send an ID-based removal after ownership becomes unclear. Pipe payload is
never consumed, and removal waits exclude a closed lifetime pipe to avoid
spinning. Events report readiness/completion and bounded parser-state counters;
output failure cannot block indefinitely.

The helper changes no panadapter or receiver, creates no raw socket, changes no
ARP or scheduling state, launches no process, installs no privilege and has no
RF-keying command. Standard input is `/dev/null`; output/error are write-only
pipes, and unrelated descriptors are closed. The full supported provider and
its crash-order supervision remain necessary. ARMv7 compilation or execution
on an8600 does not qualify an actual6000 network profile or kernel.

`iq-stream-test-host` feeds the actual parser without sockets or a simulated
radio peer. It covers both measured reply forms, command-ACK provenance,
stream/GUI/client/channel/rate/IP substitution, duplicate fields, foreign
status broadcasts, every split of a complete stream frame, hostile framing,
strict numeric inputs and standard-stream policy.

## Prepared DSP scheduling handoff

`pd-prepared-launch` is the next development handoff. It reuses the six
inherited descriptor roles and protected executable/data-directory policy of
`pd-shim-launch`. It creates exactly one fixed DSP child, waits for its private
startup readiness, and schedules only that child main thread at RR1. Other
DSP threads must remain ordinary at nice10. The child has a50ms soft/100ms
hard realtime CPU limit. Scheduler operations use raw kernel syscalls because
the tested Zig/musl wrappers return ENOSYS.

Both DSP and the parent drop supplementary groups, root and all capability
masks, including the bounding set. The parent then executes the fixed Go shim
without changing PID. Internally generated child identity and four private
owner/status/calibration/start descriptors are added to the sealed environment;
none is accepted as a privileged caller's PID or scheduling request. Go proves
that the DSP is its direct child, captures receive state, arms the unkey,
API-lease and receive guardians, and releases registration. Startup failure
closes the lifetime and reaps the child. The prepared runtime discards bounded
pre-registration IQ backlog only before any samples have been processed.

Receive-only ARM64 and ARMv7 launcher/Go/DSP tests on the8600 verified the
profile, zero capabilities, calibration arm without RF and normal retirement.
The latter test still used the ARM64 IQ owner. This is not6300 qualification
or RF qualification of this handoff. The full network/resource constructor
and supported initial privilege provisioning remain incomplete; this builder
installs no service, SUID bit or file capability. See
[session provider](../SESSION-PROVIDER.md#prepared-dsp-scheduling-handoff).

Both fixed managed constructors select validated recent-model transport
recovery. The prepared constructor sets matching fixed metadata for Go and
DSP; Go rejects a prepared session lacking that profile or owned guardians.
Callers cannot request a different recovery mode through the constructor.
After a qualifying TX deadline gap, output bypasses the quarantined model
until fresh IQ, alignment and an equivalent linear-path fit pass validation.
The model must be at most30seconds old and configuration must remain unchanged.
Stale IQ, send errors and other invalidation events purge it. This still assumes
that the PA's unexcited nonlinear response has not changed. It never keys RF or
rearms the diagnostic source. The operator selected this behavior for shipping;
finite earlier recovery evidence does not qualify this new combined build.

The prepared DSP inherits the constructor's validated write-only stderr pipe.
Standard input and output remain `/dev/null`. This preserves alignment/EQ/fit
failure diagnostics through the existing process supervisor without opening a
log path or adding a request parameter. The deployment supervisor must drain
its log pipe continuously; finite bench fixtures bound their total log volume.

## Native API resource preparation

`pd-session-launch` extends the prepared handoff with a fixed API constructor.
Its only descriptor arguments are the private IQ datagram endpoint and the
setup-lifetime writer. Trusted metadata supplies the selected GUI UUID and
handle; neither is an executable, address, PID or scheduling parameter.

A forked child closes unrelated descriptors, becomes UID78666/GID20 with empty
supplementary groups and zero capabilities/bounding set, then opens exactly
two TCP connections to `127.0.0.1:4992`. It binds both the DSP lease and the
independent unkey connection to the validated UUID and requires each bind
acknowledgment to identify the selected GUI handle. The two sockets must have
distinct non-GUI client handles. Firmware 4.3.16 reports a different interlock
context on an unbound socket; that context cannot admit the owned source.
It sends the same
bind once more with the guardian's fixed sequence910000, leaving that reply
unread for the existing guardian's independent admission check. It creates a fresh
pipe for the independent guardian. This child sends those four descriptors
and the observed unkey-client handle through its private Unix socketpair;
no radio protocol is parsed while privileged.

Protected sibling executables and the private state/run directories are
validated before either API connection is opened. This keeps rejected layouts
from leaving a pending guardian bind reply. Unexpected reply sequences are
rejected; they are never accepted as evidence for the current transaction.
The bind reader stops at its complete reply line, preserving any following
status bytes for the DSP or guardian. Socket-free injected-transport checks
cover both binding commands, rejected or mismatched replies, duplicate client
handles, and withholding the guardian's pending bind until both checks pass.

The parent accepts one exact-size, versioned, four-descriptor message from its
own child, rejects truncated ancillary data and mismatched identity, waits for
successful child exit, then applies the existing kernel descriptor-role
checks. Preparation has an eight-second parent deadline and kills/reaps only
its own unreaped child on failure. A lost-child (`ECHILD`) result never causes
a signal to the former PID. The prepared DSP/Go handoff then runs unchanged,
including its fixed scheduling profile, startup barrier and ordinary guardians.

The `SHIM_PD_UNKEY_CLIENT_HANDLE` supplied by a caller is discarded and replaced
with the observed handle. `SHIM_PD_GUI_CLIENT_ID` is consumed for API binding
and is not forwarded into Go/DSP. All other caller environment is discarded
by the prepared handoff. There is no positive RF command in this constructor.

This remains a development component: external IQ/stream/capture/network
preparation and supported initial privilege provisioning are still required.
It is built as an ordinary executable; no SUID, file capabilities, service or
boot change is installed by `build.py`.

Guardian failure diagnostics are retained up to1024 bytes in Go and all excess
stderr is consumed. The child has no dependency on a log reader keeping pace;
its failure remains observable after `Wait` completes. This applies to the
managed API-lease and receive guardians without changing their admission or
cleanup policy.

## Ordinary IQ socket and stream constructor

`pd-iq-source` creates the fixed8600 DAX IQ source under UID78666/GID20 with
empty supplementary groups and zero effective/permitted/inheritable
capabilities. Root execution, another group, substituted channel roles and
an already-ended lifetime are rejected before socket allocation. The trusted
provider supplies only a private unnamed datagram endpoint, a read-only
lifetime pipe and the sealed GUI UUID/handle. It cannot supply an API socket,
UDP socket, destination, port, frequency, command or sample rate.

The helper reserves an exclusive UDP port on `172.30.2.1`, opens its own TCP
connection to `172.30.2.1:4992`, and sends one UDP descriptor through the private
channel. The fixed16-byte `PDUDP001` message includes the kernel-assigned port
and a zero reserved field. The provider must validate the exact message and
ancillary count, compare the descriptor's kernel endpoint/ownership/exclusive
binding with that port, and retain the reservation for capture. The API
connection is never passed to the provider, Go or DSP.

After this handoff, the helper runs the existing ordinary DAX IQ stream owner
in the same process: fresh GUI-bind acknowledgment, channel1 creation,
48000Hz rate/readback, status ownership, and lifetime-controlled cleanup.
Its stdout readiness/completion records retain the stream owner's format.
An uncertain stream identity retires only its own API lease, preserving the
existing refusal to remove an unproven stream ID. A failed handoff closes the
new sockets before any stream-create command. A hung connect is bounded and
observes lifetime cancellation.

This helper does not provision ARP, create a packet socket, start DSP, key RF
or install privileges. The full provider must still own and supervise the
fixed ARP/capture helpers and the native session handoff. The development
fixture orchestrates those components; ARMv7 execution on the8600 does not
qualify the6300 network profile or kernel.


## Socket-activated Start entry

`pd-start-session` takes no arguments. Its stdin must be a connected Unix
stream from UID78666/GID20, whose process executable matches the protected
sibling `flex-tailnet-shim` inode. It reads one of two fixed frames within a
single 2-second deadline:

- `PDSTART1`: 36 ASCII UUID bytes and a nonzero 32-bit GUI handle in network
  byte order (48 bytes total). This retains channel 1 with observed pan discovery.
- `PDSTART2`: the same identity followed by a 32-bit channel (1–4) and a nonzero
  32-bit pan ID, both in network byte order (56 bytes total). Unsupported,
  partial or malformed selection is rejected before the installation lease;
  it cannot fall back to the legacy frame.

The service manager supplies the fixed executable and owns the whole process
cgroup. There is no caller-selected helper, path, PID, network profile or
scheduler setting.

After admitting the request, it requires a matching committed installation and
takes the shared transaction lock before starting the constructor. Missing or
unfinished installation state refuses startup with exit6. The ordinary collector
retains only a read-only lease for the session lifetime, preventing a concurrent
installer transaction; the constructor and DSP never receive it. See the
[installation gates](../boot/README.md#installation-gates-for-boot-and-sessions).

The entry uses the same pipe collector as `pd-service-launch`; its constructor
preserves only the admitted private control connection for the capability-free
Go session role. Source/capture/DSP children do not inherit that connection.
The ordinary Go shim owns the opposite endpoint. Neither helper is SUID and
neither is installed by `build.py`.

On the8600 the waveform UID78666 may lack an NSS account. The temporary bench
fixture creates a root-owned mode0600 socket, then assigns its numeric owner;
using `SocketUser=78666` failed account lookup. With `Accept=yes`, the socket's
matching service template is selected implicitly. Boot installation must
supply equivalent fixed ownership without a general command runner. The
existing broad diagnostics bridge is only the temporary development fixture.

### Ordinary shim boot launcher (development)

`pd-primary-launch` is the no-argument native entry for the ordinary shim's
boot service. It opens only the protected sibling `flex-tailnet-shim`, checks
fixed private state/runtime directories, closes inherited descriptors, sets
ordinary scheduling at nice10, drops supplementary groups, switches to
UID78666/GID20 and clears all capability sets before exec. The exec environment
contains only PATH and the Tailscale no-log-upload setting. Caller-supplied
PD descriptors, state paths, API ports and other environment settings are
not forwarded. No GUI handle is stored or reused at boot.
Startup requires a committed installation, or the installer's narrowly gated
activation/readiness phase while its exclusive transaction lock remains held.
This exception starts only the control service; correction sessions still require
commitment and a new authenticated Start request.

The ARM64 profile uses `/var/lib/smartsdr/aetherpd/state`: on the tested 8600,
`/user` is a symlink to `/var/lib/smartsdr`. The ARMv7 profile uses the reported
6300 mount `/user/aetherpd/state`. Both use `/run/aetherpd/primary` for volatile
logs/cache. Every parent component must be a root-owned directory without
group/other write permission; symlinks are refused. State and runtime leaves
must be UID78666/GID20 mode0700. The installer must create them. Unknown layouts
refuse startup rather than falling back to a user-selected path.

This component is not the installer or boot unit. It grants no AetherPD setup
authority to the ordinary shim; explicit authenticated Start still uses the
separate fixed socket-activated constructor. Persistent installation, removal,
update/rollback and actual 6300 boot qualification remain separate gates.

## Installed-session failure lifetime

The fixed resource launcher closes setup descriptors and drops root before
waiting for owned children during startup-failure cleanup. Failure to drop
privileges refuses the wait and reports cleanup failure; an already ordinary
parent keeps its existing identity. The child-exit result still determines
whether cleanup is confirmed. This also covers an early prepared-session
refusal that returns before the usual privilege retirement.

The prepared-DSP readiness wait checks for child exit without reaping it, so a
failed IQ handshake ends setup promptly instead of retaining setup privileges
until the readiness deadline. Cleanup confirms the child is still waitable
before sending a signal; an exited child is reaped and a lost child is left
alone. The non-reaping observation uses `waitid(P_PID, ..., WNOWAIT)`, available
since Linux 2.6.9 according to the [Linux wait manual](https://man7.org/linux/man-pages/man2/waitpid.2.html).
Actual FLEX-6300 qualification remains required.

An installed receive-only 8600 comparison terminated the exact owned capture
supervisor before DSP startup. With prompt child-exit observation, original
worker retirement took 477 ms versus 10.27 seconds on the predecessor; the
launcher was observed without root or capabilities by 414 ms after the fault.
An ordinary explicit restart delivered a complete local DAX IQ frame with FDX
disabled, followed by verified Stop cleanup. Exact artifacts and limits are in
[`PREPARED-CHILD-EXIT-INSTALLED-QUALIFICATION.json`](../PREPARED-CHILD-EXIT-INSTALLED-QUALIFICATION.json).
This is one startup-failure comparison, not an RF or FLEX-6000 timing guarantee.

On the installed ARM64 candidate, loss of the IQ-source process retires DSP,
capture, ARP and guardians. Its unprivileged private control owner and launcher
can remain to serve explicit receive restoration. Status requests refresh the
private channel's15-second inactivity deadline; an unavailable status response
does not itself close that channel. Under continuous polling, the two processes
remained through a35-second observation. Explicit Stop followed by Start
successfully replaced that channel. When polling ceased after failure, a
separate receive-only8600 run observed full session retirement15.58seconds
after killing the IQ source; Start then succeeded without an intervening Stop.

These are distinct lifecycle outcomes. The earlier continuous-polling test's
full-automatic-cleanup assertion remains a failed assertion, not a pass inferred
from idle expiry. Connected desktop handling of the restoration window remains
to be qualified. Neither receive-only test proves RF unkey behavior or actual
FLEX6000 supervision. Session start and expiry never initiate transmission.

The installed ARM64 receive-only API qualification also rejects missing or
invalid credentials, duplicate starts against an occupied session, an extra
executable field and a nonexistent GUI identity. Disconnecting only the owned
unkey guardian's non-GUI API client retired the original nine DSP/IQ/resource
workers within 417 ms; the desktop remained connected. Public status retained
`cleanup_unconfirmed`. An explicit ordinary Start then delivered a complete
local DAX IQ frame with FDX disabled, followed by verified Stop cleanup.
See [`INSTALLED-API-OWNERSHIP-QUALIFICATION.json`](../INSTALLED-API-OWNERSHIP-QUALIFICATION.json)
for exact artifacts and ownership checks. This does not qualify successful
credential revocation, two real competing GUIs, other API roles or keyed RF
failure behavior.

The installed guardian-hold policy preserves the ordinary API lease guardian
while the radio reports `RECEIVE` with `tx_allowed=0` after GUI disconnect. In
the qualified receive-only case, all original workers retired after reconnect
without service-manager SIGKILL, and the stale panel refused Start throughout.
The native guardian's final exit reason/status was not captured; the passive
API trace is separate evidence from its internal observations. The subsequent
fresh full IQ capture succeeded, but receive recovery reported an unexpected
reply and remains unqualified. Actual FLEX-6000 manager behavior remains open.
See [`GUI-RECONNECT-HOLD-QUALIFICATION.json`](../GUI-RECONNECT-HOLD-QUALIFICATION.json)
and the preserved pre-fix
[`GUI-RECONNECT-FAILURE-QUALIFICATION.json`](../GUI-RECONNECT-FAILURE-QUALIFICATION.json).

Receive-setting command replies are routed by sequence as in FlexLib
v4.3.16.42034 `Radio.ParseReply` (`reference/FlexLib/FlexLib/Radio.cs:3127`). A
well-formed reply for another sequence neither confirms nor rejects the pending
command. Matching errors, malformed replies and missing matching replies still
fail; the existing frame, byte and transport deadlines remain in force. This
component correction does not establish the exact frame behind the installed
receive-recovery refusal or qualify its installed resolution.

## Feedback pan binding

The installed constructor's IQ owner subscribes to pan status without changing
any receiver. It requires the created stream's explicit `pan` field, plus that
panadapter's `client_handle` matching the bound GUI and `daxiq_channel` matching
the selected channel. Explicit selection also requires the stream's `pan` to
match the requested pan; the constructor does not reassign a pan or channel.
Partial readback cannot report ready. Up to eight pan identities are tracked;
removed unrelated pans free their slot. The `S` prefix is a status recipient,
not resource ownership. Stream and pan IDs have no numeric relationship.

The private ready record includes the observed pan ID and selected channel. The
constructor checks both against the request and passes them to the DSP as
`AETHER_PD_FEEDBACK_PAN` and `AETHER_PD_DAXIQ`; it does not accept a
caller-selected path, executable or network setting. The DSP waits for its own
matching owner/channel readback and ignores other pans, including a foreign
pan advertising the selected channel. Removing or reassigning the selected pan invalidates
this session and cancels an armed measurement. Restoring status does not rearm
or revive the binding: stop and explicitly start a new session.

The IQ owner, constructor and DSP must be upgraded together. The new private
ready grammar requires issued/reply mask191 and field mask127, and rejects the
older record without a pan or channel. This change adds no capabilities or RF commands.
Development prepared launches without the constructor retain their earlier
unsealed behavior; they are not equivalent to this installed path.

The native constructor and public v2 shim endpoint accept explicit channel/pan
selection and require matching observed native binding before success. Desktop
startup uses that selected request after the operator chooses an owned assigned
pan. Native snapshots retain confirmed binding through the shim, desktop capture
frames and JSON/CSV/PNG exports. Live
qualification of this exact candidate remains required before claiming the full
PR4770 workflow. The binding is grounded in the published FlexLib
`DAXIQStream.StatusUpdate` (`pan`, `daxiq_channel`) and
`Panadapter.StatusUpdate` (`client_handle`, `daxiq_channel`), corroborated by
saved FLEX-8600 stream/pan status from the local IQ tests.


The selected native path is qualified offline in
[`IQ-SELECTION-NATIVE-QUALIFICATION.json`](../IQ-SELECTION-NATIVE-QUALIFICATION.json).
It is not installed or live-qualified. Ordinary-user source ownership,
fixed local ARP/capture operations, credential checks and priority policy are
unchanged. The root bootstrap remains temporary; this protocol does not add
a general privileged command surface.
