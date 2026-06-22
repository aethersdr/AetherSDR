# KiwiSDR Clean-Room Design Note

Status: receive-only client implemented from clean AetherSDR interfaces plus
black-box observations made in this thread.

## Allowed Inputs Used

- User requirements from the KiwiSDR receive-only integration request.
- User-provided waterfall adjustment input: the Kiwi waterfall exposes
  `wf cell` and `wf floor` settings, each adjustable from -30 dB to +30 dB.
- User-provided endpoint input rule: if the endpoint has no port, append port
  8073 and do not attempt fallback ports.
- User-provided applet UI requirements from 2026-06-18: move Kiwi Audio and
  Kiwi Waterfall buttons to the applet bottom, compact the waterfall controls,
  add a WF Rate slider, remove the Owned Slices label, render slice badges with
  the same slice letter/color convention as RX Controls, choose a non-active
  fallback slice for initial tracking without making it active, add
  Auto Connect, and persist these Kiwi applet settings across restarts.
- User-provided applet refinement from 2026-06-18: Kiwi applet slice badges
  should behave like RX Controls slice buttons, with the active badge filled
  and inactive badges outlined; the row outline/highlight should not enclose
  the slice badge; WF Rate should match the other slider widths; Auto Connect
  should be right-aligned beside the left-aligned Connect button.
- User-provided screenshot feedback from 2026-06-18: the Kiwi applet slice
  letters were clipped at the bottom, and the detail outline was too tight and
  overlapped the row text.
- User-provided audio-routing requirement from 2026-06-18: the Kiwi Audio
  toggle should mute the AetherSDR slice that Kiwi audio replaces, restore that
  slice when Kiwi audio is turned off, and leave other slices audible so Flex
  slice audio and Kiwi audio can be heard together through AetherSDR audio/DSP.
- User-provided regression report from 2026-06-18: NR2-related Kiwi static
  returned after the later audio-source blending changes.
- User-provided multi-endpoint RX antenna requirement from 2026-06-18:
  KiwiSDR endpoints are client-side receive-only antenna profiles that can be
  added and removed from Settings -> Antennas, each profile has
  connect/disconnect and Auto Connect controls, slice RX antenna menus can
  select a Kiwi profile without sending that profile name to the Flex radio,
  selecting a Kiwi profile silently replaces Flex audio/waterfall for that
  slice while keeping the slice mute icon available for the Kiwi audio, and
  multiple Kiwi audio streams must mix with normal Flex audio without
  reintroducing the NR2 static.
- User-provided follow-up requirement from 2026-06-18: when a slice selects a
  Kiwi RX antenna, the panadapter's FFT/spectrum trace should also be derived
  from the received Kiwi waterfall bins instead of continuing to show the Flex
  FFT trace.
- User-provided follow-up requirement from 2026-06-18: when a selected Kiwi
  profile disconnects, the panadapter should remain in Kiwi view and show a
  local "Not connected to KiwiSDR" overlay with the connection detail until the
  profile reconnects; Settings -> Antennas should show each Kiwi profile as a
  readable name/server entry; and the panadapter waterfall controls should map
  to Kiwi W/F cell, floor, and rate while a Kiwi antenna is selected, then
  return to normal Flex waterfall behavior when it is not.
- User-provided edge-case audit request from 2026-06-18: check muting,
  unmuting, volume control, band switching, and source switching behavior when
  either Kiwi or Flex receive audio/waterfall is active.
- User-provided macOS crash report from 2026-06-18 showing a shutdown crash on
  the main thread: `KiwiSdrManager::~KiwiSdrManager()` called
  `disconnectAll()`, a Kiwi state change reached `MainWindow::wireKiwiSdr()`,
  and the connected handler called back into `RadioModel::slice()` during
  teardown.
- User-provided screenshot/report from 2026-06-18: on a 20-meter Kiwi
  panadapter, zooming out left the Kiwi FFT/waterfall segment narrower than
  the visible panadapter and exposed odd horizontal lines in the side regions.
- User-provided report from 2026-06-18: switching between two Kiwi receivers
  left the panadapter showing the same waterfall data, indicating that rows
  from a previously selected endpoint or a shared Kiwi cache could still be
  displayed after selecting another receiver.
- User-provided edge-case request from 2026-06-18: when the same connected
  Kiwi receiver is selected on another slice/panadapter, that single receiver
  should move to the new slice's frequency/audio and the new panadapter's
  waterfall range, while the old panadapter stops showing that Kiwi receiver's
  waterfall.
- User-provided open protocol specification pasted on 2026-06-18, later
  corrected by the implementation-oriented draft pasted the same day:
  timestamp-style WebSocket stream paths ending in `/SND` and `/W/F`,
  `SET auth t=kiwi p=#`, `SET ident_user=...`, `SERVER DE CLIENT ... SND`,
  `SERVER DE CLIENT ... W/F`, `SET compression=0`,
  `SET mod=... low_cut=... high_cut=... freq=...`, AGC as the combined
  `SET agc=... hang=... thresh=... slope=... decay=... manGain=...` command,
  squelch initially as `SET squelch=... max=...`, waterfall positioning via
  `SET zoom=... cf=...` with compatibility `start=...`, `SET maxdb=... mindb=...`,
  `SET wf_speed=...`, `SET interp=...`, `SET keepalive`, tagged `MSG`, `SND`,
  `W/F`, and `EXT` binary/text dispatch, variable SND and W/F payload layouts,
  and display-only waterfall byte mapping.
  Binary frames are routed by the first three tag bytes so text, SND, and W/F
  frames can be accepted even if an endpoint multiplexes them on one socket.
  Remaining spec-derived receiver behavior is implemented only when it maps to
  an existing AetherSDR/Flex receive concept without adding Kiwi-only UI:
  auth-first identity setup, AGC defaults, squelch defaults, NFM mode mapping,
  SND RSSI/sequence telemetry, W/F sequence telemetry, and text metadata
  capture for users, reported frequency, ADC clipping, and GPS-good status.
  Attenuation, remote mute, PBT, extension launch, IQ streams, and compressed
  audio are not exposed because they would add Kiwi-only product behavior or
  require undefined wire details.
- Clean server-side parser check from 2026-06-21: the archived KiwiSDR server
  `rx/rx_sound_cmd.cpp` file carries a GNU Library GPL v2-or-later notice and
  was used only to verify the exposed squelch command contract. The accepted
  command shape is `SET squelch=<signed-dB-offset-or-0-off> param=<float>`;
  the older `max=` shape is retained by the server only as a compatibility
  no-op.
