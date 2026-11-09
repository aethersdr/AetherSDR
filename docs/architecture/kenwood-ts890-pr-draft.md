# Add experimental TS-890S Ethernet receive support

Adds direct Kenwood TS-890S KNS Ethernet control and UDP receive audio behind
`IRadioBackend`, without a USB cable or companion server. One physical receiver
uses A/B VFO registers. Radio readbacks own frequency, mode, filters, meters
and native settings; user requests do not fabricate confirmed state.

## Included behavior

- Ethernet login, endpoint/account-scoped credentials, receive audio/scope and owned reconnect/teardown.
- Existing frequency entry and tuning gestures, native FC/UD step controls, stationary Fixed scope tuning and admitted native spans.
- Agreeing native-relative meters, mode/filter controls and native receive DSP, including APF enable and its separate native bandwidth/shift/gain settings. Unsupported shared APF levels show `--` and stay unavailable with a reason.
- Existing exposed receive integrations and native setup pages, qualified by the validation matrix and limitations below.

The changes are confined to TS-890S integration and necessary neutral shared
capability/readback routes. Existing application gestures/defaults and other
radio behavior remain outside scope. Shared changes require regression review.

## Explicit limitations

- Native memories and memory scans are unsupported; their controls remain visibly unavailable and public verbs refuse without radio commands.
- Transverter operation is deferred. XV/XO writes are unavailable. Ordinary frequency/scope coordinates require confirmed transverter Off; an admitted On observation closes the session with an explanation and no automatic setting change.
- TX development, native transmit-input/power-limit configuration, physical TX tests and TX acceptance are deferred. Existing fences are preserved. Mixed VFO actions are not claimed as supported RX functionality.
- Advanced auto-mode tables, scan/preferences, operating display/beeps, CW/PSK decoder settings and detailed RX EQ/routing menus have an operator-accepted software-tested / hardware-unverified limitation. They are not hardware-passed.
- Native meters report uncalibrated display deflection, without invented antenna dBm or calibrated S conversion.
- Exhaustive reference-app/accessory parity is follow-up work. This contribution makes no full-parity or cross-platform hardware claim.

## Validation

The [bounded feature matrix](./kenwood-ts890-pr-ready/scope-and-validation.md) distinguishes executed checks from open gates and accepted limitations.

- Selected regression: 44 passed, one skipped, zero failed; four selected ASan/UBSan suites passed with leak detection disabled. The subsequent Scope-row change passed its dedicated behavioral regression. Three focused TCI suites also pass after restricting new state relays to confirmed receivers, with a failing-before/passing-after legacy-seed regression.
- Live deployed checkpoint: three representative native FC/UD actions, all 14 exposed mode selections and all eight CW spans converged through application controls and restored their initial state.
- Live CW filter/APF checks captured ambient-noise response and finite nonzero PCM for Off/On, bandwidth, shift and gain changes; recorded APF/filter/slice settings and host playback restored. Original persistent DATA SEND routing was not recorded around earlier PC Audio toggles and its restoration remains unverified. These are qualitative effects, not calibrated RF/filter/gain measurements.
- Checkpoint native Metal rendering and static agreement between meter locations were inspected. APF unsupported shared levels render `--`.
- Final M5 app/aetherd candidate builds and passes arm64, bundled-dependency and local-signature checks; user documentation builds. No full-suite, Windows, Intel or older-macOS claim.

**Submission remains gated:** the unverified DATA SEND routing restoration needs an explicit operator disposition. The newest candidate still needs native scope/tuning gesture checks, moving-meter proof, remaining control/integration qualification and the bounded 12-minute endurance with two reconnects and clean teardown. The operator has taken the radio offline; those checks are incomplete. An exact local source/artifact manifest and commit-file list are prepared; they must be refreshed for any later source or evidence changes before submission.

Corrections include APF wire/readout handling, safe transverter deferral, early FC frequency ordering with coherent completion, admission of backend-declared Scope modes, and confinement of new TCI status relays to confirmed receive policy. Shared GUI changes have targeted regression coverage and require maintainer review with the integration.

The generated touchpoint manifest adds two model headers: `NetworkRadioLogin` stages endpoint-scoped credentials and `NetworkRadioLookup` owns bounded, cancelable IPv4 lookup. The native pages consume existing neutral `RadioModel` extension/capability routes (includers 58→79); meter consumers use `MeterModel` (7→9). These are necessary model-facing UI dependencies; no concrete Kenwood wire header is included above the seam. Base/head architecture gates retain identical per-file violations after line-number normalization, with no baseline growth or exemptions.

The optional `kenwood_network_test` transport diagnostic is default OFF. It binds loopback TCP on an ephemeral port, UDP 60001/60002 and an ephemeral UDP sender; unavailable binds skip with exit 77. It is not firmware acceptance. New TCI protocol tests inject client/backend sinks and do not listen on WebSocket sockets. Capability-derived TCI frequency limits apply to every backend declaring nonzero limits, with neutral-profile regression coverage.

No screenshot files are included. Radio Setup/navigation/search captures may be stale after adding native pages and have not been refreshed; local TS-890S images require station-data redaction before inclusion.

## Handoff

Local preparation only. No upstream PR, push or maintainer contact is authorized
by this draft. Commit signing remains a separate final preparation gate; no
unsigned commits will be authored. The installed `/Applications/AetherSDR.app`
is untouched. Final maintainer review remains required for the integration.
