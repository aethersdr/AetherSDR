# Peripherals settings

**Radio Setup → Peripherals** configures the accessories AetherSDR talks to
directly: Tuner Genius XL (TGXL), Power Genius XL (PGXL), Antenna Genius (AG),
ShackSwitch, ACOM and SPE Expert amplifiers, VK3AMP, the LP-100A meter, and
the microHAM Trio antenna switch.

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

## Connecting automatically

TGXL, PGXL, Antenna Genius and ShackSwitch each have a toggle, on by default.
When it is off, nothing connects that device by itself: not startup, not
discovery or the radio reporting it, not the alternate-address attempt, and not
reconnect after a drop. **Connect** always works and does not change the toggle;
a session it starts is not reconnected after it drops.

- **TGXL and PGXL:** **Connect directly when available**. Off, the device is
  controlled through the radio's relay whenever the radio offers one (the applet
  shows RADIO); a live direct session continues until it drops.
- **Antenna Genius and ShackSwitch:** **Connect automatically**. These have no
  relay, so off leaves the device offline until you click **Connect**.

To keep a discovered device from connecting, keep its row and turn the toggle
off. The setting is stored as `Peripherals.<id>.AutoConnect` (`True` or
`False`) with the ids `tgxl`, `pgxl`, `ag` and `shackswitch`.

## Removing a device

**Remove** asks for confirmation first; Cancel changes nothing. Confirming
disconnects the device and clears its saved connection settings and stored
authorization code, and returns the device's toggle to its default. Remove
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
Antenna Genius leaves a ShackSwitch using the shared model alone. A row with
no address of its own can be removed while the shared slot holds the other
device's code. An address saved from the Antenna Genius applet while a deletion
is pending is newer than the Remove and is kept.

### Outcomes

| Outcome | Configuration | Credential | Reconnection | Recovery |
|---|---|---|---|---|
| Success | Settings and row removed; toggle back to default | Stored code deleted | Guard released; discovery may connect the device again unless its row is kept with its toggle off | **Add** re-creates the row |
| Failed (denied or error) | Kept; row stays | Saved code remains; error shown on the row | Connection stopped, guard released; normal reconnect events may connect it again | Retry **Remove** |
| Unconfirmed (15 s timeout), then late completion | Kept; the row's pending field edits are not saved | Deletion unconfirmed | Guard held until the request returns, then released; a late completion changes no row or newer setting | The Peripherals page stays disabled in that Setup window. Close Setup and reopen it to retry; restart the app if the backend never returns |
| No credential backend | Same as success | Session copy cleared; stored-code deletion unconfirmed, with a notice saying so | Same as success | Remove the code from the OS vault once it is available |
| Unknown owner (Antenna Genius, ShackSwitch) | Kept | Untouched; the owning endpoint is not known | Connection stopped, guard released | **Connect** the device to establish its address, then retry **Remove** |
| An earlier removal still pending | Unchanged | Unchanged; the earlier request owns the slot | **Remove** and **Connect** are refused with a notice | Retry after the keychain request finishes |

## microHAM Trio

The **microHAM Trio** row connects to the Trio antenna-switch controller's own
web interface (`ws://<address>/ui.ws`, port 80 by default) and shows what it
reports: the radio with transmit focus, its antenna (both halves in split), the
RX antenna, RX EXT and the PA loop, and the label of every radio and antenna
port. From this page AetherSDR switches nothing on the Trio. After the
handshake the session sends only keep-alives, WebSocket pongs and a close,
and the antenna commands of a pick made in the antenna menus (below). Every
name the Trio reports is shown as plain text, never as markup.

- The Trio is not discovered: enter the address its own menu shows (an IPv4
  address or a host name).
- **Connect automatically** (off: connect only on **Connect**) and the global
  **Reconnect automatically** behave as for the other devices.
