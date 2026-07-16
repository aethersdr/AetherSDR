# Cross-needle power/SWR meter — mathematical model & decision record

**Read this before changing anything about the PWR applet's cross-needle
meter.** This file is the authoritative source of truth for *how the meter
computes and draws forward power, reflected power, the needle crossing, and the
SWR contours* — and, just as importantly, *which modeling choices are settled
and why*. It exists because the model was reshaped by successive edits that each
looked locally reasonable but pulled the geometry in opposite directions. If you
are about to "correct" the physics, your correction must update this document
(and the tests) or it will be reverted by the next contributor.

Authoritative reference physics: a classic amateur cross-needle meter (Daiwa
CN-series / Diamond SX-series). The governing write-up presents **two** pictures
— an idealized flat-rest model (its §10/§11) and the real-meter deviations (its
§13). Those two pictures imply *opposite* geometries, which is the root cause of
past churn. **This document picks one and records why.**

---

## 0. Where each kind of fact lives (do not duplicate)

| Fact | Home | Notes |
|---|---|---|
| **Formulas / algorithms** | `src/gui/CrossNeedleMeterGeometry.cpp` `.h` | The only place math lives. |
| **Parameters / calibration data** | `resources/meterfaces/cross-needle-v12.json` | Numbers only, no logic. Compiled into the app via `resources.qrc`. |
| **Calibration fit + diagnostics** | `tools/fit_cross_needle_response.py` | Refits the response coefficients; `--check` guards them. |
| **Executable spec (enforcement)** | `tests/cross_needle_meter_test.cpp` | The tests are the contract. If a change is "correct," it changes the tests deliberately, not incidentally. |
| **Decisions & rationale** | **this file** | Why the model is shaped the way it is. |

Rule: the JSON never contains logic; the code never hard-codes calibration that
belongs in JSON; comments and READMEs are descriptive, **the tests are
normative.** When the model changes, update all five rows in the same change.

---

## 1. Core electrical quantities

A directional coupler yields forward/reflected envelope voltages `Vf`, `Vr`:

    rho = Vr/Vf = sqrt(Pr/Pf)          0 <= rho < 1
    SWR = (1 + rho) / (1 - rho)        (`swrFromPowers`)
    Pr  = Pf * rho^2                   (`reflectedPowerWatts`)
    inverse: rho = (SWR - 1)/(SWR + 1)

SWR depends only on the ratio `Vr/Vf`, not on absolute power — this is why one
fixed family of printed contours reads SWR at any power. Unchanged across all
revisions; do not touch.

---

## 2. Detector + movement response  `angleForValue()`

Needle deflection is a degree-5 Bernstein of **normalized power** — a real
D'Arsonval movement driving a square-root watt scale:

    p = (P - Pmin)/(Pmax - Pmin)                             // normalized power
    r = degree-5 Bernstein(p; responseCoefficients[0..5])    // monotonic + concave
    angle = lerp(responseStartRadians, responseEndRadians, r)

`responseModel` = `concave_bernstein_v1`. The coefficients (non-decreasing, with
non-positive second differences → concave, square-root-like compression) and the
start/end angles live in each scale's `response` block in the JSON. Two entry
points share this Bernstein: **`angleForValue`** clamps `p` to `[0, 1]` for live
needles and printed ticks (a movement never reads past full scale), while
**`angleForNormalizedPower`** does NOT clamp — SWR-contour construction passes
`p` slightly over 1 to extend a movement a little past full scale (§4).

**Angled rest (the key property).** `responseStartRadians` equals the scale's
printed-zero angle (`reference_angles_radians[0]` == `startRadians`). At zero
power `r = 0`, so each needle parks *on its printed 0 mark, pointing up into the
dial* — exactly like a real cross-needle meter. It does NOT rest flat on the
concealed pivot baseline. This is what keeps the low-SWR contours visible and
shallow (see Decision **D1**). `printedAngleForIndex()` therefore returns the
calibrated movement angle for every tick, including 0.

---

## 3. Needle rays and their crossing

