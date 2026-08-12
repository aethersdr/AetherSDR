# TCI Receiver Index Policy

_Changed in [e49875b2](https://github.com/aethersdr/AetherSDR/commit/e49875b2) (#2140)._

## Overview

AetherSDR exposes Flex 6000-series slices to TCI clients (WSJT-X, JTDX,
etc.) as numbered **receivers** (`trx` indexes in the TCI protocol).
Since #2140 these indexes follow a contiguous numbering scheme rather
than passing through raw Flex slice IDs.

The receiver-index policy is only one half of the routing contract. Channel
0/1, split, TX ownership, and acknowledgement ordering are defined in
[TCI Routing and Ordering Contract](tci-routing-ordering.md).

## Rules

1. **Dense initial `0..N-1` indexing.**
   Each newly seen slice is assigned the lowest free receiver index. If you
   initially own slices with Flex IDs 1 and 3, TCI advertises `trx_count:2`
   and maps them to receivers 0 and 1. Raw Flex IDs never appear on the wire.

2. **Live indexes are stable.**
   A receiver binding is pinned to its Flex slice ID. A band-stack
   destroy/recreate holds that binding through the settle window, so the
   recreated slice returns on the same receiver and surviving slices do not
   silently move. A genuinely closed slice frees its receiver after the
   settle window; a later slice fills the lowest hole. `trx_count` remains one
   plus the highest held/live receiver, so a temporary hole never invalidates
   a receiver a client is already using. Clients should still rediscover the
   mapping on a new radio session.

3. **Legacy-client fallback.**
   `TciProtocol::sliceForTrx()` includes a compatibility path: if the
   requested TRX index is out of the `0..N-1` range, it searches for a
   slice whose raw Flex `sliceId()` matches.  If that also fails it falls
   back to the first owned slice.  This keeps older clients that cached
   raw Flex IDs functional in the common single-slice case.

   **The first-slice fallback does not apply to paths that key the radio.**
   `resolveSliceForTrxStrict()` performs the same positional and raw-id
   resolution but returns `nullptr` instead of guessing, and
   `TciServer::handleTrxRequest()` uses it: an unresolvable receiver is
   declined with `trx:<n>,false;` rather than transmitting on a slice the
   client never addressed, on that slice's band and antenna (#4547).  A
   guess is a reasonable answer for a read and an unacceptable one under
   PTT.

4. **A client's declared audio receiver identifies it.**
   Every WSJT-X instance in TCI/ESDR3 mode addresses `trx:0`, so with two
   instances on two slices the wire request carries nothing that tells
   them apart.  `TciServer::effectiveTrx()` resolves a client's PTT
   against the receiver it declared in `audio_start:<n>`, falling back to
   the wire index when it declared none (control-only clients such as the
   Stream Deck plugin).  Replies still echo the trx the client sent — the
   binding changes which slice is addressed, never the wire shape.

   **Only `trx:0` is redirected.**  The declared receiver is evidence of
   intent, not an address that outranks one — and it is good evidence
   exactly where the wire carries none: `trx:0` is the ambiguous default
   every WSJT-X instance sends whatever receiver it operates.  A non-zero
   trx is a deliberate address and is honoured as sent.  A client that
   declared audio on receiver 0 and then asks for `trx:1` means receiver 1;
   overriding it there would key a slice the client never asked for, on
   that slice's band and antenna — which is #4547's own defect class, and
   the fix must not re-enter it.

   This follows Thetis, which scopes RX-audio enabled-receiver sets per
   client while radio state stays global (see the oracle's shared-versus-
   per-client table).  That split is also why the declaration informs the
   ambiguous case rather than overriding an explicit one: it is per-client
   audio evidence being read, not per-client radio state being asserted.

5. **Two channels per receiver.**
   AetherSDR advertises `channels_count:2`. Channel 0 is the receiver's RX
   slice. Channel 1 is the resolved radio-global TX slice for that RX route.
   The route uses stable Flex slice IDs internally even if public TRX indexes
   shift after topology changes.

6. **Eight commands carry no receiver index at all.**
   `cw_macros_speed`, `cw_keyer_speed`, `cw_macros_delay`, `mon_volume`,
   `mon_enable`, `cw_terminal`, `digl_offset` and `digu_offset` are global —
   the value is the *first* argument, not the second. So is `volume`, and
   `mic_level` / `tx_gain` are the AetherSDR extensions with the same shape.

   This matters because the dispatcher derives GET-versus-SET from the
   argument count (`isSet = args.size() >= 2`), which is the *trx-prefixed*
   shape every per-slice verb uses. On a global verb that derivation is wrong
   in both directions, and all eight carried the bug until #4867: the spec
   form `mon_volume:50;` was read as a GET and the value silently discarded,
   while `mon_volume:0,50;` was taken as a SET whose value was read from the
   trx slot, setting the monitor gain to `0`. Both broadcast a well-formed
   notification, so neither the sender nor a second client could tell.

   These handlers therefore re-derive GET/SET from the argument list
   themselves — **empty args = GET, otherwise SET reading the last
   argument** — and both wire forms are accepted, the spec one and the legacy
   trx-prefixed one.

   **`mon_volume:0;` is a SET of 0, not a read.** A client written against
   the trx-prefixed shape of the neighbouring verbs may send it meaning "read
   receiver 0's monitor gain"; it will instead zero the monitor and the
   operator will hear it. This is deliberate: silently refusing a legitimate
   `0` — an ordinary thing for an operator to ask for — would be the same
   undetectable-from-the-wire failure pointed the other way, and the client
   can always read the value back. Send the bare `mon_volume;` to read.

7. **Booleans are spelled `true` and `false`, and nothing else is accepted.**
   Unparseable boolean arguments are dropped, exactly as unparseable numeric
   ones are: `mute:0,yes;` used to become `false` and *unmute* the slice.

   The two exceptions are `tune` and `keyer`, which key the transmitter.
   There, anything that is not the word `true` still means **stop / key up**,
   because dropping an unparseable stop would leave a keyed transmitter keyed.
   Those two fail closed rather than fail silent (Constitution VI).

8. **`cw_macros` numeric receiver indexes fail closed.**
   The spec form is `cw_macros:<trx>,<text>;`. A base-10 integer first
   argument is always interpreted as a receiver address: an index in the
   currently advertised `0..N-1` range is stripped before the text reaches
   the radio, while an out-of-range, negative, or stale index drops the
   command. It is never reinterpreted as CW text, because that would put the
   address on the air (#4997).

   A nonnumeric first argument is retained as text for compatibility with
   clients that omit the index. This is deliberately asymmetric: an
   index-less macro beginning with a decimal number is ambiguous on the wire,
   so transmit safety and the documented receiver-prefixed form win. The
   parser fails closed rather than risk either keying the address or silently
   removing a legitimate numeric payload (Constitution VI/VII).

## IQ Stream Subscriptions

TCI IQ subscriptions are scoped by both client and receiver. One WebSocket may
send `iq_start:0;`, `iq_start:1;`, `iq_start:2;`, and `iq_start:3;` to run four
band skimmers concurrently. Each receiver maps to the corresponding 1-based
Flex DAX IQ channel (`trx 0` to DAX IQ 1, through `trx 3` to DAX IQ 4), and
type-0 binary frames carry that receiver index in their header.

The radio-side stream is shared. AetherSDR creates at most one DAX IQ stream
per receiver, keeps it while any TCI client remains subscribed, and removes it
when the last subscriber stops or disconnects. A stream that was already
owned by the DAX IQ applet is borrowed rather than removed by TCI cleanup.
`iq_stop:<n>;` affects only that receiver for that client; the client's other
band streams continue uninterrupted.

`iq_samplerate` is the shared achieved rate for active TCI IQ receivers. A
valid change is applied to every active DAX IQ stream and remembered for
streams started later. Logical subscriptions survive a radio reconnect and
are re-armed against the same stable receiver/pan binding when its slice
returns. The pan binding is required because the DAX IQ channel is centered on
the pan reported by `dds`, not merely on its slice frequency.

## Spot Click Notifications

When a visible spot is clicked, AetherSDR broadcasts the click to every
connected TCI client using both protocol spellings:

- `clicked_on_spot:<callsign>,<frequency_hz>;`
- `rx_clicked_on_spot:<receiver>,0,<callsign>,<frequency_hz>;`

The receiver is the same contiguous `trx` index used by `vfo:` and
`modulation:` events.  The channel field is `0`, matching AetherSDR's
single-VFO path for a slice.  This mirrors Thetis behavior and keeps older
clients such as Log4OM working while giving TCI v2 clients receiver context.

Both spellings are emitted **unconditionally** for every spot click — there
is no client-capability handshake.  This is the v2 protocol baseline
introduced by #3145; third-party log clients writing TCI protocol parsers
should expect to see both messages back-to-back for every click, not just
the legacy `clicked_on_spot:` form.

## Why this changed

Flex slice IDs are radio-global and not necessarily contiguous within a
single client's owned set.  TCI's `trx_count` / receiver model assumes
`0..N-1` numbering.  Passing raw IDs caused WSJT-X to address
non-existent receivers when another client owned slice 0, breaking
multi-slice TCI operation (TCI1/TCI2).
