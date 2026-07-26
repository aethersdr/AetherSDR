#version 440

// 3DSS GPU height-map mesh. Each vertex carries its grid position (u = column,
// v = row/depth) and an edge tag. The height comes from a ring-buffered dBm
// texture; geometry is a receding perspective trapezoid built entirely on the
// GPU, so pan/zoom never rebuild any vertices. The per-row coloured curtains
// and their outlines stay on a fixed perspective grid while the retained data
// advances, avoiding shimmer from translating dense geometry across pixels.
// Outputs NDC directly (matching spectrum.vert).

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
    float scrollProgressRows; // continuous movement toward the back between rows
    float texRows;
    float scrollDistanceRows; // rows delivered by the current radio tile
    float colorRangeDb;       // stable colour aperture; independent of height range
    float validRows;
    float padding17;
    float padding18;
    float padding19;
    vec4  bgFill;             // plot background colour (for haze)
    vec4  shadowBands[8];     // low u, high u, centre u, band alpha
    vec4  shadowStyles[8];    // cue rgb, centre-line alpha
    vec4  shadowMeta;         // descriptor count, enabled, plot width px, pad
};

layout(binding = 1) uniform sampler2D heightTex;  // R16F, dBm, ring-buffered rows

layout(location = 0) out float vLut;    // palette lookup coord (floor->peak gradient)
layout(location = 1) out float vDepth;  // row depth 0..1 for haze/fade
layout(location = 2) out float vEdge;   // edge tag passthrough
layout(location = 3) out float vBoundaryFade;
layout(location = 4) out float vFrequency;

float sampleHistoryDbm(float texU, float historyV, float rows,
                       float remainingRows)
{
    float sourceAge = historyV * rows;
    float cappedValidRows = clamp(validRows, 0.0, rows);
    if (sourceAge >= cappedValidRows) {
        return floorDbm;
    }

    // The oldest grid row has no older neighbour to interpolate toward. Clamp
    // its sample age to the oldest retained texture row so it stays full-height
    // until eviction instead of collapsing and reappearing at the rear edge.
    float oldestRetainedAge = max(cappedValidRows - 1.0, 0.0);
    float sampleAge = min(sourceAge + remainingRows, oldestRetainedAge);
    float age0 = max(floor(sampleAge), 0.0);
    float age1 = min(age0 + 1.0, oldestRetainedAge);
    float ageBlend = fract(sampleAge);
    float dbm0 = texture(
        heightTex, vec2(texU, fract(rowOffset + age0 / rows))).r;
    float dbm1 = texture(
        heightTex, vec2(texU, fract(rowOffset + age1 / rows))).r;
    return mix(dbm0, dbm1, ageBlend);
}

void main()
{
    float u = inVert.x;
    float sourceV = inVert.y;  // discrete retained-row age
    float rows = max(texRows, 1.0);
    float distanceRows = max(scrollDistanceRows, 1.0);
    float edge = inVert.z;     // -1 = outline, 0 = ridge, 1 = curtain floor
    // Keep geometry phase-stable. Translating this dense perspective mesh makes
    // its subpixel line spacing shimmer, especially in the compressed rear.
    float geometryV = sourceV;
    float remainingRows = clamp(
        distanceRows - scrollProgressRows, 0.0, distanceRows);

    // Sample texel CENTRES on both axes so Nearest filtering can't pick up the
    // neighbouring row/column. rowOffset already carries the row half-texel; the
    // column maps geometry u in [0,1] onto centre (u*(cols-1)+0.5)/cols.
    float sourceU = 0.5 + frequencyOffset + (u - 0.5) * frequencyScale;
    bool outsidePreview = frequencyPreview > 0.5
        && (sourceU < 0.0 || sourceU > 1.0);
    float texU = (texCols > 1.0)
        ? (sourceU * (texCols - 1.0) + 0.5) / texCols
        : 0.5;
    float dbm = outsidePreview
        ? floorDbm
        : sampleHistoryDbm(texU, sourceV, rows, remainingRows);
    // Linear strength drives COLOUR (LUT[sLin] = dbmToRgb(floor+sLin*range),
    // matching the CPU path); the zCurve lift applies to HEIGHT only.
    float sLin = clamp((dbm - floorDbm) / max(rangeDb, 1.0), 0.0, 1.0);
    float sH   = pow(sLin, max(zCurve, 0.05));   // non-linear Z: lift floor band
    float colorStrength = clamp(
        (dbm - floorDbm) / max(colorRangeDb, 1.0), 0.0, 1.0);

    // Receding perspective trapezoid in plot space [0,1] (0,0 = top-left).
    float w = mix(1.0, backWidthFrac, geometryV);      // narrows with depth
    float plotX = 0.5 + (u - 0.5) * w;
    float baseY = mix(1.0, 1.0 - depthSpanFrac,
                      geometryV);                       // baseline rises with depth
    float ridge = sH * frontMaxRidgeFrac * w;          // far ridges shorter
    float topY  = baseY - ridge;                       // up = smaller y
    float plotY = edge > 0.5 ? 1.0 : topY;

    gl_Position = vec4(plotX * 2.0 - 1.0, 1.0 - plotY * 2.0, 0.0, 1.0);

    // Restore the original curtain palette mapping: ridge = full amplitude
    // colour, floor = a dimmer share of that same amplitude colour.
    vLut = edge > 0.5 ? colorStrength * 0.6 : colorStrength;
    vDepth = clamp(geometryV, 0.0, 1.0);
    vEdge  = edge;
    vFrequency = u;
    // Hide only unpopulated startup slots. Advance the availability edge by
    // remainingRows so the newest rear slot fades in continuously over the row
    // interval instead of popping opaque on arrival (matching the height path's
    // sub-row advance in sampleHistoryDbm). The rear six-row fade is
    // deliberately gone: the perspective already supplies depth haze, and that
    // fade left a visibly blurry band at the back of a full history.
    float sampleAge = sourceV * rows + remainingRows;
    float historyAvailability = clamp(validRows - sampleAge, 0.0, 1.0);
    vBoundaryFade = historyAvailability;
}