- AetherSDR keeps Flex Manual SQL on the app-wide 0-100 `squelch_level` scale,
  but Kiwi does not use Flex units. Manual Kiwi SQL follows the server's
  signed margin scale with a tapered UI map: slider `0` maps to `-99 dB`,
  slider `49` maps to `-1 dB`, slider `50` maps to `+1 dB`, and slider `99`
  maps to `+99 dB` relative to the Kiwi median RSSI noise-floor estimate.
  Because the server reserves `SET squelch=0` for open/off, enabled manual SQL
  normalizes any exact zero threshold to `+1 dB`; disabling SQL sends
  `SET squelch=0`. This gives the operator downward adjustment below the
  measured noise floor without conflating the lowest slider position with
  squelch-off. The Kiwi SQL line is drawn as a noise-floor-relative `N dB`
  line using the SND S-meter median, because that is the same value family the
  server uses for non-NBFM squelch. Manual SQL pins that floor when the user
  sets the threshold; Auto SQL lets it track. Waterfall-derived floor is only a
  fallback until SND meter samples arrive. Auto SQL uses the smaller direct
  `5..20 dB` margin because it is already operating in dB-margin units.
- Clean server-side AGC check from 2026-06-22: the `SET agc=... thresh=...
  manGain=... decay=...` parser forwards those values to the BSD-licensed
  CuteSDR AGC implementation. That implementation documents threshold as an
  AGC knee in nominal `-160..0 dB`, manual gain as `0..100 dB`, and decay as
  `20..5000 ms`. AetherSDR therefore stores Kiwi replacement AGC-T directly in
  dB (`-160..0`) instead of using Flex's `0..100` AGC-T scale.
- Kiwi's server command takes a raw decay time, not named presets. AetherSDR
  maps its existing AGC UI names to decay values inside the supported range:
  Fast `300 ms`, Med `1000 ms`, Slow `3000 ms`; Off sends `agc=0` with the
  manual-gain field from the AGC-off slider.
- Kiwi virtual receive starts from Kiwi-safe AGC defaults rather than copying
  the active Flex slice's AGC-T state: `SET agc=1 hang=0 thresh=-100 slope=6
  decay=1000 manGain=50`. Operator changes in the RX applet still update the
  Kiwi receiver controls while the Kiwi replacement source is active.
- Clean server-side send-path check from 2026-06-21: the LGPL-marked server
  sends an SND squelch UI flag for non-NBFM squelch state, but intentionally
  does not replace non-NBFM audio with silence. AetherSDR therefore honors that
  SND flag locally by gating decoded Kiwi audio before it enters the normal
  AetherSDR RX audio path.
- User-provided report from 2026-06-18: after protocol setup changes, some
  endpoints returned "sound connection closed" during connection setup, and
  `QWebSocketPrivate::processHandshake` reported HTTP 200 for the root-query
  paths from the earlier draft. The corrected user-provided spec says not to
  use `/?A` or `/?W`; use timestamp-style stream paths ending in `/SND` and
  `/W/F`. A clean black-box handshake check against the user's active endpoints
  confirmed that `/?A`/`/?W` returned HTTP 200/307, while
  `/<timestamp>/SND`, `/<timestamp>/W/F`,
  `/ws/kiwi/<session-id>/SND`, and `/ws/kiwi/<session-id>/W/F` returned
  HTTP 101 Switching Protocols. The runtime uses the corrected
  `/<timestamp>/SND` and `/<timestamp>/W/F` paths.
- User-provided protocol correction from 2026-06-18: SND binary frames have a
  four-byte `SND` + rolling sequence byte header; the first two bytes of the
  payload are the signed big-endian RSSI value and standard audio samples
  follow immediately after that as signed little-endian 16-bit mono PCM at
  12 kHz. IQ mode uses interleaved little-endian I/Q pairs at 20.25 kHz but is
  not requested by this receive-audio implementation. W/F frames use a
  four-byte `W/F` + sequence header followed by 1024 linear waterfall bins.
  The corrected spec later cautions that SND payload offsets vary by compatible
  client/server version. The runtime therefore accepts the corrected 6-byte
  little-endian PCM layout and the previously observed 1034-byte live-frame
  layout with a 10-byte header and big-endian PCM. Sequence gaps are counted
  only when the sequence byte is considered reliable; bounded missing W/F rows
  are duplicated to avoid display drift.
- User-provided protocol correction from 2026-06-18: Kiwi proxy hostnames may
  require retrying with `wss://` on port 443 when unencrypted WebSocket
  connection setup fails or closes.
- User-provided report from 2026-06-18: some endpoints showed a TLS host-name
  mismatch after the secure proxy retry, while others still showed "sound
  connection closed during setup." The secure retry is therefore limited to
  failures before any plain WebSocket transport is established. Once SND or W/F
  reaches the WebSocket connected state, later closes are treated as endpoint or
  protocol setup failures and are not converted into a secure-proxy retry.
- User-provided corrected implementation spec from 2026-06-18: do not use
  root query endpoints such as `/?A` or `/?W`; do not require
  `SET client_type=MicroKiwi`, `SET name=...`, `SET password=...`,
  `SET audio_on=1`, or `SET wf_on=1`; use `SET auth t=kiwi p=...`,
  `SET ident_user=...`, and the stream client marker commands instead.
- User-provided protocol correction from 2026-06-18: string values in `SET`
  commands must not contain spaces. The current runtime sends only sanitized
  callsign identity strings and fixed client labels without spaces.
- Server-side W/F source check from 2026-06-22: direct uncompressed W/F row
  bytes are wrapped negative dB values. AetherSDR decodes them as
  `clamp(byte - 255, -200, 0)` for vertical FFT placement, then applies local
  auto-aperture/color shaping without claiming Flex-calibrated RF dBm.
- User-provided screenshots from 2026-06-18 showed the panadapter overlay
  displaying a full URL-encoded `MSG load_cfg=...` record as a connection
  error. The corrected parser treats `MSG` records as key-value fields and only
  raises connection errors for actual top-level `too_busy=` or
  `reason_disabled=` fields. Configuration payloads such as `load_cfg` are not
  substring-scanned for error words.
- User-provided implementation-completeness warning from 2026-06-18: this
  implementation must not treat the high-level spec as a rigid binary ABI.
  Exact SND layout, W/F layout, compression, S-meter conversion, waterfall
  calibration, redirects, directory/proxy handling, and extensions remain
  version-sensitive. The runtime is therefore conservative: SND setup is
  split across the endpoint's `audio_rate` and `sample_rate` MSG fields, UI
  connected state remains event-driven around `audio_rate`/`audio_init` or the
  first SND frame, MSG fields are parsed independently, `badp`, `too_busy`,
  `down`, `reason_disabled`, `redirect`, and `camp_disconnect` become explicit
  local errors, unknown fields/tags are ignored, and unsupported compressed
  audio, calibrated S-meter, calibrated dBm waterfall, directory discovery, and
  extension behavior are not claimed.