Pivots are concealed below the visible face (`forwardScale.center`,
`reflectedScale.center`, both below the canvas). Each needle is a straight ray
from its pivot at the deflection angle; the tip rides a circle of radius
= needle length:

    tip = center + (cos(angle), sin(angle)) * radius          (`pointOnScale`)
    crossing = line-intersection(fwd pivot→fwd tip, ref pivot→ref tip)  (`needleIntersection`)

The mechanism only ever does this: two rays, one crossing. SWR is *read* off the
printed contour that passes through the crossing, never computed by a third
movement. Verified against a stored proof case in the tests
(`validation.intersection`).

---

## 4. Constant-SWR contours  `swrGuidePath()`

A contour is the locus of crossings at fixed SWR as power sweeps up from the
hidden convergence:

    ratio = ((SWR-1)/(SWR+1))^2 = rho^2 = Pr/Pf     (infinity → ratio = 1)
    sweep forward power up (extending past full scale via angleForNormalizedPower)
        point = crossing of fwd needle at P and ref needle at P*ratio
    // terminate on ONE common boundary: a fixed graph_clearance short of
    // whichever power arc is nearer (design 19 — a consistent fan; see D1).

Within the readable range a contour is an **exact** constant-SWR locus: the
crossing at true SWR = S at any readable power lands on the S contour, so the
printed grid reads correctly. The short segment past full scale (needed only by
the low-SWR contours to reach the common boundary) is **drawn face art**, not a
readable locus — the ratio-exactness tests therefore verify only the readable
portion. Contours are **not** circular arcs.

---

## 5. SWR number placement  `swrLabelCenters()`

Derived, not authored. There are **no per-guide label coordinates.** Each number
anchors on its own contour at a fraction `label_arc_fraction` of that contour's
**visible** arc length (from where it clears the mask to its upper end), then is
decluttered:

1. Anchor at `label_arc_fraction` up the visible contour; if the box collides,
   step along the contour by `label_declutter_step` (alternating ±, and past the
   top along the upper tangent for a stub too short to host a box, e.g. 1.1),
   and along the contour normal (alternating ±), until the box is clear.
2. "Clear" = keeps `label_box_padding` from every already-placed label, every
   *other* contour path, the power-scale number boxes, the lower mask, and the
   face edge.
3. Result is memoized in `swrLabelCenterCache`, keyed by the exact `QFont` used
   by the widget. A widget/application font change therefore recomputes the
   collision layout before rebuilding the static face.

To move the whole number ring, change **one** value: `label_arc_fraction`. The
tests enforce association (each number nearest its own contour or its upper
continuation), spacing, non-overlap, mask clearance, and clearance from the
reflected-scale numbers.

---

## 6. Decisions (settled — change only by editing this section + the tests)

### D1 — Angled rest → single hidden convergence → shallow, visible low-SWR lines
**Status: ADOPTED** (this supersedes the earlier flat-rest decision; see the
changelog at the end). Implemented by `concave_bernstein_v1` with the response
starting at the angled printed-zero rest (§2).

The needles park on their printed zeros, pointing up into the dial. At zero
power both needles are angled (not colinear on the baseline), so every
constant-SWR contour begins at the **same** hidden crossing just below the lower
mask (~`(750, 979)`) and fans upward. The low-SWR lines ride up **along the
reflected needle's rest ray** — visible above the mask and rising at a shallow
angle that steepens toward SWR = ∞ (measured: 1.1 ≈ 19°, 1.2 ≈ 32°, 1.5 ≈ 62°).

