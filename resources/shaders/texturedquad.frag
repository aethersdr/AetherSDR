#version 440

layout(location = 0) in vec2 v_uv;
layout(location = 0) out vec4 fragColor;

layout(binding = 1) uniform sampler2D tex;

layout(std140, binding = 0) uniform Uniforms {
    float rowOffset;
    float padding1;
    float padding2;
    float padding3;
    float scrollSampleOffsetUnit;
    float texelHeightUnit;
    float waterfallRows;
    float padding7;
};

void main()
{
    // Do not let the reconstruction kernel cross the logical bottom edge and
    // wrap the newest ring row into a one-pixel "echo" beneath unfilled data.
    if (v_uv.y >= 1.0 - texelHeightUnit) {
        fragColor = vec4(0.0, 0.0, 0.0, 1.0);
        return;
    }

    // rowOffset advances as soon as a row arrives. The residual one-row sample
    // offset starts at the prior ring position and closes to zero over the row
    // interval, preserving the previous frame at the handoff.
    float unit = max(texelHeightUnit, 0.000001);
    float rows = max(waterfallRows, 1.0);
    float logicalTexel =
        (v_uv.y + scrollSampleOffsetUnit) / unit - 0.5;
    float base = floor(logicalTexel);
    float f = fract(logicalTexel);
    float f2 = f * f;
    float f3 = f2 * f;

    // Cubic B-spline reconstruction keeps a stable vertical frequency response
    // at every sub-row phase. Bilinear filtering alternates between a sharp row
    // and a 50/50 blend, which makes noisy waterfalls appear to pulse.
    float w0 = (1.0 - 3.0 * f + 3.0 * f2 - f3) / 6.0;
    float w1 = (4.0 - 6.0 * f2 + 3.0 * f3) / 6.0;
    float w2 = (1.0 + 3.0 * f + 3.0 * f2 - 3.0 * f3) / 6.0;
    float w3 = f3 / 6.0;
    float y0 = fract(rowOffset
        + (clamp(base - 1.0, 0.0, rows - 1.0) + 0.5) * unit);
    float y1 = fract(rowOffset
        + (clamp(base,       0.0, rows - 1.0) + 0.5) * unit);
    float y2 = fract(rowOffset
        + (clamp(base + 1.0, 0.0, rows - 1.0) + 0.5) * unit);
    float y3 = fract(rowOffset
        + (clamp(base + 2.0, 0.0, rows - 1.0) + 0.5) * unit);
    fragColor =
          texture(tex, vec2(v_uv.x, y0)) * w0
        + texture(tex, vec2(v_uv.x, y1)) * w1
        + texture(tex, vec2(v_uv.x, y2)) * w2
        + texture(tex, vec2(v_uv.x, y3)) * w3;
}
