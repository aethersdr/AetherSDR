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

The handshake test also fails if the removal guard is taken out of the
connection's socket-opening path, and the dialog test fails if the Connect
automatically gate is taken out of ShackSwitch discovery or Remove stops
resetting the toggle.

## What this does not show

The checks are local and injected. They are not live-firmware, OS-vault or TX
evidence, and they say nothing about native accessibility behaviour on other
platforms.
