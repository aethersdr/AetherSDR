#version 440

// WAVE showcase visualizations — three fully-procedural, audio-reactive
// scenes rendered per-pixel from the scope's live column texture, a small
// envelope-history texture, and a handful of energy scalars. Demoscene
// rules apply: everything is analytic (no meshes, no extra passes), the
// palette comes entirely from the active theme, and the scene keeps moving
// in an ambient "attract mode" when no audio is flowing.
//
//   mode 0  RIDGE   — perspective heightfield of the envelope's recent
//                     history, hidden-line style (Joy Division × 3DSS)
//   mode 1  TUNNEL  — Amiga demo tunnel; the walls are textured by the
//                     live waveform columns, band energy rides the rings
//   mode 2  HORIZON — synthwave grid; the live waveform IS the mountain
//                     skyline, a scanline sun breathes with RMS

layout(location = 0) in vec2 v_uv;
layout(location = 0) out vec4 fragColor;

layout(binding = 1) uniform sampler2D columns;   // RGBA32F min,max,rms,peak
layout(binding = 2) uniform sampler2D history;   // R32F envelope rows (ring)

layout(std140, binding = 0) uniform U {
    vec4 widget;   // w, h, demoMode, zoom
    vec4 anim;     // timeSec, rms, peak, hasData
    vec4 energy;   // low, mid, high, histHeadV (head row / rowCount)
    vec4 colBg;    // theme background
    vec4 colA;     // accent
    vec4 colB;     // accent.bright
    vec4 colC;     // accent.success
    vec4 colD;     // accent.warning
    vec4 colE;     // accent.danger
};

const float kHistRows = 96.0;

float hash12(vec2 p)
{
    vec3 p3 = fract(vec3(p.xyx) * 0.1031);
    p3 += dot(p3, p3.yzx + 33.33);
    return fract((p3.x + p3.y) * p3.z);
}

// Envelope height for history row r (0 = newest) at horizontal t (0..1).
// Falls back to a drifting procedural wave in attract mode.
float ridgeHeight(float r, float t, float timeSec, float hasData)
{
    float v = fract(energy.w - (r + 0.5) / kHistRows);
    // 3-tap smooth so a noisy envelope reads as a mountain line, not fur.
    float h = texture(history, vec2(t, v)).r * 0.5
            + texture(history, vec2(t - 0.012, v)).r * 0.25
            + texture(history, vec2(t + 0.012, v)).r * 0.25;
    float attract = 0.30 + 0.22 * sin(t * 12.0 - timeSec * 1.7 + r * 0.35)
                          * sin(t * 5.0 + timeSec * 0.9 + r * 0.13);
    return mix(max(attract, 0.0), h, hasData);
}

vec3 sceneRidge(vec2 uv, float timeSec, float hasData)
{
    vec3 col = colBg.rgb * 0.55;                       // deep backdrop
    // Subtle starfield so the sky above the ridge isn't dead space.
    vec2 sp = floor(uv * vec2(widget.x, widget.y) / 3.0);
    float star = step(0.9975, hash12(sp)) * (0.4 + 0.6 * sin(timeSec * 2.0 + hash12(sp.yx) * 6.28));
    col += colB.rgb * star * 0.35;

    const float frontY = 0.94;    // baseline of the nearest row
    const float horizonY = 0.30;  // rows converge toward this line
    const float ampl = 0.44;      // peak height of the nearest row

    // Hidden-line rendering, front to back: every second history row so the
    // stack reads as distinct ridgelines. Bodies are near-black (they occlude
    // the rows behind); only the crest carries color — the Unknown Pleasures
    // look, colorized by the theme.
    float minY = 10.0;
    for (int i = 0; i < 96; i += 3) {
        float r = float(i);
        float t = r / (kHistRows - 1.0);
        float s = 1.0 / (1.0 + t * 4.0);              // perspective shrink
        float baseline = horizonY + (frontY - horizonY) * s;
        float xNorm = (uv.x - 0.5) / mix(1.0, 0.55, t) + 0.5;
        if (xNorm < 0.0 || xNorm > 1.0)
            continue;
        float h = ridgeHeight(r, xNorm, timeSec, hasData);
        float yTop = baseline - h * ampl * s;
        if (uv.y >= yTop && uv.y < minY) {
            float depthFade = 1.0 - t * 0.8;
            // Dark body with a faint depth-graded tint...
            vec3 body = mix(colBg.rgb * 0.22, colBg.rgb * 0.75,
                            exp(-(uv.y - yTop) * 9.0));
            // ...and a crisp bright crest line.
            float crest = 1.0 - smoothstep(0.0, 0.010 * s + 0.0035, uv.y - yTop);
            vec3 crestCol = mix(colB.rgb, colA.rgb, t);
            col = body + crestCol * crest * depthFade
                       + crestCol * 0.18 * exp(-(uv.y - yTop) * 55.0) * depthFade;
            minY = yTop;
        } else {
            minY = min(minY, yTop);
        }
    }
    return col;
}

