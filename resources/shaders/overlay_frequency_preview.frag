#version 440

layout(location = 0) in vec2 v_uv;
layout(location = 0) out vec4 fragColor;

layout(std140, binding = 0) uniform PreviewUniforms {
    float frequencyScale;
    float frequencyOffset;
    float frequencyPreview;
    float spectrumRight;
    float spectrumBottom;
    float waterfallTop;
    float waterfallRight;
    float reserved;
};

layout(binding = 1) uniform sampler2D tex;

void main()
{
    vec2 sourceUv = v_uv;
    bool inSpectrum = v_uv.y < spectrumBottom && v_uv.x < spectrumRight;
    bool inWaterfall = v_uv.y >= waterfallTop && v_uv.x < waterfallRight;

    if (frequencyPreview > 0.5 && (inSpectrum || inWaterfall)) {
        float contentRight = inSpectrum ? spectrumRight : waterfallRight;
        float targetU = v_uv.x / max(contentRight, 0.0001);
        float sourceU = 0.5 + frequencyOffset
            + (targetU - 0.5) * frequencyScale;
        if (sourceU < 0.0 || sourceU > 1.0) {
            fragColor = vec4(0.0);
            return;
        }
        sourceUv.x = sourceU * contentRight;
    }

    fragColor = texture(tex, sourceUv);
}
