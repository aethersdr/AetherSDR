# Peripherals settings

**Radio Setup → Peripherals** configures the accessories AetherSDR talks to
directly: Tuner Genius XL (TGXL), Power Genius XL (PGXL), Antenna Genius (AG),
ShackSwitch, ACOM and SPE Expert amplifiers, VK3AMP, and the LP-100A meter.

## Layout

A device list sits on the left and the selected device's settings on the right.
The list shows, for each device, its name, its address, and where its data
comes from:

| Word | Meaning |
|---|---|
| `● DIRECT` | Connected directly to the device. |
| `● RADIO` | Not connected directly, but the radio relays the device (TGXL and PGXL only). |
| `● OFFLINE` | No connection. |
| `Connecting…` | A connection attempt is in progress. |

These are the same words the TGXL, PGXL and Antenna Genius applets use. A device that needs the operator (a rejected code, a failed connection, a
credential problem) also shows **Needs attention**.

The detail page repeats the state in a status line that is always visible. It is
the device's accessible state: its name and description follow what it shows,
and assistive technology is told when they change.

## Adding a device

Choose **Add** and pick a device type. Add creates the list entry and selects
it. It does not connect the device and changes nothing else. A type that is
already in the list stays in the menu, disabled, with "(already added)" in its
text.

Enter the address and use **Connect**. The list persists across restarts.
Existing manual configurations seed the list the first time it is shown, and a
target saved from the Antenna Genius applet appears in Setup.

A TGXL or PGXL that the radio reports and that needs authorization can appear as
a temporary recovery row. Retrying its unchanged reported address does not save
it as a manual target; editing the address or port does.

## Connect automatically

TGXL, PGXL, Antenna Genius and ShackSwitch each have a **Connect automatically**
toggle, on by default. When it is off, nothing connects that device by itself:
not startup, not discovery or the radio reporting it, not the alternate-address
attempt, and not reconnect after a drop. **Connect** always works and does not
change the toggle.

To keep a discovered device from connecting, keep its row and turn the toggle
off. The setting is stored as `Peripherals.<id>.AutoConnect` (`True` or
`False`) with the ids `tgxl`, `pgxl`, `ag` and `shackswitch`.

## Removing a device

**Remove** asks for confirmation first; Cancel changes nothing. Confirming
disconnects the device and clears its saved connection settings and stored
authorization code, and returns **Connect automatically** to its default. Remove
does not stop discovery: a device the radio or the network reports may connect
again. A removed device that is blocked on authorization and reported again
brings its recovery row back.

For an authenticated device, Setup waits for the credential deletion and shows
that it is pending. A transient guard stops every route to a connection for that
device while it waits: explicit, automatic, alternate and the reconnect timer.
After 15 seconds Setup reports the deletion as unconfirmed and can be closed;
the guard is held until the keychain request returns, and a late completion
changes no row or newer setting.

Antenna Genius and ShackSwitch share one credential slot. Remove checks the
selected device's peer endpoint before deleting, and a record for the other
endpoint is kept. If an offline host name cannot be matched to the stored peer,
removal stops: connect the device to establish its address, then retry. Removing
Antenna Genius leaves a ShackSwitch using the shared model alone.

### Outcomes

| Outcome | Configuration | Credential | Reconnection | Recovery |
|---|---|---|---|---|
| Success | Settings and row removed; toggle back to default | Stored code deleted | Guard released; discovery may connect the device again unless its row is kept with Connect automatically off | **Add** re-creates the row |
| Failed (denied or error) | Kept; row stays | Saved code remains; error shown on the row | Connection stopped, guard released; normal reconnect events may connect it again | Retry **Remove** |
| Unconfirmed (15 s timeout), then late completion | Kept; the row's pending field edits are not saved | Deletion unconfirmed | Guard held until the request returns, then released; a late completion changes no row or newer setting | Close Setup and reopen it to retry; restart the app if the backend never returns |
| No credential backend | Same as success | Session copy cleared; stored-code deletion unconfirmed, with a notice saying so | Same as success | Remove the code from the OS vault once it is available |
| Unknown owner (Antenna Genius, ShackSwitch) | Kept | Untouched; the owning endpoint is not known | Connection stopped, guard released | **Connect** the device to establish its address, then retry **Remove** |
| An earlier removal still pending | Unchanged | Unchanged; the earlier request owns the slot | **Remove** and **Connect** are refused with a notice | Retry after the keychain request finishes |

## Settings

The TGXL, PGXL, Antenna Genius and ShackSwitch endpoints stay in their own flat
keys (`TGXL_ManualIp` and so on). The device list, the global
**Reconnect automatically**, each device's **Connect automatically**, and the
ACOM, SPE Expert, VK3AMP and LP-100A connection settings live in the nested
`Peripherals` document. Lowercase list ids and the case-sensitive
connection-object names are separate namespaces; their spelling is fixed.
Credentials stay in the OS credential store.

## Accessibility

- The detail status line is always visible; its name and description follow the
  state and change events are sent.
- A disabled Add entry carries its reason in its text.
- Every control has an accessible name; fields locked while connected say so in
  their description.
- A saved code is shown only on **Show**, is never exposed to the automation
  bridge, and is concealed again when the page changes.
