# The app serving aetherd's local control socket (`--control-socket`)

Status: **implemented**, maintainer ruling 2026-11-15 (RFC #3849 family).

## Why

On the ANAN G2 the front panel's knobs and keys are read by `g2paneld`, an
AetherConsole daemon that speaks aetherd's local v1 control protocol
([`aetherd-control-protocol-v1-design.md`](aetherd-control-protocol-v1-design.md)).
While the kiosk's chooser is showing, a headless aetherd holds the radio and
`g2paneld` drives it. When the operator starts AetherSDR, the app takes the
radio, aetherd stops, and until now nothing read the panel: by the 2026-11-12
ruling there is no panel code in AetherSDR.

Making the app a client of the on-radio aetherd instead would need the binary
data plane and engine-side transmit arbitration (RFC steps 4 and 5), and the
G2's own screen would lose its spectrum, audio and transmit until then. This
is the smaller step: while the app holds the radio, **the app serves the same
local endpoint aetherd serves**, and `g2paneld` drives whichever program holds
the radio. One program controls the radio at any time, and the panel code
stays in AetherConsole.

## What it serves

`AetherSDR --control-socket <name>` serves the endpoint aetherd's
`--socket <name>` would (same path rule, same current-user-only policy,
`LocalControlServer`). Off unless the option is given. The G2 kiosk passes
`--control-socket aetherd-v1`, aetherd's default name, so `g2paneld` needs no
second address.

`src/core/control/AppControlHost` binds, for the radio of the window's first
radio session (`radio-1`):

- the **receive targets aetherd binds** with `--control`
  (`makeModelSliceFrequencyTarget`, `makeModelReceiveControlTarget`): slice
  frequency, mode, filter, audio gain and mute, AGC threshold, NB, NR, RIT,
  pan center, bandwidth and RF gain, CW pitch and the radio's speaker, each
  offered only where the connected radio's capability records offer it;
- the **resources** (`RadioResourceAdapter`): `radioSession`, `slice`,
  `panadapter`, telemetry.

It does **not** offer:

- **`radio.connect` or `radio.disconnect`.** The app's connection panel owns
  the connection. The host's connection target only observes it: Connected
  exactly while the model is, and its connect and disconnect do nothing. It is
  never bound to the service, so neither method is advertised, and a client can
  neither take, drop nor reconnect the app's radio. Destroying the host leaves
  the connection as it is.
- **Transmit verbs.** No transmit target is bound.
- **A radio catalogue.** The app's discovery is not projected.

If the name is already being served (an aetherd still running), the app logs
that the endpoint was not served and carries on without it.

## The G2 kiosk sequence (AetherConsole)

1. Chooser showing: `g2-aetherd` holds the radio; `g2paneld` drives it.
2. AetherSDR chosen: the kiosk stops `g2-aetherd` only. `g2paneld` loses its
   session and keeps retrying the endpoint. The app starts with
   `--control-socket aetherd-v1`, connects the G2's radio itself, and serves the
   endpoint; `g2paneld` reconnects to it and finds the radio already connected,
   so it sends no `radio.connect`.
3. AetherSDR exits: the endpoint goes with it, the kiosk starts `g2-aetherd`
   again, and `g2paneld` reconnects to it as before.

`g2paneld` must therefore not pull `g2-aetherd` in by itself
(`Requires=`): a panel restart while the app holds the radio would otherwise
start a second Protocol-2 controller.

## Tests

`tests/app_control_host_test.cpp`, over a real current-user socket: a second
host on a name in use is refused; a subscribe naming the absent catalogue
succeeds; tuning waits for the app's own connection; with it, a client tunes
the app's slice; connect, disconnect and transmit verbs are not offered and
`radio.disconnect` leaves the radio connected; after the app disconnects,
tuning is refused and nothing reconnects; destroying the host neither
disconnects nor keys the radio. Binding the observing target as the service's
connection target (the mutation) fails three of these checks.