vec3 sceneTunnel(vec2 uv, float timeSec, float rms, float hasData)
{
    float aspect = widget.x / max(widget.y, 1.0);
    vec2 p = (uv - 0.5) * vec2(aspect, 1.0);
    // Low-frequency energy sways the vanishing point.
    p += 0.06 * vec2(sin(timeSec * 0.6), cos(timeSec * 0.83))
             * (0.4 + energy.x * 1.2);

    float r = length(p);
    float ang = atan(p.y, p.x) / 6.2831853 + 0.5;      // 0..1 around
    float depth = 0.22 / max(r, 0.02) + timeSec * 1.4; // fly forward

    // Tunnel wall texture: the live waveform columns wrap around the ring —
    // the audio literally becomes the tunnel's wallpaper.
    float wallLive = texture(columns, vec2(fract(ang * 2.0 + depth * 0.03), 0.5)).b;
    float wallAttract = 0.35 + 0.35 * sin(depth * 6.0 + ang * 18.85)
                              * sin(ang * 12.566 - timeSec);
    float wall = mix(clamp(wallAttract, 0.0, 1.0),
                     pow(clamp(wallLive * 1.8, 0.0, 1.0), 1.4),
                     hasData);

    // Ring pulses: mid/high energy launches bright rings down the tunnel.
    float ring = smoothstep(0.42, 0.5, abs(fract(depth) - 0.5));
    float ringFine = smoothstep(0.46, 0.5, abs(fract(depth * 3.0) - 0.5));
    float ringGlow = ring * (0.45 + energy.y * 1.4) + ringFine * 0.18;
    float spokes = smoothstep(0.46, 0.5, abs(fract(ang * 8.0 + depth * 0.08) - 0.5));

    float huePhase = 0.5 + 0.5 * sin(depth * 0.55 + timeSec * 0.15);
    vec3 wallCol = mix(colA.rgb, colC.rgb, huePhase);
    wallCol = mix(wallCol, colB.rgb, ringGlow);

    float shade = clamp(r * 2.4, 0.0, 1.0);            // darker at depth
    vec3 col = colBg.rgb * 0.4;
    col += wallCol * wall * shade;
    col += colB.rgb * ringGlow * shade * 1.3;
    col += mix(colD.rgb, colE.rgb, energy.z) * spokes * shade * 0.22;

    // Core glow breathes with RMS (attract mode gets a soft heartbeat).
    float pulse = mix(0.4 + 0.25 * sin(timeSec * 2.2), clamp(rms * 4.0, 0.0, 1.2), hasData);
    col += mix(colD.rgb, colE.rgb, 0.35) * pulse * 0.12 / (r + 0.05);

    // Vignette.
    col *= 1.0 - 0.35 * smoothstep(0.55, 1.0, r);
    return col;
}

