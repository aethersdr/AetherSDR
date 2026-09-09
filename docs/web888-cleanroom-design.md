# Web-888 Clean-Room Design Note

Status: receive-only Web-888 support implemented as a **receiver family on the
existing KiwiSDR path** — no new protocol client, no new backend. The wire
authority is the open-source Web-888 server plus black-box observations against
a live receiver made in this thread.

## What Web-888 Is

Web-888 is a KiwiSDR server fork: the `RaspSDR/server` project (GPL, a clone of
`jks-prv/Beagle_SDR_GPS`) running on a Zynq SoC board. It speaks the same
WebSocket protocol as a KiwiSDR with small deltas, so AetherSDR serves it
through the same `KiwiSdr*` classes (`vendor(kiwi)` tagged — see
`docs/kiwisdr-cleanroom-design.md` for the Kiwi-side design note). A profile
records which family its endpoint serves
(`KiwiSdrProtocol::KiwiSdrReceiverFamily`) and the client applies the deltas
below; everything else is the shared Kiwi path.

## Allowed Inputs Used

- User request: add integration with a Web-888 SDR "related but not identical"
  to the KiwiSDR, with a live receiver offered for probing
  (`http://web888.servehttp.com:8074`).
- User-scoped decisions from 2026-09-08: ride the existing Kiwi-style applet
  path (explicitly not a new `IRadioBackend` family); scope is RX audio +
  spectrum/waterfall, tuning/remote control, and reuse of the Kiwi UI/applet;
  no public receiver directory; receive-only (Principle VI).
- Clean input: the `RaspSDR/server` repository (GPL open-source — open-source
  references are clean inputs per Principle IV). Its server sources were
  consulted only to confirm the wire contract AetherSDR already implements for
  Kiwi (auth, stream URIs, frame layouts) and to locate the fork deltas below;
  no Web-888-only feature outside the shared Kiwi receive contract was
  implemented from it.
- Black-box live observations against `web888.servehttp.com:8074` in this
  thread: HTTP `Server: ZynqSDR_Mongoose/2026.609`, `sw_version=Web888_v2026.609`
  in `/status`, the staged connect handshake on `/kiwi/<ts>/SND` and `/W/F`,
  and the `SET`/`MSG` traffic on both sockets.

## Verified Wire Compatibility

Observed identical to KiwiSDR on the live receiver (so all handled by the
existing, unchanged client code):

- Stream URIs `/kiwi/<ts>/SND` and `/W/F`; the same staged handshake.
- Auth: `SET auth t=kiwi p=<pwd>` (`p=#` when passwordless) and the same
  `SERVER DE CLIENT ... SND` / `... W/F` greeting strings.
- SND frames: 10-byte header, PCM16 and IMA-ADPCM layouts, sequence/RSSI
  fields; W/F frames: 16-byte header with the `0x00010000` compressed flag and
  zoom bits already handled by the Kiwi decoder.
- `/status` HTTP preflight: same tolerant `key=value` body (`rx_chans=13`,
  `wf_chans=13`, `zoom_max=11`, `wf_fft_size=1024`, `audio_rate=12000`,
  `center_freq=15360000 bandwidth=30720000`, `sw_version=Web888_v2026.609`).
- Default port: Web-888 also defaults to 8073 when the endpoint has no port;
  8074 is just an explicitly chosen port.
- `SET ident_user`, keepalive, AGC/squelch/mod command grammar: identical.

## Deltas And How Each Is Handled

1. **All server→client frames arrive as binary WS frames** — including the
   `MSG` control text Kiwi sends as text frames. AetherSDR's
   `handleBinaryMessage` already routed binary `MSG` frames to the text path;
   that dispatch is now a pure classifier
   (`KiwiSdrProtocol::classifyInboundFrameTag`, socket-free testable) and the
   client switches on it. Text frames are still accepted (per-family tolerant).
2. **W/F setup must be (re-)sent after the server's config burst.** Kiwi
   accepts `SET` waterfall commands immediately after connect; Web-888 stages
   its `wf_setup` config burst first and the setup commands sent before it are
   not applied. `KiwiSdrClient` therefore re-sends the waterfall setup sequence
   once after the first inbound W/F `MSG` — family-gated (Web888 only) and
   one-shot (`m_waterfallSetupResent`); a no-op for Kiwi, idempotent for
   Web-888.
3. **Server version markers.** `parseKiwiVersionFromServerHeader` now also
   recognizes `Web888_` and `ZynqSDR_Mongoose/` markers alongside `KiwiSDR_`,
   so the protocol summary shows the fork's version instead of "unknown".
4. **`audio_rate` in the SND config burst.** Web-888's burst is expected to
   carry the same `audio_rate` token the SND setup is staged on. If a live run
   shows `cfg_loaded` arriving without `audio_rate`, a diagnostic trace fires
   (`WEB888 cfg_loaded without audio_rate`); the family-gated fallback that
   promotes `cfg_loaded` is applied only if the live test proves the token is
   absent. No rate is fabricated.

Everything else — endpoint normalization, camp/monitor, EXT-ignore, waterfall
view/zoom clamps from server MSG values, TX mute (`KiwiSdrTxMuteLatch`) — is
profile-keyed and family-agnostic and needs no change.

## Implementation Shape

- `src/core/KiwiSdrProtocol.{h,cpp}`: family enum + name/id/fromString
  helpers; `classifyInboundFrameTag`; version-marker loop.
- `src/core/KiwiSdrClient.{h,cpp}`: `setReceiverFamily()` (set by the manager
  before the client thread starts); Web-888 waterfall re-send guard;
  family-named user-facing state text; binary-frame dispatch via the classifier.
- `src/core/KiwiSdrManager.{h,cpp}`: `KiwiSdrAntennaProfile.family`, persisted
  in settings JSON (optional key, legacy → Kiwi) and in the receiver-list CSV
  as an optional `RECEIVER_TYPE` column (`KIWI`/`WEB888`; absent → Kiwi, an
  unrecognized non-empty value is an import error). A family change reconnects
  the profile (joined with the existing endpoint-change trigger) and is pushed
  to the live client.
- `src/gui/RadioSetupDialog.cpp`: TYPE combo ("KiwiSDR"/"Web-888") on the
  configured-receiver rows and the add-receiver row.
- `src/gui/KiwiSdrApplet.{h,cpp}`: family on `KiwiSdrReceiverStatus`; a
  "Web-888" badge next to the receiver name and the family in the accessible
  text. `MainWindow_KiwiSdr.cpp` populates it.

## Non-Goals

- No TX of any kind (Principle VI) — the Kiwi path is already control-
  suppressed and TX-muted; Web-888 rides it unchanged.
- No `/EXT`, `/admin`, or `/MON` streams; no IQ/DRM 20-byte-header streams.
- No public receiver directory for Web-888 endpoints (out of scope per the
  user's scoping; Kiwi's directory feature is Kiwi-only).
- No new `IRadioBackend` family and no new vendor header (EB3 ratchet stays
  green; the touchpoint tags remain `vendor(kiwi)`).