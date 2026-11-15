# Temporary ARM64 installation carrier (development)

This component replaces the bench script's installer-container orchestration.
It does not provide operator consent or removal of its own outer waveform/image.
The native waveform and authenticated desktop transfer component live in
`../request`; the combined process graph remains to be qualified live. It is not
a supported FLEX-6000 installer. No radio command or RF interface is exposed.

The native waveform launch tests found that the radio retains its uploaded ZIP
as mode 0644 and extracts its private configuration under a different UID. That
credential-bearing native package must not ship. The old private carrier image
was loaded only through the bench path and is not suitable for public upload.

The current per-request OCI image contains four static executables, a 120-byte
`PDPUB001` public receipt/key commitment and licenses. There is no shell or
private request configuration in the image. `image.py` assembles identical
inputs deterministically and rejects private `PDXFER01` records. The caller
must authenticate the distributed code and obtain approval of the exact plan;
image construction itself grants no consent.

The installed shim keeps the private request in unlinked tmpfs. The acceptor
and installer child each fetch it once using their fixed role and the matching
public record. The native receiver authenticates the shim UID, verifies the
read-only unlinked tmpfs descriptor, receipt and commitment, then makes its own
root-owned RAM copy. The broker closes after both roles fetch or the job ends.
Local integration tests exercise both role orders, replay, mismatched records
and malformed descriptors. The actual root/container graph, writable tmpfs
availability and normal radio upload lifecycle remain to be qualified. This
upgrade path depends on an already-installed shim; first-install trust remains
unresolved. See [`request/README.md`](../request/README.md) for the wire contract.

The entrypoint `/pd/accept` retires all effective, permitted, inheritable and
bounding capabilities and enables NoNewPrivs before accepting a request. It
requires either an empty bounding set or the initial SETPCAP capability needed
to retire it. Its public abstract socket name identifies the worker envelope;
the name contains no secret. UID 78666 and the existing mutual challenge/receipt
authentication protect the descriptor transfer. After one valid request it
executes `/pd/carrier` with the fixed inherited descriptors. It waits at most
60 seconds for a connection, with a 75-second process alarm.

The Go supervisor requires UID 0 with retired capabilities and NoNewPrivs. Root
UID and its access to the Podman socket still confer substantial authority;
capability retirement is not a claim that it is an unprivileged host process.
Its code accepts no command, path, container identifier or capability list from
the operator. It connects only to `run/podman/podman.sock` beneath the authenticated
host-root descriptor, finds its own labeled container by its observed host PID,
and pins that container's image ID. Container hostnames are not used as identity.

It creates one child from that image, with `/pd/deliver --listen-relay` as the
fixed command. The child has host PID/network/cgroup context, a read-only image root,
NoNewPrivs, no restart, automatic removal, and only CHOWN, DAC_OVERRIDE, FOWNER,
SYS_CHROOT and SYS_PTRACE. The existing native host-entry guard retires SYS_CHROOT
after entering the delegated root. The read-only image is not a write barrier
for the delegated host filesystem. One signed ARM64 upgrade under this capability set has committed on the bench;
first installation and interruption recovery through this carrier remain unqualified.

Creation uses the native `/v3.0.0/libpod/containers/create` endpoint and its
`cgroupns: {nsmode: host}` field. On the tested firmware, the Docker-compatible
creation endpoint ignores `CgroupnsMode`; a private namespace presents relative
service paths and the strict running-process identity check correctly refuses
admission. Host cgroup visibility needs no additional installer capability. The
[upstream namespace schema](https://github.com/containers/podman/blob/v3.0.1/pkg/specgen/namespaces.go)
and [creation schema](https://github.com/containers/podman/blob/v3.0.1/pkg/specgen/specgen.go)
describe these native fields.

The root-to-root relay reuses the authenticated descriptor protocol. It checks
UID 0 on both ends and the request secret/receipt; the original native sender
still requires UID 78666. A fixed 12-byte `PDDONE01` result travels back on the
owned authenticated connection after delivery returns. Only results 0, 3, 4 and
5 are accepted. Malformed packets or unexpected descriptors are refused and
closed. The supervisor checks that the relay's exit code agrees with its result.

The accept entry retains the original authenticated sender connection on file
descriptor 7. The supervisor verifies its Unix seqpacket type and UID 78666 peer,
then sends `PDDONE01` back after child cleanup. Failed delivery or cleanup cannot
be reported as success; a disconnected original sender makes completion fail.
The native requester authenticates the resulting desktop acknowledgement.
This return path postdates the successful bench upgrade below and has only
local component coverage so far. The older fire-and-forget bench sender cannot
qualify it.

The supervisor has a 120-second context, bounded HTTP responses and child logs,
and a separate eight-second cleanup budget. Cleanup verifies the per-run random
ownership label and pinned image before removing a child, including when a
create response was lost. The child has a 125-second alarm and automatic container
removal to bound its lifetime after abrupt supervisor death. These crash paths
still require live qualification. The native delivery worker retains its existing
90-second limit and authenticated staging/recovery rules.

Build the native entries using `boot/build.py --accept`, `--send`, and `--deliver`
with `--arch arm64`; delivery also requires `--public-key`. Build the supervisor
from this directory with `CGO_ENABLED=0 GOOS=linux GOARCH=arm64 go build -trimpath`.
`go test -race ./...` checks the fixed child authority, self-process admission,
response/output bounds and refusal to remove foreign containers. Removing the
per-request cleanup-label guard makes its test fail. The native transport tests
cover completion framing and descriptor cleanup in addition to authentication.

On FLEX-8600 firmware 4.3.16.42034, the fixed supervisor created its own child,
relayed deliberately unauthentic release inputs, received delivery refusal 3,
and removed the child without bench cleanup. Installed metadata, boot identity
and primary process start time were unchanged. The bench harness removed the
outer container, image and native sender fixture. No RF was used. The first
probe's hostname-based self-identification failure is retained in the evidence;
the host-PID-based implementation passed the second probe.

The tested image was about 2.7 MB compressed and 6.5 MB as an uncompressed layer.
That is a measured ARM64 development artifact, not a complete installer peak-disk
budget or actual FLEX-6300 qualification. The broad diagnostics bridge remains
on the bench and was used to load/start the outer test image and arrange the
unprivileged sender fixture. Outer bootstrap lifecycle,
exact operator preview/consent, desktop upload, and the 6000 privilege adapter
remain required before shipping.

A subsequent signed candidate upgrade committed in 42.816 seconds, preserved the
public identity and restarted the primary as UID 78666 with all capability sets
empty and NoNewPrivs enabled. Observed temporary systemd worker processes had only
CHOWN, DAC_OVERRIDE, FOWNER and SYS_PTRACE, with NoNewPrivs enabled. The worker
capsule, delivery staging, temporary services and installer child were removed;
the bench harness removed the outer image/container and sender fixture. This is
one successful bench upgrade, not full deployment or crash-path qualification.
The earlier failed deliveries and namespace probes remain in the local evidence.
