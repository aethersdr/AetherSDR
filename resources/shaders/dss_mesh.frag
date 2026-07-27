#version 440

// Companion to dss_mesh.vert. Fill vertices (edge >= 0) draw the original
// full-height coloured curtains on phase-stable geometry. Outline vertices
// (edge < 0) draw the retained traces.

layout(location = 0) in float vLut;
layout(location = 1) in float vDepth;
layout(location = 2) in float vEdge;
layout(location = 3) in float vBoundaryFade;
layout(location = 4) in float vFrequency;

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
    float frequencyScale;
    float frequencyOffset;
    float frequencyPreview;
    float scrollProgressRows;
    float texRows;
    float scrollDistanceRows;
    float colorRangeDb;
    float validRows;
    float padding17;
    float padding18;
    float padding19;
    vec4  bgFill;
    vec4  shadowBands[8];
    vec4  shadowStyles[8];
    vec4  shadowMeta;
};

layout(binding = 2) uniform sampler2D paletteLut;  // 256x1 RGBA8, floor(0)->peak(1)

layout(location = 0) out vec4 fragColor;

vec3 applySliceShadow(vec3 surfaceColor, float depthFade)
{
    if (shadowMeta.y < 0.5) {
        return surfaceColor;
    }

    int descriptorCount = int(clamp(shadowMeta.x, 0.0, 8.0));
    float plotWidthPx = max(shadowMeta.z, 1.0);
    float frequencyPixel = max(fwidth(vFrequency), 1.0 / plotWidthPx);
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
    float fade = clamp(1.0 - vDepth, 0.0, 1.0);   // 1 at front, 0 at back
    float shadowFade = pow(fade, 1.15);

    if (vEdge < -0.5) {
        // Original neutral ridge highlight over the amplitude-coloured curtain.
        vec3 oc = mix(bgFill.rgb, vec3(0.92), 0.45 + 0.55 * fade);
        oc = applySliceShadow(oc, shadowFade);
        float a = (0.12 + 0.5 * fade) * vBoundaryFade;
        fragColor = vec4(oc, a);
        return;
    }

    vec3 c = texture(paletteLut, vec2(clamp(vLut, 0.0, 1.0), 0.5)).rgb;
    c = mix(c, bgFill.rgb, clamp(vDepth * haze, 0.0, 1.0));
    c = mix(bgFill.rgb, c, vBoundaryFade);
    c = applySliceShadow(c, shadowFade);
    fragColor = vec4(c, 1.0);
}
