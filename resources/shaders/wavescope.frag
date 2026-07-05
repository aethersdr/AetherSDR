#version 440

// WAVE scope waveform layer. Evaluates all four view modes per-pixel from a
// 1-D column texture (RGBA32F: min, max, rms, peak for Scope/Envelope/History;
// level, amplitude for Bands) plus a 1-D R8 clip-flag texture. Emits
// PREMULTIPLIED color, transparent where there is no trace — the grid/label
// image is composited underneath and the text overlay on top by the widget.
//
// Column sampling is LINEAR so curves interpolate between columns exactly the
// way the QPainter path drew polylines between column points.

layout(location = 0) in vec2 v_uv;
layout(location = 0) out vec4 fragColor;

layout(binding = 1) uniform sampler2D columns;
layout(binding = 2) uniform sampler2D clips;

layout(std140, binding = 0) uniform U {
    vec4 plot;        // x, y, w, h in logical px (y down)
    vec4 widget;      // w, h, mode (0 Scope, 1 Envelope, 2 History, 3 Bands), zoom
    vec4 params;      // columnCount, lineWidthPx, barCount, hasData
    vec4 colWave;     // straight (non-premultiplied) rgba
    vec4 colPeak;
    vec4 colRms;
    vec4 colClip;
    vec4 colBarEmpty;
    vec4 colWarn;
    vec4 colRmsLight;
    vec4 colCap;
    vec4 colCenter;
};

// Straight color + coverage → premultiplied.
vec4 pm(vec4 c, float cov)
{
    float a = clamp(c.a * cov, 0.0, 1.0);
    return vec4(c.rgb * a, a);
}

// src OVER dst, both premultiplied.
vec4 over(vec4 dst, vec4 src)
{
    return src + dst * (1.0 - src.a);
}

// 1px-ish antialiased line coverage from distance to the line's center.
float lineCov(float dist, float halfWidth)
{
    return 1.0 - smoothstep(halfWidth, halfWidth + 0.6, dist);
}

