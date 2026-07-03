#version 440

// 2D FFT spectrum trace, evaluated per-pixel from a width×1 R32F column
// texture holding the display trace as normalized amplitude t (0 = bottom of
// the dynamic range, 1 = ref level). Replaces the CPU vertex bake (two
// feather/core triangle strips + a 2N fill strip re-generated and re-uploaded
// every frame): the CPU now uploads ~one float per device pixel column and
// this shader reproduces the same layers —
//   1. fill under the trace (heat-map or solid-gradient, alpha from the
//      fill slider; suppressed in lean mode),
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
    vec4 fillP;      // fillAlpha, heatMap (0/1), lean (0/1), pad
    vec4 fillColor;  // straight rgb, a unused
    vec4 darkColor;  // straight rgb (solid-mode base blend), a unused
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
    bool lean = fillP.z > 0.5;
    float fa = lean ? 0.0 : fillP.x;

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

    // ── Stroke (feather under core), slope-corrected width ─────────────
    float coreHalf = stroke.x;
    if (coreHalf > 0.0) {
        // dFdx of the trace height gives the segment slope in px/px; scale
        // the vertical distance to true perpendicular distance so steep
        // flanks keep their stroke width (the old geometry offset vertices
        // along the segment normal for the same reason).
        float slope = dFdx(yTrace) / max(fwidth(v_uv.x) * plot.x, 1e-4);
        float invLen = inversesqrt(1.0 + slope * slope);
        float d = abs(py - yTrace) * invLen;

        vec3 lineRgb = heat ? heatColor(t) : fillColor.rgb;
        float featherHalf = coreHalf + stroke.y;
        float aa = 0.75;
        float covF = 1.0 - smoothstep(featherHalf - aa, featherHalf + aa, d);
        outc = over(outc, pm(lineRgb, stroke.w, covF));
        float covC = 1.0 - smoothstep(coreHalf - aa, coreHalf + aa, d);
        outc = over(outc, pm(lineRgb, stroke.z, covC));
    }

    fragColor = outc;
}
