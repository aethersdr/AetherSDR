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
};

void main()
{
    // Every physical ring row retains the frequency frame in which it was
    // rasterized. Map the current viewport into that row independently, so a
    // newly arriving radio row can expose fresh frequencies during a pan while
    // older rows remain correctly located. Center is stored relative to a CPU
    // reference to preserve sub-kHz precision at VHF/UHF absolute frequencies.
    float physicalY = fract(v_uv.y + rowOffset);
    vec2 rowFrame = texture(rowFrequencyFrame, vec2(0.5, physicalY)).rg;
    if (rowFrame.y <= 0.0) {
        fragColor = vec4(0.0, 0.0, 0.0, 1.0);
        return;
    }
    float sourceU = 0.5 + (targetCenterOffsetMhz - rowFrame.x) / rowFrame.y
        + (v_uv.x - 0.5) * targetBandwidthMhz / rowFrame.y;
    if (sourceU < 0.0 || sourceU > 1.0) {
        fragColor = vec4(0.0, 0.0, 0.0, 1.0);
        return;
    }

    // Apply ring buffer offset per-pixel (not per-vertex, to avoid fract()
    // interpolation issues).
    fragColor = texture(tex, vec2(sourceU, physicalY));
}