void main()
{
    vec4 outc = vec4(0.0);
    vec2 px = v_uv * widget.xy;
    float mode = widget.z;
    float zoom = widget.w;

    if (params.w < 0.5
        || px.x < plot.x || px.x > plot.x + plot.z
        || px.y < plot.y || px.y > plot.y + plot.w) {
        fragColor = outc;
        return;
    }

    float u = clamp((px.x - plot.x) / plot.z, 0.0, 1.0);

    if (mode < 1.5) {
        // ── Scope / Envelope ────────────────────────────────────────────
        vec4 c = texture(columns, vec2(u, 0.5));
        float centerY = plot.y + plot.w * 0.5;
        float halfH = max(plot.w * 0.5 - 2.0, 1.0);
        float yBot = centerY - clamp(c.r * zoom, -1.0, 1.0) * halfH; // min sample
        float yTop = centerY - clamp(c.g * zoom, -1.0, 1.0) * halfH; // max sample
        float peak = clamp(c.a * zoom, 0.0, 1.0);
        float rms = clamp(c.b * zoom, 0.0, 1.0);
        float peakTop = centerY - peak * halfH;
        float peakBot = centerY + peak * halfH;
        float rmsTop = centerY - rms * halfH;
        float rmsBot = centerY + rms * halfH;

        if (mode < 0.5) {
            // Scope: filled min/max band (the per-column pen stroke),
            // then peak and RMS outlines.
            float lw = params.y;
            float bandCov = clamp(min(yBot + lw * 0.5, px.y + 0.5)
                                  - max(yTop - lw * 0.5, px.y - 0.5), 0.0, 1.0);
            outc = over(outc, pm(colWave, bandCov));

            float dP = min(abs(px.y - peakTop), abs(px.y - peakBot));
            outc = over(outc, pm(colPeak, lineCov(dP, 0.5)));
            float dR = min(abs(px.y - rmsTop), abs(px.y - rmsBot));
            outc = over(outc, pm(colRms, lineCov(dR, 0.7)));
        } else {
            // Envelope: translucent RMS ribbon + center line + outlines.
            float fillCov = (px.y >= rmsTop && px.y <= rmsBot) ? 1.0 : 0.0;
            outc = over(outc, pm(vec4(colWave.rgb, 65.0 / 255.0), fillCov));

            float dC = abs(px.y - centerY);
            outc = over(outc, pm(colCenter, lineCov(dC, 0.5)));

            float dR = min(abs(px.y - rmsTop), abs(px.y - rmsBot));
            outc = over(outc, pm(colRms, lineCov(dR, 0.65)));
            float dP = min(abs(px.y - peakTop), abs(px.y - peakBot));
            outc = over(outc, pm(vec4(colPeak.rgb, 210.0 / 255.0), lineCov(dP, 0.5)));
        }

        // Clip ticks: 4 px at the plot's top and bottom edges.
        float clipped = texture(clips, vec2(u, 0.5)).r;
        if (clipped > 0.0
            && (px.y <= plot.y + 4.0 || px.y >= plot.y + plot.w - 4.0)) {
            outc = over(outc, pm(colClip, 1.0));
        }
    } else {
        // ── History bars / Frequency bands ──────────────────────────────
        float barCount = max(params.z, 1.0);
        float slot = plot.z / barCount;
        float bi = clamp(floor((px.x - plot.x) / slot), 0.0, barCount - 1.0);
        bool bands = mode > 2.5;
        float barW = max(bands ? 4.0 : 2.0, slot - (bands ? 3.0 : 1.5));
        float x0 = plot.x + bi * slot + (slot - barW) * 0.5;
        if (px.x < x0 || px.x > x0 + barW) {
            fragColor = outc;
            return;
        }

        vec4 c = texture(columns, vec2((bi + 0.5) / barCount, 0.5));
        float clipped = texture(clips, vec2((bi + 0.5) / barCount, 0.5)).r;
        float maxH = max(plot.w - 1.0, 1.0);
        float bottom = plot.y + plot.w;

        // Rail behind the bar.
        outc = over(outc, pm(colBarEmpty, 1.0));

        float level;
        vec4 fill = colWave;
        vec4 capC = colCap;
        if (!bands) {
            float peak = clamp(c.a * zoom, 0.0, 1.0);
            float rms = clamp(c.b * zoom, 0.0, 1.0);
            if (peak <= 0.0) {
                fragColor = outc;
                return;
            }
            if (clipped > 0.0 || peak >= 0.96)      fill = colClip;
            else if (peak >= 0.78)                  fill = colWarn;
            else if (peak < 0.42)                   fill = colRms;
            level = peak;

            float h = max(level * maxH, 1.0);
            float top = bottom - h;
            if (px.y >= top) {
                float g = clamp((px.y - top) / h, 0.0, 1.0);
                vec3 rgb = mix(min(fill.rgb * 1.25, vec3(1.0)),
                               fill.rgb * (1.0 / 1.5), g);
                outc = over(outc, pm(vec4(rgb, 1.0), 1.0));
            }

            float rmsY = bottom - max(rms * maxH, 1.0);
            outc = over(outc, pm(colRmsLight, lineCov(abs(px.y - rmsY), 0.5)));

            float capY = max(plot.y, top - 2.0);
            capC = clipped > 0.0 ? colClip : colCap;
            outc = over(outc, pm(capC, lineCov(abs(px.y - capY), 0.5)));
        } else {
            level = c.r;
            float amp = c.g;
            if (amp >= 0.96)                        fill = colClip;
            else if (level >= 0.82)                 fill = colWarn;
            else if (level < 0.42)                  fill = colRms;

            float h = max(level * maxH, 1.0);
            float top = bottom - h;
            if (px.y >= top) {
                float g = clamp((px.y - top) / h, 0.0, 1.0);
                // 3-stop gradient: lighter(125) → fill at 55% → darker(145).
                vec3 rgb = g < 0.55
                    ? mix(min(fill.rgb * 1.25, vec3(1.0)), fill.rgb, g / 0.55)
                    : mix(fill.rgb, fill.rgb * (1.0 / 1.45), (g - 0.55) / 0.45);
                outc = over(outc, pm(vec4(rgb, 1.0), 1.0));
            }

            float capY = max(plot.y, top - 2.0);
            capC = amp >= 0.96 ? colClip : colCap;
            outc = over(outc, pm(capC, lineCov(abs(px.y - capY), 0.5)));
        }
    }

    fragColor = outc;
}