- User-provided SND troubleshooting input from 2026-06-18: "sound connection
  closed" should be treated as an SND setup failure, and the useful reason is
  usually in MSG records before the close. The client now logs the exact SND
  and W/F WebSocket URLs, outbound SET commands with auth passwords redacted,
  every inbound MSG field, WebSocket close code/reason, and whether any SND
  binary frame was seen before close.
- User-provided additional fixes from 2026-06-18: `SET compression=0` and
  `SET wf_comp=0` are requests, not proof that incoming frames are
  uncompressed. The runtime logs SND and W/F frame shape, decodes only
  supported PCM/direct-row shapes, drops compact/encoded W/F rows, logs and
  ignores `EXT` or unknown binary tags, and does not expose calibrated S-meter
  dBm until the SND meter layout is independently verified.
- User-provided KiwiSDR meter subsystem specification from 2026-06-19:
  meter capability states, honest user-facing labels, unavailable-meter
  behavior, relative audio-level and waterfall-intensity meters, smoothing and
  peak-hold guidance, dBm-to-S-unit conversion utility, non-fatal extraction
  failures, and mandatory stubs for real SND meter extraction until the SND
  meter field layout and conversion formula are independently verified.
- User-authorized SND meter extraction from 2026-06-19, initially backed by
  short receive-only black-box SND probes against `22033.proxy.kiwisdr.com:8073`:
  observed 1034-byte frames with `SND` at bytes 0-2, byte 3 as flags, bytes 4-7
  as a little-endian rolling frame counter, bytes 8-9 as a big-endian signed
  candidate meter field, and big-endian PCM beginning at byte 10. The candidate
  field varied plausibly across 7.2 MHz LSB, 10 MHz AM, and 14.2 MHz USB probes.
  A later clean-room server-side check verified bytes 8-9 as the SND S-meter
  field encoded as `(dBm + 127) * 10`, so AetherSDR maps it as
  `raw / 10 - 127 dBm` with verified SND-meter capability for that frame
  layout. The user-facing meter surface uses the normal S-meter labels.
- Red-team review in this clean-room worktree found integration risks without
  using any Kiwi/WebSDR implementation source: unassigned profile connects could
  reuse stale slice tracking, exact 1024-bin W/F rows could be misread as older
  observed extended metadata, one WebSocket could fail while leaving the sibling
  socket alive, removed slices could leave Kiwi audio assignments enabled,
  endpoint edits could reuse saved waterfall rows from the previous endpoint,
  and NR2 toggles did not clear every Kiwi receive buffer. The implementation
  now treats those as local AetherSDR state-management bugs.
- Clean upstream AetherSDR `main` checkout.
- Project governance files read before implementation:
  `AGENTS.md`, `CONTRIBUTING.md`, `.specify/memory/constitution.md`, and
  `docs/a11y.md`.
- AetherSDR source interfaces inspected from clean upstream `main`:
  `AppletPanel`, `PanadapterApplet`, `SpectrumWidget`, `AudioEngine`,
  `SliceModel`, `RadioModel`, and `MainWindow` wiring declarations.
- Clean black-box observations against the user-approved endpoint
  `g8ure.ddns.net:8077`:
  - `HEAD /` returned `Server: KiwiSDR_1.842/Mongoose_7.14`; no body was read.
  - A headless browser network observer, without reading DOM, HTML,
    JavaScript, CSS, images, or response bodies, showed receive WebSockets at
    `/ws/kiwi/<session-id>/SND` and `/ws/kiwi/<session-id>/W/F`.
  - Observed receive-only text commands included `SET auth t=kiwi p=#`,
    `SERVER DE CLIENT ... SND`, `SERVER DE CLIENT ... W/F`,
    `SET AR OK in=12000 out=44100`, `SET mod=am low_cut=-4900 high_cut=4900
    freq=3615`, waterfall setup commands, and keepalives.
  - A single direct `SND` probe with `SET compression=0` produced binary
    `SND` frames of 1034 bytes. Earlier historical interpretation treated
    this as a 10-byte header plus 512 big-endian mono 16-bit samples at the
    reported 12 kHz audio rate; the later user-provided master reference
    supersedes this for runtime decoding with the 6-byte SND/RSSI header and
    little-endian PCM samples.
  - A single direct `W/F` probe with `SET send_dB=1` produced binary `W/F`
    frames of 1040 bytes, with a 16-byte header and 1024 one-byte waterfall
    bins. With observed `center_freq=15000000` and `bandwidth=30000000`, the
    row spans 0-30 MHz at zoom 0. The later server-side W/F source check
    confirmed the observed `byte - 255` interpretation for direct rows.
