# Peripherals settings UI

This UI/UX follow-up depends on authentication PR [#6008](https://github.com/aethersdr/AetherSDR/pull/6008) and must merge after it. The layout and workflow are proposed pending maintainer approval of the new RFC. Authentication protocol support is supplied by #6008.

## Configuring a device

Open **Radio Setup → Peripherals**. Select **Add**, choose a device type, and select its row to see its connection settings. Supported pages are Tuner Genius XL, Power Genius XL, Antenna Genius, ShackSwitch, ACOM, SPE Expert, VK3AMP and LP-100A Meter. Adding a row does not itself connect the device. Enter the appropriate endpoint and use **Connect**.

The device list persists across restarts. Existing manual configurations seed the list when the list setting has not yet been created. Antenna Genius configuration made through its applet is reconciled into Setup. Untouched fields follow current saved values; closing an older Setup view must not overwrite a newer external endpoint.

A discovered TGXL/PGXL that needs authentication can appear as a temporary recovery row. Retrying its unchanged discovered endpoint does not save a manual override; editing the endpoint explicitly selects a manual target. A revealed saved credential is display-only and is not replayed as newly entered text on Connect. Endpoint-bound credential loading continues to use the authentication implementation from #6008.

## Removing a device

Select a row and choose **Remove**. For an authenticated peripheral, removal stops its connection attempt and waits for credential deletion. Setup displays the pending state and prevents editing or closing for up to 15 seconds while waiting for completion. A transient connection guard prevents discovery or another connection entry point from reconnecting that device during deletion.

Successful removal clears its connection settings and list entry. Removing TGXL or PGXL also persists discovery dismissal until **Add** or explicit **Connect** re-enables it. This dismissal policy does not extend to AG. Removing AG does not tear down an unrelated active ShackSwitch using the shared model; a deferred ShackSwitch request or retry survives the temporary guard. Explicit disconnect cancels that request, and a newer connection supersedes it.

If credential deletion fails, configuration remains and Setup displays an explanation so removal can be retried. The canceled connection does not automatically resume as part of the failed Remove action; ordinary subsequent reconnect events may connect again once the temporary guard is released. When the credential backend is unavailable, the UI distinguishes session cleanup from confirmed persistent deletion.

If deletion has not completed after 15 seconds, Setup reports that deletion is unconfirmed and allows closing. Configuration and discovery policy are retained; closing does not save the abandoned row’s field edits; unrelated rows retain their pending edits. The Peripherals page stays disabled for that dialog. The outstanding vault request still owns the reconnect guard because its cancellation cannot be guaranteed. A late completion releases the guard but does not remove the row or overwrite newer settings. Close and reopen Setup to retry after completion; restart the app if the backend never returns.

## Settings compatibility

This UI follow-up does **not** migrate the existing TGXL, PGXL, AG or ShackSwitch flat endpoint keys. They continue to be read and written in their existing format. Only the new device-list and discovery-dismissal preferences live in the existing nested Peripherals document. A one-way endpoint migration is separate work requiring its own data-compatibility review; this PR neither claims nor performs it. Adding a row preserves the existing explicit-Connect requirement.

## Implementation and validation boundary

The follow-up changes the Peripherals UI and the persistence/removal behavior needed to support it. It includes credential-store completion/status handling and TGXL/PGXL/AG connection guards; it is not a claim that only widget files change. It introduces no new authentication wire protocol, radio family, dependency, thread, or TX command.

UI preferences remain in the existing Peripherals settings document. Credentials remain in the existing OS credential store. The new core removal guard has no GUI dependency and exists only for a pending removal operation.

Local macOS validation includes a desktop build, eight focused tests, failure-producing regression mutations, and an isolated offscreen demo check in which Add and Remove each survived a process restart. These do not establish live peripheral firmware convergence, actual OS-vault behavior, Windows/Linux runtime behavior, or native accessibility. Detailed evidence is recorded in [the peripheral evidence document](4o3a-remote-auth-hardware-evidence.md).

AG and ShackSwitch share a credential slot. Remove checks the selected row’s
peer endpoint before deleting that slot; a record for the other endpoint is
preserved. Both rows use the reconnect lease and bounded deletion workflow.
If an offline hostname cannot be matched to the stored peer IP, removal fails
closed: connect the selected device to establish its peer identity, then retry.
No assumption is made that ShackSwitch firmware does or does not request AUTH.
Lowercase UI device IDs and the existing case-sensitive connection-object names
are separate persisted namespaces; their spelling is retained for compatibility.
