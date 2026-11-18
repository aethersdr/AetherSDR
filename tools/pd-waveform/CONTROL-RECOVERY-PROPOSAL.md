# Bounded control-service recovery proposal

This is an offline draft on `draft/aetherpd-control-recovery-20261118`, based
on `14a9c1f1e23698727aaba152ce115294829d401a`. It is not installed, selected
by the runtime builder, or merged into the qualified feature branch. The
operator's choice between automatic control recovery and explicit recovery
remains pending. This proposal does not resolve the whole-radio lockup.

## Fixed policy

The optional `control_restart=True` adds these exact directives to the
ordinary-user control service's existing `[Service]` section:

```ini
Restart=on-failure
RestartSec=5
StartLimitInterval=60
StartLimitBurst=3
StartLimitAction=none
```

The intended bound is three starts per minute, including the initial start,
with five seconds between failure and retry. Reaching the limit leaves the
control service failed; it must not reboot the radio or reset its own limit.
A successful control restart restores the API using the existing authenticated
identity. The operator must explicitly Start a new correction session. The
socket/session units, capture and scheduling authority, DSP, calibration,
recent-model recovery policy and transmitter commands are unchanged.

The policy is permitted only with managed startup, guardian hold and the
generation-selection release contract (`AEPDR005`). `units.py` and `release.py`
expose the explicit development flag `--control-restart`; their default is
false. The normal runtime build does not pass this flag. All four default
unit texts were compared byte for byte for both architectures and both
startup/guardian policies. Twenty default signed fixture releases spanning
formats 1–5 and both architectures/guardian policies are also byte-identical
to the qualified implementation. Newly compiled helpers have different bytes.

## Authentication and lifecycle

The native verifier derives the policy from the signed manifest's exact
primary-unit hash. It accepts only the fixed definitions; the envelope format
and file set do not change. Arbitrary retry settings, extra commands and
automatic session restart remain refused. A helper predating this proposal
cannot admit the new unit variant. Any deployment therefore needs matching
new management/boot helpers through the authenticated installation transaction;
an administrator's ad-hoc unit edit or drop-in is not a supported deployment.

The installed-file checker recognizes the complete new layout. Loaded primary
metadata must match it, including failure-only restart, five-second delay,
three-start limit, sixty-second interval and `StartLimitAction=none`.
Both old and current interval-property names are bounded and checked; a
missing or conflicting value refuses the new layout. The native tests retain
the existing upgrade, stop, quiescence and rollback journal gates.

Permanent management code and its fixed units can remain newer than a selected
rolled-back DSP payload. Payload rollback must not be described as reverting
the control-recovery policy. Complete installed upgrade/rollback behavior
with this policy still requires qualification.

An automatic primary restart is not evidence that an old session graph has
finished cleanup. Fresh session admission must still enforce ownership and
cleanup gates. No correction session or calibration source is retried by the
service manager. A five-second delay does not replace the guardian's cleanup
obligations or the keyed fault tests.

## Evidence and limits

The accompanying [qualification record](./CONTROL-RECOVERY-PROPOSAL-QUALIFICATION.json)
records 231 passing offline tests, three detected native guard-removal
mutations, the unchanged default release comparisons, and ARMv7/ARM64 boot
cross-builds. The management tests include interruption at file, rename and
durability checkpoints for both published primary-unit copies. Activation
tests exercise candidate and rollback phases with simulated systemd and
process attestation. The systemd ordering check uses the workstation manager's
offline verifier; it does not execute radio services.

The additional live workstation user-manager fixture is **not qualified**.
Its first payload refused its capability admission; its second attempt and
single-attempt preflight were refused by the manager's capability setup.
All three transient fixture units were subsequently verified unloaded. These
failures are preserved. They prove neither the four intended payload lifecycle
cases nor operation on the radio.

The [systemd service reference](https://www.freedesktop.org/software/systemd/man/latest/systemd.service.html)
describes failure-only restart, explicit-stop suppression and start limiting.
The older [systemd 208 property implementation](https://raw.githubusercontent.com/systemd/systemd/v208/src/core/dbus-service.c)
exposes the legacy interval property. Neither establishes the actual systemd
version, parsed policy or behavior on the team's FLEX-6300. An ARMv7 build on
this workstation does not qualify Linux 2.6, Cortex-A8 scheduling or actual
radio cleanup.

## Offline delivery qualification

The [delivery qualification](./CONTROL-RECOVERY-DELIVERY-QUALIFICATION.json)
pins implementation commit `453ba2175`. With the optional policy explicitly
enabled, complete ARMv7/ARM64 releases and upgrade distributions, and an ARM64
first-install distribution, reproduce byte for byte from two source/output
paths and compiler caches. The development signing seed remains in RAM/pipes.
These artifacts use the retained development publisher, not a shipping trust
catalogue, and have not been installed.

The real native verifier admits both the previous and candidate releases and
the matching worker. The older verifier refuses the optional policy. The
unchanged desktop package verifier loads the complete distributions, constructs
public upgrade plans, and prepares the ARM64 first-install uploads without
contacting the radio. Corrupt-file, wrong-key and changed-worker-source checks
refuse the altered packages. All eight files in each distribution are pinned.

The ARMv7 upgrade worker is 503,844 bytes and the ARM64 worker is 493,496 bytes;
both fit the 512 KiB worker limit. The eight modeled upgrade-storage layouts
pass byte/inode boundary checks. ARMv7's worst modeled peak is 39,718,912 bytes,
leaving 1,281,088 bytes against the rounded 41 decimal MB inventory. This assumes
signed inputs on tmpfs and the stated identity/metadata reserves. It does not
include firmware-retained upload copies or qualify first-install transient
storage on a 6300.

ARMv7 package-format verification does not remove the desktop selection
generator's profile-1-only guard. That refusal is explicitly checked. There is
no supported 6000 first-install adapter or ordinary 6000 installation claim in
this delivery proof. The workstation lifecycle fixture's capability refusals,
the operator's policy choice, and installed qualification remain open.

Before promotion, resolve the operator's recovery choice, independently review
the authority and compatibility changes, qualify the exact loaded policy and
ordinary-user receive-only crash/restart on the 8600, verify explicit Stop and
rate-limit exhaustion, then complete the installed update/rollback and actual
6300 checks. RF and automated reboots remain held while the separate whole-radio
hang and firmware-baseline decision remain unresolved. No RF, radio reboot,
radio service changes or persistent installation occurred for this proposal.