- Clean black-box follow-up observations against user-provided endpoints
  `sdr.hfunderground.com:8077`, `w0air.ddns.net:8073`, and
  `22033.proxy.kiwisdr.com:8073`, using only rendered UI screenshots and
  WebSocket frame metadata/text commands:
  - An early `sdr.hfunderground.com:8077` check accepted a password-free sound
    socket (`badp=0`) but produced no `audio_init` or `SND` frames while the
    rendered page reported all Kiwi channels busy and offered queue/camp
    actions. A later short receive-only direct SND probe against the same
    endpoint reported `sample_rate=11998.902083`, `audio_init=0
    audio_rate=12000`, and 1034-byte `SND` frames with a 10-byte header plus
    512 big-endian signed 16-bit samples.
  - `w0air.ddns.net:8073` produced `audio_init=0 audio_rate=12000` and
    uncompressed `SND` frames. With `SET agc=0 ... manGain=0`, sustained
    samples were effectively silent. A browser trace later sent receive-side
    AGC defaults `SET agc=1 hang=0 thresh=-100 slope=6 decay=1000 manGain=50`;
    using those values with `SET compression=0` yielded sustained nonzero
    12 kHz PCM samples.
  - A later direct receive-only `w0air.ddns.net:8073` timing probe with the
    implemented uncompressed command sequence observed 1034-byte `SND` frames
    containing 512 12 kHz samples. The average cadence was about 41 ms, but
    frames arrived in bursts with gaps over 100 ms.
  - A follow-up decode check on `w0air.ddns.net:8073` compared plausible
    header offsets and byte orderings from the received `SND` frames. Even
    offsets with big-endian signed 16-bit samples were coherent in that
    historical capture, but the later user-provided master reference
    supersedes the runtime decoder with the 6-byte SND/RSSI header and
    little-endian PCM samples.
  - A direct receive-only `22033.proxy.kiwisdr.com:8073` SND probe produced
    1034-byte frames and `audio_init=0 audio_rate=12000`. A continuity check
    over 180 frames historically supported a 10-byte SND header plus
    big-endian signed 16-bit mono PCM interpretation. The later user-provided
    master reference supersedes that runtime layout with a 6-byte SND/RSSI
    header and little-endian PCM samples.
  - A later receive-only SND static-analysis probe against
    `22033.proxy.kiwisdr.com:8073` observed `sample_rate` values near
    11998.9 Hz, `audio_rate=12000`, and 1034-byte SND frames. Header bytes
    4-7 formed a little-endian sequence counter, bytes 8-9 were non-audio
    metadata, and PCM still began at byte 10. The sampled PCM was not clipping.
    The existing AetherSDR frame-boundary ramp altered otherwise contiguous
    speech frames often enough, and by a large enough amount, to plausibly
    produce speech-correlated static. This pass therefore leaves contiguous
    SND frame joins unmodified and uses the observed audio rate metadata for
    the SND resampler rate.
  - A later receive-only SND follow-up against the same endpoint confirmed
    offset 10 had the best frame-boundary continuity among tested even PCM
    offsets 8, 10, and 12. The raw stream again had stable 1034-byte frames,
    `audio_rate=12000`, and no clipping. A direct WAV playback of this raw
    SND capture had no audible static, so the Kiwi adapter keeps decoded SND
    samples transparent before sample-rate conversion.
  - Direct receive-only W/F probes against `22033.proxy.kiwisdr.com:8073`
    showed that `SET wf_speed=20` echoed `wf_fps=0` and produced no W/F
    rows. `SET wf_speed=1`, `2`, `3`, and `4` produced rows at about 1, 5,
    13, and 23 fps respectively; values 5 and higher produced no rows in
    the observed sample. Zoomed W/F rows at `zoom=7 start=30` were 533 bytes:
    16-byte header, 5-byte zoom prefix, and 512 row bytes.
  - A later short W/F row-statistics probe against
    `22033.proxy.kiwisdr.com:8073` with `zoom=7 start=30`, `send_dB=1`, and
    `maxdb=-50 mindb=-130` observed a wide raw byte distribution:
    approximately p5=1, p50=67, p95=195, and max=252 over the sampled rows.
    This makes fixed full-window color mapping visually amplify normal row
    variation into speckles.
  - A follow-up receive-only W/F range-comparison probe against the same
    endpoint changed only `maxdb`/`mindb`, but returned no W/F rows in the
    short sample. No server-side display-range behavior is inferred from that
    no-row probe.
  - A short receive-only SND probe against `zl2j.proxy.kiwisdr.com:8073`
    accepted the WebSocket upgrade but returned `badp=5` and closed before
    sending audio for both `p=#` and empty public-password auth. A non-personal
    `ident_user=AetherSDR` probe did not produce audio either. The user then
    clarified that AetherSDR should send the callsign stored in the radio as
    the Kiwi identity; the client sends the sanitized radio callsign as
    `SET ident_user=<callsign>` after public auth on both SND and W/F sockets.
  - A short receive-only SND probe against `k9czi-1.proxy.kiwisdr.com:8073`
    accepted the WebSocket upgrade and returned `badp=5` for public auth
    before closing. With the current pre-auth identity order and a
    non-personal placeholder identity, the same endpoint sent only
    `sample_rate` before closing. The client now reports setup socket closes
    immediately instead of waiting for the generic audio-ready timeout.
    Follow-up probes using the radio-stored callsign observed in AetherSDR's
    own runtime log did not produce audio: `SET ident_user=<callsign>` before
    auth, after `SERVER DE CLIENT`, or after `SET AR OK` caused a close after
    `sample_rate`, while placing the callsign in the public auth field still
    returned `badp=5`. No further callsign-auth command is inferred from these
    observations.
  - A short receive-only SND probe against `162.199.177.108:8073` reproduced
    the app's setup-close symptom when `SET ident_user=<callsign>` was sent
    before public auth. Sending public auth first returned `badp=0` and normal
    setup status messages. The client therefore sends public auth before the
    optional callsign identity.
- User-provided screenshot and reports from local AetherSDR testing:
  - Kiwi audio reached the speaker path but was rapidly choppy/staticy.
  - Native Flex audio could retain background artifacts after switching away
    from Kiwi audio.
  - The Kiwi waterfall toggle showed real Kiwi W/F rows over old native
    waterfall history instead of fully replacing the displayed waterfall.
  - A short W/F-only probe against `sdr.hfunderground.com:8077` showed that
    `SET zoom=<n> start=<m>` changes the requested server-side waterfall span,
    and that returned W/F frame headers echo the requested `start` and `zoom`.
    For the 7.1 MHz test range, `zoom=8 start=60` and `zoom=8 start=61`
    produced row data. Higher zoom/start combinations could produce short
    21-byte W/F frames with no row payload.
  - Zoom-0 W/F frames were observed as 16-byte header plus 1024 direct row
    bytes. Earlier zoomed W/F rows from the 7.1 MHz probe were observed as a
    16-byte header, a short 5-byte row prefix, then 512 payload bytes. A later
    2026-06-18 probe showed that this 5-byte-prefixed form is compact encoded
    waterfall data, not direct one-byte-per-bin row data. The short 21-byte
    W/F frames match the header plus the 5-byte prefix only and are ignored.
  - A screenshot at 7.12-7.18 MHz showed that selecting the W/F `start` from
    only the active slice frequency can leave part of the visible panadapter
    outside the returned Kiwi segment. The W/F request must therefore choose
    the highest zoom whose `start` segment fully contains the current
    panadapter visible range.
  - A later short W/F-only probe against `sdr.hfunderground.com:8077` for the
    same 7.12-7.18 MHz view confirmed the client-side request shape:
    `SET zoom=7 start=30` was echoed by `MSG zoom=7 start=30`, and matching
    `W/F` frames returned 512 payload bytes after the observed 5-byte
    zoomed-row prefix. A later 2026-06-18 probe showed that rendering this
    compact encoded payload as direct bins produces visible speckle and does
    not display coherent received signals.
  - Later short W/F-only probes against `22033.proxy.kiwisdr.com:8073` and
    `sdr.hfunderground.com:8077`, plus a short paired SND/W/F receive-only
    probe against `g8ure.ddns.net:8077`, accepted WebSocket upgrades but
    returned no W/F rows in the short sample. No header or segment behavior is
    inferred from those no-row samples.
  - A later paired receive-only SND/W/F probe against
    `sdr.hfunderground.com:8077` using the same numeric session-id shape as
    the app produced W/F rows. With `SET zoom=7 start=16`, returned 533-byte
    frames echoed `start=16` and `zoom=7`: 16-byte header, observed 5-byte
    zoom-row prefix, and 512 compact encoded payload bytes.
  - A follow-up paired receive-only SND/W/F probe against the same endpoint
    confirmed that `SET zoom=9 start=121` also returned 533-byte W/F frames
    echoing `start=121` and `zoom=9`.
  - A later receive-only browser WebSocket observation against
    `sdr.hfunderground.com:8077`, without reading page source, served
    JavaScript, DOM, CSS, image assets, or response bodies, captured the
    rendered page's own outbound W/F zoom commands. Starting from the
    endpoint's reported 0-30 MHz W/F span, the page sent examples such as
    `SET zoom=1 start=1935951` with a reported marker range beginning at
    3461.750 kHz, `SET zoom=2 start=2903926` with a marker range beginning at
    5192.624 kHz, and `SET zoom=3 start=3387914` with a marker range
    beginning at 6058.062 kHz. Those starts match
    `round((visible_low_kHz / 30000 kHz) * 2^24)`, showing that W/F `start`
    is a 24-bit fixed-point low-edge offset within the full W/F bandwidth,
    not a small segment index.

