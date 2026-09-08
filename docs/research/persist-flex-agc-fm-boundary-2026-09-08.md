# AGC reset at the Flex FM boundary

Confirmed on FLEX-8400M firmware **4.2.18.41174**, using app commit `9a8ea1b59eea5226dd6e1788608c5f9a22abc650`. This investigation changed no application code. The earlier persistence/TX tooling extensions are included in this PR; the squelch behavior change is separately proposed in #5508. No new build, CTest or CI run was required for this investigation. Two additional live receive-only runs completed and fully restored their baselines.

The immediate cause is a **radio-reported reset of non-FM AGC settings when leaving FM**. The app subscription, model adoption and visible RX control were correct in this reproduction. This does not establish whether Flex considers the reset intentional, a firmware defect, or an unsupported retention expectation; firmware internals and the SmartSDR GUI were not inspected.

## Recorded-run chronology

| Local time | Event | Radio AGC mode / threshold / off-level |
|---|---|---|
| 07:30:53 | Initial USB | med / 65 / 10 |
| 07:54:04.803 | App sends `slice set 0 mode=FM` | |
| 07:54:04.811 | Radio publishes FM state | off / 60 / 10 |
| 07:55:25.559 | App sends `slice set 0 mode=CW` | |
| 07:55:25.586 | Radio publishes CW state | med / 50 / 50 |
| 08:00:44.560 | Later FM→AM transition | med / 50 / 50 |

The app command log contains no AGC-setting command before the explicit cleanup at 08:04. No wire-log gap was recorded. This historical log is supporting observation, not a deterministic proof of a non-event. The stronger isolation below suspends the app while the change occurs.

## Live isolation

First seed threshold/off-level **43/17**. USB→LSB→AM→CW→DIGU→USB retains 43/17 in both the radio-backed model and visible AGC control. No FM or transmit occurs in this comparison.

For each FM test, subscribe directly through an independent non-GUI TCP connection, confirm the exact owned radio/slice and TX-disabled client, suspend only the owned AetherSDR process, send the same mode-only commands FlexLib uses, and record radio status before resuming the app. The suspensions lasted approximately 0.18 seconds; unconditional process resume is in a finally block.

| Starting threshold / off-level | FM state while app suspended | USB state while app suspended | App model and slider after resume |
|---|---|---|---|
| 43 / 17 | off / 60 / 10 | med / 50 / 50 | med / 50 / 50; slider 50 |
| 65 / 10 | off / 60 / 10 | med / 50 / 50 | med / 50 / 50; slider 50 |
| 65 / 10 | off / 60 / 10 | med / 50 / 50 | med / 50 / 50; slider 50 |

The final repeat explicitly issued **`sub slice all` again two seconds after resume**. That fresh radio snapshot still reported USB, `agc_mode=med agc_threshold=50 agc_off_level=50`. Re-subscribing did not reveal retained 65/10 values. Thus the result is not just an intermediate mode echo cached by the app.

## Subscription and refresh path

- [RadioModel.cpp](../../src/models/RadioModel.cpp) subscribes to `slice` status at connection setup. AGC is part of slice status, not a separate per-mode subscription.
- [FlexBackend.cpp](../../src/core/backends/flex/FlexBackend.cpp) decodes `agc_mode`, `agc_threshold` and `agc_off_level` into the slice delta.
- [RadioModel.cpp](../../src/models/RadioModel.cpp) routes that delta to its addressed slice. [SliceModel.cpp](../../src/models/SliceModel.cpp) adopts the values and emits change signals.
- [RxApplet.cpp](../../src/gui/RxApplet.cpp) listens to threshold/off-level changes. Its [slider refresh](../../src/gui/RxApplet.cpp) chooses the appropriate value for AGC mode and uses QSignalBlocker to avoid generating a user-setting command.
- [FlexBackend mode setter](../../src/core/backends/flex/FlexBackend.cpp) sends a mode-only command, matching the published FlexLib `Slice.DemodMode` setter in local FlexLib Slice.cs:287. No AGC restore is part of that reference setter.

## Practical conclusion and next steps

1. Retain this as a Flex FM-boundary reset/retention finding. Provide the minimal mode-only reproduction and firmware version in an upstream report; compare with the SmartSDR GUI to determine whether it adds an intentional restore convention.
2. The two-slice runner now includes FM entry/return and **both** AGC threshold and AGC-off level as distinct fields, with mode-dependent slider checks and guarded cleanup. This expansion awaits a live run; the earlier USB/LSB-only multislice results cannot cover it. See the [handoff](persist-flex-to-icom-handoff-2026-09-08.md).
3. The TX sweep cleanup initially omitted these receive-state side effects. Its full final comparison caught the omission, and explicit RX-only cleanup restored them. Capture and guard their restoration in future full-mode sweeps.
4. Do not add a client-side automatic AGC replay solely to mask the observed reset. That would need a documented Flex behavior contract and maintainer review, because the radio currently publishes 50/50 as authoritative state.

All original slice, pan, TX and EQ fields matched at the end of both investigations; each owned client exited normally. No TX was requested or permitted. No AGC production behavior was changed by this investigation. This PR records the finding rather than replaying cached AGC values to mask the radio publication.

The operator retains `agc-rootcause/agc.json`, `agc-refresh/agc.json` and the corresponding runner-source snapshots under run `validation-multislice-20260908`. Raw radio identifiers are excluded from this report and PR.
