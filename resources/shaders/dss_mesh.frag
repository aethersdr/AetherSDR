#version 440

// Companion to dss_mesh.vert. Fill vertices (edge >= 0) draw the original
// full-height coloured curtains on phase-stable geometry. Outline vertices
// (edge < 0) draw the retained traces.

layout(location = 0) in float vLut;
layout(location = 1) in float vDepth;
layout(location = 2) in float vEdge;
layout(location = 3) in float vBoundaryFade;
layout(location = 4) in float vFrequency;   // viewport units; leaves [0,1] when widened
layout(location = 5) in float vLayerAlpha;
layout(location = 6) in float vRibbonCoord;
layout(location = 7) flat in float vOverlayLayer;
layout(location = 8) in float vSideDistance;  // to the row silhouette, plot units

layout(std140, binding = 0) uniform U {
    float rowOffset;
    float floorDbm;
    float rangeDb;
    float zCurve;
    float backWidthFrac;
    float depthSpanFrac;
    float frontMaxRidgeFrac;
    float haze;
    float texCols;
    float targetBandwidthMhz;
    float targetCenterOffsetMhz;
    float rowFrequencyFrames;
    float scrollProgressRows;
    float texRows;
    float scrollDistanceRows;
    float colorRangeDb;
    float validRows;
    float visibleRows;
    float plotWidthPx;
    float plotHeightPx;
    float rowSpanFactor;  // see dss_mesh.vert; 1.0 = classic clipped trapezoid
    float meshCols;       // mesh columns per row (>= texCols)
    // std140 pads this 22-float scalar run out to bgFill's vec4 alignment.
    vec4  bgFill;
    vec4  shadowBands[8];
    vec4  shadowStyles[8];
    vec4  shadowMeta;
    // Must match DssRenderer::kRows; SpectrumWidget enforces 104 with a
    // static_assert so a C++ row-count change cannot silently overrun this UBO.
    vec4  rowFrames[104];
};

layout(binding = 2) uniform sampler2D paletteLut;  // 256x1 RGBA8, floor(0)->peak(1)

layout(location = 0) out vec4 fragColor;

vec3 applySliceShadow(vec3 surfaceColor, float depthFade)
{
    if (shadowMeta.y < 0.5) {
        return surfaceColor;
    }

    int descriptorCount = int(clamp(shadowMeta.x, 0.0, 8.0));
    float shadowPlotWidthPx = max(shadowMeta.z, 1.0);
    float frequencyPixel =
        max(fwidth(vFrequency), 1.0 / shadowPlotWidthPx);
    vec3 color = surfaceColor;
    for (int i = 0; i < 8; ++i) {
        if (i >= descriptorCount) {
            break;
        }
        vec4 band = shadowBands[i];
        vec4 style = shadowStyles[i];
        float edgeSoftness = frequencyPixel * 0.8;
        float insideLow = smoothstep(
            band.x - edgeSoftness, band.x + edgeSoftness, vFrequency);
        float insideHigh = 1.0 - smoothstep(
            band.y - edgeSoftness, band.y + edgeSoftness, vFrequency);
        float bandMask = insideLow * insideHigh;

        // Near-black with a trace of the slice hue: this darkens the existing
        // DSS fragment in place instead of drawing a second surface above it.
        vec3 shadowColor = vec3(0.008) + style.rgb * 0.045;
        color = mix(
            color, shadowColor,
            clamp(band.w * bandMask * depthFade, 0.0, 1.0));

        float lineHalfWidth = frequencyPixel * 1.4;
        float lineMask = 1.0 - smoothstep(
            lineHalfWidth, lineHalfWidth * 2.1,
            abs(vFrequency - band.z));
        vec3 cueColor = mix(shadowColor, style.rgb, 0.42);
        color = mix(
            color, cueColor,
            clamp(style.w * lineMask * depthFade, 0.0, 1.0));
    }
    return color;
}

void main()
{
    if (vBoundaryFade <= 0.0 || vLayerAlpha <= 0.0) {
        discard;
    }
    float fade = clamp(1.0 - vDepth, 0.0, 1.0);   // 1 at front, 0 at back
    float shadowFade = pow(fade, 1.15);

    if (vEdge < -0.5) {
        // Neutral ridge highlight over the amplitude-coloured curtain. The
        // outline is a screen-space ribbon rather than a line primitive:
        // analytical edge coverage changes continuously as the trace moves
        // through subpixels instead of toggling whole pixels on and off.
        vec3 oc = mix(bgFill.rgb, vec3(0.92), 0.45 + 0.55 * fade);
        oc = applySliceShadow(oc, shadowFade);
        float flatOutline = 1.0 - smoothstep(0.0, 0.035, vLut);
        float flatOutlineScale = mix(1.0, 0.42, flatOutline);
        float ribbonCoverage =
            1.0 - smoothstep(0.15, 1.0, abs(vRibbonCoord));
        float sideDistancePx =
            max(vSideDistance, 0.0) * max(plotWidthPx, 1.0);
        // The stacked endpoints form a high-contrast comb against the black
        // outside of the perspective surface. Successive exact FFT rows can
        // legitimately put those endpoints at different heights, so any
        // endpoint crossfade either blinks or morphs vertically. Fade the
        // narrow exposed side strip of the neutral ridge; every interior
        // outline remains exact.
        float sideOutlineScale = smoothstep(0.0, 8.0, sideDistancePx);
        float outlineOpacity =
            (0.12 + 0.5 * fade) * vBoundaryFade
            * flatOutlineScale * ribbonCoverage * sideOutlineScale;
        float a;
        if (vOverlayLayer > 0.5) {
            // Base is drawn first with opacity A*(1-t). Compensate the overlay
            // under source-over blending so coincident old/new ridge pixels
            // retain opacity A rather than dimming at the middle of every
            // crossfade: Ab + Ao*(1-Ab) = A.
            float baseAlpha =
                outlineOpacity * (1.0 - vLayerAlpha);
            a = outlineOpacity * vLayerAlpha
                / max(1.0 - baseAlpha, 0.0001);
        } else {
            a = outlineOpacity * vLayerAlpha;
        }
        fragColor = vec4(oc, a);
        return;
    }

    vec3 c = texture(paletteLut, vec2(clamp(vLut, 0.0, 1.0), 0.5)).rgb;
    c = mix(c, bgFill.rgb, clamp(vDepth * haze, 0.0, 1.0));
    c = mix(bgFill.rgb, c, vBoundaryFade);
    c = applySliceShadow(c, shadowFade);
    float sideDistancePx =
        max(vSideDistance, 0.0) * max(plotWidthPx, 1.0);
    // The curtain has the same exposed stacked endpoints as the ridge. Fade
    // only the narrow side strip so adjacent curtain heights cannot form a
    // flashing comb against the black outside of the perspective surface.
    float sideSurfaceScale = smoothstep(0.0, 8.0, sideDistancePx);
    fragColor = vec4(c, vLayerAlpha * sideSurfaceScale);
}
