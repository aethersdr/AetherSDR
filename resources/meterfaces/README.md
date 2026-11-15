# Meter-face geometry

## Standard S-meter

`s-meter-v1.json` is the editable source of truth for the standard S-meter's
responsive face construction. It preserves the original `SMeterWidget`
formulas rather than imposing the cross-needle meter's fixed design canvas:
the arc radius and horizontal center scale from widget width, while the arc
center height, typography, pivot, and readouts retain their established
width/height-relative or pixel offsets.

The file owns the shallow-arc sweep, RX calibration, selectable TX scale
ticks, tick/label placement, needle and shadow dimensions, pivot/glow sizing,
peak overlays, and readout placement. The established **Aether default** face
continues to use `SMeterWidget` and `ThemeManager` colors. The three physical
faces use the separate shared catalog described below, so geometry edits cannot
accidentally change theme behavior.

The mechanical construction has one authority. A value first selects its
calibrated point on the outer scale arc. The line from the concealed movement
pivot through that point is the value's movement ray. The needle extends along
that ray, and the inner TX tick is placed where the same ray intersects the
inner scale arc. Peak markers use the same construction. Do not calculate a
needle endpoint by extending a radius from the scale arc's center: the arc
center and physical movement pivot are intentionally different points.

Important rules:

- Keep the RX calibration anchors mechanically consistent: S0 is -127 dBm,
  S9 is -73 dBm at fraction 0.6, and S9+60 is -13 dBm at fraction 1.0.
- `center_y_height_factor` participates in
  `centerY = radius + height * factor`; it is not an absolute normalized Y.
- Power tick policies are ordered from the largest threshold to the 0 W
  default. The radio-selected maximum and warning point remain live widget
  state; this resource controls only how that scale is printed.
- Every version-1 field is required and type-checked. A missing, mistyped, or
  mechanically invalid field rejects the complete resource; fields are never
  silently mixed with compiled defaults.
- Keep the JSON and `SMeterGeometry::fallback()` exactly aligned. The test
  compares every field. The JSON is the shipping authority; the complete
  fallback is selected only when the complete resource is rejected, so a
  damaged resource cannot crash, distort, or blank the applet.
- The pivot mask uses `pivot.radius_width_factor`, but its effective width is
  capped by the preferred face aspect ratio from `sizing`. This preserves the
  approved mask at normal sizes while preventing it and its glow from
  ballooning when a detached window is made much wider without becoming taller.
- `readout.top_extra_pixels` is the minimum gap above the larger left/right
  value font. The smaller centered source label is top-aligned against those
  values using the fonts' actual ascents, maximizing its clearance from the
  printed arc at minimum, docked, or detached sizes.
- `sizing.minimum_aspect_ratio` and `maximum_aspect_ratio` bound only the
  face's render viewport. Normal widget sizes retain the original responsive
  formulas exactly; pathological tall or wide detached windows center a
  bounded face instead of stretching the concealed pivot outside its arcs or
  scaling labels until they collide. The lower bound remains below the normal
  docked aspect ratio, so the standard applet is pixel-for-pixel unchanged.

After editing, run `s_meter_geometry_test` and use the automation bridge to
grab `standardSMeter` at its normal, minimum, and enlarged sizes.

## Shared physical analog themes

`analog-meter-themes-v1.json` is the runtime source of truth for the physical
**Classic warm**, **Dark-room uplight**, and **Graphite dark** materials shared
by the standard S-meter and the PWR cross-needle meter. It owns only visual
materials: the deterministic gradients, fixed-seed paper grain, ink and text
palettes, needle colors, and the approved symmetric nine-point lower mask. It
does not own either meter's calibration, arcs, ticks, or movement geometry.

The gradient coordinates use the PWR meter's 1448 x 948 face rectangle. The
shared painter maps that reference rectangle to the target widget, so both
meters receive the same material recipe without storing or stretching a raster
image. The grain texture is generated deterministically and expensive material
layers are cached by the widgets.

The lower-mask boundary is normalized to its target face. Keep its nine points
strictly left-to-right and mirrored around x = 0.5. The standard S-meter uses
this profile only for the three physical themes; **Aether default** deliberately
retains its existing half-disc mask.

Classic warm and Dark-room uplight use the dark layered needle palette;
Graphite dark uses the restrained light metallic palette. These colors change
only the material stack around the already-calibrated pivot-to-tip line. The
standard meter's needle tip still comes exclusively from `s-meter-v1.json`, and
the PWR meter's two needle rays still come exclusively from
`cross-needle-v12.json`.

For format compatibility, `cross-needle-v12.json` still carries its original
V12 appearance values. They are no longer the runtime authority for shared
background or palette colors; edit `analog-meter-themes-v1.json` and its full
compiled fallback together. A future cleanup can remove those compatibility
fields without mixing that migration into a visual theme change.

## Cross-needle PWR meter

`cross-needle-v12.json` is the editable source of truth for the PWR applet's
cross-needle Forward / Reflected power and SWR face. It is intentionally kept
as data rather than buried as painter constants so the face can be refined
without reverse-engineering `CrossNeedleMeterWidget`.

