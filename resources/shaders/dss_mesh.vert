#version 440

// 3DSS GPU height-map mesh. Each vertex carries its grid position (u = column,
// v = row/depth) and an edge tag. The height comes from a ring-buffered dBm
// texture; geometry is a receding perspective trapezoid built entirely on the
// GPU, so pan/zoom never rebuild any vertices. Curtains carry exact retained
// rows through depth. Their white ridge outlines stay on the fixed perspective
// grid and crossfade between exact rows, avoiding both interpolated peak bounce
// and the coverage shimmer caused by translating a dense outline stack.
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
    float targetBandwidthMhz;
    float targetCenterOffsetMhz; // relative to the CPU's per-frame reference
    float rowFrequencyFrames;
    float scrollProgressRows; // continuous movement toward the back between rows
    float texRows;
    float scrollDistanceRows; // rows delivered by the current radio tile
    float colorRangeDb;       // stable colour aperture; independent of height range
    float validRows;
    float visibleRows;        // perspective depth; texture also holds exit reserve
    float plotWidthPx;        // DSS viewport width in physical pixels
    float plotHeightPx;       // DSS viewport height in physical pixels
    vec4  bgFill;             // plot background colour (for haze)
    vec4  shadowBands[8];     // low u, high u, centre u, band alpha
    vec4  shadowStyles[8];    // cue rgb, centre-line alpha
    vec4  shadowMeta;         // descriptor count, enabled, plot width px, pad
    // Fixed GLSL array sizes cannot consume a C++ constant. SpectrumWidget has
    // a matching static_assert against DssRenderer::kRows; update both shaders
    // and the frame-age clamp below if that row count changes.
    vec4  rowFrames[104];     // FFT centre/bw, supplemental centre/bw; age order
};

// RGBA16F ring rows: RG = exact FFT dBm/coverage; BA = calibrated native
// waterfall-tile overhang dBm/coverage. The exact FFT always wins. The wider
// tile is consulted only outside an FFT row's captured frequency frame.
layout(binding = 1) uniform sampler2D heightTex;

layout(location = 0) out float vLut;    // palette lookup coord (floor->peak gradient)
layout(location = 1) out float vDepth;  // row depth 0..1 for haze/fade
layout(location = 2) out float vEdge;   // edge tag passthrough
layout(location = 3) out float vBoundaryFade;
layout(location = 4) out float vFrequency;
layout(location = 5) out float vLayerAlpha;
layout(location = 6) out float vRibbonCoord;  // -1..+1 across AA outline
layout(location = 7) flat out float vOverlayLayer;

vec4 sampleHistory(float texU, float sourceAge, float rows)
{
    float cappedValidRows = clamp(validRows, 0.0, rows);
    if (sourceAge >= cappedValidRows) {
        return vec4(floorDbm, 0.0, floorDbm, 0.0);
    }

    // Amplitude is never interpolated between history rows. Interpolation
    // morphs one FFT into the next and makes every peak (and especially a
    // zoom-created floor edge) bounce vertically as it scrolls. Move the row's
    // geometry instead and sample its exact retained texture slot.
    return texture(
        heightTex, vec2(texU, fract(rowOffset + sourceAge / rows)));
}

float effectiveDbmAt(float geometryU, float sampleAge, float rows)
{
    int frameAge = int(clamp(
        floor(sampleAge + 0.5), 0.0, float(103)));
    vec4 frame = rowFrames[frameAge];
    float rowBandwidthMhz = frame.y > 0.0
        ? frame.y
        : max(targetBandwidthMhz, 0.000001);
    float sourceU =
        0.5 + (targetCenterOffsetMhz - frame.x) / rowBandwidthMhz
        + (geometryU - 0.5) * targetBandwidthMhz / rowBandwidthMhz;
    bool outsideRow = rowFrequencyFrames > 0.5
        && (sourceU < 0.0 || sourceU > 1.0);
    float texU = (texCols > 1.0)
        ? (sourceU * (texCols - 1.0) + 0.5) / texCols
        : 0.5;
    vec4 historySample = outsideRow
        ? vec4(floorDbm, 0.0, floorDbm, 0.0)
        : sampleHistory(texU, sampleAge, rows);
    if (historySample.g > 0.5) {
        return historySample.r;
    }

    float supplementalBandwidthMhz = frame.w;
    if (rowFrequencyFrames > 0.5 && supplementalBandwidthMhz > 0.0) {
        float supplementalU =
            0.5
            + (targetCenterOffsetMhz - frame.z)
                / supplementalBandwidthMhz
            + (geometryU - 0.5) * targetBandwidthMhz
                / supplementalBandwidthMhz;
        if (supplementalU >= 0.0 && supplementalU <= 1.0) {
            float supplementalTexU = (texCols > 1.0)
                ? (supplementalU * (texCols - 1.0) + 0.5) / texCols
                : 0.5;
            vec4 supplementalSample =
                sampleHistory(supplementalTexU, sampleAge, rows);
            if (supplementalSample.a > 0.5) {
                return supplementalSample.b;
            }
        }
    }
    return floorDbm;
}

vec2 ridgeNdcAt(float geometryU, float dbm, float geometryV)
{
    float strengthLinear = clamp(
        (dbm - floorDbm) / max(rangeDb, 1.0), 0.0, 1.0);
    float strengthHeight =
        pow(strengthLinear, max(zCurve, 0.05));
    float width = mix(1.0, backWidthFrac, geometryV);
    float plotX = 0.5 + (geometryU - 0.5) * width;
    float baselineY =
        mix(1.0, 1.0 - depthSpanFrac, geometryV);
    float ridge =
        strengthHeight * frontMaxRidgeFrac * width;
    float topY = baselineY - ridge;
    return vec2(plotX * 2.0 - 1.0, 1.0 - topY * 2.0);
}