## NR2 / Multiple Kiwi Audio Sources Regression Guard

Kiwi audio must not be mixed with Flex audio before NR2, and separate Kiwi
endpoints must not alternate through one shared NR2 state. The stable invariant
is: process Flex audio through `RxDspSource::Main` / `m_nr2`, process the legacy
single Kiwi stream through `RxDspSource::KiwiSdr` / `m_kiwiSdrNr2`, process each
virtual Kiwi antenna through that antenna's own NR2/resampler/output state, and
mix Flex plus Kiwi only after DSP. When NR2 is enabled, Kiwi packets stay as
whole packet-sized blocks until the audio timer processes them into post-DSP
FIFOs; Flex packets use the same packet-preserving rule for the main NR2 path.
The speaker timer must not re-chop raw source audio into timer-sized NR2 blocks.
This preserves the first clean-room checkpoint behavior that removed the
speech-correlated NR2 static without mutating output FIFOs from source callbacks.
The final post-DSP speaker mix applies strict active-source averaging before
clamping; post-DSP Flex and multiple Kiwi streams must not be summed full scale
or scaled only by `1/sqrt(N)` because clipping can sound like NR2 static when
three speech sources are active.
Kiwi enable/disable transitions clear only Kiwi buffers/state; they must not
reset `m_nr2` or other Flex RX DSP state.
The speaker timer fills each post-DSP FIFO independently; a buffered Kiwi FIFO
must not stop Flex raw audio from being processed into the Flex post-DSP FIFO.
Managed Kiwi RX antenna streams must keep that same per-source FIFO boundary
with NR2 disabled; they must not be collapsed into the legacy applet Kiwi
output buffer, because the final mixer only treats that buffer as active when
the applet-level Kiwi Audio toggle is enabled.

## Source Provenance

- The original KiwiSDR receive-path implementation used no KiwiSDR source code,
  AGPL repositories, served JavaScript, server code, decoder code, protocol
  handlers, copied snippets, blogs, forums, or implementation-derived assets.
- A later, source-attributed denial-message follow-up intentionally consulted
  the LGPL-marked KiwiSDR source repository
  `https://github.com/jks-prv/Beagle_SDR_GPS.git` at commit
  `efb38e2b25137029cb37a96a94e03366f8d2871e` to label terminal connection
  denial codes and MSG keys. No KiwiSDR code was copied, translated, or
  vendored; the AetherSDR handlers and user-facing strings are original.
  The consulted files were `rx/rx_cmd.h`, `rx/rx_cmd.cpp`,
  `support/stats.cpp`, `rx/rx_server.cpp`, `rx/rx_sound.cpp`, and
  `rx/rx_util.cpp`, each carrying GNU Library General Public License
  version 2-or-later headers in that source snapshot.
- The 2026-06-21 squelch follow-up used the LGPL-marked server files
  `rx/rx_sound_cmd.cpp` and `rx/rx_sound.cpp` only to verify the public command
  contract and SND squelch flag behavior accepted/emitted by the server. No
  KiwiSDR client code was copied, translated, or used to derive AetherSDR
  behavior.
- The 2026-06-22 SQL/AGC-T range follow-up used the same server parser file
  plus BSD-licensed CuteSDR AGC/squelch headers and implementation comments to
  verify numeric ranges. No KiwiSDR client code was copied, translated, or used
  to derive AetherSDR behavior.
- No WebSDR source code, prior WebSDR worktrees, PR #3612, prior WebSDR
  threads, contaminated temporary files, or rollout summaries were used.

## Clean-Room Boundary

The AetherSDR-side integration can be designed from the user requirements and
clean upstream AetherSDR alone:

- The side-dock UI is a normal `AppletPanel` applet wrapped by the existing
  container system.
- The current owned-slice list is available from `RadioModel::slices()`, with
  the active slice identified by `MainWindow`.
- Normal speaker audio enters `AudioEngine::feedAudioData()` as 24 kHz stereo
  float PCM and then passes through the client RX DSP chain before reaching the
  sink.
- Per-panadapter waterfall controls belong in the existing `SpectrumWidget`
  waterfall overlay control cluster, with waterfall pixels flowing through
  `SpectrumWidget::updateWaterfallRow()`.

Multi-endpoint KiwiSDR support treats Kiwi entries as AetherSDR-owned virtual
RX antenna profiles. These profiles are not radio antenna tokens and must not
be written through `slice set ... rxant=...`. The RX antenna menus append the
virtual profile labels on the client side and intercept those selections before
the radio command path. A normal Flex antenna selection clears the virtual
assignment and uses the radio's ordinary `rxant` command. A Kiwi profile
selection records a slice-to-profile assignment, starts or reuses that
profile's receive connection, points the Kiwi client at the selected slice's
frequency/mode/filter and panadapter range, and switches that panadapter to the
Kiwi waterfall stream. While that stream is active, the visible panadapter
trace is also sourced from the most recent Kiwi waterfall bins for the current
view. This is a display projection of received Kiwi W/F data only; it does not
derive spectrum data from audio and does not mix in Flex FFT bins.

