# Native installer input transfer (development)

This directory contains the unprivileged temporary native installer waveform,
its RAM input receiver, a Python desktop transfer client and private package
builder. The current development desktop integrates public first installation,
upgrade, saved-attempt recovery and QtKeychain credentials. See
[FIRST-INSTALL.md](FIRST-INSTALL.md) for exact qualified cases and limits;
production package selection and FLEX-6000 installation remain incomplete.

**Rejected private-archive format:** on FLEX-8600 firmware 4.3.16.42034, native
extraction assigns UID/GID 1001 to these files, while the intended waveform identity is
UID 78666/GID 20. The private configuration fails both read access and the
owner check. The firmware also retains `/tmp/AetherPDInstall.ssdr_waveform` as
root-owned mode 0644 after uninstall. Therefore this package format cannot
protect a bundled request credential and must not ship. The three bounded
launch probes did not reach the listener. A read-only inspection confirmed
the ownership mismatch; it did not establish which startup check ran first.
Test inputs granted no signed installation authority, and the exact leftover
test archives were removed through the bench helper after hash verification.

Keep the private configuration guard. Production integration needs request
credentials delivered into process memory through an authenticated channel,
without including them in the native archive or another public upload artifact.
The installed shim's authenticated API now implements the upgrade request's
RAM handoff to the fixed native requester and carrier credential receivers.
Public-path upgrade and rollback tests exercise that handoff. Independent
first installation, Linux desktop credential storage, selected crash/retry cases
and receive-only IQ have FLEX-8600 evidence linked below. These development
results do not qualify production package distribution, other desktop platforms
or the FLEX-6000 adapter.

### Independent first installation

The development implementation now has an explicit first-install plan and
transaction; see [FIRST-INSTALL.md](FIRST-INSTALL.md) for its authorization
boundary, identity behavior, component and live evidence, and remaining
qualification.
The legacy migration path still refuses a missing predecessor.

The standalone admission probe runs through a shell launcher on the 8600. It
observed UID 78666/GID 20, empty capabilities and NoNewPrivs; the extracted
mode-0600 fixture belonged to UID/GID 1001 and was unreadable (`EACCES`). The old
production requester exited 3. This confirms the private-archive problem. The
new requester uses public archive inputs and generates its private handoff in
RAM. Probe evidence is `fresh-install-implementation/probe-script-launch/radio.json`.

The protocol below describes the older input-transfer component used by the
installed-shim upgrade flow. The independent first requester does not expose
its TCP input listener or read a bundled private configuration.

The production entry accepts no command-line arguments or radio commands. It
requires UID 78666/GID 20 and empty effective, permitted and inheritable
capabilities, reads its private sibling `config`, and admits one connection on
fixed dual-stack TCP port 48995. It enables NoNewPrivs where the kernel supports
it; only the ARMv7 build permits its absence on legacy kernels.

The waveform receives the large signed inputs into RAM before handing
their descriptors to the fixed carrier. A small installed waveform can then avoid
unpacking candidate and predecessor releases onto the 6300's small `/user`
partition. This component bounds each release to 18 MiB and the signed worker
envelope to 512 KiB plus its 184-byte prefix. Actual package, runtime and update
peak-memory/disk budgets remain separate qualification requirements.

## Wire contract

All integer fields are unsigned big-endian. The receipt is the existing 80-byte
`PDDELIV1` record, independently known at both endpoints. Keyed hashes use
BLAKE2b with a 32-byte output and the private 32-byte request key. The request key
never travels on this stream. The stream authenticates public installer inputs;
it does not encrypt them or replace release-signature verification.

1. Receiver sends 72 bytes: `PDUPCH01`, 32 random bytes from `/dev/urandom`, then
   MAC(`PDUSRV01` || challenge || receipt). The desktop verifies this before
   sending input data.
2. Desktop sends a 152-byte header: `PDUPLD01`, four uint32 lengths, then four
   32-byte unkeyed BLAKE2b digests, in order: envelope, plan, candidate, previous.
   It follows with MAC(`PDUCLI01` || challenge || receipt || header).
3. Receiver verifies authentication, bounds and envelope/plan receipt binding
   before allocating files. Desktop sends the four contents in order and
   half-closes its write side. Initial installation has no previous content and
   requires both its length and digest fields to be zero.
4. Receiver verifies each complete content digest and requires EOF with no
   trailing bytes. It returns read-only descriptors and sends a stage-1 receipt.
5. The waveform connects to the fixed abstract Unix carrier endpoint, checks
   peer UID 0 and mutual request authentication, then hands over its root
   directory descriptor and the fixed input descriptors. It closes its own
   input descriptors after successful handoff and waits for the carrier result.
6. A stage-2 receipt reports delivery result 0, refusal 3, failure 4, cleanup
   failure 5, or unavailable/invalid result 6. Neither stage asserts that the
   installed upgrade committed; the desktop must verify installed state.

