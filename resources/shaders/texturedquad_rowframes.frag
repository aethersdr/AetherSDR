#version 440

layout(location = 0) in vec2 v_uv;
layout(location = 0) out vec4 fragColor;

layout(binding = 1) uniform sampler2D tex;
layout(binding = 2) uniform sampler2D rowFrequencyFrame;

layout(std140, binding = 0) uniform Uniforms {
    float rowOffset;
    float targetCenterOffsetMhz;
    float targetBandwidthMhz;
    float rowFrequencyFrames;
    float scrollSampleOffsetUnit;
    float texelHeightUnit;
    float padding6;
    float padding7;
};

vec4 sampleWaterfallRow(float rowIndex, float unit)
{
    float rowY = fract((rowIndex + 0.5) * unit);
    vec2 rowFrame = texture(rowFrequencyFrame, vec2(0.5, rowY)).rg;
    if (rowFrame.y <= 0.0) {
        return vec4(0.0, 0.0, 0.0, 1.0);
    }
    float sourceU = 0.5 + (targetCenterOffsetMhz - rowFrame.x) / rowFrame.y
        + (v_uv.x - 0.5) * targetBandwidthMhz / rowFrame.y;
    if (sourceU < 0.0 || sourceU > 1.0) {
        return vec4(0.0, 0.0, 0.0, 1.0);
    }
    return texture(tex, vec2(sourceU, rowY));
}

void main()
{
    // Terminate the logical bottom edge instead of allowing the four-row
    // reconstruction kernel to wrap the newest row into a one-pixel echo.
    if (v_uv.y >= 1.0 - texelHeightUnit) {
        fragColor = vec4(0.0, 0.0, 0.0, 1.0);
        return;
    }

    // Every physical ring row retains the frequency frame in which it was
    // rasterized. Reconstruct four adjacent rows independently so each sample
    // is remapped through its own frame while the vertical motion remains
    // phase-stable instead of alternating sharp/blurred.
    float physicalY = fract(
        v_uv.y + rowOffset + scrollSampleOffsetUnit);
    float unit = max(texelHeightUnit, 0.000001);
    float texel = physicalY / unit - 0.5;
    float base = floor(texel);
    float f = fract(texel);
    float f2 = f * f;
    float f3 = f2 * f;
    float w0 = (1.0 - 3.0 * f + 3.0 * f2 - f3) / 6.0;
    float w1 = (4.0 - 6.0 * f2 + 3.0 * f3) / 6.0;
    float w2 = (1.0 + 3.0 * f + 3.0 * f2 - 3.0 * f3) / 6.0;
    float w3 = f3 / 6.0;
    fragColor =
          sampleWaterfallRow(base - 1.0, unit) * w0
        + sampleWaterfallRow(base,       unit) * w1
        + sampleWaterfallRow(base + 1.0, unit) * w2
        + sampleWaterfallRow(base + 2.0, unit) * w3;
}