vec3 sceneHorizon(vec2 uv, float timeSec, float rms, float peak, float hasData)
{
    const float horizonY = 0.55;
    vec3 col;

    // Skyline height from the live columns (peak envelope), procedural in
    // attract mode.
    float live = texture(columns, vec2(uv.x, 0.5)).a;
    float attract = 0.35 + 0.3 * sin(uv.x * 9.0 + timeSec * 0.8)
                          * sin(uv.x * 23.0 - timeSec * 0.5);
    float sky = mix(clamp(attract, 0.0, 1.0), clamp(live, 0.0, 1.0), hasData);
    float mountainY = horizonY - sky * 0.28;

    if (uv.y < horizonY) {
        // Sky gradient.
        col = mix(colBg.rgb * 0.35, colBg.rgb, uv.y / horizonY);

        // Scanline sun, breathing with RMS.
        vec2 sunC = vec2(0.5, horizonY - 0.16);
        float aspect = widget.x / max(widget.y, 1.0);
        float sunR = 0.13 * (1.0 + mix(0.12 * sin(timeSec * 1.4), clamp(rms * 2.5, 0.0, 0.6), hasData));
        float d = length((uv - sunC) * vec2(aspect, 1.0));
        float sun = 1.0 - smoothstep(sunR - 0.004, sunR, d);
        // Horizontal gaps widen toward the sun's lower edge.
        float below = clamp((uv.y - sunC.y) / sunR, 0.0, 1.0);
        float gap = step(below * 0.55, fract((sunC.y - uv.y) * 46.0 + timeSec * 1.2));
        sun *= gap;
        vec3 sunCol = mix(colD.rgb, colE.rgb, clamp(below + 0.2, 0.0, 1.0));
        col = mix(col, sunCol, sun);
        col += sunCol * 0.25 * exp(-max(d - sunR, 0.0) * 18.0);   // corona

        // Waveform mountain silhouette in front of the sun.
        if (uv.y >= mountainY) {
            float crest = 1.0 - smoothstep(0.0, 0.01, uv.y - mountainY);
            col = mix(colBg.rgb * 0.3, colBg.rgb * 0.55, (uv.y - mountainY) / max(horizonY - mountainY, 1e-3));
            col += colB.rgb * crest * (0.7 + 0.6 * mix(0.3, peak, hasData));
        }
    } else {
        // Perspective grid floor.
        float dz = (uv.y - horizonY) + 1e-4;
        float worldZ = 0.06 / dz;                     // distance
        float worldX = (uv.x - 0.5) / dz * 0.35;

        col = mix(colBg.rgb, colBg.rgb * 0.25, dz * 2.0);
        float speed = 2.2 + mix(0.0, energy.x * 4.0, hasData);
        float lz = abs(fract(worldZ * 1.5 - timeSec * speed) - 0.5);
        float lx = abs(fract(worldX * 1.5) - 0.5);
        float fade = clamp(dz * 3.2, 0.0, 1.0);       // lines sharpen up close
        float gridZ = 1.0 - smoothstep(0.0, 0.06 * fade + 0.012, lz);
        float gridX = 1.0 - smoothstep(0.0, 0.06 * fade + 0.012, lx);
        vec3 gridCol = mix(colA.rgb, colC.rgb, mix(0.2, energy.z, hasData));
        col += gridCol * max(gridZ, gridX) * (0.35 + 0.65 * fade);

        // Sun reflection shimmer down the middle.
        float refl = exp(-abs(uv.x - 0.5) * 9.0) * (1.0 - dz) * 0.25;
        col += mix(colD.rgb, colE.rgb, 0.5) * refl
             * (0.6 + 0.4 * sin(uv.y * 120.0 + timeSec * 3.0));
    }

    // Horizon flash on peaks.
    float flash = exp(-abs(uv.y - horizonY) * 90.0)
                * (0.5 + mix(0.2 * (0.5 + 0.5 * sin(timeSec * 1.1)), clamp(peak * 1.4, 0.0, 1.2), hasData));
    col += colB.rgb * flash;
    return col;
}

void main()
{
    vec2 uv = v_uv;
    float mode = widget.z;
    float timeSec = anim.x;
    vec3 col;

    if (mode < 0.5)
        col = sceneRidge(uv, timeSec, anim.w);
    else if (mode < 1.5)
        col = sceneTunnel(uv, timeSec, anim.y, anim.w);
    else
        col = sceneHorizon(uv, timeSec, anim.y, anim.z, anim.w);

    fragColor = vec4(clamp(col, 0.0, 1.0), 1.0);
}
