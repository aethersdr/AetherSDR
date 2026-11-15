#version 440

// 2D FFT spectrum trace, evaluated per-pixel from a width×1 R32F column
// texture holding the display trace as normalized amplitude t (0 = bottom of
// the dynamic range, 1 = ref level). Replaces the CPU vertex bake (two
// feather/core triangle strips + a 2N fill strip re-generated and re-uploaded
// every frame): the CPU now uploads ~one float per device pixel column and
// this shader reproduces the same layers —
//   1. fill under the trace (heat-map or solid-gradient, alpha from the
//      fill slider),
//   2. a feather stroke (line width + featherPx, low alpha),
//   3. the core stroke (line width, high alpha),
// with slope-corrected stroke width via screen-space derivatives so steep
// peak flanks keep the same pixel width the old perpendicular-offset
// geometry had. LINEAR column sampling interpolates between columns exactly
// like the old per-point polyline. Emits PREMULTIPLIED color; the fill/line
// layer composites over the waterfall + background quads.

layout(location = 0) in vec2 v_uv;
layout(location = 0) out vec4 fragColor;

layout(binding = 1) uniform sampler2D columns;

layout(std140, binding = 0) uniform U {
    vec4 plot;       // wPx, hPx, columnCount, hasData
    vec4 stroke;     // coreHalfWidthPx, featherPx, coreAlpha, featherAlpha
    vec4 fillP;      // fillAlpha, heatMap (0/1), pad, pad
    vec4 fillColor;  // straight rgb, a unused
    vec4 darkColor;  // straight rgb (solid-mode base blend), a unused
    vec4 lineColor;  // straight rgb — trace stroke, independent of fill (#4239); a unused
};

// Straight color + coverage → premultiplied.
vec4 pm(vec3 rgb, float a, float cov)
{
    float pa = clamp(a * cov, 0.0, 1.0);
    return vec4(rgb * pa, pa);
}

// src OVER dst, both premultiplied.
vec4 over(vec4 dst, vec4 src)
{
    return src + dst * (1.0 - src.a);
}

// Same 4-stop heat ramp the vertex bake used (blue → cyan → green → red).
vec3 heatColor(float t)
{
    if (t < 0.25) {
        return vec3(0.0, t / 0.25, 1.0);
    } else if (t < 0.5) {
        return vec3(0.0, 1.0, 1.0 - (t - 0.25) / 0.25);
    } else if (t < 0.75) {
        return vec3((t - 0.5) / 0.25, 1.0, 0.0);
    }
    return vec3(1.0, 1.0 - (t - 0.75) / 0.25, 0.0);
}

void main()
{
    vec4 outc = vec4(0.0);
    if (plot.w < 0.5) {
        fragColor = outc;
        return;
    }

    float hPx = plot.y;
    float py = v_uv.y * hPx;                       // y-down device px
    float texX = (v_uv.x * (plot.z - 1.0) + 0.5) / plot.z;
    float t = clamp(texture(columns, vec2(texX, 0.5)).r, 0.0, 1.0);
    float yTrace = (1.0 - t) * hPx;                // trace top edge in px

    bool heat = fillP.y > 0.5;
    float fa = fillP.x;

    // ── Fill under the trace ────────────────────────────────────────────
    if (fa > 0.0 && py >= yTrace) {
        float g = (hPx > yTrace)
            ? clamp((py - yTrace) / (hPx - yTrace), 0.0, 1.0) : 0.0;
        vec3 rgb;
        float a;
        if (heat) {
            // Heat: trace color at the line fading to dark blue at the base.
            rgb = mix(heatColor(t), vec3(0.0, 0.0, 0.3), g);
            a = mix(fa * 0.3, fa, g);
        } else {
            // Solid: bright at the line, darker + fainter toward the base,
            // converging to a uniform solid as the slider rises (same
            // topAlpha/botAlpha/colorBlend math as the old yColor bake).
            vec3 topRgb = fillColor.rgb;
            vec3 botRgb = mix(darkColor.rgb, fillColor.rgb, fa);
            rgb = mix(topRgb, botRgb, g);
            a = mix(fa, fa * fa, g);
        }
        outc = over(outc, pm(rgb, a, 1.0));
    }

    // ── Stroke (feather under core), exact segment distance ────────────
    // #3967: the first cut approximated stroke distance as the vertical
    // distance scaled by a dFdx()-derived slope — distance to the segment's
    // INFINITE line. On steep segments that line passes near the entire pixel
    // column, so the stroke shot full-height spikes above and below the trace
    // ("spiky both above and below the line"). Compute the true distance to
    // the two polyline segments adjacent to this column instead (plus one
    // neighbor each side so wide strokes bleed across columns like the old
    // geometry quads did), clamped to the segment endpoints.
    float coreHalf = stroke.x;
    if (coreHalf > 0.0) {
        vec2 px = vec2(v_uv.x * plot.x, py);       // this fragment in device px
        float nCols = max(plot.z, 2.0);
        float colW = plot.x / nCols;               // px per column
        float i0 = floor(px.x / colW - 0.5);
        vec2 pt[4];
        for (int k = 0; k < 4; ++k) {
            float ci = clamp(i0 + float(k) - 1.0, 0.0, nCols - 1.0);
            float ty = clamp(texture(columns, vec2((ci + 0.5) / nCols, 0.5)).r,
                             0.0, 1.0);
            pt[k] = vec2((ci + 0.5) * colW, (1.0 - ty) * hPx);
        }
        float d = 1e9;
        for (int k = 0; k < 3; ++k) {
            vec2 a = pt[k];
            vec2 ab = pt[k + 1] - a;
            float tt = clamp(dot(px - a, ab) / max(dot(ab, ab), 1e-6), 0.0, 1.0);
            d = min(d, length(px - (a + tt * ab)));
        }

        // Falloff parity with the old vertex strips (spectrum.frag): solid
        // out to halfWidth-1, a 1 px fade to halfWidth; the feather adds one
        // more 1 px annulus at low alpha. The first cut's fixed ±0.75 px
        // smoothstep band read as a soft "blurred" stroke at 1x DPI.
        vec3 lineRgb = heat ? heatColor(t) : lineColor.rgb;
        float featherHalf = coreHalf + stroke.y;
        float covF = 1.0 - smoothstep(coreHalf, featherHalf, d);
        outc = over(outc, pm(lineRgb, stroke.w, covF));
        float covC = 1.0 - smoothstep(max(coreHalf - 1.0, 0.0), coreHalf, d);
        outc = over(outc, pm(lineRgb, stroke.z, covC));
    }

    fragColor = outc;
}
