#version 440

// Companion to dss_mesh.vert. Fill vertices (edge >= 0) are coloured by a
// floor->peak palette LUT with a vertical gradient (ridge = signal colour,
// floor = floor colour) and hazed toward the background with depth. Outline
// vertices (edge < 0) draw a dim, depth-faded hairline along each ridge — the
// per-row trace line.

layout(location = 0) in float vLut;
layout(location = 1) in float vDepth;
layout(location = 2) in float vEdge;

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
    float pad0;
    float pad1;
    float pad2;
    vec4  bgFill;
};

layout(binding = 2) uniform sampler2D paletteLut;  // 256x1 RGBA8, floor(0)->peak(1)

layout(location = 0) out vec4 fragColor;

void main()
{
    float fade = clamp(1.0 - vDepth, 0.0, 1.0);   // 1 at front, 0 at back

    if (vEdge < -0.5) {
        // Dim trace outline — light hairline, brighter at the front, fading back.
        vec3  oc = mix(bgFill.rgb, vec3(0.92), 0.45 + 0.55 * fade);
        float a  = 0.12 + 0.5 * fade;
        fragColor = vec4(oc, a);
        return;
    }

    vec3 c = texture(paletteLut, vec2(clamp(vLut, 0.0, 1.0), 0.5)).rgb;
    c = mix(c, bgFill.rgb, clamp(vDepth * haze, 0.0, 1.0));
    fragColor = vec4(c, 1.0);
}
