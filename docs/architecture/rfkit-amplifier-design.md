# RF-KIT amplifier — design note

Status: implemented; **not validated on real hardware**.

## 1. Scope

A direct-connect peripheral for the RF-KIT amplifier, alongside ACOM, SPE
Expert, VK3AMP and the Elecraft KPA1500. It has no FlexRadio awareness: the
radio neither discovers it nor relays its telemetry, so it is a
`peripheral(rfkit)` accessory, not an `IRadioBackend` implementor, and it
deliberately does not route through `AmpModel` (the same reasoning as the ACOM
and KPA1500 notes: a shared model would make the PGXL panel appear whenever an
unrelated peripheral connects).

## 2. Protocol authority

The vendor's published "RFKIT API and SDK Documentation" (OpenAPI 0.9.0), a
manufacturer document and therefore a clean-room input (Constitution Principle
IV). Nothing here is reverse-engineered. Anything the document does not state
is listed in §7 rather than guessed.

The wire is HTTP/1.1 with JSON bodies, unauthenticated, on a port the document
shows as 8080.

| Method | Path | Use |
|---|---|---|
| GET | `/info` | connect handshake; device name, firmware versions |
| GET | `/power` | forward / reflected / SWR / temperature / voltage / current |
| GET | `/data` | band, frequency, free-text status |
| GET | `/tuner` | tuner mode, setup, L / C (read-only) |
| GET | `/antennas` | every antenna with `ACTIVE` / `AVAILABLE` / `DISABLED` |
| PUT | `/antennas/active` | select an antenna |
| GET / PUT | `/operate-mode` | `OPERATE` / `STANDBY` |
| GET | `/operational-interface` | `UNIV` / `CAT` / `UDP` / `TCI`, plus a read-only `error` |
| POST | `/error/reset` | clear the error state |

`GET /antennas/active` and `PUT /operational-interface` exist in the API and are
deliberately not used (§5).

## 3. Layers

- `core/RfkitProtocol.{h,cpp}` — stateless: one parser per endpoint (bytes in,
  `std::optional` out) and one body builder per writable endpoint. Covered by
  `tests/rfkit_protocol_test.cpp`.
- `core/RfkitConnection.{h,cpp}` — owns the `QNetworkAccessManager`, the poll
  schedule, the request queue and reconnect. Covered by
  `tests/rfkit_connection_test.cpp`, a loopback mock of the documented HTTP API
  (the case `docs/agents/tests-ci.md` allows).
- `gui/RfkitApplet.{h,cpp}` — the applet, category *Amplifiers*, id `RFKIT`.
- Wiring in `MainWindow_Wiring.cpp`; the Peripherals row in
  `RadioSetupDialog_Peripherals.cpp` (settings group `Rfkit`: `ManualIp`,
  `ManualPort`).

## 4. Connection model

HTTP has no connection to be "up", so:

- `connectNetwork()` issues `GET /info`. The link is **connected** only when
  that returns an RF-KIT-shaped document. A 404, an HTML page, a non-JSON body
  or an oversized body fails the connect (`connectionFailed`), so pointing the
  field at a router or another web server does not half-connect.
- **Disconnected** after three consecutive transport failures (refused,
  timeout, reset, oversized). An HTTP error *status* (4xx/5xx) is the device
  answering, so it is logged and does not count as link loss.
- Requests are **strictly serialized**, one in flight. Commands jump the queue;
  a poll round is only enqueued when the queue is empty, so a slow device
  lowers the poll rate instead of growing a backlog. The device is a small
  embedded controller.
- Poll: `/power` and `/data` every 250 ms, plus one of `/operate-mode`,
  `/antennas`, `/tuner`, `/operational-interface` per tick in rotation. A full
  refresh runs once on connect so the applet fills immediately.
- Each reply carries the session epoch it was issued under; a late reply from a
  previous session is discarded unread (#2380).
- Boundary hygiene (Principle VII): bodies capped at 64 KiB (also enforced
  while streaming), no redirects, no proxy, no cache, a 4 s transfer timeout on
  every request, and the address field is built into a `QUrl` in strict mode so
  a stray `/`, `@` or space cannot change the request target.

## 5. What the operator can and cannot do

Controls: OPERATE/STANDBY, antenna selection (internal 1–4, external 1–16,
range-checked before the wire), and error reset behind a confirm dialog (an
error latch exists to protect the hardware).

Never latched optimistically (Principle II): every control asks, and the
applet repaints only from the device's own reply. The external-antenna combo
snaps back to the last confirmed state immediately after a pick.

Deliberately absent:

- **Transmit keying.** The API has no keying endpoint; nothing here can put RF
  on the air (Principle VI).
- **Operational interface changes.** `PUT /operational-interface` could cut the
  amplifier off from the radio that is driving it, silently. It is shown, not
  switchable.
- **Tuner control.** The API exposes the tuner read-only.

## 6. Applet behaviour

- Gauges scale to the device's own `max_value`; until it is known a 1500 W
  starting scale is used. Reflected power and SWR are held at baseline below
  1 W forward, as in the other amplifier applets.
- An unset field renders as "—", never 0; disconnecting drops every remembered
  reading.
- Frequency is formatted from the unit the device reports (Hz / kHz / MHz); an
  unrecognized unit is shown as given rather than mis-scaled. Temperature is
  converted if the device reports °F.

## 7. Open points — to check on real hardware

The API document is silent or ambiguous on these; the implementation chooses
the conservative reading and each is a one-line change:

1. The unit strings actually sent for `band` and `frequency`.
2. The vocabulary of `/data`'s `status` text. It is displayed verbatim; it is
   not parsed into a fault state.
3. Whether the device sends `max_value` on `reflected` and `swr` in practice.
4. The `false` value in the tuner `mode` enum (a generator artifact in the
   document). Accepted as a boolean or the string `"false"` and shown as `OFF`.
5. Whether `POST /error/reset` should be offered only while an error is
   present. Today the button is always enabled when connected, and the device
   answers 409 when the function is disabled.
6. Real poll latency: if the device cannot sustain 250 ms rounds, the
   serialization above degrades the rate gracefully, but the interval may want
   raising.
