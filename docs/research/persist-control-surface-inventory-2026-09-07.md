# RadioCert Persist control-surface inventory

September 7, 2026. Expansion of the first FLEX-8400M run; this is a coverage
inventory, not evidence that the additional controls have been tested.

## Coverage matrix

| Group | Settings to cover | Context and failure cases to resolve | Existing bridge evidence / first-run coverage |
|---|---|---|---|
| Power | RF power and Tune power setpoints; maximum-power limit where exposed | Per-band / TX-profile ownership; ordinary quit; values must not be confused with measured forward power | `get transmit`: `rfPower`, `tunePower`, `maxPowerLevel`. Not exercised by persist. |
| Receive audio | Slice AF gain, balance/pan, mute; client master volume/mute; radio headphone/line-out levels where available | Per-slice versus global; source replacement; slice recreation; startup device readiness; muted/zero values | Slice radio/effective gain/pan/mute fields exist; first run used only mute as an untouched sentinel. Master/radio-output contracts need audit. |
| Squelch | Off/manual/auto intent, manual threshold, radio squelch enable/level; FM-specific semantics | Preserve manual threshold through auto/off and mode changes; display state must follow the replacement slice | `get slice` has radio/effective SQL fields. UI intent and saved manual threshold need separate observation. Not exercised. |
| AGC | Mode, AGC-T, AGC OFF level | OFF changes the meaning of the level control; mode/band/slice changes must not overwrite the other remembered value | `get slice` has effective/Flex mode, threshold and OFF-level fields. Not exercised. |
| Radio RX DSP | NB and level; NR and level; ANF; APF and bandwidth; WNB and level; supported family-specific algorithms | Mode applicability; per-slice versus pan state; on/off preserves remembered level; delayed startup replay | Existing slice/pan snapshots provide many values. WNB startup ordering observed, but distinct-value persistence not exercised. |
| PHONE source and gain | Microphone source/profile, mic gain, accessory input, boost, bias, DAX selection | Source/profile can intentionally recall a different gain; missing input must not silently rewrite saved intent | `get transmit` exposes most source/gain fields. Boost/bias exist in TransmitModel but are missing from that snapshot. Not exercised. |
| PHONE shaping | AM carrier level; radio speech-processor enable/level; downward-expander enable/threshold; TX low/high cuts | Mode gating; microphone/TX-profile dependencies; disabled levels survive toggles; unrelated status must not reset custom settings | `get transmit`, with capability-gated DEXP and TX-filter fields. Not exercised. |
| VOX and timing | VOX threshold/delay; CW delay; radio TX delay and applicable accessory output delays | Each delay needs its own units, owner and scope. No generic “delay” setting. VOX enable is a separate arming contract | VOX/CW delays exposed; hardware delay fields need targeted observation audit. VOX enable must not be turned on under the current no-TX authorization. |
| Monitor and CW audio | Phone MON enable/volume; CW sidetone enable/volume/pan; CW pitch/speed | Phone and CW monitor values must not bleed; test mode switch and source/profile changes | `get transmit` includes separate phone/CW fields. Not exercised. |
| CW configuration | Iambic enable/mode, paddle swap, CWU/CWL behavior; break-in intent | Radio-owned versus client keyer; actual Radio Setup route versus applet route; dialog reopen and restart | Most fields exposed by `get transmit`. Break-in/keying changes require separate safety applicability; no keying tests in RX-only run. |
| Radio EQ | Independent RX/TX enable flags and all eight gains: 63, 125, 250, 500 Hz, 1, 2, 4, 8 kHz | RX/TX selector; distinct asymmetric curves; bypass/re-enable; profile/mode/band/restart; verify inactive curve is untouched | `get equalizer` already exposes both complete curves. Entire domain absent from the persist snapshot/runner. Not exercised. |
| Client DSP and CHAIN | Client EQ; compressor, gate/expander, de-esser, reverb, tube; NR2/RN2/NR4/DFNR and available alternatives; stage enable/order/presets/parameters | Client store and feature scope, not Flex radio persistence; bypass retains configuration; another applet/status event must not rewrite it | Some `get dsp` / client-chain surfaces exist; per-stage coverage needs inventory. Not exercised. |
| VFO details | Tuning step; RIT/XIT offset and enable; lock; filter preset and adaptive-filter intent | Mode-family context, transient versus retained state, same-frequency new source; active/TX ownership is not automatically a setting to replay | SliceModel has step/RIT/XIT/DAX values that are absent from the current slice snapshot. First run covered mode/filter tuples only. |
| FM/repeater | Tone mode/value, receive tone, DTCS code/polarity when supported, offset direction/magnitude, reverse | Capability and mode gating; destination-context recall; return to simplex; no keying needed for setpoint readback | Core tone/offset values exposed; additional UI/model fields need audit. Not exercised. |
| Audio routing / digital | PC input/output selection, saved unavailable-device intent, DAX/TCI channel/gain/routing, sample-rate choices | Device reconnect; stable identity versus index; per-slice routing; digital/phone mode handoff | Existing audio/DAX/TCI diagnostics are distributed, not a complete persistence contract. Not exercised. |
| Profiles and memories | Global/TX/mic profile selection and their documented members; BandStack and memory-bank contents/annotations | Profile load is a compound transition, not a neutral trigger. Preserve each touched context and intentional shared state | Existing profile/memory views; not tested. Profile loads that could arm VOX or another TX source are excluded from current RX-only mutations. |
| Applet presentation | Visible/hidden, order, selected tab/page, expanded state, dock/float geometry; meter display/smoothing preferences where configurable | Close/reopen applet, layout switch, disconnect/restart; temporary layout must not erase durable intent | `dumpTree` plus scoped settings/document observations; first run did not exercise these. |