- The Trio serves one web session. When another client takes it (most often
  the Trio's page in a browser) the row shows **Needs attention** and AetherSDR
  does not take it back by itself; **Connect** does.
- **Test Connection** takes the session once, reads the state, shows whether the
  Trio answered and the RADIO and antenna labels it read, and releases the
  session. It selects nothing and stores nothing; with the session already
  held it answers from that session.
- **Radio Ports** lists the Trio's three radio ports with their names; the
  marker **this AetherSDR** says which one the connected radio is cabled to
  (stored for that radio only; it changes nothing on the Trio).
- **Antenna Ports** lists the six antenna ports with their names, read-only.
- Names are shown as the Trio reports them; a port is renamed on the Trio's
  own web page (**Web UI**).
- **Web UI** closes AetherSDR's session first, then opens the Trio's page in
  the browser. **Connect** takes the session back.
- Stored as `Peripherals.trio.ManualIp`, `Peripherals.trio.ManualPort` and
  `Peripherals.trio.AutoConnect`; the radio's port as the radio-scoped feature
  `TrioBinding`.

### In the antenna menus

Once **this AetherSDR** is set on a radio port, the Trio is a station switch
of that radio: the slice's **RX ANT** and **TX ANT** menus keep the radio's
own ports at the top (on the HL2: ANT, and RF1 for TX) and add a submenu
**microHAM Trio (station switch)** with the Trio's six antenna ports. The
panadapter's RX antenna list has no submenu: there the Trio is a disabled
heading row with its ports under it. The field shows the port the Trio
reports, never the last pick.

- Split is implied, with no button of its own: an RX pick other than the TX
  port turns the Trio's split on with that RX port; an RX pick equal to the TX
  port turns it off; a TX pick moves RX with it while they are equal, and in
  split moves only TX (or ends split when it equals RX). The Trio changes over
  between the two halves on PTT in split; that hot-switch protection is the
  Trio's own (PTT is cabled to it).
- A pick takes up to three commands, each sent after the one before is
  confirmed by its reply. A pick the Trio does not take, or does not answer
  within two seconds, stops and says so, and the state is read again.
- Every pick is refused while the radio may still radiate: keyed, MOX, tuning,
  CW key down, the radio's interlock in any state but a settled receive one
  (on a Flex: PTT requested, transmitting, un-key requested and the fault
  states all refuse), or its transmit tail after un-key. It is
  asked again before every command of a pick. The sentence is the one every
  station switch uses: *Antenna can't change while transmitting.* Nothing here
  refuses a key-on.
- The submenu and its ports stay listed, disabled, with the reason, while the
  Trio is offline, another client holds its session, its focus is on another
  radio, or the radio port's label no longer matches the one stored; a TX
  port the Trio marks receive only, and every port until the Trio has
  finished the previous pick, are disabled the same way.
- The RX-only antennas (RX1-RX3) and RX EXT stay at the Trio.
- Only the `v` command form was recorded on the wire, sent by the Trio's own
  page. The split toggle and the `txv` / `rxv` half commands are built from
  the Trio page's own source and the state document's fields, and have not
  been sent to a Trio yet.

### Transport and protocol limits

**The protocol is reverse-observed, not published.** microHAM documents no
network protocol for the Trio. What AetherSDR speaks was read from the Trio's
own web page (`/ui.shtml`) and recorded on the wire against one unit; its
firmware version is not readable over the network, so no firmware version is
known to be covered.

**Transport.** One `QTcpSocket` at a time (`core/MicrohamTrioTcpTransport`)
carrying a minimal HTTP and RFC 6455 client (`core/MicrohamTrioWebSocket`):
`GET /ui.shtml`, whose response sets a `WID` cookie, then a second connection
upgrading `/ui.ws` with that cookie. QtWebSockets is not used: it is an
optional dependency of this build, and the Trio row must work without it. The
session (`core/MicrohamTrioSession`) takes bytes and time through injected
interfaces, so its tests drive it against a scripted peer; those tests show
what the code does with the recorded bytes, not how other firmware behaves.

**Observed on one unit:**

- The `WID` cookie is required: an upgrade without it is answered `400`. It is
  a session handle, not a credential.
- The complete state document arrives unasked right after the upgrade.
- A held session is pushed a complete state document when the front panel or
  band data changes a selection, the radio focus included (seen on a session
  held for an hour).
- The text `ping` is answered with the text `pong`; the Trio also sends an
  empty WebSocket pong about once a second. The 5 s no-frame bound below
  requires that pong: replies to AetherSDR's own `ping` alone do not hold it.
- One session only: a second connection closes the first at once.
- Selections are the controller's echo of its last selection, not relay
  position sense: the schema has no field for a relay that disagrees, and the
  reply to a selection comes back faster than a relay can move. The page shows
  them as the Trio's selections; a failed relay or cable reads the same.

**Assumed, not shown:** that other units and firmware use the same schema; that
a session longer than an hour stays up; how fast a change is pushed.

**What the code does with an unexpected answer:**

| Answer | Result |
|---|---|
| No WebSocket acceptance of our key, a redirect, or another status than `101` | Attempt failed; retried after a doubling backoff (5 s up to 10 min) while **Reconnect automatically** is on, else the session is closed |
| An HTML error page as the first frame, or no state within 6 s of the upgrade | Retried after 1.5 s, up to three attempts, then as a failed attempt |
| A state document with any missing, extra or mistyped group, key or field (an RX extension module included) | No partial reading: the attempt fails as unreadable and is retried as above |
| A frame the decoder refuses (fragmented, masked, reserved bits, over 16 KiB) | The connection is dropped as a protocol error and retried as above |
| No frame at all for more than 5 s (the Trio sends one every second) | Reported as no heartbeat (network): a failed attempt, retried as above |
| Frames arrive, but a `ping` goes unanswered for more than 10 s (as the Trio's own page) | The session is treated as dead: a failed attempt, retried as above |
| The held session closed by the Trio | Taken as another client holding it: not retaken, even with **Reconnect automatically** on; **Connect** takes it back. A Trio restart that closes the connection reads the same |

## Settings

The TGXL, PGXL, Antenna Genius and ShackSwitch endpoints stay in their own flat
keys (`TGXL_ManualIp` and so on). The device list, the global
**Reconnect automatically**, each device's toggle, and the
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