Both receipts contain `PDUPACK1`, uint32 stage, uint32 result, then
MAC(`PDUACK01` || challenge || receipt || preceding 16 bytes). Stage 1 requires
result zero. The client authenticates each response and its request binding.

A fresh challenge prevents reusing a response from a previous connection.
The production receiver has one total 90-second deadline, including all headers,
contents and EOF; intermittent progress does not extend it. The test build uses
one second so timeout regressions finish quickly. The native entry additionally
bounds admission to 120 seconds, carrier connection to 60 seconds and carrier
completion to 120 seconds, with an independent 400-second process alarm.

## Files and authority

Each input is opened mode 0600 with an unpredictable exclusive name in verified
tmpfs. The receiver opens a matching read-only descriptor, checks inode identity,
and unlinks the name **before receiving any content**. It uses neither
`memfd_create` nor `O_TMPFILE`, which the minimum Linux 2.6 platform lacks. Failed
transfers close all acquired descriptors; death during content transfer leaves no
named data. Death in the short create-to-unlink interval can leave an empty tmpfs
file; the final waveform lifecycle must handle that bounded startup residue.

Only the fixed input roles and bytes are accepted, never paths, PIDs, addresses,
commands or capability lists. The existing root delivery gate must still check
the signed worker and releases, exact approval and live radio identity before
acting. A valid upload alone grants no installation authority. The configuration
must be a regular read-only-opened, owner-matching, mode-0600 file. The waveform
does not fix unsafe permissions or acquire privileges to make itself run.

`package.py` produces a deterministic four-file `AetherPDInstall.ssdr_waveform`
archive for identical inputs, with a direct ELF entrypoint and no setup commands.
It contains the executable, configuration, manifest and licenses; release inputs
remain outside the archive. The package holds a per-request secret and must be
kept private and removed after use. This remains an offline/bench fixture because
the radio does not preserve that privacy. Building it is not operator approval.

## Installed-shim RAM handoff (development)

The authenticated shim endpoint `POST /v1/aetherpd/install-request` accepts
exactly one 120-byte `PDXFER01` record with `application/octet-stream`. It accepts
upgrade receipts only and blocks concurrent AetherPD session startup. The shim
opens the fixed protected `/run/aetherpd/bin/pd-primary-launch` executable and
runs its `--install-request` entry as the existing unprivileged identity.
No request field chooses an executable, path, argument, environment or port.

The inherited configuration on fd3 must be read-only, mode 0600, owner-matching,
unlinked and on tmpfs. The shim unlinks its temporary file before writing the
credential. The native entry additionally requires a write-only readiness pipe
on fd4; it reports readiness only after its fixed listener opens. Cancellation
kills the owned child, closes its broker, and releases the RAM descriptor.
Credential revocation cancels the owned job. The job has a 400-second limit.

The public record is `PDPUB001` + the 80-byte receipt +
BLAKE2b-256(`PDKEY001` + the 120-byte private record). It contains no private
request key. An abstract Unix broker at `aetherpd-config-<digest hex>` accepts
only UID-0 peers and the exact public record with one of two fixed role headers:
`PDCFGAC1` for the acceptor or `PDCFGDL1` for the installer child. Each role can
fetch once. It sends `PDSECR01` with one read-only private RAM descriptor and
closes after both roles have fetched or the operation is cancelled. Unexpected
incoming descriptors are closed and refused.

The native receiver verifies the shim peer, descriptor properties, receipt and
key commitment before creating its own root-owned, read-only, unlinked tmpfs
copy. The image builder includes only the public record at `/pd/request` and
rejects private request records. Native receiver/broker integration and image
layout have local test evidence. One FLEX-8600 signed successor upgrade through
the public carrier committed; an independent bench audit verified the selected
release and cleanup. Delivery result 0 precedes commit. The core desktop
`AetherPDInstallationMonitor` separately verifies the exact approved commit or
rollback with bounded polling, including across the tested shim restart.
Crash paths and actual FLEX-6000 qualification remain incomplete.

`GET /v1/aetherpd/install-request/<request_id>` reports the owned job's state;
`DELETE` cancels it. Both require the current operator token. Readiness and
delivery results do not assert installed commit. This API inherits the shim's
existing trusted-LAN, plain-HTTP transport; RAM-only storage does not add
transport encryption. The development desktop coordinator connects this API to
normal operator approval, upload, exact-plan monitoring and public cleanup.

## Offline verification

Build with `python3 build.py --output OUT` and run:

```sh
python3 test_upload.py --checker OUT/upload-check --disk-directory DISK_DIR -v
```