The first live run only established bounded observations for mode/filter,
selected Display fields, RX antenna context and restart. Capturing a value in a
snapshot is **not** exercising its persistence contract.

## Contract and scenario design

Every control gets a stable setting ID with owner, scope, applicability, actual
UI action route, units/range, semantic dependencies, fresh-authority observation,
expected behavior at each transition, and a context-aware restore action. Scope
must be verified per setting and radio capability; the groups above do not imply
that every member is globally shared or stored per band/profile.

For example, model a disabled processor's remembered level separately from its
currently effective processing. Model manual SQL intent separately from an
automatically computed radio threshold. Model phone MON and CW sidetone as
separate values. For EQ, capture both RX and TX curves even while editing only one.

Use three distinct states where practical: untouched baseline, seeded context A,
and seeded context B. Include valid zero, false/off and same-value explicit user
intent. An all-flat EQ curve, equal gains on both slices, or two identical
profiles cannot expose swapped destinations or cross-context leakage.

Apply the real control, then collect command response, fresh radio/backend
publication, all relevant model values and visible consumers. Record provenance:
a request ACK, a fresh status publication and an optimistic cached getter are
not interchangeable. Retain first, intermediate and settled observations with
ordered writes, so a later matching value cannot hide a read→wrong write→read race.

Transitions: applet close/reopen; enable→disable→enable; mode A→B→A; band A→B→A;
slice select/recreate and second-slice sentinel; microphone/input source change;
app reconnect; normal process restart; separately authorized profile and external
client changes. Couple only settings that share a documented recall dependency;
otherwise use an unrelated control change to detect accidental signal fan-out.

Expand the persistence snapshot with existing transmit/EQ resources and fill
only demonstrated observation gaps (e.g. mic boost/bias, RIT/XIT/step and specific
delays). Do not create broad mutation verbs merely to bypass the real applet.
The report needs separate **inventoried**, **observable**, **action exercised**,
**transition checked**, **authority verified**, and **cleanup verified** columns.
A readable field is not coverage.

## RX-only applicability

Many TX-chain values can be inspected and their setpoint persistence exercised
without producing RF: power levels, mic/MON, AM carrier, processor/DEXP, TX cuts
and TX EQ. VOX must remain off; enabling VOX can key autonomously even without
an explicit bridge PTT request. Treat break-in, profile loads with arming side
effects, TUNE/ATU and queued transmit actions separately. No physical power,
modulation, compression or EQ-response claim follows from this RX-only work.

## Source map

- `src/core/AutomationServer.cpp`: slice/transmit/equalizer/audio snapshots and `radiocert persist`.
- `src/models/SliceModel.h`: SQL intent, radio/effective audio/AGC and RIT/XIT/step semantics.
- `src/models/TransmitModel.h`: mic/processor/monitor/VOX/CW and timing state; operator-intent versus status signals.
- `src/models/EqualizerModel.h`: independent eight-band radio RX/TX EQ.
- `src/gui/RxApplet.cpp`, `PhoneApplet.cpp`, `PhoneCwApplet.cpp`, `TxApplet.cpp`, `EqApplet.cpp`: actual control labels/routes.
- Existing [research](radiocert-persist-research-2026-09-07.md) and [first live findings](persist-flex8400m-first-run-2026-09-07.md).

This inventory does not authorize a new live mutation sweep or expand the
operator's explicit no-transmit authorization.

## September 7 implementation update

The expanded runner now includes 40 applet setting contracts and all existing
transmit/EQ snapshot fields, plus the additional model observations described in
`docs/automation-bridge.md`. This inventory's original first-run column remains
a historical baseline; consult the [expanded run report](persist-flex8400m-expanded-run-2026-09-07.md) for executed evidence.
Radio-setup, automatic SQL/AGC OFF, client DSP/CHAIN, audio devices, multi-slice
and external-client scenarios remain explicit gaps, not implied certification.