The filename is a stable resource URI retained for compatibility. The payload's
authoritative `format_version` and `design_version` fields are currently 6 and
19; inspect those fields rather than inferring either version from the filename.

The geometry is expressed in a 1500 x 1000 design coordinate system. The
widget applies one uniform scale and centers that design in its available
rectangle; never compensate for a different widget aspect ratio by changing
individual points.

Important construction rules:

- The runtime background materials and palettes come from
  `analog-meter-themes-v1.json`. The older `face_gradient`, `uplight_gradient`,
  and `dark_theme` blocks remain compatibility data in the stable geometry
  resource; they are not the runtime material authority.
- The shared `uplight_gradient` defines the selectable **Dark-room uplight** treatment:
  one low-exposure vertical card gradient, a broad bottom-center halo, a
  smaller warm hotspot just behind the lower mask, a tight paper-diffusion
  bloom, and a symmetric vignette. The broad halo has an additional shoulder
  stop so it can illuminate the middle of the card without washing out the
  top. The hotspot and bloom use Screen blending to model emitted light, while
  `paper_grain_opacity` applies luminance-neutral fixed-seed Overlay grain. The procedural
  texture is generated near the nominal applet resolution so its fine and
  horizontally correlated fibres survive downsampling. The renderer uses
  these same layers for the graph underpass and SWR label boxes. Keep all three
  light centers at x = 750 unless an intentionally asymmetric physical lamp is
  being designed. `scale_separator_rgba` replaces the classic face's
  opaque cream graph separator with translucent copper so the uplight remains
  visible through the complete left and right scale bands.
- The shared `dark_gradient` and Graphite palette define the selectable
  **Graphite dark** treatment. It combines
  a matte charcoal card, a restrained neutral ambient lift, a low amber edge
  glow, a symmetric vignette, and scale-stable fixed-seed paper/paint grain.
  The same block owns its
  aged-ivory scale/text colors, copper SWR ink, steel-blue inner rules, dark
  mask palette, and metallic needle colors. These are material overrides
  only; all arcs, ticks, guides, pivots, and calibration remain shared.
- `needles` defines the PWR meter's code-drawn physical material dimensions
  without changing the
  calibrated movement rays. `line_*` is the exact pivot-to-tip body;
  `soft_shadow_*` and `shadow_*` form a restrained two-depth cast shadow;
  `edge_*` shades the side opposite the light; and `highlight_*` adds a narrow
  reflection on the upper-left edge; its colors come from the shared palette.
  Edge offsets are measured perpendicular
  to each needle, while shadow offsets are ordinary design-space x/y values.
  Keep both edge strokes narrower than `line_width` and the soft shadow wider
  than the contact shadow so the effect survives at 260 x 173 without reading
  as a bevel or glow.
- `forward_scale.center` is the concealed right-hand movement pivot and
  `reflected_scale.center` is the concealed left-hand movement pivot.
