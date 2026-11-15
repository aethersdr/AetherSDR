# Local session-provider proposal

Status: proposed, not approved or implemented as a shipping service. Track
review locally at the maintainer's request; do not open a GitHub issue.

## Problem

The managed Go shim, native DSP, fixed IQ constructor and unkey guardian work
with trusted inherited descriptors. The native constructor prepares those
resources; the development root bridge supplies its initial launch authority
and temporary protected binary layout. Shipping that bridge would expose much
more authority than AetherPD needs. Ordinary waveform execution on the 6300 does not establish
a supported installation path for the required privileged setup.

## Proposed boundary

The privileged component is a fixed compiled session constructor, not an HTTP
service, shell, command runner or general process launcher. It accepts only
inherited private descriptors from a trusted, supported installer/launcher.
It must not accept requester-selected commands, executables, PIDs, file paths,
interfaces, addresses, BPF programs, capabilities or scheduling parameters.
Only one owned session is admitted at a time. Radio identity, GUI ownership,
slice, pan, frequency, receiver input and stream identity are observed through
ordinary API status and sealed before handing off the session.

The constructor has exactly these responsibilities:

1. Validate a compiled, hardware-qualified network profile and descriptor
   provenance. Reject an unknown model/profile or unavailable kernel feature.
2. Provision one ordinary-API DAX IQ stream and its fixed local ARP mapping.
   Preserve exact prior network state; leave `accept_local` unchanged. Restrict
   capture to that stream's measured source, destination, ports and VITA class.
3. Start the linked capture worker, keeping its raw socket out of Go and DSP.
   Retire setup capabilities before parsing untrusted IQ. The existing strict
   worker contract must not silently weaken on Linux 2.6.
4. Prepare the dedicated API lease, independent unkey connection and lifetime
   pipes for one freshly identified GUI. Go/DSP run as the ordinary waveform
   user with zero effective, permitted and inheritable capabilities.
5. Apply a fixed bounded scheduling profile to the constructor's own DSP
   child only. The 8600 bench uses main-thread RR priority1 and inherited
   `RLIMIT_RTTIME` soft/hard50/100ms; qualify a separate 6000 budget. Never
   accept an external PID or change firmware thread priorities.
6. Supervise owner/helper/DSP death, unkey confirmation, API-lease retention,
   stream retirement and exact ARP cleanup. Lost acknowledgments and helper
   failure must be observable and must not duplicate allocations or rekey.

Temporary setup may require NET_ADMIN, NET_RAW, SETUID/SETGID, SETPCAP and
SYS_NICE. These belong to the small constructor/cleanup responsibilities,
not the Go/Tailscale process, packet parser or DSP. The implementation review
must map each capability to one fixed operation and demonstrate retirement.
The supported installation mechanism determines how those initial capabilities
are delivered; this proposal does not grant or install them.

No component gains a positive RF-keying verb. Calibration arm accepts no
caller-supplied frequency, power, samples or duration. Transmission begins
only through the client's existing operator TX gate. Bench-specific 17/19/20s
test cutoffs are qualification fixtures, not a proposed change to normal
operator timeout defaults.

## Development components