The profile list is one feature-owned `AppSettings` object. Each profile stores
only client-side state: id, display label, normalized endpoint, Auto Connect,
and waterfall display/rate adjustments. It deliberately does not persist radio
frequency, mode, filter, or Flex antenna state because those remain
radio-authoritative. Startup Auto Connect opens only profiles marked for it;
shutdown disconnects every open Kiwi socket so an endpoint does not see a stale
duplicate session on the next launch.
When a selected Kiwi profile is disconnected or errors, the panadapter remains
assigned to that profile and shows a local connection overlay rather than
falling back silently to Flex waterfall data. Reconnection hides the overlay and
new W/F rows continue into the same Kiwi waterfall stream.

While a slice is assigned to a Kiwi profile, the Flex slice audio is muted only
as an implementation detail of replacing that slice's receive source. The UI
must keep the slice's speaker/mute controls available for the effective Kiwi
source rather than showing the Flex mute icon as the active state. The mute and
volume controls therefore route to the Kiwi source when a Kiwi profile is
selected, and route back to the Flex slice when a normal antenna is selected.

Multiple Kiwi streams are independent receive sources at the connection,
raw jitter-buffer, NR2, resampler, and post-DSP FIFO boundaries. The audio
engine must not mix Flex with Kiwi before NR2, and must not alternate separate
Kiwi endpoints through one shared adaptive NR2 state. Each virtual Kiwi antenna
therefore owns its own Kiwi NR2/resampler/output path. The speaker drain mixes
those post-DSP Kiwi FIFOs with post-DSP Flex audio.
Managed Kiwi RX antenna clients decode SND audio only while their profile is
the active audio source. Connected but inactive profiles may continue serving
waterfall data, but they must not spend CPU decoding/resampling unused audio
frames because that can starve the active NR2 path.

