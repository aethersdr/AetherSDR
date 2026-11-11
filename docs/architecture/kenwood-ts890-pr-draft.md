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
- Native filter-bank resets, network audio levels, AGC/receiver options and TCI/CAT/rigctl/host recording retain the same explicitly operator-accepted software-tested / hardware-unverified limit for this first PR.
- Native meters report uncalibrated display deflection, without invented antenna dBm or calibrated S conversion.
- Exhaustive reference-app/accessory parity is follow-up work. This contribution makes no full-parity or cross-platform hardware claim.

## Shared integration boundary

The new backend uses shared factory, credential, model, meter and capability interfaces. Additional receive behavior is controlled by explicit per-feature records declared only by Kenwood, rather than the broader existing Confirmed policy. Existing backends retain their original wheel paths, client dialects/limits, meter cache policy, applet visibility/colors and Scope help. Native setup pages are admitted only by the backend's page catalogue.

Connection cancellation retains native-login ownership guards when entering or leaving that new login path. Ordinary legacy queued connects and disconnected-error behavior retain their established policy. Shared QPointer lifetime checks remain to stop callbacks after synchronous destruction; they do not add radio features.

The final isolation fixes passed 28 selected suites with one environment-dependent
IPv6 source-path skip, plus four ASan/UBSan suites with leak detection disabled.
Independent GUI and connection reviewers found no remaining blocker in the
fixes. Regressions cover native-to-legacy APF presentation, late slice binding,
Local/SmartLink takeover, saved-family completion, queued native cancellation and
reservation lifetime/reentrancy. Two connection guards were mutation-checked.
Native M5 app/aetherd and documentation builds pass. November 9 hardware checks
use a separately staged isolated candidate. A subsequent Kenwood-only span fix
prevents a settled repetition of a zoom request from reversing the native span;
shared zoom gestures are unchanged. All eight spans passed settled live button
checks, with a failing-before/passing-after backend regression, four focused
passing suites and two passing ASan/UBSan suites (leak detection disabled).

## Validation

The [bounded feature matrix](./kenwood-ts890-pr-ready/scope-and-validation.md) distinguishes executed checks from open gates and accepted limitations.

- Selected regression: 44 passed, one skipped, zero failed; four selected ASan/UBSan suites passed with leak detection disabled. The subsequent Scope-row change passed its dedicated behavioral regression. Three focused TCI suites also pass after restricting new state relays to an explicit receive-client opt-in, with a failing-before/passing-after legacy-seed regression.
- Live deployed checkpoint: three representative native FC/UD actions, all 14 exposed mode selections and all eight CW spans converged through application controls and restored their initial state.
- Live CW filter/APF checks captured ambient-noise response and finite nonzero PCM for Off/On, bandwidth, shift and gain changes; recorded APF/filter/slice settings and host playback restored. November 9 span-fix candidate APF Off/On also restored with nonzero raw/post/final PCM. The operator explicitly accepted DATA SEND LAN as intended after read-only observation; the unrecorded original route remains unknown. These are qualitative effects, not calibrated RF/filter/gain measurements.
- Checkpoint native Metal rendering and static agreement between meter locations were inspected. APF unsupported shared levels render `--`.
- The November 9 span-fix candidate builds app/aetherd and has 81 arm64 Mach-O files, verified bundled dependencies and a valid deep/strict local signature. Its maximum observed dependency minimum is macOS 27.2. No full-suite, Windows, Intel or older-macOS claim.

The corrected current-candidate endurance run completed 720.87 seconds, with 12
exact tune/restore cycles, two owned reconnects, 707 changing waterfall samples
and five finite nonzero raw/post/final PCM captures. Both reconnects recovered
native tuning, scope and captured playback audio within seven seconds. Final
readbacks verified the owned RX baseline before normal Quit and clean process
exit. This is bounded firmware 1.01 evidence, not continuous drop-free playback
or leak-free proof.

**Before submission:** complete signed-commit/fork-push handoff. Final file-set
review retained the audited 220 tracked paths and excluded untracked research
and raw captures. The remaining experimental-group hardware limits were
explicitly accepted by the operator. The operator accepted the meter topic as working;
the completed rerun also measured native LEVEL changing from 0 to 32 dots, with
maximum age 307 ms. No new visual needle-motion measurement is claimed. The
DATA SEND disposition is resolved by explicit acceptance of LAN. An earlier
timed attempt stopped at its first post-reconnect native-context check and
recorded one response-time outlier; it is retained as incomplete evidence, not
an endurance pass. Source/artifact manifests and the commit-file list must be
refreshed after final evidence changes.

Corrections include APF wire/readout handling, safe transverter deferral, early FC frequency ordering with coherent completion, admission of backend-declared Scope modes, and confinement of new TCI status relays to an explicit receive-client feature. Shared GUI changes have targeted regression coverage and require maintainer review with the integration.

The generated touchpoint manifest adds two model headers: `NetworkRadioLogin` stages endpoint-scoped credentials and `NetworkRadioLookup` owns bounded, cancelable IPv4 lookup. The native pages consume existing neutral `RadioModel` extension/capability routes (includers 58→79); meter consumers use `MeterModel` (7→9). These are necessary model-facing UI dependencies; no concrete Kenwood wire header is included above the seam. Base/head architecture gates retain identical per-file violations after line-number normalization, with no baseline growth or exemptions.

The optional `kenwood_network_test` transport diagnostic is default OFF. It binds loopback TCP on an ephemeral port, UDP 60001/60002 and an ephemeral UDP sender; unavailable binds skip with exit 77. It is not firmware acceptance. New TCI protocol tests inject client/backend sinks and do not listen on WebSocket sockets. Capability-derived TCI frequency limits require an explicit receive-client opt-in; established backends retain their previous client responses.

No screenshot files are included. TS-890S Radio Setup/navigation/search captures have not been refreshed; established-radio navigation retains its previous entries; local TS-890S images require station-data redaction before inclusion.

## Handoff

Local preparation only. No upstream PR, push or maintainer contact is authorized
by this draft. Commit signing remains a separate final preparation gate; no
unsigned commits will be authored. The installed `/Applications/AetherSDR.app`
is untouched. Final maintainer review remains required for the integration.