- `reference_angles_radians` preserves the perspective-corrected photographic
  tick observations; it is fitting evidence, not runtime calibration. Keep it,
  `values`, and `labels` the same length. Each scale's `response` is the sole
  movement authority used to derive major ticks, power-valued minor ticks,
  live needles, inverse readings, and SWR construction.
  `concave_bernstein_v1` is a normalized degree-5 Bernstein response in
  normalized POWER (a real D'Arsonval movement with a sqrt watt scale). Its six
  controls are non-decreasing with non-positive second differences (concave,
  square-root-like compression). `response.start_radians` is the ANGLED rest =
  the printed-zero angle (`reference_angles_radians[0]` == `start_radians`), so
  at zero power the needle parks on its printed 0 mark pointing up into the dial
  like a real cross-needle meter — it does NOT rest flat on the pivot baseline.
  Positive-power tick fits deviate from the perspective-corrected observations
  by at most ~26 design pixels on Forward and ~19 on Reflected (a few pixels at
  the normal applet width), within the declared `maximum_reference_error_pixels`.
  See `docs/cross-needle-meter-math.md` (Decision D1) for why angled rest, not
  flat-baseline rest, is the settled model.
- Verify response edits with `python3 tools/fit_cross_needle_response.py --check`.
  The dependency-free utility reports positive-tick residuals plus, for every
  guide, its curvature reversals and how many samples reach the visible face
  (low-SWR contours must be visible, mirroring the real meter).
- `scale_overlap` controls the upper crossing. The reflected trajectory is one
  continuous circle, but a short flat-ended mask is applied at its crossing
  before Forward is overprinted. Adjust `reflected_gap_half_span_radians` to
  change only the visible underpass opening; do not splice the circle.
- `titles.*.rotation_degrees` uses image-space degrees: the left Forward label
  is negative and the mirrored right Reflected label is positive.
- Keep the two `(W)` centers mirrored and outside the upper scale endpoints;
  moving them inward makes the unit text collide with the opposite graph.
- `label_offset` is measured radially beyond the scale baseline. It must leave
  room for the outward half of the number glyph plus the major tick; when it is
  increased, move the mirrored Forward / Reflected titles outward with it.
- `range.label` accepts explicit `\n` line breaks. Keep each multiplier on its
  own short line in the open top-right area instead of extending text back
  toward the right `(W)` label or upper power graphs.
- The Range legend's visibility is a client preference in the versioned
  `CrossNeedleMeter` AppSettings object, not face geometry. The right-click
  **Show Range** action defaults on and may hide only this printed legend;
  range selection, multiplier calibration, and needle positions are unchanged.
- `typography` contains every face font size in design pixels. These values are
  intentionally larger than the photographic proof so labels survive the
  normal 260 x 173 applet size; positions remain controlled by the geometry.
- An SWR guide stores only its semantic `swr` value; its number's position is
  derived, not authored (see `label_arc_fraction` below). Its curve is generated
  deterministically from the two calibrated movement maps. For finite SWR,
  reflected/forward power is
  `((SWR - 1) / (SWR + 1))^2`; the infinity guide uses a ratio of 1. Every
  sampled point is the intersection of the Forward needle at `F` and the
  Reflected needle at `F * ratio`. Sampling advances uniformly in Forward
  detector voltage, so screen-space segments do not bunch according to power.
  This is why some guides have a slight, smooth curve: the two printed power
  scales are non-linear.
- All SWR guides share ONE hidden convergence. Because the needles rest angled
  (on their printed zeros), the zero-power crossing is a single point just below
  the lower mask (~`(750, 979)`) that every contour fans up from. The low-SWR
  lines (1.1–1.5) rise from there along the reflected needle's rest ray —
  visible above the mask and shallow, steepening toward SWR = ∞. This mirrors
  the real Daiwa CN-801 face.
- `curve_samples` controls the polyline resolution. It must be high enough that
  straight segments between exact movement intersections preserve the target
  ratio after rasterization. It only bounds path approximation error; it cannot
  cure a noisy movement response and must not be used as a substitute for the
  smooth response fit. At 256 samples the current maximum midpoint deviation
  from the exact constant-ratio locus is below 0.019 design pixel.
- `graph_clearance` is the visual inset from the nearer power-scale arc at which
  EVERY contour terminates, so the whole SWR family is a consistent fan parallel
  to the arcs (smaller = closer to the arcs). Low-SWR crossings reach that
  boundary only a little past full forward scale, so contour drawing extends the
  movement angle modestly past unity to get there (live needles and ticks still
  clamp at full scale). A contour is thus an exact SWR locus within the readable
  range and drawn face-art beyond it.
- `mask_gap` trims the leading (lower) end of every contour this many design
  pixels above the lower mask, leaving a small gap between the line and the mask
  like the real meter face. It is a render trim only; the geometry path is
  unchanged, so it does not affect SWR readings or label placement.
- SWR numbers are placed by one derived rule, not per-guide coordinates. Each
  number anchors at fraction `label_arc_fraction` of its contour's VISIBLE arc
  length (from the mask crossing to the upper end). It then searches along the
  contour — and past the top along the contour's smooth upper tangent when a
  low-SWR stub is too short to host a box (e.g. 1.1) — and along the contour
  normal. `label_declutter_step` is the search increment; `label_box_padding`
  the required gap from other labels. Placement is part of resource validation:
  a box that cannot clear the mask, another number, a power-scale number, or
  another SWR guide makes the geometry invalid instead of silently accepting the
  last attempted position.
- The lower mask deliberately hides the two movement centers and the shared
  convergence. Its boundary is symmetric about x = 750.
- Both power scales use the same range multiplier: x1 = 20 W, x10 = 200 W,
  and x100 = 2 kW.

After editing, run `cross_needle_meter_test` and use the automation bridge to
grab `crossNeedleMeter`. The validation block records a mechanical proof:
100 W forward and SWR 1.5 derive 4 W reflected and intersect the printed 1.5
guide within half a design pixel.

The test suite verifies the ratio along all 12 guides and between their sampled
points, that all contours share one convergence concealed below the mask, that
the low-SWR contours (1.1, 1.2) reach the visible face and the low-SWR lines
emerge shallow and steepen toward higher SWR, monotonic concave response
coefficients, needles parked on their printed zeros, exact positive-tick/
live-needle alignment, explicit reference-observation budgets, at most one gentle
curvature inflection per guide, the 0.03-pixel path-approximation budget,
calibrated endpoints, ordering, range-multiplier invariance, every label's
own-contour or tangent-continuation association and full font-box clearances, and
both the 10 W / 0.4 W x1 and 100 W / 4 W x10 SWR 1.5 intersections. It also
renders every material theme and checks the physical needle layers over the
corrected guide ink.

The face proportions and visual envelope were derived from
perspective-corrected measurements of a Daiwa-style physical cross-needle
meter. Mechanical SWR calibration comes from the stored movement scales and
the power-ratio relationship above. The resource intentionally contains no
product logo or copied face artwork.
