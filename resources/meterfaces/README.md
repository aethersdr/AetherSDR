# Cross-needle meter geometry

`cross-needle-v12.json` is the editable source of truth for the PWR applet's
cross-needle Forward / Reflected power and SWR face. It is intentionally kept
as data rather than buried as painter constants so the face can be refined
without reverse-engineering `CrossNeedleMeterWidget`.

The geometry is expressed in a 1500 x 1000 design coordinate system. The
widget applies one uniform scale and centers that design in its available
rectangle; never compensate for a different widget aspect ratio by changing
individual points.

Important construction rules:

- `face_gradient` defines the old-instrument material entirely in code: a
  three-stop vertical base, one broad center backlight, and one symmetric edge
  vignette. All centers and radii use design coordinates. The painter also
  reuses this exact construction when clearing the upper scale underpass and
  the SWR label boxes, so those regions never become flat-color patches.
- `uplight_gradient` defines the selectable **Dark-room uplight** treatment:
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
- `dark_theme` defines the selectable **Graphite dark** treatment. It combines
  a matte charcoal card, a restrained neutral ambient lift, a low amber edge
  glow, a symmetric vignette, and scale-stable fixed-seed paper/paint grain.
  The same block owns its
  aged-ivory scale/text colors, copper SWR ink, steel-blue inner rules, dark
  mask palette, and metallic needle colors. These are material overrides
  only; all arcs, ticks, guides, pivots, and calibration remain shared.
- `needles` defines a code-drawn physical material stack without changing the
  calibrated movement rays. `line_*` is the exact pivot-to-tip body;
  `soft_shadow_*` and `shadow_*` form a restrained two-depth cast shadow;
  `edge_*` shades the side opposite the light; and `highlight_*` adds a narrow
  reflection on the upper-left edge. Edge offsets are measured perpendicular
  to each needle, while shadow offsets are ordinary design-space x/y values.
  Keep both edge strokes narrower than `line_width` and the soft shadow wider
  than the contact shadow so the effect survives at 260 x 173 without reading
  as a bevel or glow.
- `forward_scale.center` is the concealed right-hand movement pivot and
  `reflected_scale.center` is the concealed left-hand movement pivot.
- Tick angles are calibrated and non-linear. Keep `values`, `angles`, and
  `labels` the same length and do not replace them with evenly-spaced angles.
  The renderer derives a shape-preserving monotone cubic movement curve from
  those ticks. It passes through every stored calibration angle while keeping
  the first derivative continuous, so live needle motion does not form a
  visible elbow at each calibration interval.
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
- `typography` contains every face font size in design pixels. These values are
  intentionally larger than the photographic proof so labels survive the
  normal 260 x 173 applet size; positions remain controlled by the geometry.
- Each SWR guide retains the approved V12 source construction directly:
  `visible_upper`, the registered `registered_datum` at y = 880,
  `hidden_lower`, the source-fitted `quadratic_a`, and an independent
  `label_center`. The visible segment is an exact quadratic x(y) with at most
  six design pixels of bow; a C1 Hermite segment continues it behind the mask.
  Three source guides (`infinity`, `8`, and `1.7`) are deliberately straight.
  Do not regenerate this fan from the movement calibration: that discarded
  the perspective-corrected physical tracing and produced large S-shaped
  curves absent from both the source meter and approved V12 proof.
- The lower mask deliberately hides the movement centers and the convergence
  of the SWR constructions. Its boundary is symmetric about x = 750.
- Both power scales use the same range multiplier: x1 = 20 W, x10 = 200 W,
  and x100 = 2 kW.

After editing, run `cross_needle_meter_test` and use the automation bridge to
grab `crossNeedleMeter`. The validation block records the approved V12 proof:
100 W forward and SWR 1.5 derive 4 W reflected and intersect the printed 1.5
guide within two design pixels.

The test suite verifies all 12 registered guide paths, their straight/curved
classification, six-pixel bow limit, C1 hidden continuation, label anchors,
and the approved 100 W / 4 W active intersection. This prevents a mechanically
recomputed fan from replacing the perspective-corrected source geometry.

The construction was derived from perspective-corrected measurements of a
Daiwa-style physical cross-needle meter and checked against manufacturer
documentation. It intentionally contains no product logo or copied face
artwork.
