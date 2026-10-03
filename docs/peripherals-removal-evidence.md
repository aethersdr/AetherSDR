# Peripherals redesign and removal — evidence

Evidence for the `peripherals-settings-update` branch (PR #6027): the device
list/detail Setup page, Add/Remove, and credential removal. The authentication
work it builds on is recorded in
[`4o3a-remote-auth-hardware-evidence.md`](4o3a-remote-auth-hardware-evidence.md).

Scope notes that belong to this branch rather than PR #6008:

- PR #6008 reflowed the existing table so each device's credentials and wrapped
  status had their own lines, avoiding clipped Clear code controls and errors at
  the default dialog size. The list/detail redesign below is not part of #6008.
- A saved manual TGXL/PGXL host/port takes precedence over the radio-reported
  endpoint. Clearing only the manual override allows the radio-reported address
  to be used when discovery has not been dismissed. Remove additionally
  dismisses TGXL/PGXL discovery when the device had a saved manual address or a
  live or connecting session; Add or an explicit Connect re-enables it.

## Current Peripherals redesign branch

`peripherals-settings-update` includes a device list with per-device detail
pages, Add/Remove controls, and a persisted visible-device list. Connection,
authorization, and wrapped status controls live in each detail page. A blocked
radio-discovered TGXL or PGXL can surface a temporary recovery row without
persisting that row or its address as manual configuration. Retrying an
unchanged radio-reported address and default port preserves discovery;
changing the address or port saves a manual override.

Remove first acquires an in-memory reconnect guard, then disconnects the
selected device, retiring any pending AUTH attempt before credential deletion.
TGXL, PGXL, and AG connection entry points enforce the guard, covering
manual targets, radio discovery, retry timers, and the shared ShackSwitch model.
The gate is in the engine rather than optional wrappers at individual callers.
Setup reports that removal is pending and blocks editing and closing for up
to 15 seconds. After that bound the dialog can be closed while the reconnect
lease stays held until the keychain request finishes. Only after
successful deletion (or a reported session-only clear) does it clear connection
settings and persist discovery dismissal. Add or explicit Connect clears that
dismissal. Failed deletion retains the row, settings, and discovery policy for
retry. The canceled connection is not explicitly resumed by the removal handler;
subsequent normal auto-connect events are allowed again. A separate removal
notice states that deletion failed and the connection was stopped; it survives
later Connected/status updates and explains that normal reconnect events may
connect the device again. The operator can retry Remove.

An unrelated ShackSwitch using the shared AG model retains its retry schedule
while AG credential removal is pending. An expiry during the guard restarts
the existing five-second timer without connecting; after the guard is released,
the next expiry reconnects normally. Explicit Disconnect still cancels that
timer. The socket-free dialog test covers repeated expirations, successful and
failed deletion, and an operator cancellation during the pending window.
Removing the retry deferral made the regression fail with “AG removal consumed
ShackSwitch retry”; the guard was restored before the final build and tests.

One-shot ShackSwitch requests from initial discovery or radio connect are also
retained while the AG guard is held, even if auto-reconnect is disabled. The
model keeps at most one deferred target, leaving current device metadata alone;
the next timer expiry after guard release initiates that target. Explicit
Disconnect cancels it, and a newer connection after release supersedes it.
Successful AG removal does not repeat the initial disconnect and inadvertently
cancel this unrelated request. Tests inject both discovered IP and manual DNS
targets, success/failure of deletion, cancellation, and supersession. Mutation
checks failed when request capture or deferred dispatch was removed, and when
the redundant completion-time disconnect was restored. All three mutations
were reverted before the final build and focused test sweep.

The vault completion owns the transient guard independently of Setup. Parent
window destruction bypasses dialog close guards, but cannot persist a pending
dismissal or release reconnect suppression before the vault finishes. If Setup
has been destroyed, the completion releases the guard without touching its UI
or changing configuration. On a subsequent run the original configuration and
discovery policy remain; credential deletion may have completed, so a new auth
challenge can require entering the code again. No cross-store atomic shutdown
guarantee is claimed. Discovery dismissal applies to TGXL/PGXL, not AG discovery.

AG targets configured through the applet are reconciled into Setup both when
it opens and while it remains open, including rows already listed. Untouched
prefilled fields follow the stored endpoint; edits in progress are preserved.
Removal resets those field baselines. Save-on-close clears a target only when
the operator edited its address to empty and the stored host and port still
match the field's baseline. It preserves a newer externally saved target,
even when Setup closes before its refresh timer runs.

Latest local validation built the desktop application and passed all eight
focused tests: peripheral auth dialog, handshake, and Keychain; both automation
redaction tests; TGXL docked parity and ports; and the amp applet. The dialog
tests use injected state and an in-memory credential store, not a device peer
or an OS-vault prompt. They cover discovery dismissal and re-add, authentic
radio-reported recovery versus manual targets, removal during unanswered
credential reads, late AUTH acceptance while deletion is pending, deletion
failure and success, and blocked close/accept/reject paths during deletion.
They also destroy Setup’s parent during a held deletion, reload persisted
settings, and verify reconnect suppression survives until vault completion.
The fake store rechecks pending deletion when delivering credential reads,
matching the production refusal behavior. They also cover AG configuration
before and during Setup, and close-time
preservation of externally updated settings and connected state.

The pending-removal regressions were mutation-checked for all three devices:
removing each early disconnect admitted a late AUTH success. Removing the
close-event guard allowed unsaved edits to be committed during deletion;
removing the done guard allowed Setup to close before deletion finished.
Each mutation failed the test, and the production guards were restored.
The subsequent transient-guard regressions also failed when reconnect gating
was bypassed, vault completion stopped retaining the guard, or discovery
dismissal was persisted at the start of removal. The latest tests call the
production connect methods with injected transport initiation, rather than
calling a guard helper. They verify requests reach transport before/after the
lease, and never during it, including AG discovery, manual address overloads,
and ShackSwitch. Each of the three entry-point gates and the persistent failure
notice were independently mutation-checked. All mutations were restored before
the final build and test run.

Mutation checks caught removal-policy, recovery-persistence, initial/live AG
visibility, each of the three device teardown orders, discovery-provenance,
and stale-prefill regressions. The latest four mutations also failed when
reconciliation was restricted to unlisted AG rows, Remove stopped resetting
the port baseline, save-on-close ignored edit intent, or save-on-close ignored
a newer saved endpoint. All mutations were restored before the final build
and eight-test pass. Registration, manifest, engine-boundary,
capability-record, command-plane, and whitespace checks passed. Relative to authentication PR #6008, the UI follow-up adds one AppSettings
includer (104 to 105): the connection-persistence helper, which reuses the
existing manual endpoint keys. The manifest grows from 231 to 232 headers
(core: 194 to 195) through `PeripheralRemovalGuard.h`, tagged
`peripheral(4o3a)`, with two GUI consumers. ThemeManager remains at 157
includers. The authentication implementation's earlier validation header,
AppSettings store includer and ThemeManager helper are part of #6008,
not additions in this UI follow-up. See
[Peripherals settings UI](peripherals-settings-ui.md) for the proposed workflow
and its dependency on #6008.

The latest read-only Claude review reported no surviving code defects; it did
not execute the tests. These checks add no live-hardware, OS-vault, or TX
evidence. Maintainer review of the redesigned UI and the existing credential
binding decision remains outstanding.


### Issue #6026 follow-up: bounded removal wait

The pending deletion UI now allows closing after a 15-second deadline with an
explicit unconfirmed-deletion message. It retains configuration and the
vault-owned reconnect lease; a timeout cannot safely cancel the OS job. Late
completion never finishes the abandoned Remove or commits stale field edits.
Socket-free dialog coverage injects timeout for TGXL/PGXL/AG with both successful
and failed late completion, with the dialog retained or destroyed. Additional
coverage removes AG while ShackSwitch is live and ShackSwitch while AG is live.
Legacy endpoint keys remain unchanged; this UI follow-up is not their migration.

The macOS desktop build and focused dialog test passed after this follow-up.
Mutation checks failed when the timeout kept close blocked and when a late
completion was allowed to apply Remove; the restored implementation passed.
The test injects the single-shot timer expiry, rather than sleeping or opening
sockets. Real backend cancellation is deliberately not claimed.


### PR #6027 review follow-up: shared credentials and independent edits

Remove now includes ShackSwitch in the guarded, bounded credential workflow.
AG/ShackSwitch use endpoint-scoped deletion: a credential for the other endpoint
is retained, and an unknown offline-hostname owner fails closed with instructions
to connect the selected device before retrying. The deletion queue rechecks its
endpoint/revision before starting a conditional delete so a newer save survives.
This does not assume any specific ShackSwitch firmware AUTH capability.

Timeout suppresses only the selected row's pending clear-on-close edit. The
regression includes an unrelated cleared ACOM endpoint and verifies that its
edit still persists. Shared-slot tests exercise both selected-device directions,
owned/other credentials, absent endpoint identity and replacement saves.

The device list now uses minimum extents and expanding layout space instead of
fixed width/height. The user guide includes LP-100A and documents stable lowercase
UI IDs versus legacy connection-object names. The removal guard explicitly warns
that all non-atomic acquire/release/query operations must run on the main thread.

Validation for this follow-up: macOS desktop build and all eight focused tests
passed (17.81 seconds). The dialog test also passed with QT_SCALE_FACTOR=1.25
and 1.5. An isolated offscreen demo screenshot at 125% showed the TGXL fields,
Show/Clear controls and Add/Remove without clipping; the model reported one slice,
one panadapter and transmitting=false. The review-owned process was stopped.
Mutations that cleared all row savers, bypassed endpoint ownership or excluded
ShackSwitch from the lease each failed their regression test; the restored code
passed. Engine, capability, command-plane, registration and manifest checks passed.
This remains local/injected evidence, not a claim about firmware or native vaults.