**Why this model (motivation, not proof).** The real Daiwa **CN-801** face
(its manual's 第2図, https://sq7fpd.boff.pl/CN801.pdf) shows the SWR 1.1–1.5
lines visible and shallow and the fan converging near the bottom — which is what
motivated the angled-rest choice over the idealized flat-rest picture (the
reference write-up's §13 vs §10/§11). Be honest about what the manual actually
establishes: it confirms SWR is read at the needle crossing and that 10 W / 0.4 W
reads SWR 1.5 (our `validation` proof asserts exactly this). It does **not**
document the detector response, the single hidden convergence, or the
past-full-scale extension — those are our engineering choices, informed by the
face's appearance, not proven by the manual. Treat them as settled-by-decision
here, not as facts derived from the source.

**Retired model (do NOT reintroduce without flipping this decision + the tests):**
a `monotonic_voltage_bernstein_v1` response that rested both needles flat on the
pivot baseline (forward −π, reflected 0) so contours left independent baseline
origins vertically (§10). That is mathematically clean but it pins the low-SWR
crossings onto the baseline: SWR 1.1/1.2 fell entirely below the mask and 1.3–2
drew as steep stubs — visibly wrong against the CN-801. The tests now assert the
angled-rest behaviour (`all contours share one convergence concealed below the
lower mask`, `low-SWR contours (1.1, 1.2) reach the visible face`, `low-SWR lines
emerge shallow and steepen toward higher SWR`).

### D2 — SWR labels are derived, never authored per-guide
**Status: ADOPTED.** See §5. An SWR guide in JSON stores only
`{label, display_label, swr}`. The three global knobs
(`label_arc_fraction`, `label_declutter_step`, `label_box_padding`) plus the
declutter algorithm produce every position. This replaced 24 hand-tuned
`label_y`/`label_offset` pixel values that had to be re-nudged after any
geometry change.

### D3 — JSON is data, code is math, tests are the contract
**Status: ADOPTED.** See §0. A change to the physics is not "done" until the
formula (code), any parameters (JSON), the fit tool, this document, and the
tests all agree in the same change.

---

## 7. Changing the model safely — checklist

1. State the intent here (edit the relevant Decision, or add a new one).
2. Change the formula in `CrossNeedleMeterGeometry.cpp`.
3. If calibration changed: re-run `tools/fit_cross_needle_response.py`, paste the
   coefficients into the JSON, and confirm `--check` passes.
4. Update `tests/cross_needle_meter_test.cpp` to assert the new behavior *on
   purpose*, and `resources/meterfaces/README.md` for the parameter docs.
5. Bump `format_version`/`design_version` in the JSON if the schema/geometry
   meaning changed.
6. Rebuild and run `cross_needle_meter_test` — it is the acceptance gate.

---

## 8. Changelog

- **design_version 19 — consistent fan: all contours terminate on a common
  boundary.** Every contour now sweeps until its crossing reaches one shared
  envelope a fixed `graph_clearance` short of whichever power arc is nearer, so
  the whole SWR family is an even fan parallel to the arcs (matches the CN-801).
  Low-SWR crossings reach that boundary only a little past full forward scale,
  so `angleForNormalizedPower` extends the movement angle modestly past unity
  (≤ ~2.5× full-scale power) for contour drawing only — **live needles and the
  power ticks still clamp at full scale.** Consequence: a contour is an exact
  constant-SWR locus within the readable range and drawn face-art beyond it; the
  ratio-exactness tests verify only the readable portion (see D3 note).
  **`format_version` bumped 5 → 6** here: the `response` and `swr` schema changed
  across designs 16→19 (removed `linear_end_voltage`/`blend_end_voltage`, renamed
  `label_arc_radius` → `label_arc_fraction`, added `mask_gap`), so a stale
  format-5 file is now rejected at the format gate rather than on content.
- **design_version 18 — contours reach the arcs + leading-end mask gap.**
  `graph_clearance` reduced (154 → 60) so the high-SWR contours extend close to
  the power arcs instead of stopping far short. Low-SWR contours are unchanged
  (they stop at full forward scale — the readable limit). Added `mask_gap` (a
  render-only trim, applied in `CrossNeedleMeterWidget::drawSwrGuides`) so every
  contour's lower end sits a small gap above the mask, matching the real face.
- **design_version 17 — angled-rest / shallow low-SWR (D1 flipped).** Moved the
  needle zero-rest from the flat pivot baseline back to the angled printed-zero
  position (`concave_bernstein_v1`, power domain). Reason: the flat-rest model
  (design 16) hid the SWR 1.1/1.2 contours below the mask and drew 1.3–2 as
  steep stubs, contradicting the real Daiwa CN-801 face. Now the low-SWR lines
  are visible and shallow. Also: SWR label anchor changed from an absolute
  arc-length (`label_arc_radius`) to a fraction of each contour's visible length
  (`label_arc_fraction`), so it adapts to the single-origin fan.
- **design_version 16 — flat-rest / spread-origin (retired).** See D1's retired
  model. Introduced the voltage-domain response and vertical baseline departure.
