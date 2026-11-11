# TS-890S Ethernet receive backend

This experimental fork adds direct TS-890S Ethernet receive operation behind
`IRadioBackend`. It needs no USB cable or companion control server. One physical
receiver uses the radio's A/B VFO registers; VFO B is not a second receiver.

The [bounded feature and validation matrix](./kenwood-ts890-pr-ready/scope-and-validation.md)
defines the first PR's scope and outstanding acceptance work. The
[user guide](../user/docs/kenwood-ts890.md) describes connection and controls.
Historical research is retained locally; exhaustive reference-app parity is
follow-up work. Final current-source hardware, endurance and package validation
remain required; this document does not claim they have passed.

## Backend boundary

`src/core/backends/kenwood/` contains the KNS transport, socket-free protocol
parser and native control implementations. `KenwoodBackend` is built into
`libaethercore`; models expose normalized capabilities, requests, deltas and
PCM/spectrum frames to the existing application. GUI code does not construct
Kenwood wire commands or include concrete backend headers.

Control sockets and bounded timers run on the backend owner's thread. The
implementation adds no worker thread or external runtime. Session generations
fence delayed callbacks and readbacks. Fatal transport/protocol failures revoke
connected and PCM authority before notifying observers; teardown retires timers,
sockets, pending controls and audio producers. Reconnection reads fresh state
rather than replaying radio-owned settings or local transmit intent.

## Existing-backend compatibility

Pending tuning, receive-client dialect/readback policy and presentation use
explicit per-feature opt-ins. Only this backend currently declares them;
established Confirmed-policy backends do not acquire the new behavior. Native
setup navigation is admitted by an extension page catalogue. Relative axis labels
and native Scope help are explicit presentation data, preserving other backends'
existing display. The compatibility contracts are described in the
[capability map](./radio-capabilities-map.md#receive-compatibility-and-native-presentation).

## Connection and credentials

KNS uses semicolon-delimited TCP control, normally on port 60000, with
`##CN`/`##ID` authentication. Login, TCP establishment, model identification and
response liveness have bounded deadlines. The backend requires TS-890S identity
`ID024` before enabling native operation. Standby or restricted timer operation
retires the session with an explanation; connection never powers the radio on.

The connection flow resolves an IPv4 endpoint and honors the selected source
interface. It exclusively binds receive-audio UDP port 60001 and reserves the
existing transmit-audio source port 60002. Another session owning those ports
causes a reported connection failure rather than shared audio consumption.

Passwords are session-only by default. Explicit **Save password in keychain**
consent permits an OS-keychain write after successful identification. Saved
entries are scoped to endpoint, TCP port, username and administrator role.
Connection metadata uses the owned `KenwoodConnection` settings document;
passwords are excluded from ordinary persistent/exported settings. Revision and
endpoint checks prevent delayed login/lookup results from replacing newer
operator choices. KNS is not an encrypted transport; use a trusted network or
an independently secured network path.

## Receive state and tuning

Radio readbacks own frequency, mode, filter, meter and native-setting state.
A successful socket write records a request, not a confirmed radio change.
Admission checks include current session, physical source and relevant receive,
TF-SET and local-intent conditions. Pending drafts and stale observations cannot
restore retired control authority.

Existing frequency entry and tuning gestures use the application's normal
routes. Explicit native FC/UD actions let firmware apply its own tuning steps,
then read back the result; they do not predict a frequency or change existing
wheel, keyboard or client STEP behavior. Normal tuning and scope coordinates
require a confirmed transverter-Off observation.

Native setup pages expose the supported receive controls through existing
model/extension routes. Mode-dependent and unsupported controls remain visible
but unavailable with accessible reasons. APF uses firmware-observed `AP00` for
Off and `AP01` for On, translated to stable internal choices. The unsupported
shared 0–100 APF adjustment shows `--`; native bandwidth, shift and gain remain
separate controls. Detailed experimental pages retain the qualifications in the
bounded matrix and user guide.

## Scope, meters and audio

The bandscope decodes bounded `##DD2` frames containing 640 relative levels.
Native mode, span and edge observations determine geometry. Fixed mode retains
its bounds while the VFO marker moves; Center and Auto-scroll retain their
native semantics. Unknown or unsupported geometry is not fabricated.

Meters use native display deflection in dots in both UI locations. Spectrum
levels are relative. Neither path invents calibrated antenna dBm or a calibrated
S-unit conversion. The retained [firmware 1.01 span observation](./kenwood-ts890-rx-parity/hardware-span-2026-11-03.json)
is a bounded historical measurement with restoration, not final acceptance of
all scope behavior or later source revisions.

Receive audio uses RTP payload type 96 with 16 kHz signed little-endian mono
PCM. The backend validates peer address, header and payload bounds, tracks
sequence/restart discontinuities and resamples through the existing typed PCM
path. Speaker and slice consumers retain existing playback, recording and
receive-integration routes. Historical audible/recorded receive evidence does
not replace final-bundle recovery and endurance checks.

## Explicit limitations

- Native normal/program/extended/quick memories and memory scans are unsupported.
  Their public verbs refuse without radio commands; the unavailable surface
  explains the limitation. Existing host bookmarks are unaffected.
- Transverter operation and full RF-coordinate projection are deferred. XV/XO
  changes are unavailable. A current admitted transverter-On observation retires
  the session without changing radio settings; turn it off at the radio before
  reconnecting for ordinary receive operation.
- TX development, physical TX tests and TX acceptance are deferred. Existing
  experimental code and operation/session fences remain; no receive acceptance
  result establishes TX support. Mixed RX/TX VFO actions are not included as
  supported RX functionality. Reconnect never restores keying or queued TX audio.
- Advanced native configuration pages have the operator-accepted software-tested,
  hardware-unverified limitation recorded in the bounded matrix. This is not
  permission to alter persistent radio settings during validation.
- Other Kenwood models, accessory-app parity, Windows/Intel Mac hardware behavior
  and unavailable platform-specific credential/device checks are not claimed.

## Protocol provenance and review

The clean sources are Kenwood's TS-890S PC Control Command Reference (2019),
KNS Setting Manual and instruction/in-depth manuals, public open-source
implementations, and explicitly qualified radio observations. Kenwood's
[official download index](https://www.kenwood.com/i/products/info/amateur/software_download.html)
provides the manuals. Public reference projects include
[tonyc/open890](https://github.com/tonyc/open890),
[aneitzel/open890rc](https://github.com/aneitzel/open890rc) and
[w9fyi/open890](https://github.com/w9fyi/open890).

Firmware behavior can differ from the older command enumeration. The APF
0/1 wire mapping is independently corroborated by
[Open890RC's pinned parser/encoder](https://github.com/aneitzel/open890rc/blob/10c87c51628482cd1626a1aa0dc3da7c6a787c4d/internal/cat/messages.go#L171)
and [Open890's pinned control mapping](https://github.com/tonyc/open890/blob/9d134e5e5c060a35366357c8b09c6ebb3b267164/lib/open890_web/components/buttons.ex#L459),
and by local firmware 1.01 readbacks. No proprietary binary inspection is a
source for this contribution.

Socket-free parser, backend/model and real-widget tests remain in the product
test graph. The opt-in loopback transport diagnostic is not firmware acceptance.
Final test results, physical checks, limitations and artifact identities belong
in the bounded matrix and PR handoff. Local historical inventory validators
and their three newly added CTest registrations are excluded from the first PR;
no existing upstream test gate is removed. Maintainer review of the new backend
and necessary shared integration remains required before upstream acceptance.