The KiwiSDR-side receive behavior is implemented only where it was observed
directly, except for the terminal denial-message labels called out in
[Source-Attributed Denial Messages](#source-attributed-denial-messages). The
client opens the observed `SND` and `W/F` sockets, sends only receive-side setup
commands, requests uncompressed audio, converts 12 kHz mono samples to 24 kHz
stereo float PCM, and projects received W/F rows onto the current AetherSDR
panadapter using the server-reported full W/F center and bandwidth.
The existing panadapter waterfall controls are source-aware: while a Kiwi
profile is selected they update the selected profile's W/F cell, W/F floor, and
W/F rate settings only; while a normal Flex antenna is selected they retain the
radio waterfall gain, black-level, auto-black, and rate behavior.
The requested W/F row must fully contain the current AetherSDR panadapter
range after the Kiwi fixed-point `start` value is quantized. When an exact
high-resolution row does not cover the viewport after quantization, the client
chooses the next wider row rather than displaying a narrower segment with blank
or stale side margins. It does not add an arbitrary safety margin to the visible
span, because direct Kiwi W/F rows have a fixed bin count and any extra RF span
reduces bins-per-Hz. Rendering of cached Kiwi rows treats columns with no
frequency overlap with the row as black, so a zoom-out cannot smear edge bins
into horizontal side-line artifacts.
When multiple Kiwi receiver profiles are connected, waterfall rows are accepted
for a panadapter only if the row's profile id matches the currently selected
Kiwi profile for that panadapter. Each Kiwi profile has its own cached
waterfall stream state, including the Kiwi-derived FFT trace, so switching
receivers does not display stale spectrum or waterfall history from a
different endpoint.
A single Kiwi profile is owned by at most one slice at a time. Selecting that
same profile on another slice clears the old slice/panadapter assignment,
restores that old slice's Flex audio state, moves the Kiwi client tracking to
the new slice frequency/mode/filter, and requests the new panadapter's visible
waterfall range.

SND startup is staged from endpoint messages. On socket open, the client sends
only auth, identity, and `SET compression=0`. When `MSG audio_rate=...` arrives,
the client sends `SET AR OK in=<audio_rate> out=24000`, matching AetherSDR's
Kiwi decoded-audio path. When
`MSG sample_rate=...` arrives, the client sends squelch, generator disable,
AGC, demodulator/tuning, and keepalive commands. Connection state is still
considered receive-ready only after `audio_init` or the first accepted `SND`
frame. Completing the `sample_rate` follow-up setup is not sufficient by
itself; if no SND frames arrive before the timeout, the connection reports that
sound setup completed but no SND audio frames arrived instead of presenting a
silent audio toggle.
If an already-established sound or waterfall WebSocket later closes or reports
a socket error, the client reports the local error and the profile manager
retries the profile after a short delay only when the profile is still
operator-relevant: Auto Connect is enabled or a slice is still assigned to that
Kiwi RX antenna. Server-declared terminal conditions such as `badp`, busy,
disabled, down, redirect, or camp disconnect are not retried by this local
recovery path.

## Source-Attributed Denial Messages

The source-attributed follow-up uses the LGPL-marked KiwiSDR source snapshot
identified in [Source Provenance](#source-provenance) only as a protocol
reference for terminal denial labels:

- `rx/rx_cmd.h` defines `badp` values 0-7 as OK, try-again/authentication
  rejection, still determining local IP, IP not allowed, no admin password set,
  no multiple connections, database update in progress, and admin connection
  already open.
- `rx/rx_cmd.cpp` and `support/stats.cpp` send `MSG ip_limit=<minutes>,<ip>`
  when a per-IP 24-hour usage limit is reached. This message shape was also
  observed on the wire, so AetherSDR keeps the defensive `ip_limit` parser and
  reports the server's minute value when valid.
- `support/stats.cpp` sends `MSG inactivity_timeout=<minutes>` when a connected
  client exceeds the server inactivity timer.
- `rx/rx_server.cpp` sends `MSG wb_only` for wideband-only servers and
  `MSG exclusive_use` when a receiver is locked for exclusive use.
- `rx/rx_sound.cpp` sends `MSG password_timeout` when password authentication
  times out.
- `rx/rx_util.cpp` sends encoded `MSG kiwi_kick=<count>,<message>` when the
  server kicks sound/waterfall/extension clients.

The runtime still treats these as terminal denial/disconnect states only. It
does not add new setup commands, does not infer extension behavior, and ignores
unknown MSG keys. `badp=5` is intentionally worded with both the source-derived
duplicate-IP meaning and the observed public-access/password hint, because
deployed receivers have returned that value during public-auth rejection.

Because the observed uncompressed `SND` payload is 512 mono samples at 12 kHz,
arrival cadence averages about 43 ms per frame but can bunch into burst/gap
delivery. The client therefore uses AetherSDR's existing resampler to convert
12 kHz mono to 24 kHz stereo, then holds a Kiwi-only jitter buffer before
mixing decoded Kiwi audio with the normal Flex RX speaker path.

Decoded Kiwi audio is queued as whole aligned 24 kHz stereo float chunks. When
NR2 is off, the speaker drain owns Kiwi-only jitter smoothing and drains each
raw Kiwi source into that source's own post-DSP FIFO. When NR2 is on, each Kiwi
packet remains whole until the audio timer processes it through its dedicated
Kiwi NR2 path and appends it to that source's post-DSP FIFO. This keeps NR2
adaptive state and output resampler state from interleaving Flex and Kiwi
cadences, and from interleaving different Kiwi endpoints. The final speaker
drain sample-mixes the post-DSP Flex FIFO, every post-DSP Kiwi FIFO, and
decoded RADE speech so other slices remain audible without pushing Flex-only
or zero-muted radio frames through Kiwi's NR2 state.
If the Kiwi FIFO runs dry, the client re-enters the Kiwi prebuffer state before
playing more Kiwi audio. Enabling or disabling Kiwi Audio clears the current
Kiwi source/output FIFOs and Kiwi DSP state at the transition so stale
pre-toggle audio cannot be mixed into the delayed Kiwi stream; it must leave
the Flex RX DSP state intact.

For SND decode rate, the client uses the observed 12 kHz default unless an
explicit `audio_rate` status supplies a plausible 8-48 kHz value. Generic
`sample_rate` status is not used for SND resampling because it can describe
other endpoint state; using it as audio rate can make one endpoint sound like
static while another endpoint remains clean.

Because Kiwi SND is already remote-demodulated audio, AetherSDR treats it as a
transparent alternate decoded source. A follow-up listening test showed the
remaining Kiwi speech-correlated static disappeared when NR2 was disabled.
Local inspection found Kiwi audio reaches NR2 as packet-sized bursts that can
be much larger than NR2's native 128-sample hop cadence. NR2 now splits
oversized `process()` calls internally so bursty Kiwi packets produce the same
output as the normal Flex RX cadence instead of wrapping the overlap-add ring
within one call.

When Kiwi Audio is enabled, AetherSDR also applies a client-owned mute to the
tracked AetherSDR slice that Kiwi is replacing. The previous mute state is
remembered and restored when Kiwi Audio is turned off or the Kiwi connection
ends. If the tracked slice changes while Kiwi Audio is active, the old slice is
restored before the new tracked slice is muted. This is deliberately limited to
the slice audio mute command; it does not tune, change mode/filter, transmit,
or otherwise alter Flex radio state.

Kiwi waterfall rendering uses only the received `W/F` socket rows. It does not
derive waterfall pixels from audio and does not use AetherSDR FFT bins for Kiwi
rows. Native and Kiwi waterfall image/history buffers are kept as separate local
stream states so toggling between them restores the prior waterfall instead of
clearing and starting over. Both streams continue ingesting rows while the other
stream is displayed.

When the user pans or zooms a panadapter, both the native and Kiwi waterfall
states are reprojected using the same local mechanism as the native Flex
waterfall, and a new `SET zoom=<n> start=<m>` request is sent immediately from
the local pan/zoom gesture whenever the pan contains the active owned slice and
KiwiSDR is connected. Matching W/F rows then continue the reprojected history.
A small horizontal and temporal smoother is applied only to Kiwi W/F rows to
reduce byte-level speckle without inventing waterfall data or using
audio-derived data.

The W/F socket is kept on a server-side span near the active pan/slice by
sending `SET zoom=<n> start=<m>` derived from the current AetherSDR
panadapter center/bandwidth and from the server-reported full W/F
`center_freq`/`bandwidth`. A 2026-06-18 clean black-box W/F probe observed
server setup text reporting `wf_fft_size=1024`, 1040-byte W/F frames with a
16-byte header and 1024 direct payload bytes after `SET wf_comp=0`, and W/F
headers echoing the requested 32-bit `start` plus one-byte `zoom`. A later
receive-only browser WebSocket observation showed that the rendered page sends
million-scale `start` values matching a 24-bit fixed-point low-edge offset
within the full W/F bandwidth. AetherSDR therefore treats `start` as
`round(((row_low - full_low) / full_bandwidth) * 2^24)`, not as a coarse
segment index. The chosen zoom is the narrowest row span that covers the
visible panadapter bandwidth after fixed-point `start` quantization, and
`start` centers that row on the visible RF range while clamping to the server's
full W/F span. After rounding `start`,
AetherSDR converts the sent fixed-point value back to an RF low edge and uses
that exact quantized row span when stamping incoming rows into the local
waterfall history. A request is recorded as current only after the W/F socket is
connected and the `SET zoom/start` command is sent; this prevents pre-connect
view calculations from suppressing the real setup command. Incoming W/F frame
headers are checked against the requested `start`/`zoom` so stale rows from an
older server span are dropped.

When AetherSDR's active slice changes, the Kiwi client is explicitly updated
from the new active slice and the `SpectrumWidget` attached to that slice's
panadapter. If Kiwi waterfall display was active on a different panadapter, the
visible Kiwi toggle is moved to the new active slice panadapter and the W/F
socket is sent that panadapter's current center/bandwidth-derived view. This
keeps cross-panadapter slice switching from leaving the remote W/F stream on
the previous panadapter range.

Kiwi waterfall row flow is server-paced. AetherSDR does not locally drop W/F
rows to imitate the Flex line-duration value because dropping rows causes the
stored Kiwi history and on-screen scroll rate to fall behind the native
waterfall. The W/F socket is sent an approximate monotonic speed request in
the observed working 1..4 range derived from the active AetherSDR waterfall
rate so larger Flex rates request faster Kiwi W/F delivery without disabling
row delivery on endpoints that return `wf_fps=0` for larger values.

Remaining uncertainties are deliberately conservative:

- Only password-free public receive access is supported.
- Audio uses uncompressed `SND` requested via `SET compression=0`; no
  compressed KiwiSDR audio codec is implemented. The user-provided protocol
  says an SND flag can indicate ADPCM, but does not define the flag bit or the
  compressed block framing, so AetherSDR does not guess a decoder path.
- A short receive-only SND timing probe against `w0air.ddns.net:8073` observed
  1034-byte uncompressed SND frames with about 42 ms average cadence, but burst
  delivery with a 275 ms max inter-frame gap in the sample. The Kiwi-only
  jitter buffer therefore targets 360 ms before draining to the speaker path.
- SND frames are decoded only across layouts independently observed in this
  clean-room thread. AetherSDR does not request Kiwi IQ mode in this
  implementation. AetherSDR does not smooth or ramp normal frame boundaries,
  because clean-room captures showed those joins are ordinary audio continuity
  points and the previous ramp could impose audible speech-correlated
  artifacts. When the SND or W/F sequence counter skips, AetherSDR applies
  bounded gap concealment by repeating the previous decoded audio frame or
  previous W/F row.
- Kiwi S-meter display uses the verified SND meter field for the independently
  observed 1034-byte frame layout described above. Unsupported layouts remain
  unavailable rather than guessed.
- Kiwi meter display is capability-aware. The SND metadata extraction path is
  explicit and must not silently guess unsupported layouts. Decoded Kiwi audio
  and W/F rows may still produce internal
  `relative_audio` and `relative_waterfall` readings for diagnostics, but those
  uncalibrated readings do not drive the user-facing VFO or applet S-meter. The
  existing Flex-style meter surfaces show either the verified SND meter or an
  unavailable readout. Flex `SLC/LEVEL` meter updates are ignored for
  slices currently assigned to a Kiwi virtual RX antenna, preventing local
  calibrated Flex dBm from racing the remote Kiwi meter state.
- Direct uncompressed W/F row bytes are decoded as wrapped negative dB values
  and color-mapped through a Kiwi-only automatic aperture with square-root
  contrast stretch across the active waterfall color scheme. This is
  client-side display normalization only: it does not synthesize waterfall data
  from audio and does not claim Flex-calibrated RF power. Earlier black-box W/F
  captures showed both extended 16-byte-header direct rows and compact encoded
  rows. AetherSDR requests direct uncompressed W/F rows and drops compact
  encoded rows until a clean decoder is available.
- The applet exposes Waterfall Cell and Waterfall Floor sliders using the
  user-supplied -30 dB to +30 dB range. The corrected protocol draft maps
  portable waterfall aperture control to `SET maxdb=... mindb=...`, so slider
  changes bias the profile's max/min dB settings sent to the endpoint. The
  same values are also applied as local display adjustments to the Kiwi
  waterfall renderer so the controls provide immediate visible feedback even if
  a public endpoint ignores or quantizes the setting command.
- The applet exposes WF Rate as a compact 0..5 control. Value 0 preserves the
  existing default behavior: derive the remote W/F speed from the active Flex
  waterfall line duration. Values 1..5 are user-requested fixed speed values
  sent as `SET wf_speed=<n>` so endpoint behavior can be tested without
  changing the default automatic tracking path.
- Kiwi RX antenna profile settings are stored as one
  `AppSettings["KiwiSdrRxAntennas"]` JSON object containing each profile's
  id, required display label, normalized endpoint, Auto Connect flag, WF Cell,
  WF Floor, and WF Rate.
- If AetherSDR has owned slices but no active slice when KiwiSDR tracking is
  initialized, the applet selects the first owned slice as a tracking-only
  fallback. It emits that slice to the Kiwi client so receive data can start,
  but it does not call AetherSDR slice activation or otherwise alter Flex
  radio state. The fallback row is marked with an outline/bar rather than the
  filled active-slice highlight.
- Kiwi waterfall rows use the same per-row retained-history model as the
  native waterfall. Each stored Kiwi row records the server RF span it was
  received from, and the visible row is remapped into the current panadapter
  frame. Live pan/zoom changes rebuild the visible waterfall from those
  row-level frames instead of stretching the current on-screen bitmap as one
  image. This avoids smearing older Kiwi rows when the operator zooms out while
  preserving the available data and leaving newly exposed frequencies black
  until fresh rows arrive.
- Kiwi W/F row smoothing is reset whenever the incoming row's server RF span
  changes. Consecutive rows can only be temporally blended when they describe
  the same `zoom/start`-derived frequency frame; otherwise strong carriers from
  an old frame would be blended into the new frame at the wrong horizontal
  position during pan/zoom gestures.
- The corrected user-provided protocol specification names `cf` in kHz as the
  W/F center-frequency request. Earlier black-box observations showed some
  public endpoints also accept a fixed-point `start` offset. AetherSDR sends
  the spec-defined `cf` and includes the previously observed `start`
  compatibility key while recording incoming rows against the RF span computed
  from the selected zoom and quantized start value.
- Unsupported AetherSDR demod modes fall back to AM while still tracking
  frequency.
- Kiwi `nfm` is used only when the AetherSDR slice mode is already FM/NFM.
  Kiwi `iq` is not requested because the current AetherSDR Kiwi audio path is
  remote-demodulated mono PCM, and treating IQ pairs as audio would corrupt the
  existing receive chain.
- The pasted spec includes attenuation, remote mute, `SET pbt=...`, and
  `SET ext=...`. AetherSDR does not expose those for Kiwi receivers in this
  implementation because there is no existing selected-Kiwi receiver control
  surface equivalent being reused here. Adding them would be a new Kiwi-only
  feature rather than parity with an existing Flex receive control.

## Implementation Shape

The first safe step is protocol-independent scaffolding:

- Add a `KiwiSdrApplet` with manual endpoint entry, connection state display,
  owned-slice tracking, active-slice highlighting, and a local audio-source
  toggle.
- Add an isolated `KiwiSdrClient` state owner whose connect path validates
  endpoint shape, normalizes pasted endpoints by removing leading
  `http://`/`https://` and trailing slashes, opens the observed receive
  sockets, and emits decoded audio and waterfall rows through AetherSDR-local
  signals. The last endpoint is saved only after a successful connection.
- Add a `AudioEngine::feedKiwiSdrAudioData()` entry point that accepts already
  decoded 24 kHz stereo float PCM and routes it through the same RX DSP and
  speaker path as normal AetherSDR audio. This method does not decode KiwiSDR
  data and does not alter Flex radio state.
- Add a per-panadapter Kiwi waterfall toggle in the `SpectrumWidget` waterfall
  overlay control cluster, visible only when `MainWindow` determines that
  KiwiSDR is connected and that the pan has an active owned slice. The toggle
  controls display routing state only.

## Non-Goals For This Scaffolding Pass

- No transmitted state, PTT, MOX, tune, or Flex radio mutations.
- No KiwiSDR wire commands or decoder behavior guessed from memory; every
  implemented wire command and frame interpretation above came from the
  black-box observations in this thread. The terminal denial-message labels are
  the source-attributed exception documented above.
- No reuse of WebSDR code paths or assumptions.
- No visual-theme or main-layout changes beyond the requested applet and
  per-panadapter toggle.