`DISK_DIR` must be on a non-tmpfs filesystem to exercise disk-staging refusal.
Tests use local TCP loopback and `/dev/shm`. They cover complete and fragmented
transfers, install/upgrade shapes, authentication, replay, receipt binding,
length/content tampering, trailing bytes, short inputs, descriptor cleanup,
read-only unlinked output, process death during transfer, and total deadlines
with both a silent and slowly progressing sender. Wrong-authentication tests
send otherwise complete valid inputs, so EOF cannot mask an absent MAC check.
Removing that check makes the wrong-key test fail.

`--request` builds the production waveform entry. `--request --fixture` builds
the host integration fixture; only that fixture accepts an inherited test
listener and substitutes the test user's identity. `test_request.py` exercises
the entry and Python uploader against a simulated carrier, including refused
authentication, failed delivery, cleanup failures and authenticated results.
The fixture clears capabilities inherited from the test runner before startup.

`--request --inherited-fixture` substitutes only the test identity, using the
real fixed listener and inherited descriptors. The shim's `TestPDInstall` tests
use `AETHERPD_REQUEST_FIXTURE` to locate this executable. Set
`AETHERPD_TEST_DISK_DIRECTORY` to a non-tmpfs directory for the disk refusal test.
Run the suite with inherited runner capabilities cleared, for example
`setpriv --inh-caps=-all go test -race -run '^TestPDInstall' -v .` in the shim
directory. It exercises the real native requester and Python client, with a
simulated carrier, and checks cancellation and unsafe credential rejection.

`--sanitize` enables ASan/UBSan; `--arch arm64` and `--arch armv7` cross-build.
Cross-builds do not establish Linux 2.6 runtime, resource or actual FLEX-6300
qualification. The rejected private archive remains unsafe regardless of those
builds. The public-input desktop workflow has scoped 8600 live qualification;
interrupted transactions, platform coverage and the final 6000 carrier remain
separate gates.

## Desktop request client

`src/core/AetherPDInstallRequestClient` owns one already-approved upgrade's
authenticated HTTP request. The coordinator must verify the distributed
artifacts, obtain exact-plan consent, and supply the RAM-only `PDXFER01` record
and approved `PDUPGR01`/`PDUPGR02` bytes. The client validates their plan digest and profile
binding and derives the public request ID locally before sending anything.
Its public-record helper contains no private request key. Callers must not put
the private record in a package, settings, logs or command-line arguments.

The client sends one fixed POST, and exposes explicit inspect and receiver-stop
operations only for that same request ID. It follows no redirects, bypasses HTTP
caches, bounds replies to 2 KiB, allows 10 seconds per HTTP request and retains
the credential context for at most 400 seconds. No mutation is automatically
retried. A lost acknowledgement is `Unknown`; the coordinator may inspect that
same ID without replaying the POST. A 404 after restart is `Missing`, not proof
that installation failed. An authorization refusal retires the context.

`StopRequested` acknowledges receiver cancellation only. It does not undo an
installer already handed off to the carrier. Destruction aborts local HTTP
observation without sending an additional remote mutation. Neither delivery
result zero nor any request-client state asserts commit; the separate monitor
must match the approved transaction. The desktop installation coordinator and
model supply operator UI, credential/session fences and package upload
coordination. Independent first installation uses its separate public-plan
path; this request client does not establish initial-install trust or a 6000
adapter.

## Desktop authenticated upload

`src/core/AetherPDInstallUpload` implements the duplex input transfer directly
in Qt, using the already-vendored Monocypher for keyed BLAKE2b and constant-time
MAC comparison. It accepts only an upgrade's four bounded input blobs. Before
connecting to the fixed port 48995, it checks the private receipt against the
approved plan and worker, and the two releases against the plan's digests. The
coordinator must authenticate the release signatures and obtain consent first;
hash matching alone does not establish publisher trust.

No artifact bytes are sent before the receiver's challenge authenticates.
Writes have at most 64 KiB queued. After all writes drain, the socket's write
side sends EOF while its read side remains open for the two authenticated
acknowledgements. Input reception has a total 90-second budget, including
connection and stage one; stage two has a separate 190-second budget. Partial
progress does not extend either budget. A failed connection is not retried.
Cancellation closes the stream and reports a local canceled transfer, without
asserting that a handed-off installer stopped. Only the independent exact-plan
monitor may report installed commit or rollback.

The registered test uses an injected byte channel and clock, with independent
Python vectors generated by `client_vectors.py`. It covers partial writes,
fragmented/coalesced acknowledgements, authentication, bounded buffers,
deadlines and destruction during callbacks. A separate local Qt-to-native test
transferred the public candidate/predecessor set (18,452,908 input bytes) into
read-only unlinked RAM files, checked every hash and descriptor cleanup, and
exercised EOF and both replies. Its stage-two refusal was injected by the test
adapter; it did not execute an installer or contact the radio. Windows/macOS
socket qualification remains required. The combined development operator workflow
has separate 8600 live evidence; the release catalogue and final platform/hardware
gates remain open.
