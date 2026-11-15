# Authenticated AetherPD boot payload (development)

This component verifies and expands a fixed compressed payload into radio RAM.
The payload transaction supports atomic activation and explicit rollback. It
is not a complete production installer or release trust configuration. The build requires an explicit Ed25519 public key. Bench keys
must never become default release keys.

`bundle.py` builds a deterministic signed bundle containing eleven fixed records.
`build.py` builds the native boot helper for ARM64 or Cortex-A8 ARMv7;
`--update` builds the one-shot payload transaction. Host parser checks use
`--test`; `test_bundle.py` exercises the actual native verifier.
`--store-test` builds a host-only transaction adapter for `test_store.py`.
`units.py` renders fixed units for inspection and does not install them.

The format has an 840-byte signed manifest, a 64-byte Ed25519 signature and
concatenated gzip records. Compressed and expanded BLAKE2b-256 digests, exact
lengths, an architecture tag and an input source archive SHA-256 bind the payload.
Total compressed size is at most 16 MiB; total expanded size is at most 48 MiB.
Record names and executable modes come from the native verifier, not the bundle.
All compressed records are authenticated before expansion. Extraction also
checks expanded hashes, gzip integrity, exact lengths and ELF architecture.

The boot entry accepts no caller-selected paths, commands or environment
settings. It reads a protected fixed `active` selector and one fixed payload slot,
expands through a private RAM staging directory, and atomically publishes `bin`.
An existing different RAM payload is refused; stopping and retiring an old
process graph before update remains the installer's responsibility. An abandoned
stage containing only known, protected files can be removed. Unknown entries
cause refusal. `--socket-owner` only assigns the fixed mode-0600 Unix socket to
UID 78666/GID 20. Neither entry starts a correction session or transmits.
Both entries require the installation gate described below. A prepared store
without a committed migration cannot boot by itself; the earlier development
reboot fixtures predate this gate and do not qualify the current installer.

ARM64 uses `/var/lib/smartsdr/aetherpd` because `/user` is a symlink on the bench
8600. ARMv7 uses `/user/aetherpd`; every parent must be protected. Expanded
executables and volatile state live under `/run/aetherpd`. Persistent identity
and configuration belong in the small ordinary-user `state` directory.

The 8600 development reboot proof installed only the boot service, session
socket and matching session template, then removed them after inspection.
The boot helper finished at uptime 10.50 s and the socket listened at 10.57 s;
all eleven payload hashes and the state marker survived correctly. No setup
helper remained running. This is one software reboot, not a power-interruption
or FLEX-6300 qualification. The existing shim already owned port 48992, so this
proof did not replace it or enable the rendered primary shim service. Persistent
credentials, automatic primary startup, update/rollback, and the final installed
desktop workflow require separate qualification. A post-reboot manual ordinary-user client on a spare port passed authenticated
Start, six unkeyed status checks and Stop through the boot-restored socket.
This manual fixture does not establish primary-service autostart.

The boot helper includes the already-vendored Monocypher and zlib code. Include
`BOOT-LICENSES.txt` with its binary in a distribution, alongside the payload's
own `LICENSES` record. The bundle signature authenticates payload records; the
outer installer must independently authenticate the boot helper, units and
trust anchor before any privileged execution.

## Payload activation and rollback

`pd-update` accepts no paths: stdin must be a bounded regular bundle file.
Its sole optional argument, `--rollback`, selects the recorded previous payload.
It must run once under the approved bootstrap authority and be removed afterward;
it is not a persistent privileged API. Both entries require host root and the
compiled architecture/trust key. They execute no payload and run no shell or
service-management command.

The outer installer must create the protected store and `slots` directories,
stop and retire the complete runtime graph, and remove its RAM tree before
calling this component. `pd-update` refuses while `/run/aetherpd` exists. Boot
and update serialize using the same protected store lock. This is not a
substitute for proving graph retirement in the outer installer.

The 88-byte `PDSEL002` selector records current and previous slot identifiers
and their whole-bundle digests in one atomic file replacement. The boot reader
also accepts the original 48-byte `PDSEL001` format. Names A, B and C are fixed;
only two payloads remain after successful collection. The unused third slot is
staging space, so updating never overwrites either referenced payload. Each
copied bundle is authenticated and fully decompressed into a bounded discard
buffer to verify expanded hashes, gzip integrity and ELF architecture before
publication. No expanded payload is stored persistently.

Ordering is: write and validate candidate; fsync candidate; rename candidate
into unused slot; fsync slots directory; write and fsync complete new selector;
rename selector; fsync store; collect authenticated unreferenced slots. Refuse
an update unless available space covers the candidate plus an 8 MiB reserve.
This accounts for a transient third compressed payload. Filesystem behavior and
actual disk headroom still require qualification on each supported radio.

A failure after selector rename can mean activation committed but cleanup did
not. The caller must read back state rather than report that nothing changed or
blindly toggle rollback. Retrying the same update reconciles authenticated
leftovers without changing a matching selected payload. Interrupted first
installs are similarly resumable. These guarantees are tested with process
termination and injected failure returns, not physical power loss.

A retry that observes its candidate already selected syncs the store directory
before collecting an unreferenced slot. The preceding process may have died
between selector rename and directory sync; a visible selector alone is not a
durable commit. The storage regression closes the actual store descriptor at
that boundary, requires failure with all three authenticated slots preserved,
then verifies that a later successful retry collects only the orphan. Removing
the sync makes the regression fail. All12 storage cases pass on the host and
under address/undefined/leak sanitizers; the focused failure/retry case also
passes in disposable fixtures with ARM64 and ARMv7 executables on the8600.
This does not qualify physical power loss or FLEX6000 storage behavior.

Explicit rollback validates the previous payload before switching. A corrupted
current payload can be rolled back, but its unverified former slot is retained;
updates refuse to delete a corrupt or foreign orphan. A bounded, operator-visible
repair transaction for this case remains required in the outer installer.
Unknown files, symlinks and hard links are not treated as disposable bundles.

### Full installed-release upgrade requirements

The payload updater does not replace the installed boot verifier, trust anchor,
units or migration approval. The migration worker recognizes its committed
transaction as terminal; replaying it is cleanup, not an upgrade. Never rewrite
that committed journal into an earlier phase to force another installation.

A complete upgrade must preserve an authenticated boot-verifier/payload pair
through every interruption, including a verifier or signing-key change. It must
bind operator approval to the actual radio, current installation and candidate,
stop intake and retire the complete owned runtime before publication, preserve
the latest private identity, and verify primary readiness before commitment.
Recovery must be local and independent of the desktop connection, must retain
the previous bootable pair, and must remove temporary upgrade authority only
after a durable terminal result. ARMv7 disk accounting must include candidate,
rollback pair, temporary recovery inputs and reserve, not merely one bundle.

The installed8600 development candidate uses a generated signing key whose
private half was not retained. Another package cannot be signed with that key.
A transition requires separately authenticated, operator-approved provisioning
of the new trust anchor together with its matching boot verifier and payload;
accepting an unsigned bundle or changing only the compiled key is insufficient.
Shipping signing-key custody and rotation are still open release requirements.
The installed candidate remains unchanged while this controller is developed.

The `pd_upgrade` component implements read-only preflight and private staging
for that controller. Its352-byte `PDUPGR01` approval binds two complete release
digests, two public keys and the same radio profile, firmware and serial. The
candidate key must equal the independently authenticated bootstrap's expected
key; a key supplied in an approval is not its own trust authority. Both releases
must authenticate. The previous release must match the actual installed boot
executable, selected payload, installed trust marker, licenses and both copies
of the fixed service definitions. Inspection holds a shared committed-install
lease; it refuses an unfinished or exclusively locked migration.

Staging requires a new protected directory. It records approval, then copies
only the fixed `previous` and `candidate` release members. Each directory also
retains `release.header`, containing the exact authenticated manifest and
signature. Concatenating that header with the eight files in manifest order
reconstructs the approved outer release. Copying rechecks member hashes and
fully validates the inner payload before declaring staging successful. A signed
outer envelope alone does not establish that its compressed inner payload is
valid. Failure leaves an uncommitted stage; no installed selector, unit, identity
or process is changed, and no staged executable is run.

`build.py --upgrade-test --arch host --output <directory>` builds the fixture
adapter for `test_upgrade.py --checker <directory>/upgrade-check`. Its arbitrary
fixture paths and input-replacement controls are absent from production entry
points. The fixed worker below integrates selection, activation and recovery;
authenticated delivery, successive updates and installed qualification
remain required.

`pd_upgrade_reopen` validates a complete stage in a fresh process without the
original input files or an unchanged installed store. It requires exactly the
approval file and two protected release directories, with no extra entries,
links, special files, changed ownership/modes or cross-device members. The
shared release parser authenticates each signed header; every member is checked
for its signed size/hash and fixed executable/unit semantics. The reconstructed
whole release must match its approved digest, and each inner payload is fully
validated again. The candidate key and observed radio must match the expected
bootstrap key and approved target. Header, member and directory syncs complete
before success is reported, covering a process killed before final stage sync.

Recovery tests kill the staging process at nine boundaries, including after
the last release is written but before final directory sync. Complete stages
reopen; missing or partial stages are refused without deletion or repair.
Corrupted headers/members, a valid signed envelope with an invalid inner
payload, changed approval digests, unexpected entries and failed final sync
are refused. This proves complete-stage recovery, not physical power-loss
behavior. The fixed worker must serialize
stage access and recheck current radio identity before any live transition.

`pd_upgrade_resume` continues an interrupted stage before activation. It first
reads both supplied releases into private memory (at most36MiB total), then
authenticates their complete approved digests, signed headers and members.
Later changes to the input files cannot change the bytes being copied. It
checks the entire existing stage before writing: every existing file must be
an exact prefix of its approved contents, with protected ownership, mode,
single-link and same-filesystem metadata. Partial files must have mode0600;
final executable/data modes require complete files. Child directories require
complete approval, and release members require a complete signed header.

Matching partial files are appended in place; missing fixed files are created
exclusively. Unknown entries, mismatched bytes, links and special files are
preserved and refused, including when an earlier file could otherwise be
repaired. Completion syncs files and directories and invokes the independent
whole-stage verifier, including inner-payload validation. Invalid inner payloads
may leave an uncommitted stage but cannot produce a successful resume.

The fixture suite covers every approval/header prefix length, partial members,
idempotent retries, changed sources before and after snapshot authentication,
all19 completed-file boundaries and a real partial16KiB write interrupted by
SIGKILL. These are component tests; no installed upgrade or hardware power-loss
qualification is implied. The future fixed worker must hold exclusive stage
access, repeat committed-installation and live-target checks, and account for
additional recovery inputs before invoking these functions.

Both initial staging and resumption call `pd_upgrade_space` before their first
write. It counts all missing approval, signed-header and release-member bytes
for both releases, rounded to the filesystem allocation unit. A partial file's
boundary block is charged conservatively even if already allocated. Matching
existing prefixes are credited; complete files require no further content
allocation. Three directory-growth blocks and the existing8MiB store reserve
are included. Filesystems reporting inode capacity must also have enough free
inodes for missing files and directories. Insufficient capacity leaves the
stage unchanged.

This is an admission check, not a filesystem reservation. Concurrent external
writers may still cause later writes to fail; those failures leave an
uncommitted stage for authenticated resumption. Already allocated installed
files and inputs reduce the observed free space naturally. A future worker
must separately budget any recovery worker, retained inputs or extra slots it
has not created yet. Staging admission alone does not qualify the full6300
installation or update footprint.

`pd_pair` supplies the linked selection primitives for that worker. Its separate
protected control directory contains only an empty lease file, `active`, and an
optional `active.new`. The80-byte `PDPAIR01` selector binds one fixed choice
(previous or candidate) to the whole approval record and selected release
digest. It contains no executable path or command. Initial publication must
select the previous release; candidate activation verifies both releases so a
verified rollback pair is available at the point of selection.

Publication holds an exclusive nonblocking lease, authenticates existing
selection and any partial temporary record, writes and syncs the replacement,
renames it atomically, then syncs the directory. A matching retry also syncs the
directory before success. Temporary records must match a prefix of one of the
two approved choices; foreign bytes or unsafe metadata are preserved and
refused. File sync/rename/commit interruption tests observe either the old or
new complete selection, and retries finish durable publication.

Acquisition holds a shared lease and returns separately pinned, authenticated
boot-verifier and payload descriptors from the selected release. The lease
remains open until the consumer releases it. A concurrent publisher is refused,
and process death releases the lease. Boot acquisition validates only the
selected side, using `pd_upgrade_reopen_choice`: a damaged or missing rejected
candidate cannot prevent selecting and booting an intact previous release.
Selecting the candidate still requires both sides to validate. Approval, target,
bootstrap-key binding and the selected release's full inner-payload validation
remain mandatory in either direction.

The selection primitives do not execute either boot verifier. `pd_handoff`
provides the fixed exec handoff: descriptors64/65/66 carry the shared lease,
verified payload and verified boot executable. It duplicates them without
collisions, closes unrelated descriptors, sets cwd to `/`, and supplies only a
fixed PATH plus the modern contract marker. The only optional argument is
`--socket-owner`. The receiving modern verifier checks its own inode against
the inherited executable, the lease against the fixed protected control file,
read-only payload/executable access, ownership/modes and the payload signature
under its compiled key. It retains the lease until boot preparation exits.

