# Peripherals list and removal: evidence

What the Setup Peripherals page does, and what backs each claim. The behaviour
is described in [Peripherals settings](peripherals-settings-ui.md), which holds
the outcome table for Remove. Authentication evidence is in
[`4o3a-remote-auth-hardware-evidence.md`](4o3a-remote-auth-hardware-evidence.md).

## Tests

`peripheral_auth_dialog_test`, `peripheral_auth_handshake_test` and
`peripheral_auth_keychain_test` use injected state and an in-memory credential
store. No device, socket or OS vault is involved.

| Behaviour | Test |
|---|---|
| Every route to a socket is stopped while a removal is pending, and a blocked attempt leaves the retry target alone | `checkRemovalGuardStopsEveryConnectPath` |
| Add creates and selects the row only; Remove asks first, Cancel changes nothing, OK resets the toggle and stores no suppression | `checkConnectAutomaticallyToggle` |
| Connect automatically blocks reconnect and discovery connects for each device and key; an explicit Connect still connects and leaves the toggle alone | `checkConnectAutomaticallyGates` |
| List and detail follow the explicit status, with no text parsing; the Add menu says why an entry is disabled; there is no presentation timer | `checkStatusPresentation` |
| Removal during unanswered credential reads, late AUTH acceptance, deletion failure and success, blocked close paths | `checkPendingRemoval`, `checkRemovalOwnerTeardown` |
| The 15 s bound with a late completion, with Setup retained or destroyed | `checkRemovalTimeout` |
| AG and ShackSwitch share a slot: each selected-device direction, owned and other credentials, unknown owner | `checkSharedCredentialRemoval`, `checkRemovalWithUnknownOwner` |
| An unrelated ShackSwitch keeps its retry through an AG removal | `checkShackSwitchRetryDuringRemoval`, `checkOneShotShackSwitchDuringRemoval` |
| AG targets configured through the applet reconcile into an open Setup without overwriting edits in progress | `checkExternalAgConfiguration` |

A regression test counts only if it fails without the code it covers. These
mutations each failed their test and were restored:

- removing the removal guard from the connection's socket-opening path
  (`peripheral_auth_handshake_test` and the dialog test);
- removing the Connect automatically gate from ShackSwitch discovery;
- removing the toggle reset from Remove.

The removal lifecycle tests (retry deferral, close and done guards, the early
disconnect in each device's Remove, endpoint ownership) were mutation-checked
when they were written; they were not re-mutated for the Connect automatically
work.

## What this does not show

The checks are local and injected. They are not live-firmware, OS-vault or TX
evidence, and they say nothing about native accessibility behaviour on other
platforms.
