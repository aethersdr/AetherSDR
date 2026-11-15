# Native installer input transfer (development)

This directory contains the unprivileged temporary native installer waveform,
its RAM input receiver, a Python desktop transfer client and private package
builder. It is development code, not a complete desktop installation workflow.

**Live packaging blocker:** on FLEX-8600 firmware 4.3.16.42034, native extraction
assigns UID/GID 1001 to these files, while the intended waveform identity is
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
RAM handoff to the fixed native requester. The carrier's credential receiver
and initial-install trust setup remain unfinished; this does not yet replace
the complete live installation workflow.

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
request key. A one-use abstract Unix broker at `aetherpd-config-<digest hex>`
accepts only a UID-0 peer and the exact `PDCFG001` + public-record frame. It sends
`PDSECR01` with one read-only private RAM descriptor. Unexpected incoming
descriptors are closed and refused. The carrier still needs to consume this
record, verify the commitment and pass the credential to its fixed child;
its current image format is not eligible for public radio upload.

`GET /v1/aetherpd/install-request/<request_id>` reports the owned job's state;
`DELETE` cancels it. Both require the current operator token. Readiness and
delivery results do not assert installed commit. This API inherits the shim's
existing trusted-LAN, plain-HTTP transport; RAM-only storage does not add
transport encryption. It is not yet connected to the normal desktop installer.

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
qualification. Native extraction permissions, the combined live carrier graph,
automatic outer waveform/image removal, exact-plan approval and the normal
desktop installation UI remain separate qualification requirements.
