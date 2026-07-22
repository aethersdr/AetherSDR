#version 440

// 3DSS GPU height-map mesh. Each vertex carries its grid position (u = column,
// v = row/depth) and an edge tag. The height comes from a ring-buffered dBm
// texture; geometry is a receding perspective trapezoid built entirely on the
// GPU, so pan/zoom never rebuild any vertices. Outputs NDC directly (matching
// spectrum.vert); occlusion is by back-to-front draw order, not a depth test.

layout(location = 0) in vec3 inVert;   // x = u [0,1] col, y = v [0,1) row(0=front), z = edge

layout(std140, binding = 0) uniform U {
    float rowOffset;          // (headRow + 0.5) / rows — ring scroll + half texel
    float floorDbm;           // dBm mapped to the baseline (strength 0)
    float rangeDb;            // dB span from floor to full ridge
    float zCurve;             // <1 expands the floor region (more floor visible)
    float backWidthFrac;      // back row width as a fraction of the front
    float depthSpanFrac;      // how far up the plot the back row recedes
    float frontMaxRidgeFrac;  // max ridge height (front) as a fraction of plot H
    float haze;               // atmospheric fade toward bgFill with depth
    float texCols;            // height-texture width, for column texel-centre sampling
    float frequencyScale;     // target bandwidth / retained-frame bandwidth
    float frequencyOffset;    // (target centre - retained centre) / retained bandwidth
    float frequencyPreview;   // non-zero while the interaction preview is active
    vec4  bgFill;             // plot background colour (for haze)
};

layout(binding = 1) uniform sampler2D heightTex;  // R16F, dBm, ring-buffered rows

layout(location = 0) out float vLut;    // palette lookup coord (floor->peak gradient)
layout(location = 1) out float vDepth;  // row depth 0..1 for haze/fade
layout(location = 2) out float vEdge;   // edge tag passthrough

void main()
{
    float u    = inVert.x;
    float v    = inVert.y;     // 0 = front/newest .. ~1 = back/oldest
    float edge = inVert.z;     // 0 = ridge, 1 = floor, -1 = outline

    // Sample texel CENTRES on both axes so Nearest filtering can't pick up the
    // neighbouring row/column. rowOffset already carries the row half-texel; the
    // column maps geometry u in [0,1] onto centre (u*(cols-1)+0.5)/cols.
    float texY = fract(rowOffset + v);
    float sourceU = 0.5 + frequencyOffset + (u - 0.5) * frequencyScale;
    bool outsidePreview = frequencyPreview > 0.5
        && (sourceU < 0.0 || sourceU > 1.0);
    float texU = (texCols > 1.0)
        ? (sourceU * (texCols - 1.0) + 0.5) / texCols
        : 0.5;
    float dbm = outsidePreview
        ? floorDbm
        : texture(heightTex, vec2(texU, texY)).r;
    // Linear strength drives COLOUR (LUT[sLin] = dbmToRgb(floor+sLin*range),
    // matching the CPU path); the zCurve lift applies to HEIGHT only.
    float sLin = clamp((dbm - floorDbm) / max(rangeDb, 1.0), 0.0, 1.0);
    float sH   = pow(sLin, max(zCurve, 0.05));   // non-linear Z: lift floor band

    // Receding perspective trapezoid in plot space [0,1] (0,0 = top-left).
    float w     = mix(1.0, backWidthFrac, v);          // narrows with depth
    float plotX = 0.5 + (u - 0.5) * w;
    float baseY = mix(1.0, 1.0 - depthSpanFrac, v);    // baseline rises with depth
    float ridge = sH * frontMaxRidgeFrac * w;          // far ridges shorter
    float topY  = baseY - ridge;                       // up = smaller y
    float plotY = (edge > 0.5) ? 1.0 : topY;           // floor edge -> plot bottom

    gl_Position = vec4(plotX * 2.0 - 1.0, 1.0 - plotY * 2.0, 0.0, 1.0);

    // Ridge carries the full signal colour; the floor edge keeps a dimmer share
    // of it (not pure palette[0]) so the whole curtain stays tinted by the
    // colormap instead of fading to black between the white trace lines.
    vLut   = (edge > 0.5) ? sLin * 0.6 : sLin;
    vDepth = v;
    vEdge  = edge;
}
