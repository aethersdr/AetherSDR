#version 440

layout(location = 0) in vec4 v_color;
layout(location = 1) in float v_edge;
layout(location = 0) out vec4 fragColor;

void main()
{
    float coverage = 1.0;
    float edge = abs(v_edge);
    if (edge > 0.0) {
        float aa = max(fwidth(v_edge), 0.03);
        coverage = 1.0 - smoothstep(1.0 - aa, 1.0, edge);
    }
    fragColor = vec4(v_color.rgb, v_color.a * coverage);
}