The fixed ARP keeper, strict capture component, API lease guardian and
[fixed shim handoff](session/README.md#fixed-shim-handoff) implement separate
parts of this boundary. `session/pd_radio_launch.c` composes them into the
development resource constructor. It accepts no command-line arguments and
only the validated GUI UUID/handle from its trusted launcher. It creates the
fixed ARP helper, ordinary-user UDP/stream owner, strict capture worker and
private lifetime channels, then uses the API/scheduling handoff below. Its own
fork results supply Go's direct-child supervision metadata. It retires root
and all capabilities before Go runs.

The constructor still uses the qualified 8600 network profile and a fixed
bench management address. The root bench fixture supplies its protected
layout and initial authority; GUI preparation and measurement belong to the
bench. This is not supported privilege delivery or an ordinary waveform
installation. The required feedback transport is local DAX IQ; the shelved DAX audio experiment
does not satisfy this proposal.

The constructor's current authority split is:

| Component | Retained authority after initialization | Accepted operation |
|---|---|---|
| `pd-radio-launch` | None after replacing itself with Go | One fixed sibling graph, compiled network/scheduling profile and validated GUI identity |
| `pd-fixed-arp` and its cleanup watcher | UID65534, NET_ADMIN only while cleanup is owned | Preserve or remove the one unchanged owned neighbor; private lifetime EOF, no request listener |
| `pd-iq-source` | UID78666/GID20, no capabilities | Exclusive local UDP reservation and fixed ordinary API IQ stream transaction |
| Capture supervisor | UID65534, no capabilities after raw-socket creation | Own and retire its strict worker; no requests or raw descriptor transfer to Go |
| Capture worker | No capabilities, strict seccomp | Filtered IQ delivery over the inherited private channel |
| Go, DSP and guardians | UID78666/GID20, no capabilities | Authenticated fixed PD operations, processing, independent unkey and checked receive restoration |

The DSP main thread alone retains the constructor-applied RR1 scheduling
policy and fixed realtime CPU limit; Go cannot request scheduling changes.
The ARP lock prevents another cooperating session from claiming the same
profile while cleanup remains owned. It is not authorization for a caller to
launch arbitrary binaries. The installed binary tree and initial privilege
delivery still need their own authenticated update and ownership guarantees.

## Prepared DSP scheduling handoff

The development scheduling boundary preserves the existing Go parent/child
relationship. The C constructor creates exactly one DSP child from its fixed
protected executable. After initialization, it schedules only the PID returned
by its own `fork()`, then retires root/capabilities and executes the fixed Go
shim in the same parent process. Go can wait for that already-existing direct
child; an ordinary-user CPU proof checks preserved parent PID, kernel direct-
child admission and both normal exit and dedicated API-lease-failure exit7.
The PID is generated inside the constructor, never accepted by a privileged
request interface. Go receives no scheduling capability.

The runtime's optional `AETHER_PD_START_FD` is a private unnamed datagram
barrier. After its workers initialize and the existing local IQ configuration
handshake completes, DSP sends `PDREADY1`. It cannot register a waveform with
the radio until Go releases it with `PDRUN001`. The IQ handshake precedes this
barrier because the capture component already has a three-second handshake
deadline; extending that deadline is not part of this change. After release,
the prepared path discards at most2048 queued startup datagrams before API
registration, provided no IQ has yet been processed. It does not clear runtime
faults or relax freshness and discontinuity guards after input begins.

The implemented sequence is:

1. The trusted constructor prepares the sealed local IQ, API leases and
   lifetime/status/calibration descriptors, then creates its fixed DSP child
   with zero capabilities and the fixed runtime budget.
2. DSP initializes workers under ordinary scheduling and completes the private
   IQ handshake. It reports readiness and waits at the pre-API barrier.
3. The constructor applies RR priority1 to that direct child's main thread
   only, verifies the fixed profile, retires all authority and executes Go.
4. Go validates the prepared descriptors and direct child, captures the
   original receive snapshot, and arms its independent unkey, API-lease and
   receive guardians before releasing the barrier.
5. DSP registers normally. Operator selection, calibration arm and RF keying
   retain their existing intent gates. Stop closes the owned lifetime; Go
   waits for and reaps the actual child and guardians as it does today.

Barrier release has a fixed30-second absolute deadline. Parent cancellation,
malformed/oversized messages, failed readiness output and clock failures do
not release it. Readiness or release cannot key RF, choose a command, request
a PID/priority, change network state, or rearm calibration. Missing barrier
configuration preserves the current launcher contract.

Status: `session/pd_prepared_launch.c` and the Go prepared-child attachment
implement this handoff. Receive-only ARM64 and ARMv7 launcher/Go/DSP runs on
the8600 verified the direct-child identity, main-thread RR1, ordinary helper
threads at nice10, a50/100ms realtime CPU limit, zero capability masks and
normal child/guardian retirement. The ARMv7 run used the separately qualified
ARM64 IQ stream owner; it is not an all-ARMv7 system qualification.

Scheduling uses the kernel syscalls directly: the tested Zig/musl scheduler
wrappers return ENOSYS. The privileged constructor accepts six inherited
session descriptors and fixed GUI handles, never an external PID, priority,
command or executable path. Go claims only its kernel-confirmed direct child
and arms the existing guardians before releasing DSP registration.

`session/pd_session_launch.c` now removes four externally prepared descriptors
from this handoff. Its capability-free child opens the DSP API lease and the
independent unkey connection, verifies a fresh GUI-bind acknowledgment, and
creates the guardian pipe. The parent accepts the exact private descriptor
message only from that child and applies the existing kernel role checks.
Only the IQ endpoint and setup-lifetime writer remain descriptor arguments
at this internal boundary. `pd_radio_launch` supplies both from its own setup;
this API component alone does not supply the full provider.

`session/pd_iq_source.c` also moves the UDP reservation and stream API
connection into the ordinary-user stream owner. It sends only the exclusive
UDP descriptor to its private parent and retains the API lease itself. The
same validated channel1/48000Hz create/readback/removal transaction runs after
the handoff. `pd_radio_launch` orchestrates this source with the fixed ARP and
capture helpers and creates their lifetime channels. Supported privilege
delivery is not supplied by these components.

The network/resource constructor has finite receive-only and RF evidence on
the 8600. Normal ARM64/ARMv7 starts, individual setup-helper failures and Go
owner death have separate local reports. The ARMv7 cases retain ARM64 API and
unkey guardians and do not qualify a complete 6000 runtime. The ARP helper's
pre-mutation cleanup watcher covers individual helper SIGKILL; simultaneous
loss of both cleanup owners and restart reconciliation remain open.

Supported initial privilege provisioning remains unfinished. Fixed boot and
session entries have been installed for bounded8600 tests and removed afterward;
no production privileged installation is qualified. An actual6300 still needs
separate kernel, scheduling and resource qualification; execution of ARMv7
binaries on the8600 does not establish it. RF qualification of an earlier
launcher does not automatically qualify this new handoff.

## Installation and updates

### Operator-approved bootstrap installation

The operator reports that the host-root bootstrap method used on the 8000 was
originally developed on the 6000. Treat that as the candidate installation
path, not as an absent capability requiring a new vendor mechanism. The
current archived 8600 implementation uses a native waveform's authenticated
host-directory descriptor handoff to the diagnostic container, followed by
access to the host Podman administrative socket. That exact implementation
has not been qualified on the target 6300 firmware. Its reported 6000 origin
is provenance, not a substitute for present hardware qualification.

The intended installer presents the selected radio identity, authenticated
bundle identity, exact files and permissions, persistence changes, temporary
setup operations, and rollback/uninstall actions before explicit operator
approval. Approval is bound to that immutable installation plan; changing the
bundle, target or plan invalidates it. The bootstrap performs only this fixed
transaction, verifies the protected executable tree, and closes/removes its
temporary administrative access. It must not expose a general job-upload or
command-execution interface as the shipped AetherPD control service.

Installation and session setup are distinct. Installation provisions the
protected fixed helper and its narrowly scoped launch mechanism. Each later
session establishes its own bounded ARP/capture/scheduling resources and drops
privileges before Go and DSP run. Dropping privileges in the installer alone
does not authorize the next session after reboot. The launch mechanism,
update path and removal path therefore belong in the reviewed installation
plan. No positive RF command or automatic calibration rearm belongs in any
of these operations.

The intended reboot behavior retains the installed payload, settings and only
the protected fixed launch mechanism. The broad installation bootstrap is
removed. A new session invokes the same fixed setup after reboot; it does not
resume RF, calibration excitation or a stale model automatically. Without a
retained privileged launch entry, setup must instead be bootstrapped again.
One8600 software reboot has qualified the fixed boot helper, local session
socket and ordinary primary startup in a controlled bench installation. The
primary started at24.53s uptime with its existing Tailscale identity and admin
credential; receive-only Start/status/Stop passed afterward. The temporary
predecessor-stop fixture is not a shipping migration transaction. Actual6000
launch, sudden-power-loss recovery and post-reboot RF remain unqualified. See
[boot qualification and limitations](boot/README.md#persistent-preparation).

The operator requires the smallest necessary authority and immediate removal
when each component's work is complete. The bootstrap must terminate and
remove its temporary executable, container, listener and session credentials
once the narrow runtime has taken ownership. No retained administrative SSH
or arbitrary-job bridge belongs to AetherPD. Go and DSP continue with zero
capabilities; only the fixed ARP cleanup owner retains NET_ADMIN until its
owned neighbor is retired, then exits. Code files without SUID bits or file
capabilities do not themselves confer launch authority.

A finite 8600 receive-only service probe has now started the exact existing
native constructor through `pd-service-launch`, deleted the temporary root
job/container, and obtained six successful registered/idle status reads before
an authenticated stop. The collector, Go and DSP had UID78666 and zero
capabilities; the two ARP cleanup owners had UID65534 and NET_ADMIN only.
Final checks found no test graph, ARP entry or runtime files, and the temporary
native bootstrap waveform was absent from the radio inventory. The existing
owner-managed diagnostic container was retained. This proves separation from
the temporary launch job, not complete shipping-installer removal, RF
correction after removal, reboot persistence, or 6000 compatibility.

Qualification must explicitly remove the bootstrap while AetherPD is running
and demonstrate continuing local IQ/correction, independent unkey and final
cleanup without the bridge. Existing privilege-drop tests alone do not prove
this handoff: bootstrap process/container teardown must not kill its runtime
children or revoke required descriptors. A later session or reboot still
needs a new authorized setup invocation. If the design retains a fixed launch
entry for this purpose, its precise authority and persistence must be shown
in the installation plan; it is not permission to retain the broad bootstrap.

Initial bootstrap authority is broad host root even when the executable
transaction accepts only fixed operations. Show that fact to the operator;
do not describe the entire bootstrap as capability-limited. The retained
runtime boundary is narrow. Authenticate the bootstrap and helper artifacts,
reject unsupported radio/firmware profiles before mutation, and verify
rollback plus cleanup when installation is interrupted.

The ordinary waveform process remains UID78666/GID20. Waveform upload and
automatic restart alone do not grant capture, ARP or scheduling authority.
The bootstrap supplies that missing installation authority. A vendor-provided
installation/launch service remains an alternative if the existing method
cannot be qualified; it is no longer assumed to be a prerequisite. Actual
6300 installation, reboot, update, rollback and runtime/RF qualification are
still required. No development privileged installation is retained on the bench
radio after these bounded tests.

A supported installer must provide immutable, root-owned binaries and
configuration in a directory tree the waveform user cannot replace. Its bundle
must be authenticated before any privileged execution. Do not make a
user-writable executable SUID, expose the Docker socket, or implement a generic
admin-token upload-and-execute API. Do not assume a ZIP upload preserves file
capabilities or confers root authority on a native 6000 waveform.

The native preparation entry now requires a protected approval record binding
the exact release digest, supported profile, firmware and radio serial. It
checks live loopback API identity before persistent writes. Approval for a
different radio with the same model and firmware is refused. This target check
does not provide the remaining predecessor migration or activation transaction.

The internal identity-transfer component copies only the admitted predecessor's
two fixed state files through a protected, journaled transaction. It preserves
the originals, refuses an occupied destination, and can resume without source
files once the prepared snapshot is durable. Ownership passes to the ordinary
user only after publication, with the parent directory transferred last.
Synthetic ARM64 and ARMv7 tests on the8600 cover12 interruption points and
metadata/corruption refusals. Predecessor admission, restart prevention and
activation are still separate work; this component alone is not the shipping
migration path. See [the component contract](boot/README.md#identity-transfer-component).

The reverse identity operation now prepares a complete root-owned rollback
tree from quiesced ordinary-user state, with a direction-specific journal.
Both directions pass host/sanitizer checks and native ownership/interruption
tests on the 8600. A live fixture using native preparation and one private
identity-directory bind served the same identity; the original container was
retained and restored afterward. Production container reconstruction and its
boot worker remain unwired. See
[rollback preparation](boot/README.md#rollback-identity-preparation-and-reconstruction).

Migration now saves an immutable recovery recipe before stopping the original
shim. It binds the approved migration to the observed radio address, preserving
that setting for rollback. A separate restored-container metadata checker
allows only the fixed private identity bind and recipe-derived configuration;
original-container admission still refuses mounts. Fixed native retirement
and reconstruction adapters now verify the retained image, enforce transaction
phases, verify image/container layers without mount operations, publish identity
before creation, and reconcile a matching container
after interrupted creation. Their filesystem/process-death tests use a Podman
resource model; native read-only verification of the real retained image passes.
The adapters still need to be connected to the independent recovery worker and
qualified through actual retirement, reconstruction and reboot.
See [recovery admission](boot/README.md#recovery-recipe-and-restored-container-metadata).

The8000 predecessor inspector now admits only the fixed legacy0.5.2 container
configuration and reports its ID, image and running executable hash. It is
read-only and accepts no path, command or container-name arguments. Its native
probe and parser refusals pass on the8600. A disposable, never-started container
also demonstrated reconstruction of the two identity files after removal while
preserving the image. This is not yet an approved retirement/activation
transaction; the real shim remains running. The6000 waveform adapter is still
required. See [predecessor admission](boot/README.md#legacy-predecessor-admission).

The native migration begin entry now binds the exact predecessor IDs and
executable digest to the same target/release approval. It prepares files,
stops only that admitted instance and copies its final identity state. Returned
copy failures reconcile the original shim, with an explicit unresolved-recovery
result if its running state cannot be verified. Host/sanitizer tests and a
bounded 8600 integration passed, including recovery from a deliberate failure
and an exact 2,645-byte identity copy. The test installation was removed and
the original identity returned. No retirement or activation occurred. Process
death/reboot recovery for the complete local controller remains required; see
[native migration begin](boot/README.md#native-migration-begin).

A separate fixed recovery entry now restores the approved stopped predecessor
without inherited filesystem descriptors. It requires the matching durable
migration journal, verifies the stopped executable through a bounded mount
path, removes only that container's residual mount references, and verifies
the running fingerprint after restart. A live 8600 test killed the installer
after stop and recovered the same identity from a fresh process in 18.05 seconds.
No service activation or RF occurred. This closes the lost-descriptor recovery
gap; the automatic transaction/boot controller and retirement rollback remain
unfinished. See [stopped predecessor recovery](boot/README.md#recovery-of-the-stopped-predecessor).

An 8600 disposable-fixture test now distinguishes container retirement from
image retirement. Firmware removal deleted both when a container existed.
Direct container removal with its image retained did not recreate a container
after one soft reboot. However, firmware accepted an image-only removal
request without deleting the image; the bounded test detected that and cleaned
the exact image up separately. The real shim's identity survived, all fixture
resources were removed, and no RF was used. These observations qualify neither
the complete migration transaction nor 6000 behavior; see
[firmware retirement observations](boot/README.md#firmware-retirement-observations).

Update and rollback need validated artifacts, two complete slots and atomic
activation. Keep expanded code, logs, captures and caches in RAM; persist only
compressed slots and small identity/configuration state. Reject an update that
cannot preserve rollback and the configured free-space reserve. The measured
6300 storage budget is not a budget for every 6000 model.

The installation transaction driver now records preparation, copy, retirement,
activation and commitment durably. Its process-death tests enforce rollback
before commitment and cleanup-only retries afterward, including preservation
of simulated identity updates. It currently uses test adapters; the real
retirement/reconstruction, service adapters and independent boot worker are
not wired. This is not an installable controller. See
[local transaction driver](boot/README.md#local-installation-transaction-driver).

Native boot, primary and session entries now enforce
[installation gates](boot/README.md#installation-gates-for-boot-and-sessions).
Sessions require commitment and retain a read-only shared transaction lease.
The control runtime may start during activation only while the installer owns
the exclusive lock, allowing readiness checks without opening correction sessions.
The gates passed host tests and native disposable-journal tests on the 8600;
the signed release and independent worker integration remain unfinished.

The worker's [temporary authenticated inputs](boot/README.md#temporary-installer-inputs)
can now be published atomically, reopened by a fresh process and removed only
after a matching terminal transaction. A protected copy of the trusted worker
and exact signed release removes dependence on transient upload descriptors.
Native component tests exercised that independence and interrupted cleanup on
the 8600. They did not install a recovery service or run the full migration worker.

## Cross-platform impact and alternatives

This component runs inside the Linux radio; desktop clients on Linux, macOS
and Windows use the same authenticated, bounded lifecycle/status interface.
Client integration must handle preparation, explicit arm, stop, failed
startup, missing receivers and manual recovery without optimistic success or
automatic rearm. The client/model and operator panel have component and
disconnected-UI evidence; their connected installed lifecycle remains unqualified.

The current broad development bridge remains a bench fixture. Giving the
whole shim capture/network capabilities or rooting the DSP fails the narrow
boundary. External-computer feedback does not meet the local-latency goal.
The operator-approved bootstrap must supply verified setup authority on each
qualified platform; an unavailable mechanism must be reported explicitly.

The FLEX-8600 bench also tested an ordinary-user alternative using a separate
receive-only RAW waveform on a second slice. Under UID78666 with zero
capabilities it delivered valid loopback IQ while receiving, but delivered
zero keyed IQ packets during a bounded 17-second 10 W corrected transmission.
The primary RAW waveform also delivered zero keyed receive packets. The
working DAX IQ path continued to drive correction during that test. This
finite observation does not establish a usable RAW feedback replacement or
qualify the two-slice arrangement on a FLEX-6000.

## Acceptance before shipment

- Review the boundary and supported provisioning mechanism before implementing
  or installing the privileged service. No SUID/capability/boot changes are
  authorized by building this proposal.
- Qualify malformed handoffs, descriptor substitution, replay, cancellation,
  root/capability rejection, all owner/helper/API death orders, lost ACKs,
  resource exhaustion, identity changes and exact cleanup. Preserve failed
  cases and mutation-check the critical guards.
- Demonstrate installation, update, rollback, reboot and identity persistence
  on actual target hardware. None may transmit, rearm or replay stale state.
- Exercise Linux 2.6 feature availability, single-core load, IQ age, scheduling,
  RAM and storage on a 6300. ARMv7 compilation and 8600 RF results do not pass
  these gates.
- Complete client lifecycle integration, independent source review, signed
  commits, required CI and the maintainer's release decision. The current
  finite bench results are evidence for a development candidate only.

## Setup-child supervision attachment

The Go owner accepts an optional all-or-none handoff of the three setup child
PIDs (`SHIM_PD_SOURCE_PID`, `SHIM_PD_CAPTURE_PID`, `SHIM_PD_ARP_PID`). These are
constructor-generated identities, not privileged requester-selected targets.
The privileged launchers currently discard caller-provided values for these
keys. `pd_radio_launch` creates the children and emits this handoff only from
its own fork results before executing Go.

Attachment requires the existing prepared DSP profile. PID syntax, duplicate
roles, self and DSP aliases are rejected. Kernel `wait4(WNOHANG)` admits only
live direct children, and waiters are started for admitted siblings even if a
later role fails admission. The owner never signals or schedules these PIDs
and never uses `wait(-1)`. This avoids requiring pidfds, ptrace access to the
UID65534 helpers, or Linux3.4 subreaper support just to collect direct children.

An unexpected helper exit, including exit0, closes the same owned DSP lifetime
used by guardian-failure shutdown. Monitoring is attached before DSP gate
release, and an already-failed setup refuses release. On DSP exit the owner
marks setup retirement expected, closes the existing setup lifetime and waits
for all three helpers. Nonzero helper cleanup remains an error. A bounded wait
that expires reports incomplete reaping; it does not claim cleanup succeeded.

Local process fixtures exercise each role, normal teardown, non-child admission,
partial-admission cleanup and pre-registration refusal. Integrated native
constructor tests additionally exercise cross-UID attachment, real helper
failures and normal retirement. All startup/death orders, restart reconciliation
and keyed helper-failure unkey confirmation still need qualification. No new RF keying,
privileged service or persistent installation is provided by this attachment.