void main()
{
    float u = inVert.x;
    float sourceV = inVert.y;  // discrete retained-row age
    float rows = max(texRows, 1.0);
    float distanceRows = max(scrollDistanceRows, 1.0);
    float depthRows = max(visibleRows, 1.0);
    float encodedEdge = inVert.z;
    bool ribbonOutline = encodedEdge <= -10.0;
    float ribbonCode = -encodedEdge;
    bool overlayLayer = ribbonOutline
        ? ribbonCode >= 20.0
        : (encodedEdge >= 2.0 || encodedEdge <= -2.0);
    float edge = ribbonOutline
        ? -1.0
        : (encodedEdge >= 2.0
               ? encodedEdge - 2.0
               : (encodedEdge <= -2.0 ? -1.0 : encodedEdge));
    float ribbonLocalCode = overlayLayer
        ? ribbonCode - 20.0
        : ribbonCode - 10.0;
    float ribbonSide = ribbonLocalCode < 0.5 ? -1.0 : 1.0;
    vRibbonCoord = ribbonOutline ? ribbonSide : 0.0;
    float sourceAge = sourceV * depthRows;
    float scrollPhase = clamp(
        scrollProgressRows / distanceRows, 0.0, 1.0);
    float remainingRows = clamp(
        distanceRows - scrollProgressRows, 0.0, distanceRows);
    bool boundaryRow =
        sourceAge < 0.5 || sourceAge > depthRows - 1.5;
    float sampleAge;
    float geometryV;
    if (ribbonOutline) {
        // Keep the dense white ridge stack phase-stable. Crossfade whole exact
        // FFT shapes at each fixed depth instead of either morphing their
        // heights or translating their raster coverage through subpixels.
        sampleAge =
            sourceAge + (overlayLayer ? 0.0 : distanceRows);
        geometryV = sourceV;
        vLayerAlpha =
            overlayLayer ? scrollPhase : 1.0 - scrollPhase;
    } else if (boundaryRow) {
        sampleAge =
            sourceAge + (overlayLayer ? 0.0 : distanceRows);
        geometryV = sourceV;
        vLayerAlpha = overlayLayer ? scrollPhase : 1.0;
    } else {
        sampleAge = sourceAge;
        geometryV = (sourceAge - remainingRows) / depthRows;
        vLayerAlpha = overlayLayer ? 1.0 : 0.0;
    }

    // Sample texel CENTRES on both axes so Nearest filtering can't pick up the
    // neighbouring row/column. rowOffset already carries the row half-texel; the
    // column maps geometry u in [0,1] onto centre (u*(cols-1)+0.5)/cols.
    // A zoom-created gap is not an amplitude measurement. Pin it to the
    // baseline before both height and colour mapping so later range changes
    // cannot recolour or lift it, while leaving row/layer visibility untouched.
    float dbm = effectiveDbmAt(u, sampleAge, rows);
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

    vec2 ndc = vec2(plotX * 2.0 - 1.0, 1.0 - plotY * 2.0);
    if (ribbonOutline) {
        // Metal's one-pixel line primitives toggle coverage as the mesh moves
        // through subpixels, producing a whole-surface strobe. Expand each
        // outline into a two-pixel screen-space ribbon; the fragment shader
        // analytically softens its edges.
        float du = 1.0 / max(texCols - 1.0, 1.0);
        float previousU = max(u - du, 0.0);
        float nextU = min(u + du, 1.0);
        vec2 previousNdc = ridgeNdcAt(
            previousU, effectiveDbmAt(previousU, sampleAge, rows), geometryV);
        vec2 nextNdc = ridgeNdcAt(
            nextU, effectiveDbmAt(nextU, sampleAge, rows), geometryV);
        vec2 viewportHalf =
            vec2(max(plotWidthPx, 1.0), max(plotHeightPx, 1.0)) * 0.5;
        vec2 tangentPx = (nextNdc - previousNdc) * viewportHalf;
        vec2 normalPx = length(tangentPx) > 0.0001
            ? normalize(vec2(-tangentPx.y, tangentPx.x))
            : vec2(0.0, 1.0);
        vec2 pixelToNdc = 1.0 / viewportHalf;
        if (u <= 0.0 || u >= 1.0) {
            // A perpendicular ribbon offset moves the endpoint sideways by an
            // amount that changes with the last FFT segment's slope. Against
            // the black outside of the perspective surface, the stacked row
            // endpoints then form a crawling one-pixel silhouette. Use a butt
            // boundary at the exact side plane: stable X and a fixed vertical
            // AA width. Interior vertices retain the perpendicular ribbon.
            ndc.y += ribbonSide * pixelToNdc.y;
        } else {
            ndc += normalPx * ribbonSide * pixelToNdc;
        }
    }
    gl_Position = vec4(ndc, 0.0, 1.0);

    // Restore the original curtain palette mapping: ridge = full amplitude
    // colour, floor = a dimmer share of that same amplitude colour.
    vLut = edge > 0.5 ? colorStrength * 0.6 : colorStrength;
    vDepth = clamp(geometryV, 0.0, 1.0);
    vEdge  = edge;
    vFrequency = u;
    vBoundaryFade = clamp(validRows - sampleAge, 0.0, 1.0);
    vOverlayLayer = overlayLayer ? 1.0 : 0.0;
}