The boot entry dispatches through fixed `upgrade-control` and `upgrade-pair`
directories only when the control directory exists. The permanent loader checks
runtime admission, probes the radio identity and acquires the selected pair.
It retains the transaction and selection leases through preparation and verifies
the payload with the selected generation's authenticated key. It does not execute
an older boot verifier: older installation-only gates cannot admit controlled
upgrade restoration or recognize pending upgrade authority. Without upgrade
control, the existing boot path remains available. The paired-exec primitives
remain separately tested, but are not the dispatcher's preparation path.

The permanent boot helper also has fixed `--primary` and `--session` entry points.
They use the current launcher gates and resolve programs only under the protected
`/run/aetherpd/bin` directory. The selected payload cannot substitute older gates
by supplying its own launcher. These modes reuse the existing privilege-drop,
local-peer and request validation. `units.py --managed-startup` and
`release.py --managed-startup` select the signed `AEPDR003` layout, routing primary
and session startup through these entries. The default remains the legacy layout
until the independent upgrade worker and full launch paths are qualified.

The service adapter accepts either complete fixed layout for quiescence and
original installation. It refuses mixed installed files, mismatched loaded
commands, and session instances using a different layout. Upgrade activation
and restoration require the permanent-helper layout; a legacy release may supply
the restored DSP payload, but its old service entries cannot manage the upgrade.
Component tests and archived release verification pass; actual service-layout
replacement and installed rollback remain unqualified.

The permanent entries have also run as real ARM64 and ARMv7 executables on the
8600 inside disposable chroots and private mount/network namespaces. Each build
passed13 cases: primary privilege/argument/environment/scheduling checks,
controlled versus stale upgrade admission, and session peer/executable/frame/
transaction checks. The successful session passed its fixed request and socket
to an inert constructor and dropped the log collector to the waveform UID with
empty capabilities. This verifies the entry and handoff; it does not qualify an
installed service transition or the full constructor/DSP graph through that entry.

Native preparation additionally passed seven cases per architecture with two
different fixture signing keys. Both generations prepared under their respective
keys, wrong/compiled keys and corrupted headers were refused, retries were
idempotent, and a different existing RAM payload was preserved. These tests invoke
`boot_prepare_key` through a test-only descriptor wrapper; full dispatcher target
probing, recovery orchestration and reboot behavior remain to be exercised.
The installed primary was unchanged and temporary jobs/files were removed. There
was no RF. ARMv7 execution on the8600 does not qualify Linux2.6 or6300 hardware.

Real host exec fixtures exercise lease retention, descriptor/environment cleanup,
socket-owner arguments, child death and mismatched payload/lease/executable
refusal. These are not a complete dispatcher/worker installation proof. In
particular, radio API availability and boot-unit ordering must be qualified
before installing the dispatcher; a failed identity probe currently refuses
startup. A recovery worker that synchronously starts the boot service must not
itself be ordered Before that service. Live quiescence, upgrade-phase admission, independent recovery before
replacing the boot entry, latest identity preservation and authority retirement
remain required. The worker must use the same lease for every stage mutation.
An atomic selector alone does not make those operations atomic.

### Upgrade transaction and session admission

`pd_generation` stages each successor beneath
`upgrade-generations/<approval-blake2b>/`, with separate fixed `pair` and `control`
directories. The 40-byte `PDGEN001` record in `upgrade-generation` selects that
approval, so pair and control cannot come from different generations. Names are
derived from validated approvals; the interface accepts no arbitrary path. The
active reader also requires agreement with the upgrade journal and staged
approval, and refuses a pending selector. Missing selection after an upgrade is
an error, not permission to start the original installed payload.

Selector publication requires the matching QUIESCING or ROLLBACK journal,
retained capsule and live exclusive transaction owner. It verifies the complete
selected previous payload before publishing. A predecessor selected by the
trusted origin record may be replaced; foreign selectors or partial prefixes
remain untouched. File fsync, atomic rename and directory fsync make retries
idempotent. The fixed `upgrade-pair`/`upgrade-control` layout remains readable.
Boot dispatch, inherited handoff lookup and successor preflight use this reader.

The signed AEPDR005 contract enables generation publication in the combined
bootstrap/worker. Recovery is armed before executable authority is published;
interruption during management replacement or selection restores the previous
payload. A successor installation and software reboot have passed on the 8600.
Predecessor and original-slot retirement have host component and worker coverage.
Final 6300 peak-space accounting remains unfinished; this does not
establish production or cold-power-cycle qualification.

`pd_upgrade_transaction` uses the existing exclusive `transaction.lock` and a
separate `upgrade-transaction` journal. It never rewrites the committed original
installation approval or journal. An existing session's shared lease refuses an
upgrade before any callback runs. New sessions remain blocked until the upgrade
has a durable terminal result and its temporary recovery capsule is absent.

The forward sequence is quiesce, select, activate/readiness-check, commit, then
cleanup. A callback failure or re-entry before commitment takes rollback,
restore/readiness-check, and cleanup. Recovery does not advance a candidate,
including after an interrupted initial journal publication. A committed retry
syncs the terminal decision before cleanup and never rolls back. A subsequent
upgrade must name the release and key selected by the previous terminal result.
Foreign records and unsafe metadata are refused without replacement.

Successor preflight verifies the terminal journal against the protected staged
pair and its selected side under the pair lease. The next approval must name that
selected release and signing key. Installed boot, licenses and both copies of the
service definitions must match the predecessor's candidate management, which
remains installed even after payload rollback. Both staged predecessor releases
must be intact unless a protected terminal receipt authorizes removal of the
unselected payload. The selected release and all remaining management members
must still authenticate. Pending selectors/journals and retained recovery
authority refuse admission. Distinct terminal predecessors require the candidate's
generation-aware boot contract and exact predecessor journal captured at bootstrap.

Boot preparation and primary readiness have a narrower exception: while an
exclusive worker is alive, they may run in upgrade ACTIVATING or RESTORING with
the protected recovery capsule present. Sessions never receive this exception.
Worker death revokes it. Pending partial journals block all normal admission.

`build.py --upgrade-transaction-test --arch host --output <directory>` builds
the fixture for `test_upgrade_transaction.py`. Its service callbacks manipulate
inert test files; they are not installed service adapters. The tests cover
publication/callback interruption, all initial-record prefix lengths, lock
exclusion, cleanup retry, retained-authority rejection and identity preservation
by the simulated adapters. `test_session_gate.py` separately exercises kernel
locks and worker death in distinct processes.

The transaction requires the fixed worker to authenticate its own capsule,
both releases and live target before entry. Capsule-directory metadata alone
does not authenticate an upgrade. The fixed worker integrates the service
adapters and management replacement. Independent recovery installation, actual
credential preservation and live upgrade/rollback qualification remain unfinished.

`pd_runtime_upgrade_retire` connects the upgrade journal to the fixed runtime
cleanup adapter. It requires QUIESCING or ROLLBACK, the matching approval, an
exclusive transaction owner, retained recovery capsule and committed original
installation. It stops and verifies the owned service graph before inspecting
and removing only the fixed RAM layout. Unknown entries, links and unsafe
metadata are preserved and refused. Persistent state is outside this cleanup.
Filesystem tests exercise both permitted phases, process interruption at each
unlink, phase/approval/lease refusal and persistent-state preservation. These
fixtures simulate service quiescence; installed upgrade-driven quiescence has
not yet been exercised.

`pd_service_upgrade_prepare_runtime` and `pd_service_upgrade_activate` use the
same fixed units and commands as installation. Their upgrade gate requires the
matching approval in ACTIVATING or RESTORING, the recovery capsule and live
exclusive transaction owner, and the permanent-helper service layout. Preparation checks the loaded unit definitions and
active boot service. Activation pins the expected executable's actual primary
process, verifies its public identity/listener, and rechecks process identity,
MainPID, boot/socket state, unit definitions and transaction admission before
success. Both paths recheck admission after the service operation.

`build.py --service-activation-test --arch host --output <directory>` builds
the fixture used by `test_service_activation.py`. It exercises the real service
adapter and journal gates with simulated systemd responses and process-attestation
results. It refuses failed commands, mismatched/replaced processes, wrong public
identity, changed definitions, inactive boot/socket and lost worker authority.
The original installation entry points share this implementation and retain
their installation-phase gate. This is not installed upgrade qualification;
the fixed recovery worker still must authenticate the selected payload, bind
the executable and expected identity, and invoke these adapters.

### Independent upgrade recovery services

`upgrade_units.py` renders two temporary fixed units for each architecture.
Only `aetherpd-upgrade-recover.service` is enabled for boot; the forward
`aetherpd-upgrade.service` is explicitly started once after approval. Both call
the protected sibling `aetherpd-upgrade/worker` directly, so recovery can be armed
before replacing a legacy `pd-boot`. They have no `Before` relationship to the
runtime services they must synchronously start during validation. Neither unit
accepts an operator-supplied path or command. Rendering/building does not install
or start them. Actual old-systemd behavior on6300 remains unqualified.

`pd_upgrade_units` uses the same atomic file/link implementation as the original
installer. It verifies exact text, root ownership/modes, single links and absence
of drop-ins before publishing; matching prefixes resume after interruption and
foreign objects remain untouched. Only recovery receives a boot link. The fixed
service adapter validates loaded metadata, exact worker executable/arguments,
current MainPID and that the other worker is quiet. Its launch operation starts
only the forward unit with `--no-block`.

Removal requires the matching terminal upgrade record with no partial record,
and absence of active, pending and retired executable-authority directories.
The permanent boot path calls `pd_upgrade_reconcile_terminal`: it leaves unfinished
upgrades to recovery, but finishes authenticated retired-capsule cleanup and
removes temporary units after a durable commit or rollback. This allows cleanup
to finish when the temporary executable is already gone. Wrong keys, malformed
state, foreign files and remaining active authority are refused.

Host tests exercise real unit files and process-death checkpoints, with simulated
systemd metadata for worker admission. Existing installer/capsule regressions
also pass after extracting the shared file operations. The fixed worker below
uses these units. Bootstrap publication is namespace-tested; end-to-end installed
rollback remains open. Launching the independent worker verifies its own units
without requiring a consistent runtime-unit generation, so a management-file
replacement interrupted by a crash can be repaired.

### Fixed upgrade worker and permanent management transition

The independent `pd_upgrade_worker` entry executes only from its authenticated,
protected capsule. Its separate `--bootstrap` entry requires independently
authenticated root execution and protected operator approval.
It checks its executing inode, compiled candidate key, embedded complete-release
digest, exact systemd worker identity, live radio target and committed original
installation before stopping runtime services. Existing sessions are stopped
before requesting the exclusive transaction lease. A terminal retry only cleans
temporary authority; it does not stop or reactivate the selected runtime. Before
the first runtime stop, the worker syncs a 40-byte `started` record bound to the
capsule intent. Every restarted attempt takes rollback, including a crash before
the transaction journal could be written. Matching partial record prefixes also
mean recovery; foreign records are refused.

The worker connects quiescence, permanent management replacement, atomic pair
selection, selected-payload verification, primary readiness and rollback to the
durable upgrade transaction. Recovery never advances a candidate. It can restore
the authenticated previous payload even when the rejected candidate payload is
damaged. Persistent private state is outside replacement and retirement.

`pd_management_apply` replaces only the fixed boot executable, its licenses and
the four cached and four installed service definitions. It accepts only the
authenticated previous or desired bytes, rejects foreign files and drop-ins
before writing, budgets temporary allocation with the store reserve, and uses
fsync plus atomic rename for each publication. Matching partial writes can
resume. For the four installed system-unit destinations, differing file and
directory device numbers are accepted only when both descriptors identify the
same overlay filesystem. All other destinations require matching device numbers;
ownership, mode, single-link, signature and no-symlink checks still apply.
These operations require a matching QUIESCING or ROLLBACK journal and
live exclusive worker. The current management helper remains installed when
the DSP payload rolls back to its previous generation.

`pd_upgrade_inspect_origin` captures that installed management as an 836-byte
`PDORIG01` record after authenticating actual files and predecessor selection.
The record binds the next approval, the terminal predecessor journal (or zeros
for the initial upgrade), and the signed management header. It contains no paths,
commands, credentials or private identity. Its header is not a standalone trust
grant: the authenticated bootstrap supplies the complete record in its protected
recovery capsule. `pd_management_apply_origin` rechecks lineage and the management
signature, then applies the same fixed-file replacement gates. The worker can
therefore replace retained management from a different signing key than its
selected previous DSP payload, without keeping that management release's payload.

The opt-in signed `AEPDR004` release contract (`release.py --managed-startup
--origin-recovery`) attests that the permanent boot helper can finish version2
capsule cleanup after the temporary worker is gone. Bootstrap uses `PDUPCAP2`
for this contract and its successors and retains `PDUPCAP1` for older managed releases. The
worker refuses a version2 capsule paired with an older candidate before stopping
runtime services. This does not enable successive updates by itself: staging,
durable generation selection and retirement remain separate requirements.

The worker embeds the signed candidate management header, boot bytes and licenses.
`management_blob.py` snapshots a bounded managed release, verifies its complete
outer signature/digest and inner payload with the native release verifier, then
emits a deterministic immutable C object. No private signing key is embedded.
Thus repairing partially replaced service definitions does not require an intact
rejected candidate payload or the original bootstrap inputs.

