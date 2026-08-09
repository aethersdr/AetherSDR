#version 440

layout(location = 0) in vec2 v_uv;
layout(location = 0) out vec4 fragColor;

layout(binding = 1) uniform sampler2D tex;
layout(binding = 2) uniform sampler2D rowFrequencyFrame;
layout(binding = 3) uniform sampler2D supplementalTex;

layout(std140, binding = 0) uniform Uniforms {
    float rowOffset;
    float targetCenterOffsetMhz;
    float targetBandwidthMhz;
    float rowFrequencyFrames;
    float scrollSampleOffsetUnit;
    float texelHeightUnit;
    float waterfallRows;
    float padding7;
};

vec4 sampleWaterfallSourceAge(float sourceAge, float unit, float rows)
{
    // The source age is logical waterfall history, not a physical ring row.
    // Clamp it before applying the write-row origin so the cubic kernel cannot
    // alias the opposite end of the ring at either history boundary.
    float clampedAge = clamp(sourceAge, 0.0, rows - 1.0);
    float rowY = fract(rowOffset + (clampedAge + 0.5) * unit);
    vec4 rowFrame = texture(rowFrequencyFrame, vec2(0.5, rowY));
    if (rowFrame.y <= 0.0) {
        return vec4(0.0, 0.0, 0.0, 1.0);
    }
    float sourceU = 0.5 + (targetCenterOffsetMhz - rowFrame.x) / rowFrame.y
        + (v_uv.x - 0.5) * targetBandwidthMhz / rowFrame.y;
    if (sourceU >= 0.0 && sourceU <= 1.0) {
        return texture(tex, vec2(sourceU, rowY));
    }
    if (rowFrame.w > 0.0) {
        float supplementalU =
            0.5 + (targetCenterOffsetMhz - rowFrame.z) / rowFrame.w
            + (v_uv.x - 0.5) * targetBandwidthMhz / rowFrame.w;
        if (supplementalU >= 0.0 && supplementalU <= 1.0) {
            return texture(supplementalTex, vec2(supplementalU, rowY));
        }
    }
    return vec4(0.0, 0.0, 0.0, 1.0);
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
    float unit = max(texelHeightUnit, 0.000001);
    float rows = max(waterfallRows, 1.0);
    float logicalTexel =
        (v_uv.y + scrollSampleOffsetUnit) / unit - 0.5;
    float base = floor(logicalTexel);
    float f = fract(logicalTexel);
    float f2 = f * f;
    float f3 = f2 * f;
    float w0 = (1.0 - 3.0 * f + 3.0 * f2 - f3) / 6.0;
    float w1 = (4.0 - 6.0 * f2 + 3.0 * f3) / 6.0;
    float w2 = (1.0 + 3.0 * f + 3.0 * f2 - 3.0 * f3) / 6.0;
    float w3 = f3 / 6.0;
    fragColor =
          sampleWaterfallSourceAge(base - 1.0, unit, rows) * w0
        + sampleWaterfallSourceAge(base,       unit, rows) * w1
        + sampleWaterfallSourceAge(base + 1.0, unit, rows) * w2
        + sampleWaterfallSourceAge(base + 2.0, unit, rows) * w3;
}
