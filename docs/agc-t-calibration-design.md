# AGC-T Noise-Floor Calibration — Design

**Status:** Proposal (no code yet)
**Author:** Claude (Opus 4.8) with architecture by @jensenpat
**Date:** 2026-06-01
**Scope:** Help users set the "perfect" AGC-T value per band by calibrating it
against the receiver noise floor, with a live visual aid and per-band recall.

---

## 0. Locked decisions (decision pass, 2026-06-01)

Bias for all decisions: **successful, consistent operation for every operator
skill level.** Still subject to maintainer (Jeremy/KK7GWY) sign-off on visuals.

| # | Decision | Choice |
|---|----------|--------|
| Surface | How it's shown | **Panel + optional ambient pan line** (Option C, §5) |
| Default mode | First action on open | **Auto-sweep first**; live curve shown, manual fine-tune always available (§4) |
| Entry point | How it launches | **Right-click the AGC-T slider** — clean, no new chrome (§7) |
| Per-band recall | On band change | **Suggest, never auto-apply** — lightweight prompt, no silent changes (§6) |
| AGC-off target | Comfortable-noise setpoint | **Sensible fixed default**, exposed behind an "advanced" reveal (§4) |

**Two flagged tensions** (chosen options trade against the all-skill-levels
bias; mitigations adopted so we don't regress beginners):

- *Right-click-only hurts discoverability.* Mitigation: the slider tooltip
  states "Right-click to calibrate AGC-T against the noise floor," plus a
  one-time discovery hint. Keeps the clean look while making the feature
  findable.
- *"Suggest, never auto-apply" adds per-band friction.* Mitigation: the
  suggestion is a **non-modal, dismissible chip/banner** (never a blocking
  dialog), with a per-band "don't suggest again" so it can't nag.

---

## 1. Goal

SmartSDR users must re-tweak AGC-T every time band conditions or noise floor
change, and the 0–100 knob has no obvious relationship to anything visible on
screen. We want to (a) make finding the AGC-T "sweet spot" fast and visual,
(b) optionally automate it, and (c) remember a good value per band so band
changes don't force a re-tweak.

---

## 2. Background: what AGC-T actually does (the load-bearing distinction)

There are **two different radio knobs**, both 0–100, both surfaced through the
single AGC-T slider in `RxApplet`, selected by AGC mode:

| AGC mode | Radio command | FlexLib property | What the value means |
|----------|---------------|------------------|----------------------|
| slow / med / fast | `slice set <id> agc_threshold=<0-100>` | `AGCThreshold` | The **knee**: signal level above which AGC begins applying gain. Set it just above the noise floor so AGC never pumps up band noise. |
| off | `slice set <id> agc_off_level=<0-100>` | `AGCOffLevel` | A **fixed gain** applied to everything regardless of level. No knee. |

The classic FlexRadio procedure — "tune to a clear spot, lower AGC-T until the
background noise *just begins to decrease*, then back off slightly" — is a
description of finding the **knee**, and it only applies when **AGC is ON**.
([FlexRadio: How AGC works](https://helpdesk.flexradio.com/hc/en-us/articles/360029494371),
[AGC-T adjustment procedure](https://community.flexradio.com/discussion/8027708/smartsdr-agc-t-adjustment-procedure))

When AGC is **OFF**, `agc_off_level` is just fixed gain: turning it up makes
both noise and signals monotonically louder, with no SNR benefit, and the
operator must ride RF Gain to avoid overload. So "calibrate against the noise
floor" with AGC off means something different: **pick the fixed gain that
places the band-noise audio at a comfortable target level without overload** —
a setpoint solve, not a knee-find.

**Design consequence:** the tool must be AGC-mode aware. It runs a *knee-find*
when AGC is on (`agc_threshold`) and a *target-level solve* when AGC is off
(`agc_off_level`). The user's stated interest in the off-mode case is fully
supported, but we present it honestly as "set comfortable noise gain," not as a
mythical off-mode knee.

### The crucial observability fact

The 0–100 value is **arbitrary** — neither the radio status nor FlexLib exposes
its dBm equivalent; the firmware maps it internally. Two consequences:

1. We **cannot** analytically compute the "perfect" value from the noise floor.
   We must find it **empirically** by observing the receiver's response while
   we move the knob.
2. The **RF noise floor in dBm does not change when AGC-T moves** (it's measured
   off the pre-AGC FFT). The *only* observable that bends at the knee is the
   **post-AGC audio level**. This is what the FlexRadio procedure means by
   "the background noise begins to decrease" — it's an *audio* observation.

---

## 3. Signals we can measure (and which one is useful)

| Signal | Source | Units | Moves with AGC-T? | Use |
|--------|--------|-------|-------------------|-----|
| RF noise floor | `SpectrumWidget::noiseFloorDbm()` (getter only, EMA-smoothed two-pass trimmed mean) `src/gui/SpectrumWidget.cpp:911` | dBm | **No** | Quiet-spot guard; ambient pan reference line; band fingerprint |
| S-meter / LEVEL | `MeterModel::sLevelChanged(slice, dbm)` `src/models/MeterModel.cpp:620`, ~100 Hz | dBm | **No** (RF domain) | Quiet-spot guard (passband signal+noise vs floor) |
| **Post-AGC audio RMS** | **`AudioEngine::levelChanged(float rms)`** `src/core/AudioEngine.h:476`, per audio frame (~10–20 ms) | linear 0–1 | **Yes — bends at the knee** | **The calibration observable** |

There is **no AGC-gain-reduction meter** exposed by the radio, so we infer the
knee from audio RMS, not from a gain-reduction readout.

Note: `levelChanged` is emitted at several taps (pre-NR raw at
`AudioEngine.cpp:1320`, and post each NR stage). For a clean, repeatable curve
the calibration should read the **raw pre-NR tap** and hold AF gain + all NR
constant during a sweep, so the only variable is AGC-T. (Implementation detail:
either select the raw tap, or temporarily freeze NR/AF for the sweep duration
and restore.)

---

## 4. The calibration engine (shared by manual and auto)

Manual and auto are the **same engine** at different speeds — this is why
"manual vs auto" isn't really a fork. The engine is: *set AGC-T → wait for AGC
to settle → record audio RMS → repeat*. Manual = the user is the stepper; Auto
= a timer is the stepper.

### Sweep (auto)
1. **Pre-flight guards:**
   - AGC mode known; remember current AGC-T value for restore-on-cancel.
   - Verify the passband is **quiet**: S-meter (LEVEL) within a small margin of
     the measured noise floor, and no signal peak inside the filter passband on
     the FFT. If not quiet, surface "tune to a clear spot" and refuse/auto-find
     a clear segment within the current pan.
   - Freeze AF gain and NR (or read raw tap) for the sweep.
2. **Coarse pass:** step AGC-T high→low (e.g. 100→0 in steps of ~5), with a
   per-step **settle delay** (~250–300 ms — the radio AGC needs time to react;
   FlexRadio explicitly warns to wait after each change). Record `(value, rms)`.
3. **Knee detection (AGC on):** find the point of maximum downward curvature /
   where the slope first exceeds a threshold as noise starts dropping. Apply the
   documented "back off slightly" by nudging a couple of points toward higher
   threshold (more conservative). This is the sweet spot.
4. **Fine pass (optional):** re-sweep ±10 around the candidate in steps of 1–2
   for a precise knee.
5. **Target solve (AGC off):** instead of a knee, interpolate the monotonic
   `value→rms` curve to hit a chosen audio-noise setpoint (default ≈ a
   comfortable dBFS), clamped to avoid overload (watch for clipping / S-meter
   spikes).
6. **Result:** propose a value; user Applies (sends the `slice set` command) or
   Cancels (restores original). Optionally "Save for this band."

Full coarse sweep cost ≈ 20 points × ~275 ms ≈ **~6 s**; coarse+fine ≈ **~10 s**.
Acceptable; show a progress indicator and keep Cancel responsive.

### Manual
Same pipeline, but the user drags the AGC-T slider and the panel plots the live
`(value, rms)` curve as points accumulate, with the detected-knee marker
updating live. The user sees exactly where the noise starts to fall and can
click "set to knee" or fine-tune by ear. This is the more trustworthy default
(matches the user's instinct that manual is more consistent) and doubles as the
data source for auto.

---

## 5. Viewing surface — options and recommendation

The calibration variable (AGC-T, 0–100, audio domain) and its response (audio
RMS) live in a coordinate space that is **not** the pan's frequency×dBm space.
That governs the choice.

**Option A — Dedicated live-graph panel (RECOMMENDED).**
A small non-modal, frameless calibration window with a 2D plot:
**x = AGC-T (0–100), y = audio-noise level (dB, relative)**. Overlays: live
sweep curve, current-value marker, detected-knee marker (the sweet spot),
optional target-level line (AGC-off mode). Controls: Start/Stop auto sweep,
"Apply", "Save for band", AGC-mode/quiet-spot status chips. *Pros:* the curve
makes the knee unmistakable; honest representation of what AGC-T does; doesn't
clutter the pan; reusable across modes. *Cons:* a new window.

**Option B — Line on the pan surface.**
Draw the *measured RF noise floor* as a horizontal dBm reference line on the
pan (a thin label like "NF −124 dBm"). *Pros:* always-on context, cheap (the
floor is already computed; `DisplayNoiseFloor*` settings already exist).
*Cons:* it **cannot honestly show AGC-T** — AGC-T is 0–100 in the audio domain
and the floor line doesn't move when you turn the knob. Presenting it as "the
AGC-T line" would mislead. Good as *ambient context*, not as the calibration
instrument.

**Option C — Hybrid (RECOMMENDED overall): A + B.**
Use the dedicated panel (A) to *do* the calibration, and offer an optional
persistent noise-floor reference line on the pan (B) for at-a-glance context
between calibrations. Plus per-band recall (Section 6) so the result actually
sticks. This gives the best of both without conflating domains.

**Rejected:** trying to render an "AGC-T line" directly on the pan in dBm. It
would require inventing/learning a 0–100→dBm mapping the firmware refuses to
expose, and it would still be a static line that doesn't respond to the knob —
exactly the wrong mental model.

---

## 6. Per-band persistence

Per-band recall is the feature that makes this stick: calibrate once per band,
and switching bands re-applies the good value.

**Existing hooks (reuse, don't reinvent):**
- `BandSnapshot { QString agcMode; int agcThreshold; }` `src/models/BandSettings.h:19`
- `BandStackEntry { QString agcMode; int agcThreshold; }` `src/core/BandStackSettings.h:16`
  persisted to `~/.config/AetherSDR/BandStack.settings`.
- `bandForFrequency()` maps MHz → band name; `SliceModel::frequencyChanged` is
  the trigger point.

**Behavior (decision: suggest, never auto-apply):** on a deliberate band change
to a band that has a stored calibration, show a **non-modal, dismissible
chip/banner**: "Calibrated AGC-T (N) available for 20m — apply?" Applying
**sends** the value as a `slice set` command (radio stays authoritative).
Dismissing leaves the radio's current value untouched. A per-band "don't suggest
again" suppresses the chip for that band. No value is ever applied silently.

**Radio-authoritative policy nuance (important):** CLAUDE.md lists AGC/DSP flags
as radio-authoritative — we must not *override radio status* from client
persistence. The correct pattern (and what BandStack already does):
- Applying a suggestion **sends** a `slice set` command (we're acting like a
  user turning the knob), then normal status echo flows back.
- **Never** intercept `applyStatus()` to clamp the radio's reported value to our
  saved one — that creates the reconnect fight the policy warns about.
- Store a calibrated value alongside `agcMode` (a value calibrated for med-AGC
  is meaningless under off-AGC); only suggest when the saved mode matches the
  current mode.

Optionally store the noise-floor dBm at calibration time as a "band fingerprint"
so the UI can warn "noise floor has shifted ~8 dB since you calibrated — re-run?"

---

## 7. Available APIs (reference)

**Radio commands (already implemented in `SliceModel`):**
- `SliceModel::setAgcMode("off|slow|med|fast")` → `slice set <id> agc_mode=...` `src/models/SliceModel.cpp:250`
- `SliceModel::setAgcThreshold(0..100)` → `slice set <id> agc_threshold=...` `:258`
- `SliceModel::setAgcOffLevel(0..100)` → `slice set <id> agc_off_level=...` `:267`
- Status echo parsed in `applyStatus()` `:757`; signals `agcModeChanged`,
  `agcThresholdChanged`, `agcOffLevelChanged`.

**GUI control today:** single slider in `RxApplet`, range 0–100, routes to
`setAgcOffLevel` when mode==off else `setAgcThreshold` `src/gui/RxApplet.cpp:851`.
The calibration panel should drive these same setters so the existing slider and
the panel stay in sync via the model signals.

**Entry point (decision):** **right-click the AGC-T slider** opens the panel
(add a context menu / `contextMenuEvent` on the slider). To offset the
discoverability cost, set the slider tooltip to mention it ("Right-click to
calibrate AGC-T against the noise floor") and show a one-time discovery hint.

**Frameless dialog conventions:** follow `docs/dialog-patterns.md` and the
`NetworkDiagnosticsDialog` pattern — `FramelessWindowTitleBar`,
`FramelessResizer::install`, `setFramelessMode`, init from
`AppSettings "FramelessWindow"`. Persist geometry. Use `AppSettings`, not
`QSettings`. Use `MeterSmoother` for any displayed level value.

---

## 8. Edge cases & risks

- **Not a quiet spot:** signal in the passband poisons the curve. Guard with
  S-meter-vs-floor check + in-passband peak detection; auto-suggest the nearest
  clear segment or block the sweep with guidance.
- **Audible disruption:** sweeping changes loudness. Keep sweeps short, show
  progress, restore on cancel, and consider a brief "calibrating…" mute option.
- **AGC settle time:** too-fast stepping reads a transient, not steady state.
  Honor the ~250–300 ms settle; make it a tunable constant.
- **Mode mismatch on recall:** only apply a saved per-band value if the saved
  AGC mode matches the current mode.
- **FM:** AGC container is hidden in FM (`RxApplet.cpp:2232`); disable the tool
  in FM.
- **Multi-slice / multi-client:** calibrate the active/owned slice only; gate on
  slice ownership like the rest of the app.
- **NR/AF interference:** freeze or bypass for the sweep so only AGC-T varies.

---

## 8.5 Implementation status (branch aether/agc-t-noise-calibration)

Implemented in this branch:
- **Engine** — `src/core/AgcTCalibrator.{h,cpp}`: headless QObject, auto-sweep +
  manual recording, knee detection (max-distance-from-chord) for AGC-on and a
  target-level solve for AGC-off, quiet-spot guard, restore-on-stop.
- **Panel** — `src/gui/AgcCalibrationDialog.{h,cpp}`: `PersistentDialog` subclass
  with a live curve widget (`AgcCurveWidget`), Auto Sweep / Stop / Apply, mode
  awareness, AGC-off target control, quiet-spot status line.
- **Entry point** — right-click on the AGC-T slider in `RxApplet`
  (`calibrateAgcTRequested(sliceId)` → `MainWindow::showAgcCalibrationDialog`),
  with a tooltip discovery hint.

Deferred to follow-up PRs (to keep this change reviewable):
- **Per-band suggest chip** (§6) — store calibrated value + suggest-on-band-change.
- **Ambient noise-floor line on the pan** (§5 Option B).

## 9. Phased implementation plan

1. **Engine (headless):** a `AgcTCalibrator` that, given the active slice +
   audio-level + S-meter + noise-floor sources, runs manual-step and auto-sweep,
   does knee detection / target solve, and emits curve + result. Unit-testable
   with synthetic curves.
2. **Panel (Option A):** frameless non-modal window with the live 2D graph,
   markers, controls; wired to the engine and the existing `SliceModel` setters.
3. **Per-band recall (Section 6):** extend BandStack/BandSnapshot, apply-on-
   band-change, optional fingerprint warning.
4. **Ambient pan line (Option B):** optional persistent noise-floor reference
   line, reusing existing `DisplayNoiseFloor*` settings.
5. **Polish:** quiet-spot auto-find, mute-during-sweep option, accessibility.

---

## 10. Decisions resolved + remaining maintainer sign-off

Decision pass (2026-06-01) resolved the five open questions — see §0 for the
table and the two flagged-tension mitigations. Summary:

1. **Surface:** Option C — panel + optional ambient pan line. ✅
2. **Entry point:** right-click the AGC-T slider (+ tooltip/discovery hint). ✅
3. **Default mode:** auto-sweep first, manual fine-tune always available. ✅
4. **Per-band recall:** suggest via dismissible chip, never auto-apply. ✅
5. **AGC-off target:** sensible fixed default, advanced reveal to override. ✅

Per CLAUDE.md, **visual design and UX direction remain Jeremy/KK7GWY's
authority.** These decisions set engineering direction; the panel's visual
design, exact chip styling, and copy still need maintainer review before/at PR
time. Nothing here changes defaults that affect existing users until merged.