Build with `build.py --upgrade-worker --arch arm64 --public-key <key>
--management-release <signed-release> --output <directory>` (or `armv7`). The
build enforces the capsule's 512 KiB executable limit. Host namespace fixtures
use `--worker-fixture`; hardware adapters are never linked into radio builds.
The `--bootstrap` entry receives protected `PDUPGR01` approval on FD3 and signed
previous/candidate releases on FD4/5. It verifies the original committed
installation, exact embedded candidate, both releases and live target. Before
copying the pair it budgets remaining staging, full worker/intent copies,
management temporaries, fixed service files, selectors, journals and directory
growth, retaining an 8 MiB reserve on each affected filesystem. Byte and inode
capacity are checked together; matching partial staging receives space credit,
while partial management/capsule copies do not.

Generation-aware updates additionally reserve the three retirement records and
four blocks/inodes for receipt directories and parent growth. Their previous and
candidate release descriptors must reside on tmpfs; persistent release inputs
are refused before staging or runtime changes. The signed release parser bounds
each input to 18 MiB. Matching active-worker retries use their already verified
capsule and do not require the original input descriptors. Host fixtures measure
filesystem byte/inode peaks across full initial and successor transactions and
check them against admission, including the 8 MiB byte reserve. A separate
fixture uses actual persistent storage to verify refusal without quiescence.

Bootstrap resumes authenticated staging, selects the previous payload and verifies
the running previous primary's signed executable and public identity. The signed
`AEPDR005` contract (`--managed-startup --origin-recovery --generation-selection`)
enables independent approval-derived generations and generation-aware permanent
boot. For these candidates, bootstrap durably arms recovery before publishing the
active worker. Older candidates retain their legacy capsule publication path.
Bootstrap queues the worker without stopping runtime services itself. Matching capsule retries need neither the
original release inputs nor a still-running primary. Terminal retries finish
detached cleanup. Separate bootstrap/store/capsule leases serialize these paths.
The entry does not supply privilege or authenticate its own delivery: release
trust configuration and operator consent remain external requirements.

Twenty management tests pass normally and with ASan/UBSan/LSan, including 36
replacement interruption points, reproducible compiled embedding, overlay unit
replacement, foreign-filesystem refusal and separately signed retained management.
Thirty-nine
combined bootstrap/worker tests run in disposable user/mount/network namespaces with real
signatures, filesystem updates, leases, process death and cleanup. Radio identity,
systemd commands and primary attestation are simulated in that suite. It covers
forward commitment, early recovery, mixed management files, damaged candidate
payloads, repeated interruption, admission refusal, future allocation reserves,
bootstrap retry and terminal cleanup. Removing the restarted-attempt guard makes
the pre-journal crash test select the candidate incorrectly; that mutation is
caught. Three additional legacy-worker cases prove version1 bootstrap/operation
and refusal of an unsupported version2 capsule before quiescence. The service
suite has 45 cases and the upgrade capsule suite 30, both
passing ASan/UBSan/LSan. Capsule tests cover every started-record prefix and
interruption during terminal removal. The transaction regression suite has
nineteen passing cases from the preceding worker checkpoint.

Both radio architectures cross-build with the actual management boot binary
embedded, but compile-fixture DSP payloads are inert and must never be installed.
No installed package or radio state was changed by these tests. Bootstrap
delivery/consent, actual target disk budgeting, successive update generations,
complete cold-boot dispatch and actual installed upgrade/interruption/rollback
remain unqualified; these component results do not establish production readiness.

### Durable upgrade recovery inputs

`pd_upgrade_capsule` publishes two fixed files in the protected sibling directory
`aetherpd-upgrade`: a bounded worker executable and a `PDUPCAP1` or `PDUPCAP2`
intent record. Version2 adds the complete authenticated management-origin record;
the started receipt hashes the entire versioned intent. Both versions support
matching partial publication, replay and interrupted terminal cleanup.
The intent binds the approved previous/candidate release and key transition,
worker digest and canonical public identity expected after activation or
restoration. Private credentials stay in the existing persistent state directory.
The candidate key comes from the independently authenticated bootstrap or
compiled worker; the intent cannot supply its own trust authority.
The worker adds the fixed `started` receipt before stopping any runtime service.
The receipt carries only the intent digest, not credentials or an executable path.

Publication uses a separate namespace lock, a private pending directory,
matching-prefix resumption, complete inventory validation, file/directory sync
and atomic directory rename. It reserves space before copying and rejects an
oversized worker, foreign partial content, links or unsafe metadata. Open pins
the namespace and worker and revalidates the intent/hash; self-admission compares
the executing inode with the pinned worker. The installer and upgrade capsules
share the protected-file copy primitives in `pd_capsule_io`.

Removal requires the exact approval in a durable COMMITTED or ROLLED_BACK
upgrade journal with no partial journal publication. It detaches the active
directory, removes the started receipt and worker before their intent, then syncs removal. Interrupted
removal can finish from the terminal journal and retained retired directory;
foreign retired content is preserved and refused.

Publishing a capsule alone neither installs a recovery service nor changes the
boot entry. The bootstrap and independent worker above connect pair/target
authentication, executable readiness, management replacement and cleanup.
Installed service-manager and hardware qualification remain required.

The 8600 component proof exercised three real development payloads, boot
expansion after each change, rollback, bad-signature rejection and refusal while
RAM existed. Retained payload storage measured 17,230,480 bytes. The native
transaction binary was reproduced byte-for-byte. No service, DSP, calibration
or transmission was started, and all fixture files were removed. These results
do not qualify migration of the existing container shim, primary autostart,
credential migration, full release installation or Linux 2.6 execution.

## Outer release authentication

`release.py` assembles `AEPDR002`: a signed, bounded manifest naming a radio
profile, exact firmware string, source archive hash, and eight fixed records.
Those records are the boot helper, temporary updater, compressed payload,
boot licenses and four fixed systemd units. The whole release has an 18 MiB
limit. Both the outer release and inner payload use the configured trust key;
there is no default development key in the build.

Version2 declares the fixed paired-boot descriptor contract under the release
signature. The opt-in `--managed-startup` format `AEPDR003` additionally requires
primary/session units to call the permanent boot helper. The native verifier
selects the exact allowed unit text from the authenticated format marker;
correctly signed units from another layout are still refused. Version3 retains
the paired-boot contract and cannot be combined with `--legacy-boot`. The opt-in
`--origin-recovery` format `AEPDR004` retains that layout and additionally attests
permanent boot-helper support for version2 upgrade capsule cleanup. It requires
`--managed-startup`; older managed releases are not implicitly upgraded to it.
The verifier also accepts preserved `AEPDR001` legacy releases;
`--legacy-boot` is only for packaging a boot verifier that predates that
contract. The header sizes and eight member layouts are unchanged. Altering
the contract marker without resigning, or naming an unknown version, is refused.

The native verifier checks the approved whole-release BLAKE2b-256 digest as
well as the signature and each record digest. It computes approval over the
same private bytes used for semantic validation, rather than approving one
read and interpreting a later mutable read. Executable architecture must match.
Unit text must exactly equal the compiled definitions produced by `units.py`;
a signed extra service command is refused. Extraction rechecks copied hashes
and authenticates and fully validates the inner compressed payload before
publishing the staging directory.

`build.py --stage-release` builds `pd-stage-release`. This temporary entry takes
no arguments and requires host root. Stdin is the regular release file; fd3 is
a root-owned, single-link, mode-0600 regular approval record of exactly140bytes:
`PDAPRV02`, profile uint32 big-endian, four reserved zero bytes, the approved
32-byte digest, a zero-padded28-byte firmware string and a zero-padded64-byte
radio serial. Empty or unterminated fields and nonzero padding are refused.
The authenticated bootstrap must supply that record from the operator-approved
plan. `approval.py` encodes the record but confers no installation authority.

The entry publishes only `/run/aetherpd-install`, through a private0700
`aetherpd-install.stage` directory. Existing destinations are refused. A failure
after publishing but before the final directory sync does not destroy the
published files; the outer transaction must inspect state before retrying.
There are no persistent mutations, service commands, payload execution, sockets,
RF commands or calibration actions in this entry. The bootstrap removes it and
its staging directory when its transaction is over.

Profile matching here compares the signed manifest with the approved context;
it does **not** independently identify the running radio or firmware. That live
admission, persistent installation, service retirement, predecessor identity
migration, interrupted-install recovery and operator UI remain separate work.
The generated plan lists the intended full installation actions but labels
itself an artifact-only plan, not an executable installer.

Host tests build with `--release-test` and run `test_release.py --checker PATH`.
Twelve cases cover both architecture profiles, approved-digest substitution,
wrong keys, modified signed unit text, incompatible helper ELF headers, bounds,
inner-payload damage, output symlinks and source changes after approval. The
8600 native staging proof verifies all eight real files, rejects unsafe approval
permissions, a mismatched digest, extra arguments and duplicate staging, and
removes all test files. The candidate release is8,814,902bytes and was assembled
twice with identical bytes. This development evidence uses disposable keys and
does not constitute production release signing or FLEX-6300 qualification.

## Persistent preparation

`build.py --install-release` builds the temporary `pd-install-release` entry.
It accepts no arguments and consumes the same release and protected approval
record as staging. It authenticates the release again and issues only
`C970201|info` to127.0.0.1:4992. The bounded reply parser requires unique model,
firmware and serial fields; the observed model, firmware and serial must match the
signed profile and approved context before persistent writes.
An approval for another radio of the same model and firmware is refused.
`--approval-test` and `test_approval.py` exercise that native admission and
the protected-record boundary without a radio or network simulator.
Seven host and sanitizer tests cover this boundary, including a serial-check
bypass mutation. A bounded8600 test refused the wrong serial before persistent
writes, rejected obsolete records and wrong ownership/mode, then prepared the
correct target in0.97s and retried without replacing unit files. All test files
were removed; no service activation, predecessor change, reboot or RF occurred.

The prepared release's fixed files are copied through exclusive temporary files
and checked against the authenticated manifest. A fresh protected store is
assembled at `.aetherpd-prepare`, with a release-specific intent record, before
atomic publication as `aetherpd`. Payload selection uses the existing store
transaction. Ordinary state belongs to UID78666/GID20, mode0700; its parent is
traversable but not writable by that user. The updater and installer are not
copied into the persistent store.

Only after publication are the four signed unit definitions copied atomically
under `/etc/systemd/system`. They must be readable before the payload partition
mounts; symlinks into that partition cannot provide early boot discovery.
Preparation never enables units, reloads the service
manager, starts a service or changes the existing shim. On an existing recognized
installation it preserves activation state; the receipt reports
`activation_changed=false`, not an assertion that the machine was disabled.
The complete runtime must be retired before preparation; the absence of the RAM
tree is checked again while holding the same store lock used by boot.

The session socket starts after the payload preparation service, which runs after
`basic.target`. Its default socket dependencies are disabled to avoid an ordering
cycle through `sockets.target`; explicit shutdown ordering and conflict remain.
`test_units.py` checks both platform graphs with systemd without executing them.
It checks diagnostic output as well as the exit code: systemd can report success
after deleting a required startup job to break a cycle.

Retry admits only the same pending release, or a completed store bearing the
expected installer marker and trust key. Existing bootstrap files and unit
text must match the authenticated release; this component does not silently
replace an incompatible bootstrap ABI or rotate trust keys. Partial copies,
selector transactions and unit-file publication can resume. An empty state
directory left root-owned by an interrupted chown is repaired; existing ordinary
state contents are preserved. Foreign stores, writable directories, unknown
pending contents, conflicting unit definitions and dangling initial enable links
are refused. None are overwritten or deleted to make installation proceed.

Thirteen host tests cover preparation, idempotency, six process-interruption points,
partial-file mode changes, foreign stores/units, symlinks and dangling activation
links, plus unit availability before the payload mount and changed installed
definitions. The earlier native ARM64 preparation proof used live8600 identity
admission, prepared in1.04s,
repeated without replacing installed file inodes, repaired the interrupted state
ownership case and refused a deliberately conflicting test unit. Prepared disk
usage was8,736,350bytes. The test installation was removed without activation,
service changes or RF. That proof used the superseded unit-symlink layout and
does not qualify the corrected publication path. ARMv7 compiles; actual6300
execution is still required.

The remaining activation transaction must preserve the predecessor's identity,
stop only the recognized old primary, resolve port48992 ownership, and enable
and start the new ordinary-user service with recovery on interruption. Persistent
preparation alone does not qualify that transaction. A controlled8600 software
reboot with the corrected publication and ordering brought the primary up at
24.53s uptime, retaining its Tailscale address, DNS name and existing admin
credential. Post-reboot receive-only Start,37 registered status checks and Stop
passed with FDX disabled. All13 observed AetherPD processes were non-root; the
primary and DSP had no capabilities, and only the two fixed ARP owners retained
NET_ADMIN. This used a temporary bench action to stop the exact retained legacy
container; it is not the shipping migration transaction. The test installation
was removed afterward. It does not qualify sudden power loss, RF correction
after reboot, full diagnostics-bridge removal or actual6300 hardware. Signed
bootstrap delivery, bounded corrupt-slot repair and full firmware/platform
qualification also remain release requirements.

## Identity-transfer component

`pd_state_transfer` is an internal installer transaction, not a privileged
request interface. Its caller must authenticate the release and exact radio,
admit the existing installation, stop its writer, and supply read-only
descriptors for only `provision.json` and `tsnet/tailscaled.state`. The component
does not discover or stop a predecessor and is not yet wired into activation.
The original two files are preserved. Each must be a single-link mode0600
regular file of at most64KiB, with the profile's expected owner. A changing
source, foreign staging contents, or a nonempty destination is refused.

A protected staging directory receives both durable copies and their hashes.
Its journal binds the same140-byte release/target approval as preparation.
After publication of that journal, recovery needs no source descriptors.
The verified directory atomically replaces only an empty state directory;
a completion marker becomes durable before ownership passes to UID78666/GID20.
The parent directory is transferred last, keeping partially transferred children
inaccessible to the waveform user. A completed transfer preserves subsequent
ordinary identity changes instead of reimporting the old files. The common store
lock and absence of the runtime directory guard the transaction; the caller
must still prevent the predecessor from restarting during migration.

`build.py --state-test` builds a test adapter that is never packaged for release.
`test_state_transfer.py --checker PATH` exercises17 cases, including12 process
interruption points, corrupt copies, symlinks, hardlinks, unsafe permissions,
target substitution, lock contention and both source-owner profiles. Host and
ASan/UBSan/LeakSanitizer runs skip the distinct-owner case. ARM64 and ARMv7
adapters each passed all17 cases on the8600 with production UID/GID checks and
synthetic files in a temporary directory. Removing the copy-hash comparison
makes the corruption test fail. No existing identity, service, network or RF
state is changed by this fixture. These process-interruption tests do not prove
sudden-power-loss durability or actual Linux2.6/Cortex-A8 behavior.

## Rollback identity preparation and reconstruction

`pd_state_restore_legacy` uses the same atomic two-file transaction in the
reverse direction for the 8000 profile. Its inputs must belong to UID 78666;
its published tree belongs to UID 0/GID 0, with mode 0700 directories and mode 0600
files. It requires a separate, owned rollback store and an empty destination.
The caller must quiesce the new runtime before supplying the descriptors.
The reverse journal uses `PDROLL01`; a forward `PDSTATE1` journal cannot be
replayed as a restore or vice versa. Completed retries preserve later changes
to the restored identity and sync the journal directory before returning.
The 6000 profile is refused by this container-specific reverse operation.

Both directions pass 17 host cases and their sanitizer runs, with the distinct
UID case skipped on the host. Both ARM64 and ARMv7 adapters passed all 17 cases
per direction on the 8600 with real ownership checks, including 12 interruption
points. Bypassing the direction check makes its regression fail. The 24 migration
regression tests also pass. This tests the atomic data operation; it does not
implement container creation or a privileged request interface.

Two bounded 8600 reconstruction probes retained the original container. The
first created a same-image fixture and restored its writable-layer identity;
it served the same hostname, Tailscale address and DNS name in 2.48 seconds while
the original stayed stopped. The second used native forward transfer followed
by native reverse preparation, then created the fixture with exactly one
private identity-directory bind. That fixture served the same identity in
2.30 seconds. The original radio-address environment was preserved. Launch
configuration matched the original except the explicit fixture name/hostname,
disabled automatic restart, and the second fixture's single private bind.

After each probe, the fixture was stopped before reading its final identity
files and copying them back to the original. Both fixtures, private data copies,
mounts and temporary host jobs were removed, and the original resumed with the
same identity. The two identity files did not change during the second short
run, so this does not prove recovery from actual live key rotation. Subsequent
identity edits are covered by the synthetic data tests. No RF or reboot was
performed, and the original container was never deleted.

The intended production rollback layout prepares its entire private identity
tree before creating an autostarting container. This avoids exposing a partly
copied identity if startup follows a power interruption. Podman 4.3's
[update command](https://docs.podman.io/en/v4.3/markdown/podman-update.1.html)
only changes cgroup configuration; a restart-disabled container cannot simply
be switched to automatic restart with that command. The private bind contains
identity data only, not the host root or a command socket.

The fixed native retirement/reconstruction adapters below still require
worker integration and actual interrupted-create/autostart qualification.
The reconstruction probes used a bounded
Python harness and restart-disabled fixtures; they are not a shipping installer.

## Recovery recipe and restored-container metadata

Before stopping the predecessor, `pd_migration_copy` durably publishes a
root-owned, mode 0600, single-link `recovery-recipe`. Its `PDREC001` record is
332 bytes: an eight-byte tag, the exact 308-byte migration approval, and the
original canonical IPv4 radio address in a zero-padded 16-byte field. A changed
address or different approval cannot rewrite an existing recipe. Partial writes
can be resumed only when they match the intended record. Publication syncs the
file, renames it, then syncs the directory; a completed retry syncs the directory
again before allowing the stop operation.

The recipe derives a deterministic container hostname from its BLAKE2b digest.
This identifies the intended reconstructed configuration after a process dies
during creation; it is not authentication. The root-protected recipe and the
caller's authenticated target/release approval remain required.

`pd_predecessor_parse_restored` is separate from original-container admission.
It requires the approved image, a different container ID, the derived hostname
in both Config and Env, the original radio address, and exactly one bind from
`/var/lib/smartsdr/aetherpd/legacy/state` to `/var/lib/flex-tailnet-shim`.
The bind must be writable with private propagation and the fixed `nosuid,rbind`
options. The original namespace, capability, label, executable-path and
environment restrictions still apply. Original admission continues to refuse
all mounts. These checks admit metadata only: the retirement/reconstruction
adapter must verify executable content, state ownership and transaction state
before starting or adopting a container.

The 32 migration tests include process kills before and after recipe rename,
changed-address refusal, unsafe metadata and foreign/partial record handling.
The 12 recovery tests and 11 original-inventory tests pass on the host and under
sanitizers; the latter two suites also pass as ARM64 and ARMv7 binaries on the
8600. Removing the approval binding or skipping recipe preparation makes its
respective regression fail. A never-started fixture confirmed actual Podman
format compatibility. It was correctly refused as a production predecessor;
acceptance was checked only after normalizing its test name, source path and
restart policy in the captured text. That is parser qualification, not a live
creation/adoption or reboot test.

A bounded live copy test used the native helper with the existing signed
development release. It verified the persisted 332-byte recipe against both
the migration approval and the actual original radio address. A deliberate
copy refusal recovered the original shim; the subsequent successful copy
preserved both identity files exactly (2,645 bytes). The original container was
retained and restarted with the same identity. Disabled test units, state,
staging files and temporary host jobs were removed. This did not retire the
original, activate the new primary, reboot or transmit.

## Fixed retirement and reconstruction adapters

`pd_predecessor_retire` and `pd_predecessor_restore` are linked native
transaction operations, not client request handlers. They require root, the
canonical store, a matching recovery recipe, the precise transaction phase,
an exclusive migration lock and absence of the new runtime. Their caller must
hold the transaction lock, authenticate the target/release and stop the new
process graph before invoking rollback.

Image admission is read-only. The supported 0.5.2 image has one overlay layer;
the helper validates the full image configuration, image/layer IDs, fixed
storage paths and protected directory ancestry. It hashes the regular,
single-link executable directly in that layer and checks the image inventory
again afterward. No image mount or temporary container is needed. A native
8600 probe verified the real retained image and refused a wrong executable
hash and an ordinary UID without changing the running shim.

Container verification also reads the protected upper layer directly and
requires its sole lower layer to be the verified image. It refuses redirected
or opaque upper roots, whiteouts, symlinks and altered executables. If the
upper layer has no replacement executable, the verified image supplies it.
Podman omits `MergedDir` when stopped; that form is accepted only with exactly
the three remaining expected graph fields and an empty merged-path field.
Neither verification nor recovery adds or removes mount references. This
avoids the restart race inherent in a separate stopped check followed by
[forced unmount](https://docs.podman.io/en/v4.3/markdown/podman-unmount.1.html).

Retirement requires the `RETIRING` phase and a completed forward identity
transfer. It verifies the stopped original executable, removes exactly its
approved container ID without force, and verifies that the rollback image remains.
It does not remove the image or an unrelated container.

Reconstruction requires `ROLLBACK` and absence of the original container ID.
It publishes the latest quiesced identity into the separate root-owned legacy
tree before creating a container. Creation uses fixed arguments, the approved
image, the original radio address, a recipe-derived hostname, one private
identity bind and automatic restart. On retry, a matching existing container
is inspected and verified; a foreign configuration is preserved and refused.
A stopped container's executable is verified before starting its exact ID.
A running container is verified without another start. Completed identity
preparation preserves later legacy identity edits across retries.

The adapter return value verifies the running executable, not HTTP/Tailscale
readiness. The independent worker must perform that readiness check before
marking rollback complete, and must retain temporary recovery authority until
the terminal journal is durable.

The 31 adapter tests use real filesystem journals/state and an injected Podman
resource model. They include process death after removal, creation
and start; creation timeouts with committed side effects; latest-identity
preservation; exact command arguments; wrong-image and foreign-container
refusals; transaction/lock/runtime fences; direct-layer path/whiteout/redirect
checks; mounted/stopped graph metadata forms; and preservation of preexisting
mount references.
They pass under ASan/UBSan/leak detection. Removing image verification or the
exact phase check makes its regression fail. The 32 migration, 16 transaction,
12 recovery and 11 original-parser regressions pass. These are component tests:
the real adapters have not yet retired or reconstructed the radio's original
container. Worker integration, actual interruption/reboot tests and final
bootstrap removal remain required.

## Legacy predecessor admission

`build.py --inspect-predecessor` builds the temporary, read-only
`pd-inspect-predecessor` entry for the8000. It accepts no arguments, requires
root and invokes only `/usr/bin/podman inspect` for `flex-tailnet-shim`, using a
fixed formatter, an8KiB output limit and an8-second child-command deadline.
Environment strings are hex-encoded before parsing. Admission currently covers
the observed0.5.2 configuration only: exact entrypoint, no arguments or mounts,
host networking, private PID namespace, expected dropped capabilities and a
small environment allow-list. Foreign names, changed launch configuration,
additional privileges and state-path overrides are refused. It compares the
running executable inode with the fixed image path, hashes that regular file,
then checks the container inventory again for changes.

The resulting container ID, image ID and executable BLAKE2b digest are an
observation, not an authorization or proof that the old image came from a
signed release. The installation controller still must bind that observation
to operator approval and revalidate it before mutation. This entry does not
stop or remove anything, copy identity files, install services, or grant an
arbitrary Podman request interface. The ARMv7 entry explicitly refuses: native
6000 waveforms require a separate adapter and hardware qualification.

Eleven socket-free parser tests and their ASan/UBSan/LeakSanitizer runs pass.
Removing the privileged-container refusal makes its regression fail. The native
8600 probe completed in4.23s; extra arguments, non-root invocation and the ARMv7
container entry were refused. A separate disposable-container experiment on
Podman4.3.1 removed a never-started, network-disabled fixture, confirmed the
image remained, recreated the fixture and restored two synthetic identity
files across unmount/remount. The real shim retained its PID, and all fixture
containers, mounts and backup files were removed. This establishes the storage
mechanism only; running rollback, interruption recovery across retirement and
activation, and any real predecessor removal remain unqualified.

## Native migration begin

`build.py --copy-predecessor --public-key KEY` builds the temporary
`pd-copy-predecessor` entry. It accepts no arguments. Standard input supplies
the signed release, fd3 the existing target approval, and fd4 a protected
308-byte `PDMIG001` record. That record contains the identical target approval,
the exact predecessor container and image IDs, and its executable BLAKE2b digest.
`migration.py` encodes it after consent; the record is not itself a signature.
The trusted bootstrap still owns authenticating its own delivery and presenting
the complete installation/recovery plan to the operator.

The native entry authenticates the release and live radio identity, and checks
the approved predecessor before persistent writes. It prepares the release
without activation, takes a migration lock, and durably records that approval.
It pins the predecessor's root and state directories, verifies the same process
and executable again, and stops only that exact container with a fixed command.
State files are opened after the writer exits so final atomic file replacements
are included. The identity-transfer transaction publishes the copies. Both
source files must still have the expected owner, mode, type, size and link count
on retries, and the selected copies must match the current source bytes. An
older snapshot is never silently substituted after the old shim has restarted
and changed its state.

Success leaves the predecessor stopped and the copy ready, but does not remove
the container, activate units, start AetherPD or transmit. A returned failure
after attempting stop tries to restart only the same admitted predecessor and
verifies its executable and running state. A conflicting AetherPD runtime fences
that recovery; an unverified recovery has a distinct exit code7. Child Podman
commands use fixed arguments, an empty inherited descriptor set beyond stdio,
bounded output/deadlines, and a parent-death signal. The bootstrap must invoke
the entry in the host cgroup namespace when recovery may start a container.

Twenty-four socket-free lifecycle/record tests and their ASan/UBSan/LeakSanitizer
runs pass. They inject predecessor lifecycle operations while using real file
copy/journal code. They cover post-stop atomic rename, exact approval binding,
source permission changes on retry, stale snapshots, returned failures,
failed recovery and runtime exclusion. A deliberately bypassed snapshot check
is caught. Ten predecessor parser tests cover running/stopped consistency.

The final ARM64 entry passed a bounded 8600 integration test: invalid record
ownership/mode, arguments, ARMv7 use and a wrong executable digest were refused;
the wrong predecessor was rejected before persistent changes. A deliberate
occupied-destination failure restarted the original shim with its existing
identity. The successful path copied all 2,645 bytes exactly to UID78666/GID20
and left the original container stopped. The harness then removed the test
installation and restarted the original shim with the same hostname, Tailscale
address and DNS name. No container removal, service activation, reboot or RF
was performed. The initial harness attempt was refused by the old shim's
LAN-only HTTP policy before any installation or stop; using its normal LAN
status endpoint resolved that harness error without changing access controls.

This is a begin operation for the future local transaction controller, not a
standalone operator installation workflow. Recovery after process death or
reboot for the complete transaction, retirement/activation and temporary-authority
removal remain unfinished. The recovery component and disposable-fixture
observations below constrain that
transaction; they do not qualify removal of the real predecessor.
The full transaction must continue
locally when stopping the old shim interrupts its remote control connection.
ARMv7 explicitly refuses this container operation; native 6000 waveform or
fresh-install admission requires its own implementation and qualification.

## Recovery of the stopped predecessor

`build.py --recover-predecessor --public-key KEY` builds a temporary ARM64
entry with the same signed-release stdin, target approval on fd3 and migration
approval on fd4. It accepts no arguments and requires root. It authenticates
the release and live radio, then requires an existing protected store, migration
lock, byte-identical durable `migration-approved` journal, recovery recipe and
expected public-identity record. It never creates
an approval journal or prepares an installation. An existing AetherPD runtime
fences recovery. ARMv7 explicitly refuses this container-specific operation.

Recovery re-admits only the fixed predecessor name, approved container/image
IDs and launch configuration. A stopped predecessor uses the direct layer
verifier described above; it performs no mount or unmount operation. Every
path component is opened without following symlinks and must be root-owned
and protected from ordinary writes. The fixed executable must match the
approved digest before restart. Migration checks the retained image before
stopping the original, so an unsupported image is refused while it still runs.

The helper rechecks that the container remains stopped and AetherPD remains
absent before restart. Successful recovery verifies the running executable
again; an already-running matching shim is verified without restarting it.
It also waits for the matching ready HTTP identity described below before success.
Exit 0 means verified recovery, 6 means refusal before attempting recovery,
and 7 means recovery could not be verified. Prior mount-based fixture results
remain in the saved development reports; they do not qualify this direct-reader
implementation's live behavior. The direct-reader helper has now passed a
live ARM64 copy-and-recovery test on the FLEX-8600: after copying 2645 bytes
of identity and leaving the original stopped, a fresh authenticated process
recovered it in 18.26 seconds. The public Tailscale identity was unchanged.
The forced copy-failure path also recovered the original, and temporary units,
state and the host job were removed. This test did not remove the original
container, activate AetherPD or reboot the radio.

The lifecycle suite includes recovery journal equality, metadata, lock and
runtime checks; bypassing journal equality makes its regression fail. The
parser suite also retains fixed mount-path boundary coverage. These are component and live
process-death tests, not an autonomous boot-recovery controller. The caller
must still survive loss of its remote connection, reconcile identity changes
after rollback, and complete or recover retirement/activation after reboot.

## Public identity and readiness

`pd_readiness` reads only the shim's public `GET /v1/status` endpoint. Ready means
provisioned, `running`, not claimable, and zero relay sessions, with a valid
hostname, canonical Tailscale IPv4 address and DNS name. Required duplicate keys,
missing fields, malformed values, excessive nesting and oversized responses refuse.
Unknown extensions are skipped within bounded parsing limits. The HTTP client uses
port48992, an HTTP/1.0 close request, no credentials, no redirects and no DNS lookup.
It accepts only a canonical private IPv4 address assigned to a local interface,
excluding the shim's internal container network. An individual probe has a five
second deadline; readiness retry shares a fifteen second deadline.

The protected 652-byte `PDREADY1` record in `expected-identity` binds the exact
migration approval to hostname, Tailscale address and DNS name. It contains no
private identity or token. Publication uses file sync, atomic rename and directory
sync; matching partial prefixes resume. Foreign records, unsafe ownership/mode,
links and inconsistent partial state are retained and refused. The caller holds
the migration lease. Copy records this expectation before stopping the old shim;
fresh recovery requires it and waits for the same public identity after verifying
the approved running executable. An executable that runs with a different or
unready public identity does not satisfy recovery.

The firmware API address in the predecessor's configuration is not the LAN HTTP
address. On the tested8600, `SSDR_RADIO_ADDRESS` is `172.30.2.1`; the observed
`info` reply reports `ip=192.168.50.100`. Migration rechecks model, serial and
firmware in that reply, then uses its canonical LAN address for HTTP. The original
internal address remains in the rollback recipe and container configuration.
Missing or invalid LAN information refuses the operation. FlexLib's `GetInfo` /
`UpdateInfo` define the command/reply framing; the LAN field is grounded in the
captured8600 reply. Older identity-only callers still accept an absent `ip` field.

Eighteen host and sanitizer tests cover JSON/HTTP bounds, readiness, exact record
binding, metadata, partial publication and process death. The migration suite has
36 host and sanitizer cases, including pre-stop identity refusal and recovery that
rejects a running process with the wrong public identity. Four mutations removing
the running-state check, migration binding, immutable expectation or recovery
identity comparison are caught. Identity parser tests cover canonical and invalid
LAN addresses; the 31 retirement/reconstruction regression cases still pass.

ARM64 and ARMv7 native probes on the8600 matched an independent Python status
client, recovered interrupted record publication and refused ordinary-user-owned
records. The final ARM64 copy/recovery integration refused a deliberately wrong
identity without stopping the original PID, then copied2645 bytes of private state
and retained the652-byte protected expectation. Fresh native recovery verified the
original executable and matching ready public identity in18.27seconds. The
temporary disabled installation and host job were removed; no container removal,
AetherPD activation, reboot or RF occurred. The earlier failed integration used
the internal address for the LAN-only endpoint and safely refused; that result is
retained alongside the corrected proof.

These checks do not authenticate an HTTP responder by themselves. The full worker
must additionally prove the new primary's executable, UID/capabilities, service
membership and listener ownership, and bind those observations across its readiness
check. The process and fixed-service components below now supply those checks
and have admitted the actual signed primary on the8600. Complete worker wiring
remains incomplete. Owned-runtime quiescence now has the receive-only native
proof described below. This is component and predecessor-recovery evidence, not
a completed production installer or6300 hardware qualification.

## Primary process and listener verification

`pd_primary` is a linked worker component, not a privileged command interface.
Its caller must obtain MainPID from the verified fixed `aetherpd-shim.service`
and supply an already authenticated protected runtime executable descriptor.
The component pins the process directory and executable, then verifies matching
executable device/inode, host PID/network namespaces, exact service cgroup,
UID78666/GID20 in all four credential fields, empty supplementary groups, zero
capability sets, SCHED_OTHER and nice10. On kernels supporting the corresponding
features, NoNewPrivs and ambient-capability fields are required. Older kernels may
omit those fields; the other credential and capability checks remain mandatory.

Bounded `/proc/net/tcp` and `tcp6` parsing requires one wildcard listener on48992,
owned by UID78666, with its socket inode present in the primary process's own
file descriptors. A listener held only by another process fails admission. The
process start tick and listener inode must remain the same on subsequent checks.
`pd_primary_wait` checks before and after matching public HTTP readiness. These
are kernel metadata snapshots around the request, not cryptographic authentication
of the HTTP connection or an atomic proof across the interval. The caller must
also recheck the verified service MainPID before committing. The kernel documents
[process metadata and pinned process descriptors](https://docs.kernel.org/filesystems/proc.html)
and the [TCP table fields](https://docs.kernel.org/networking/proc_net_tcp.html).

Fifteen host tests and the same sanitizer tests cover metadata, legacy field
omissions, capability/credential refusals, scheduler/start-time parsing, service
membership, duplicate listeners and malformed or oversized tables. Native ARM64
and ARMv7 checkers passed twelve cases on the8600 against an inert ordinary-user
listener in the actual fixed service cgroup: admission, matching HTTP readiness,
different executable inode refusal, closed listener refusal, process death
refusal, and refusal when a child alone owns the listener. The temporary service
was removed and the original shim restarted with the same public identity.
No RF, reboot, persistent installation or original-container removal occurred.

The fixture reused the primary launcher's privilege drop but did not run the Go
shim. Signature authentication of the expected executable, loaded-unit validation,
MainPID acquisition, whole-graph quiescence and actual worker integration remain
caller responsibilities and are not qualified by these tests. ARMv7 execution on
the8600 does not qualify the6300. Both TCP tables are currently required; a kernel
without IPv6 needs explicit compatibility work rather than silently skipping a
failed table read.

## Fixed loaded-service admission

`pd_service` adds an internal fixed-unit adapter for the boot service, local start
socket and ordinary-user primary. It compares all four installed unit definitions
(including the session template) to the compiled definitions, walking the canonical
unit directory without following links and requiring root-owned single-link0644
files. Its bounded systemctl subprocess uses fixed arguments and a clean environment;
no caller supplies commands, unit names or executable paths.

Loaded admission checks exact identity, fragment path, absence of aliases and
reported overrides, no pending daemon reload, and fixed executable/argument lists.
It rejects additional start/stop/reload commands, environment or filesystem-root
changes, unexpected service types, and inconsistent PID/state/cgroup values.
The socket must retain its fixed path, per-connection service behavior and fixed
owner-setting command. Empty command-array properties may be omitted by systemctl;
missing required scalar properties refuse. The format is grounded in the observed
systemd252 output on the8600. Other manager versions, including the6300, remain
unqualified. The
[systemctl documentation](https://www.freedesktop.org/software/systemd/man/latest/systemctl.html)
and [unit lookup rules](https://www.freedesktop.org/software/systemd/man/latest/systemd.unit.html)
explain loaded properties and override precedence.

The activation path requires an ACTIVATING transaction. Runtime preparation starts
only the fixed boot unit; the caller then authenticates the extracted runtime and
opens the expected primary executable. Primary activation verifies the definitions,
starts the fixed primary service, checks matching unprivileged process and public
readiness, then rechecks boot/socket activity, primary MainPID and transaction phase.
The caller retains the installation lease and authenticates the release and target.
These activation entry points have been exercised by the bounded native harness,
but are not yet wired into the complete migration worker. The primary-stop
operation verifies its loaded definition and waits for an inactive result. The
separate quiescence operation below handles session instances and surviving
runtime processes.

Fifteen socket-free host tests and the same sanitizer tests cover fixed definitions,
overrides, command/argument changes, metadata bounds, file ownership modes and
links, command refusal and inactive stop postconditions. Three mutations removing
loaded-override refusal, exact executable arguments or unit content comparison are
caught. The first content mutation exposed a weak test that changed only length;
the strengthened case also changes bytes without changing file size.

Twelve native ARM64 admission cases passed on the8600 with the exact inactive unit
definitions. The adapter accepted the loaded definitions, refused an executable
override and a changed definition, then accepted the restored definitions. The
original shim retained the same running PID; temporary definitions and the root
job were removed. This did not start AetherPD, remove the original container,
transmit or reboot.

A subsequent ARM64 native run used the actual signed development shim. Preparation
refused a RETIRING transaction before creating the runtime, then accepted
ACTIVATING while the harness held the exclusive transaction lease. The harness
verified the selected bundle signature, selected digest and every extracted
runtime member before supplying the primary executable descriptor. Preparation
completed in23.78seconds and activation with process/public-identity admission in
20.73seconds, including the adapter's systemctl checks. The admitted primary ran
as UID78666/GID20, with empty supplementary groups, zero capability sets and
NoNewPrivs1. Native active stop succeeded in5.76seconds.

The harness stopped the socket/boot units and checked that no executable under
`/run/aetherpd` remained before recovering the original container. Both private
identity files were unchanged, and the original returned with the same public
identity. The temporary installation and host job were removed. The harness has a
copy-back branch for changed private state, but that branch was not exercised by
this run. This was the earlier signed development bundle with the current
activation adapter, not a rebuilt final package. No RF, reboot or container removal
occurred. The complete migration worker, changed identity-state rollback,
independent recovery and reboot qualification remain.

## Owned session quiescence

`pd_service_quiesce` first verifies the installed definitions, loaded intake units
and session inventory. It stops the fixed primary and socket, then inventories
sessions again to include any connection accepted while intake was closing. Each
instance must have the observed numeric connection/PID/UID78666 name, the exact
session template, executable, socket input and cgroup, with no alias or override.
Only those verified exact names enter the stop command; no wildcard is used to
stop sessions. The boot service is stopped last. Every remaining loaded instance
must report inactive or failed with no main PID.

A final bounded process scan requires the host PID, network and cgroup namespaces
and an actual proc filesystem. It refuses success for any process in an owned
service cgroup (including descendants) or executing a file under `/run/aetherpd/`.
The executable check also catches an owned runtime that escaped its expected
cgroup. It never kills a process by path or PID. Unreadable or malformed metadata
refuses cleanup. Inventory is capped at32 instances and64KiB; process inspection
is capped at16384 processes and five seconds. These limits fail closed.
The current canonical service/cgroup layout is qualified on the8600 only.

Twenty-eight host and sanitizer tests cover the existing service checks plus
instance grammar, duplicate/oversized inventories, template and command overrides,
cgroup boundaries, executable survivors, stop ordering, and intake still active.
Mutations bypassing the final survivor scan, owned-cgroup detection, session
executable check or pre-session template check are caught.

Loaded admission also probes a fixed inactive template instance before any real
session exists. This loads but never starts the template, exposing template-level
overrides that checking only running instances would miss. A separate16-case
native test of the final source accepted the clean template, refused a controlled
executable override before the first session, and accepted it after removal. It
also passed inactive quiescence; the original shim retained its running PID.

Before the added pre-session template guard, the ARM64 native adapter was
exercised against a real13-process receive-only session using the existing desktop credential and GUI client identity. The
session registered with local DAX IQ and FDX off; it never keyed or corrected.
Native quiescence succeeded in24.10seconds including systemctl verification.
Independent bench checks found no runtime executable remaining and confirmed the
session-owned fixed ARP entry was removed. The original shim resumed with the same
public identity; private identity files were unchanged. Test units, state, payload,
coordination listener and host job were removed. That tested executable was
reproduced byte-for-byte with the preserved pre-guard source overlay; the final
source has the separate native template-admission proof above.

This run used the earlier signed development runtime and the current native
adapter. The complete worker is not wired, and the final signed package has not
been rebuilt. The native operation verifies process/service quiescence, not every
possible network side effect: the ARP assertion was an independent bench check.
Forced hung-process cleanup, changed-state rollback, final-candidate failure/unkey
coverage and actual6300 qualification remain separate gates.

## Local installation transaction driver

`pd_transaction.c` implements the durable ordering policy for the future local
installer. Its 320-byte `PDTXN001` journal binds the identical 308-byte migration
approval to one phase. The protected store, journal, partial record and lock
are checked before actions run. Journal publication uses file sync, atomic
rename and directory sync. A matching interrupted partial record can be
discarded; a foreign record, unsafe metadata or unknown phase is preserved
and refused.

The forward sequence records intent before preparation, final identity copy,
predecessor retirement and activation. The activation adapter must verify the
new service's identity and readiness before returning success. Only then is
commitment recorded and temporary recovery authority removed. The adapters
are linked native functions, not commands or paths supplied by a client.

A new process finding a published pre-commit phase rolls back instead of
blindly repeating destructive actions. The rollback adapter must inspect real
resources, quiesce the owned new runtime and preserve its latest identity
before restoring the approved predecessor. After a committed or rolled-back
record, re-entry only finishes cleanup. It syncs the directory again before
cleanup, because the previous process may have died after the record's rename
but before its directory sync. Cleanup failure never turns a committed
installation into a rollback.

`pd_transaction_recover` is the explicit boot-recovery entry. It accepts only
rollback and cleanup callbacks and never takes a forward step, including when
no journal has been published or only the initial partial record exists.

Twenty-one host tests and the ASan/UBSan/LeakSanitizer run pass. They kill child
processes after actions and at journal publication boundaries, retry from a
separate process, and check resource ownership and simulated identity updates.
They also cover partial commit records, foreign/unsafe records, locking,
failed rollback and cleanup, and a real failing `fsync` after closing the test
store descriptor. Removing the committed-phase distinction is caught by a
regression test. These tests use a separate resource model; they do not qualify
actual container retirement, service activation or radio rollback. ARM64 and
ARMv7 test harnesses build, but were not executed on a radio in this checkpoint.
The migration approval still admits the 8000 container profile only.

There is no deployable transaction worker yet. Fixed temporary unit definitions,
publication and boot dispatch exist as tested components. The
remaining integration must provide:

- Exact container retirement while retaining the approved rollback image,
  and reconstruction of the admitted predecessor from that image and the
  freshest quiesced identity. The observed image-only firmware-removal behavior
  means a successful API reply cannot stand in for inventory verification.
- A trusted, independently launched local worker and fixed boot-recovery unit,
  armed durably before stopping the old shim. Temporary release/approval files
  and the worker must remain disk-bounded and be removed after completion.
- Real preparation, copy, retirement, activation/readiness, rollback and cleanup
  adapters. The boot/primary and session gates below are wired into their native
  entries; their activation exception still needs the independent worker.
- Qualification after installer death, lost desktop connection and reboot at
  the retirement/activation boundaries, followed by the separate 6000 adapter.

## Installation gates for boot and sessions

`pd_transaction_session_lock` accepts only a valid `COMMITTED` transaction whose
approval matches the protected `migration-approved` record. It opens the existing
mode0600, root-owned, single-link transaction lock read-only and takes a shared
nonblocking lock. Missing, malformed, mismatched, writable or linked records,
unfinished publication, an exclusive installer lock, and any other phase refuse
admission. The check creates no records. It syncs the containing directory before
admitting a commit that might have been observed after a worker died.
Committed startup also refuses while the sibling `aetherpd-installation` exists,
including a dangling symlink. This prevents cleanup failure from admitting a
session while temporary executable authority remains. Controlled `ACTIVATING`
readiness still permits the worker's capsule to exist.

`pd-start-session` performs this check after peer/request admission and before
the constructor can configure IQ or scheduling. Its ordinary-user log collector
retains the read-only shared lock until the constructor exits; the constructor
and DSP do not inherit it. Installer transactions use the same exclusive lock,
so a session cannot race their actions. Maintenance must stop owned sessions
before acquiring that exclusive lock.

`pd-boot` and `pd-primary-launch` use the runtime gate. Normal startup also
requires commitment. For controlled readiness checks, the runtime gate accepts
`ACTIVATING` only while an exclusive installer lock is observed before and after
reading the matching journal. A stale activation record or another shared holder
does not qualify. This exception never admits a session. The worker must start
and await these units inside its activation callback; interrupted-installation
recovery must stop them before restoring the predecessor. The boot gate holds
its committed shared lock through payload preparation. The primary checks the
gate before dropping privileges and executing the ordinary control service.

Nineteen host tests and their ASan/UBSan/LeakSanitizer run cover phases, approval
binding, unsafe records, process death and actual shared/exclusive kernel locks.
Launcher tests verify that refusal never calls the constructor and that descriptor
cleanup preserves only the supervisor's read-only lease. Mutations of commitment,
worker-presence, lease retention and launcher admission checks each fail their regression. Native
ARM64 and ARMv7 gate binaries passed 44 checks on the 8600 with disposable records;
the original shim kept the same PID and the fixture was removed. ARMv7 execution
there does not qualify the 6300 kernel, and its waveform migration remains absent.

The actual ARM64 boot entry also passed a receive-only fixture using the fixed
store path, synthetic journals and no payload. It refused incomplete or absent
transactions before creating a runtime directory. The prior signed boot binary
created that directory for the same pending record, demonstrating the missing
guard. Committed and worker-held activation records reached the deliberately
missing-payload refusal; stale activation was rejected at the earlier gate.
All fixture files and directories were removed, with the original shim unchanged.

These gates are included in newly built native helpers, not in the earlier signed
development release. The release must be rebuilt with the independent worker.
The gate alone neither installs recovery authority nor completes its cleanup.

## Temporary installer inputs

`pd_capsule.c` provides the protected inputs needed by an independent worker.
These are linked native functions, not a command or path request interface.
The caller must authenticate the live target and predecessor and pass its own
trusted executable descriptor. Publication does not start a process, install a
unit, stop the predecessor or change the network.

The fixed `aetherpd-installation` directory contains exactly three files:

| File | Mode | Content |
| --- | --- | --- |
| `intent` | 0600 | 380-byte `PDCAP001`: trusted public key, worker BLAKE2b-256, exact migration approval |
| `worker` | 0700 | Copy of the trusted bootstrap/worker executable, bounded to 512 KiB |
| `release` | 0600 | Exact approved, signature-verified fixed-file release, bounded to 18 MiB |

The directory is root-owned mode0700; each file is root-owned and single-link.
The release signature is verified against the caller's pinned key and its exact
approved digest. The worker binding is a protected record of the trusted caller's
executable, not a substitute for authenticating that bootstrap's distribution.
The eventual worker entry must also call `pd_capsule_self` to match its executing
inode to the stored worker, then recheck live radio identity before operations.

Publication writes a private fixed staging directory, syncing each file before
rename and syncing the final directory and its parent. Matching interrupted
prefixes resume; foreign prefixes, links, unsafe permissions and unknown entries
are preserved and refused. Existing complete records are reauthenticated without
replacement. Free-space admission counts the remaining bytes of matching partial
files and preserves the 8 MiB reserve. A fresh process can reopen and verify all
inputs without the original upload files or file descriptors.

A root-owned mode0600 zero-length `.aetherpd-installation.lock` serializes writers
and workers. It remains as inert metadata after completion so the lock inode
cannot split between concurrent attempts. Ordinary waveform processes cannot
open it; it grants no execution or request access.

Retirement requires a matching durable `COMMITTED` or `ROLLED_BACK` transaction.
The capsule is renamed to the fixed retired directory before the release, worker
and intent are removed. Temporary units enter through the permanent boot helper,
so they can finish cleanup even after the temporary executable has been removed.
`pd_capsule_finish_cleanup` refuses an active capsule, reconciles retired files
and removes matching temporary unit files under one namespace lease. It never
starts a service. The caller releases its capsule lease before invoking it.
Interrupted removal resumes against the same terminal approval; unknown or unsafe
files are retained. A new publication refuses unreconciled retirement debris.

Twenty-five host tests and their ASan/UBSan/LeakSanitizer run cover signature/digest
binding, executable identity, namespace locking, space reserve, twelve publication
kill points, four retirement kill points and three unit-removal kill points.
They verify recovery stays armed until executable authority is detached and that
cleanup cannot remove units for an active capsule. Mutations removing release binding,
worker binding, terminal-phase enforcement or exact self identity fail their tests.
Native ARM64 and ARMv7 fixtures on the 8600 copied the real signed development
release, executed the copied test entry after deleting its source files, recovered
interrupted publication/retirement and refused premature cleanup. Their temporary
records occupied 8,898,698 and 8,910,778 bytes respectively. All fixtures and host
jobs were removed; the original shim kept the same PID, with no RF or reboot.

The native entry used for those checks is a test adapter. The combined native
worker is now a compiled development draft; its end-to-end qualification is
still outstanding (see below).
This version admits the 8000 container migration profile only; ARMv7 execution on
the 8600 does not qualify 6000 installation or its disk budget. The earlier signed
release was an input fixture, not a newly published runtime release.

## Temporary worker units and boot dispatch

`worker_units.py` defines two fixed FLEX-8000 services. The forward service is
never enabled at boot and has no ordering dependency before the runtime it must
start. Only the recovery service is enabled at `multi-user.target`; it waits for
firmware and Podman startup and orders before the AetherPD runtime. This avoids
having an unfinished oneshot wait for a service ordered after itself. These are
the [systemd oneshot ordering semantics](https://www.freedesktop.org/software/systemd/man/247/systemd.service.html).

Both services enter through the canonical `pd-boot`, with fixed
`--install-approved` or `--recover-installation` arguments. The helper authenticates
the stored capsule with its compiled key, pins the exact executable descriptor,
closes unrelated descriptors, supplies only a fixed PATH, and executes either
the forward worker or its fixed `--recover` mode. A busy namespace lease is a
retryable failure; an invalid capsule is refused. With no active capsule, the
entry only attempts matching terminal cleanup. It does not prepare a runtime.
The development worker independently verifies its executing inode, live radio
identity and service MainPID before invoking the transaction adapters.

`pd_worker_units` publishes exact root-owned files and the single recovery link
with atomic renames and directory sync. Matching interrupted prefixes resume;
foreign text, links, metadata, direct drop-ins and forward boot enablement refuse
before publication. Terminal cleanup removes only matching fixed files. Loaded
service-manager definitions and other drop-in locations still require verification
in the launch adapter; filesystem validation alone does not prove those properties.

Fifteen host and sanitizer tests cover the publisher and cleanup, including seven
publication and three removal kill points, plus an offline dependency graph check.
The graph check substitutes an inert executable and does not start a service.
Four mutations removing recovery-only behavior, the active-capsule startup gate,
the active-capsule cleanup guard and foreign-unit preservation are caught.

On the 8600, ARM64 and ARMv7 publisher fixtures passed in disposable directories.
The production ARM64 boot entry passed eleven dispatcher cases with an inert
worker: exact executable identity, fixed arguments/environment, descriptor
closure, busy-lock retry, tamper refusals and terminal cleanup after worker removal.
Three combined cleanup cases recovered from process death. Canonical fixture
paths and host jobs were removed; the original shim retained its PID. No actual
temporary service was enabled or started, and no RF or reboot occurred in these
checks. Power interruption and service-manager restart behavior remain unqualified.

## Firmware retirement observations

On FLEX-8600 firmware 4.3.16.42034, FlexLib's fixed
`waveform remove_container NAME` request is asynchronous. With a running
disposable waveform, it removed both the container and its installed image
and published the matching `removed` status. A successful reply alone is not
evidence that either resource has disappeared.

A second fixture was created through `waveform recreate NAME`, stopped and
removed by exact container ID while retaining its image. After one software
reboot, the image remained but firmware did not recreate its container or
advertise it in the waveform subscription. The radio returned with a changed
boot ID, AetherSDR reconnected in about 66 seconds, and the real shim retained
its hostname, Tailscale address and DNS name. FDX stayed disabled and no RF
commands were sent. This is evidence for this firmware and one soft reboot,
not a guarantee for other versions, cold power loss or the 6000 platform.

Removing that image-only fixture through the waveform API returned success
but left its image present throughout the 35-second observation. The test
assertion failed; cleanup then verified the fixture's image ID/name label and
removed only that image with Podman. All fixture resources and temporary
host jobs were removed, and the real shim remained running. An installation
controller must reconcile actual inventory after asynchronous replies and
distinguish a retained rollback image from completed image retirement. Full
predecessor retirement, running rollback and installer-death recovery remain
unqualified.


## Combined installer worker — development draft

`pd_install_worker.c` connects the native transaction adapters. Build with
`build.py --install-worker --arch arm64 --public-key KEY --output DIR`. The
ARMv7 build deliberately refuses this 8000 container-migration entry; it is not
a 6000 installation implementation.

The fixed `--bootstrap` mode receives a release on stdin, a target approval on
fd3 and the matching migration plan on fd4. The delivery mechanism must
independently authenticate this bootstrap executable before running it as root;
root ownership alone is not a release signature. It validates the signed release,
live target and predecessor, prepares disabled storage, durably binds the plan,
publishes the temporary capsule, arms recovery and queues the fixed independent
forward service. No command, path, unit, PID, priority or capability is selected
by these input records. A fixed bootstrap lease serializes staging and publication.
Matching complete RAM staging can be reused. Interrupted partial extraction can
be replaced only when every existing file is a bounded prefix of its authenticated
release member; unknown files, links and differing bytes are preserved and refused.
An existing authenticated capsule is rearmed without requiring the predecessor
to remain running or preparing the store again. The durable transaction determines
whether the independent entry proceeds or rolls back.

The independent entry authenticates its capsule and executing inode, checks the
live target and verifies that it is the MainPID of its expected loaded service.
It connects preparation, final identity copy, retirement, verified activation,
rollback and terminal cleanup to `pd_transaction_run`. `--recover` uses the
recovery-only transaction entry. The selected bundle must match both the signed
release and the store selection. Primary autostart is durably enabled only after
readiness; rollback removes the fixed enable link before stopping the new runtime.

`pd_runtime_retire` removes only the fixed volatile runtime layout in ROLLBACK,
after service and process quiescence. It inventories at most 256 entries,
accepts only the fixed payload filenames and fixed data-directory layout,
rejects links, foreign owners, writable executable files, cross-device objects,
unknown directories and oversized files, and rechecks each object's identity
before deletion. Persistent state is outside this operation. Partial deletion
can resume. Terminal cleanup verifies the staged release files and approval,
removes their RAM copies, then detaches the capsule and removes temporary units.

Fourteen filesystem tests cover runtime retirement, including process death
after each removal and preservation of persistent identity and foreign objects.
The service suite now has 35 cases, migration 39, and install preparation 17;
all four suites pass host and ASan/UBSan/LSan runs. These are component tests.
The isolated whole-worker suite has 26 cases. It exercises the actual signature,
staging, identity transfer, journal, lease, capsule and cleanup code while replacing
radio, container and service operations with linked test adapters in a disposable
network/user/mount namespace. It covers successful commitment, rollback after
stop/removal/activation, process death, partial staging, bootstrap retry, changed
identity preservation and terminal cleanup. It does not establish real service
manager or container lifecycle behavior. Production builds omit those adapters.

Ordinary boot can finish removal of matching temporary unit definitions left by
interrupted terminal cleanup. It requires a matching COMMITTED or ROLLED_BACK
record and refuses to detach an active capsule. This reconciliation never executes
an installation worker or resumes forward installation. Capsule/reconciliation
coverage has 29 host cases, also passing ASan/UBSan/LSan.

Current signed development packages include the combined worker and runtime gates.
The ARM64 release is 8,846,912 bytes and the ARMv7 release is 9,173,606 bytes; all
13 executable inputs for each architecture reproduced byte-for-byte in a separate
build. The signing key is a development key, not configured release authority.
The ARMv7 container-migration worker refuses execution; the native6000 installation
path and actual6300 hardware qualification remain required.

The ARM64 combined installer committed on FLEX-8600 firmware4.3.16.42034 through
the owner-authorized development bridge. The bootstrap returned after26.03 seconds;
the complete observed transaction took43.33 seconds. It removed the exact approved
predecessor container, retained its image, started the authenticated primary as
UID78666/GID20 with no capabilities and NoNewPrivs, preserved public identity, and
removed RAM staging, the executable capsule and temporary install/recovery units.
The temporary root job and its input files were removed. The broad diagnostics
bridge itself remains present; this is not proof of its removal.

One software reboot of this installed candidate changed the boot ID and brought
the primary back automatically, with identical executable bytes, identity and
unprivileged process attributes. Boot payload preparation completed at uptime10.52
seconds, the socket became active at10.62 seconds and the primary service at10.67
seconds. External observation first reached HTTP44.20 seconds after the reboot
request and the radio API61.67 seconds after it. These timings have different
origins; service activation is not a claim that the radio API was already ready.
Both temporary worker units were absent from disk and unloaded after reboot.
Authenticated receive-only Start/Status/Stop passed before and after reboot using
the existing desktop credential and GUI client, with FDX disabled and no keying.
After the final stop, the session endpoint became unavailable, the temporary ARP
entry was absent, and the process audit found only the primary shim: no owned
session or helper process remained. This is lifecycle evidence, not an RF
correction or IQ-quality measurement.

Actual reconstruction/changed-private-state rollback, client loss, worker death,
reboot during installation, authenticated bootstrap delivery and the connected
operator workflow remain release requirements. One successful software reboot
does not qualify sudden power loss or the6300. Existing finite component or RF
proofs do not establish production readiness. Live development evidence is recorded
separately from the isolated suite and from final release qualification.


## Managed startup and live upgrade qualification

Managed cold boot waits up to sixty seconds for the radio identity API and
revalidates the transaction admission gate around every probe and after selecting
the release. Other entry points make one bounded probe. The managed boot unit
allows ninety seconds for preparation. Ten host and ten sanitizer cases cover
readiness, timeout and loss of upgrade-worker authority; cold-boot hardware
qualification of this candidate is still pending.

Runtime retirement recognizes the bundled Tailscale cache layout
`tsnet/profile-data/<four lowercase hex digits>/netmap-cache` beneath the three
fixed volatile data roots. It still enforces the global inventory bound, owner,
mode, device, link and leaf-size checks; no arbitrary nested directory is admitted.
The cache layout appears in both the filesystem and combined-worker fixtures.
Thirty-eight filesystem cases pass normally and under ASan/UBSan/LSan; thirty-four
isolated combined-worker cases pass. Persistent identity is outside this tree.

The first installed upgrade required development recovery. The recovered 8600
now runs its previous unprivileged shim with the same public identity, and the
upgrade journal records completed rollback. The temporary recovery worker and
units have been removed. Recovery required both receipt repair and a constrained
overlayfs management-file fix; it does not qualify autonomous rollback. The broad
diagnostics bridge remains installed.

Successive updates use independent approval-derived generations and a durable
generation selector. Boot preparation understands both the original fixed pair
and the new selector. The signed AEPDR005 contract enables bootstrap and worker
successor admission. Admission requires the exact terminal predecessor captured
in the authenticated capsule, including its selected payload and separately
retained management. Isolated three-release sequences cover both commit and
rollback, including recovery before the new journal and during management and
selection changes. Authenticated retirement and installed qualification remain
unfinished; these fixtures do not establish production readiness.

The armed capsule publication API stages verified inputs in the inert fixed
`.aetherpd-upgrade-staging` directory, enables and syncs the fixed recovery units,
then publishes the active worker under the capsule namespace lease. Terminal
cleanup retains the same lease through unit removal. Interrupted inert staging
can be retried after old terminal cleanup removes the units; publication always
rearms them. While holding the capsule lease, publication verifies that the fixed
boot service is active/exited with no main or control process, and the session
socket is active with no control process. This excludes legacy boot/socket-owner
cleanup already past its lease; later invocations cannot acquire the held lease.
The guard sends read-only fixed service queries and refuses missing process-state
evidence. Direct invocation of privileged helpers outside the verified service
graph is not a supported concurrent operation. The guard passes on the recovered
8600 without changing its installed state. Isolated crash/concurrency tests and
that read-only guard check do not qualify an installed update or reboot.


The first installed AEPDR005 successor trial on the 8600 committed in38.88 seconds
from the previously rolled-back fixed layout. The selected generation changed to
the new authenticated pair; the original pair and selector stayed intact. The
primary retained its public identity and ran as UID78666/GID20 with empty groups,
zero capabilities and NoNewPrivs. Temporary upgrade authority was removed.

One subsequent software reboot changed the boot ID and automatically restarted
that exact candidate. The boot service was active at uptime39.70 seconds, the
session socket at40.43 seconds and the primary at40.46 seconds. External polling
observed both radio API and shim available65.91 seconds after starting the reboot
check. These are different timing origins. Postboot inspection verified the
committed generation, exact executable, identity/private-state metadata and absent
temporary install/upgrade units and fixed ARP entry. The broad diagnostics bridge
remains installed. No RF was used, and ordinary receive-session verification of
this candidate still requires the operator's automation bridge.

The actual ARM64 and ARMv7 release envelopes are8,934,482 and9,258,598 bytes.
An independent rebuild matched all thirteen executable members on each
architecture. ARMv7 compilation/reproducibility does not qualify a6300. Keeping
all old pairs is not disk-bounded: authenticated retirement and final peak-space
accounting against the reported41MB free remain required, along with live
interruption/rollback and final DSP qualification.

### Terminal payload compaction

The development worker authenticates both releases after a durable commit or
rollback, then writes a protected `PDKEEP01` receipt under
`upgrade-retained/<approval-blake2b>/receipt`. The receipt binds the exact
terminal journal and both signed release headers. It is a record created by the
trusted worker, not an operator credential or a separately signed attestation.
The worker syncs it before removing only the unselected `payload.bundle`.
The selected release remains complete and pinned under its pair lease. All
management files remain available, including candidate management retained
after rollback. No nonterminal rollback input is removed.

Successor inspection accepts this compacted layout only with the matching
protected receipt, complete selected release and authenticated remaining members
of the other release. Missing receipts, altered files, foreign entries and
pending selectors refuse inspection or cleanup. Cleanup refusal preserves the
unselected payload and still removes temporary upgrade authority. It may leave
storage maintenance necessary before another update.

Host tests cover both terminal selections, interrupted receipt publication and
unlink, receipt tampering, exact preservation of selected files, subsequent
updates and whole-worker recovery. Address, undefined-behavior and leak sanitizer
checks pass for the compaction suite. ARM64 and ARMv7 worker builds remain below
the 512 KiB limit. These changes have not been installed on the radio.
Compaction alone does not establish the 6300's peak disk budget or preserve
arbitrary historical rollback choices.

### Exact predecessor retirement

After terminal compaction, the development worker can retire the exact
predecessor named by its authenticated capsule origin. It validates the complete
selected successor, acquires the predecessor's exclusive pair lease and verifies
the predecessor releases, or their compacted layout and protected receipt.
The operation accepts the fixed legacy pair or an approval-derived generation;
it accepts no caller-supplied path and does not sweep unrelated generations.

A protected 1,804-byte `PDRET001` removal record binds the capsule origin, layout
and fingerprints of 23 fixed possible files. Its checksum detects corruption;
authority comes from the authenticated worker and protected storage, not the
checksum. The record is synced before deletion. Every remaining file is checked
against it on recovery. Unknown names, changed members, unsafe metadata,
directory aliases and active readers refuse retirement. The old lock is removed
only after its release directories are gone. The selected generation and private
identity are never retirement targets.

The record lives beside the successor's compaction receipt, so a later retirement
can remove it with that predecessor. A later upgrade refuses unfinished removal
records rather than discard them. A cleanup refusal still removes temporary
privileged authority; a persistent refusal may require explicit maintenance
before another upgrade. Process-death recovery retains the current capsule and
resumes the same terminal cleanup without reactivating an older release.

Host fixtures cover both terminal choices, both directory layouts, every removal
boundary, lease exclusion and combined-worker recovery. These changes are not yet
installed on a radio. Four-update chains with distinct signing keys cover
commits, alternating commit/rollback and interruption while removing inherited
records. They retain exactly one generation and one receipt directory. Older
orphaned bench generations remain outside this operation; measured peak-space
admission remains required before claiming a disk-bounded 6300 installation.

### Original install slots

Once a complete terminal generation is selected, the development worker can
retire the fixed original `slots/A.bundle`, `B.bundle` and `C.bundle` files.
Each existing slot must authenticate with the protected original installation
key; the selected and previous slot hashes must also match the original selector.
Unknown entries, temporary selectors, aliases, wrong modes and damaged payloads
refuse deletion. An original selected slot missing before a receipt exists is
an error, not evidence that cleanup already happened.

The 652-byte protected `PDSLOT01` receipt records the original installation
marker, selector, fixed slot fingerprints and the terminal approval under which
retirement began. It is synced before removal. Recovery verifies remaining
slots against that record, and later generations retain the same receipt across
signing-key changes. Original installation metadata and private identity remain
unchanged. Loss of the generation selector still refuses boot; it cannot fall
back to the removed slots. The original slot updater and rollback helper refuse
both complete and pending retirement records.

The slot cleanup suite, memory-safety checks and combined-worker crash tests pass
offline. Four-update chains also preserve the original receipt and empty slots.
ARM64/ARMv7 workers remain below 512 KiB. These changes are not installed on the
radio. Final peak-space accounting must include receipt allocations, directory
growth, private state, worker/management temporaries and the upload inputs; it
must not assume the full release inputs can remain on the 6300's persistent disk.

### Installed retention candidate

The retention candidate committed on the 8600 in 44.93 seconds and increased
store filesystem free space by 17,149,952 bytes. It removed the original slots,
the immediately preceding generation and the unselected payload of the new pair.
The selected payload and identity remained intact; temporary upgrade authority
was removed. The older legacy bench pair and broad diagnostics bridge remain.

One software reboot changed the boot ID and automatically started the candidate.
The first external observer failed its public-status assertion just after the
shim port opened; that response was not captured, so its exact cause is unproven.
A subsequent status read and native audit passed without another reboot or service
restart. The primary service became active at uptime 40.067 seconds and ran as
UID 78666/GID 20 with empty groups, zero capabilities and NoNewPrivs. The audit
verified the selected payload digest, identity/private-state metadata, removed
predecessor/original payloads and absent temporary installer/upgrade units.
This establishes automatic startup after cleanup, not a precise full-readiness
latency or interrupted-update rollback qualification. No RF was used.

## Bootstrap distribution authentication (development)

`bootstrap_artifact.py` publishes a separate `AEPDX001` envelope for the exact
privileged installer or upgrade worker. Its Ed25519 signature binds architecture,
fixed role, executable length/hash, source snapshot and the complete candidate
release digest. The executable is bounded to 512 KiB. Keeping this envelope
outside the release avoids a circular hash: the upgrade worker already embeds
that release's authenticated management header. The publisher verifies the full
release with the native release verifier before signing the worker; the release
and artifact use the same explicit publisher key. The current initial installer
is ARM64-only, so signing or admitting an ARMv7 install-role artifact is refused.

`pd_bootstrap_artifact_read` checks a caller-pinned publisher key and expected
architecture, role, source and release. It returns an owned verified executable
snapshot and its complete envelope digest; a delivery entry must use those bytes rather than reopen an
untrusted input path after verification. It accepts no command, destination,
PID, capability or scheduling parameter and neither writes nor executes a file.
The input may be untrusted storage: authenticity comes from the signature and
exact bindings, not root ownership of an uploaded file.

Build the test adapter with `build.py --bootstrap-artifact-test --arch host
--output DIR`, then run `test_bootstrap_artifact.py --checker
DIR/bootstrap-artifact-check --release-checker PATH_TO_RELEASE_CHECK`. The
adapter's file arguments are for tests only. The publisher CLI accepts
`--worker`, `--release`, `--signing-seed`, `--role install|upgrade`, `--output`
and `--verifier`; its seed input must be a private single-link 32-byte file.

This authenticates artifact distribution only. It is not operator consent,
live-radio identity evidence, proof of the worker's source-to-binary provenance,
or a shipping privilege delivery endpoint. The trusted release process must
build the worker with the matching release/key, and the delivery entry still
needs an independently pinned key, exact-plan preview/consent, protected staging,
fixed execution, bounded lifetime and removal. Existing native target/transaction
checks remain mandatory. The bench diagnostics bridge is not removed by this
component, and no radio installation is performed by these tools.

## Fixed bootstrap delivery entry (development)

`pd-deliver` consumes an authenticated worker envelope on FD0, a protected
80-byte `PDDELIV1` receipt on FD3, the exact target plan on FD4, the candidate
release on FD5 and, for upgrades, the previous release on FD6. The receipt binds
architecture, role and both envelope and plan hashes. `delivery_consent.encode`
encodes this record; it does not authenticate an operator or grant permission.
The shipping consent interface must create it only after approval of the exact
plan. The entry accepts no arguments and uses a compiled publisher key.

The production entry requires root, protected `/run` on tmpfs and inputs on
tmpfs. It authenticates complete releases and the worker, checks the live target,
stages only fixed names beneath `/run/aetherpd-delivery`, then rechecks staged
releases and target identity. It executes the pinned verified worker with
`--delivered`, a minimal environment and only the worker's required descriptors.
FD7 carries the directory lease across execution; both native workers retain it
until exit and mark it close-on-exec so executed helpers do not inherit it.
The worker is bounded to 90 seconds. Cancellation kills and reaps that child;
normal completion removes only files whose identities match those it created.
Foreign files are preserved and prevent a successful cleanup result.

Build with `build.py --deliver --arch arm64|armv7 --public-key KEY --output DIR`.
Initial installation remains ARM64-only. For host component tests, build with
`--delivery-test --arch host`, then run `test_delivery.py --checker
DIR/delivery-check`. The test adapter substitutes live target observation and
execution with inert fixture bytes; these tests do not qualify live installation.

A protected receipt is staged before the executable or release inputs. After
abrupt supervisor death, a retry requires the same approved envelope and plan,
an exclusive directory lease, and fixed files containing only exact prefixes of
the authenticated inputs. It validates the entire directory before removing any
files, then stages and validates the delivery again. Unknown files, changed
bytes, links, unsafe permissions and a live worker all prevent recovery. The
receipt binds recovery to this delivery; another approved release cannot clear it.

If interruption leaves the receipt absent or incomplete, recovery preserves the
directory. This includes the short windows before receipt creation and between
receipt removal and directory removal. A radio reboot clears this tmpfs state;
the entry does not guess ownership from an empty directory or partial receipt.

The 26 host cases cover both roles, partial-copy recovery, parent death, leases
across real exec, and lease closure in executed helpers. They also pass under
address, undefined-behavior and leak sanitizers; removing the recovery lease or
content comparison makes the corresponding tests fail. ARM64 delivery/install/
upgrade and ARMv7 delivery/upgrade static binaries have been built. The operator
preview/consent interface, temporary privilege carrier and its removal remain
unfinished. No live delivery through this entry, actual 6300 execution or
broad-bridge removal is claimed.

### ARM64 carrier host entry

`pd-deliver --carrier` accepts the delegated host-root directory on FD8 in
addition to its fixed delivery inputs. This entry requires UID 0, host PID and
network context, a real procfs, and a root descriptor matching PID 1's root.
It changes only its own process root; it does not enter a mount namespace or
accept a path or command from the caller. ARMv7 refuses this entry: the 6000
privilege carrier remains a separate, unqualified adapter.

The entry permits only CHOWN, DAC_OVERRIDE, FOWNER, SYS_CHROOT and SYS_PTRACE
in its effective/permitted/bounding masks, with empty inheritable masks. The
first three support installer ownership/filesystem operations; SYS_PTRACE
supports existing process/executable identity checks. It requires SYS_CHROOT
to enter the delegated root, then removes it from effective/permitted masks and
sets NoNewPrivs before executing anything else. Its bounding bit remains set;
NoNewPrivs prevents executable handoff from restoring the retired capability.

On the 8600, a temporary container with host PID/network context, default seccomp,
`Privileged=false`, and only SYS_CHROOT could read the installed service and open
its process handle. Adding SYS_PTRACE made its executable-identity check pass.
The native entry also demonstrated SYS_CHROOT retirement across actual exec.
The container mount namespace remained distinct from the host's: no SYS_ADMIN
or `nsenter` was needed for these operations. ReadonlyRootfs applies to the
container image; it is not a write barrier for the delegated host root. These
probes performed reads only and removed their temporary containers/helpers.

This is host-entry qualification, not a complete shipping carrier. The bench
still used its existing administrative bridge to create the temporary container
and deliver descriptors. A fixed authenticated sender/receiver, exact-plan
operator consent, constrained container creation/removal, and an end-to-end
installation through this entry remain required. The filesystem/ownership
capabilities allowed above have not yet been qualified against a full installer
transaction under this reduced capability set.

### Authenticated descriptor handoff (development)

`pd-send` and `pd-deliver --receive` use an owned, connected Unix SEQPACKET
socket on FD9 and private configuration on FD3. The 120-byte `PDXFER01` record
contains its eight-byte tag, a fresh 32-byte request secret, and the exact
80-byte delivery receipt. Configuration must be read-only, mode 0600, owned by
the expected local user, and have no additional hard links. Encoding or
possessing it does not establish operator consent: its trusted producer must
first present and approve the exact plan. Neither entry accepts a path or command.

The native sender requires UID/EUID 78666 and zero effective, permitted and
inheritable capabilities. It opens only its native `/` directory and sends that
descriptor with the envelope on FD0, plan on FD4, candidate on FD5 and previous
release on FD6 for an upgrade. Both sides check peer UIDs (receiver 0, sender
78666). The receiver generates a fresh challenge. Domain-separated keyed BLAKE2b
authenticates both directions over that challenge and the complete receipt;
recorded replies cannot answer a new challenge. The connection adopts blocking
mode with three-second send/receive timeouts. Exactly four or five read-only
descriptors are accepted, with bounded regular tmpfs inputs and no extra rights.
Malformed, truncated or rejected messages close all received descriptors.

After authentication, the ARM64 receiver enters the verified host root using
the carrier guard above. It creates an anonymous mode-0600 receipt using
O_TMPFILE on protected `/run`, reopens it read-only, and supplies the fixed
delivery descriptors. Delivery accepts an anonymous receipt or one private
link, and continues to require publisher signatures, exact hashes and the live
target. O_TMPFILE support is required for this receiver; there is no named-file
fallback. ARMv7 refuses `--receive`, while the native sender cross-builds for
both architectures. A successful sender result means descriptors were sent,
not that installation succeeded.

The transport suite exercises real socketpairs and descriptor passing for our
protocol, including an independent Python encoder, replay, wrong peers, malformed
packets, descriptor leaks, configuration metadata and silent peers.

On the FLEX-8600 running 4.3.16.42034, the compiled sender and receiver completed
the authenticated handoff, host entry, anonymous receipt creation and descriptor
mapping, then reached delivery validation and rejected intentionally unauthentic
release inputs. The sender ran as UID 78666 with zero effective, permitted and
inheritable capabilities. Temporary containers and files were removed; installed
metadata and the primary process identity were unchanged. No RF was used.
This qualifies the admission path, not signed worker execution or installation.
The delivery entry uses raw scheduling syscalls, as the session launchers do:
the linked musl scheduling wrappers return ENOSYS on this build.

The connected socket/configuration producer, authenticated desktop upload, fixed temporary
container lifecycle, operator consent UI and live end-to-end installation remain
unfinished. The bench diagnostics bridge remains outside this shipping design.

The [temporary ARM64 carrier](../carrier/README.md) performs the installer
child's fixed lifecycle itself. Its signed upgrade committed on the FLEX-8600
under the restricted worker capability set and removed temporary installation
authority. The bench still loaded/removed the outer image and supplied the
native sender fixture. First installation, interruption recovery through this
carrier and the complete ordinary-user bootstrap workflow remain unqualified.

## Terminal installation status mirror (development)

The fixed upgrade worker and boot helper can publish a read-only summary at
`/run/aetherpd/installation`. The unprivileged shim exposes it through authenticated
`GET /v1/aetherpd/installation`, with no parameters or request body. This adds no
privileged query daemon and gives the shim no descriptor into the protected store.
The response is a terminal transaction observation, not a claim that correction
is running or that the primary is healthy.

Publication requires the shared terminal transaction lease, a committed original
installation, no temporary install/upgrade authority, a terminal upgrade journal,
and an exact match between that journal and the selected generation/control
record. The boot call follows authenticated payload preparation; the worker call
follows successful transaction cleanup. Publication failure is logged without
changing a committed transaction into failure or initiating rollback. During
activation the journal is not terminal, so no success is published. First installs
without an upgrade journal have no mirror yet.

The 204-byte record is atomic on tmpfs and contains no credentials:

| Offset | Length | Value |
|---|---|---|
| 0 | 8 | `PDVIEW01` |
| 8 | 4 | Big-endian phase: 4 committed, 7 rolled back |
| 12 | 4 | Big-endian radio profile |
| 16 | 32 | BLAKE2b-256 of the exact 352-byte approved upgrade plan |
| 48 | 32 | Selected whole-release BLAKE2b-256 |
| 80 | 32 | Selected Ed25519 publisher public key |
| 112 | 28 | NUL-padded firmware identifier |
| 140 | 64 | NUL-padded radio serial |

The writer publishes mode 0444 with one link beneath protected root-owned runtime
directories. It writes and syncs `.installation.new`, renames, then syncs the
runtime directory. Only a matching partial write can be resumed. Runtime retirement
recognizes these two fixed names with bounded sizes and modes; foreign entries
still prevent deletion. The shim validates protected parents, no symlinks, file
ownership, mode, link count, exact size, tmpfs, format and bounded fields on each
read. Missing or invalid observations return HTTP 503 with
`state: unavailable` and `installation_committed: false`. All responses disable
caching. A committed response includes `state: committed`,
`installation_committed: true`, `upgrade_plan_blake2b_256`,
`selected_release_blake2b_256`, `selected_release_key`, `profile`, `firmware`, and
`serial`. Rollback uses `state: rolled_back` and false with the selected previous
release/key.

A desktop must match every identity and digest against its exact approved plan.
A previous transaction's valid mirror must not complete a new upgrade. Delivery
result 0, a healthy shim, HTTP 200 alone, or a record for another plan are
insufficient. A transport failure during shim restart is an unknown outcome;
only bounded read-only polling may retry automatically. An older shim without
this endpoint cannot confirm a rollback through this interface. Such a rollback
must remain unconfirmed in the client until supported evidence is available.
Older boot/runtime builds also do not understand the new runtime filenames;
backward upgrade/rollback compatibility must be qualified before shipping.

Host checks build with `build.py --arch host --installation-test --output DIR`.
Run `test_installation.py --checker DIR/installation-check --disk-directory DISK
--go-shim ../flex-tailnet-shim` from this directory, using a real disk directory
for the non-tmpfs refusal. The fixture changes only owner checks to the invoking
UID and exposes crash checkpoints; it does not bypass journal or selector gates.
Its native-to-Go test reads the actual C publisher's commit and rollback files
through the production Go reader. The same native tests support `--sanitize`.
`test_runtime.py` covers retirement of complete/partial mirrors and preservation
of invalid files. These are host component checks, not installed upgrade,
reboot, physical power-loss or FLEX-6300 qualification. Desktop polling and its
operator workflow remain unfinished.
