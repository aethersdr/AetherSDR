#pragma once

#include <QString>
#include <QtMath>

#include <algorithm>
#include <cmath>

// One home for every number the two city-light renderers must agree on.
// CityLightsSource::nightImage() applies these on the CPU for the flat map;
// GlobeMapView compiles the same values into its fragment shader. Keeping
// them here means a tuning change cannot silently diverge the projections.
namespace AetherSDR::CityLightsShading {

// Saved-control defaults shared by the dialog, the map facade, the CPU
// source and the globe. Warmth 0 keeps NASA's original grayscale appearance.
constexpr int kDefaultBrightness = 70;
constexpr int kDefaultFaintLights = 50;
constexpr int kDefaultWarmth = 0;

// Faint lights: gamma applied to light coverage, 1.0 at 0 percent falling to
// 1.0 - kFaintLightsGammaSpan at 100 percent.
constexpr double kFaintLightsGammaSpan = 0.65;
// Warmth: fraction removed from green and blue at 100 percent.
constexpr double kWarmthGreenLoss = 0.15;
constexpr double kWarmthBlueLoss = 0.40;
// Civil twilight: lights are absent at the horizon and full at this
// solar depression.
constexpr double kCivilTwilightDegrees = 6.0;

inline double faintLightsGamma(int percent)
{
    return 1.0 - kFaintLightsGammaSpan * std::clamp(percent, 0, 100) / 100.0;
}

inline double twilightSine()
{
    return std::sin(qDegreesToRadians(kCivilTwilightDegrees));
}

inline QString glslNumber(double value)
{
    return QString::number(value, 'f', 9);
}

// Globe fragment shader. Gamma is a uniform (computed by faintLightsGamma);
// the warmth coefficients and twilight span are baked in from the constants
// above so they cannot drift from the CPU path.
inline QString fragmentShaderSource()
{
    return QStringLiteral(R"(
        varying highp vec3 earthPosition;
        uniform sampler2D lights;
        uniform highp vec4 bounds;
        uniform lowp float opacity;
        uniform highp float lightsGamma;
        uniform highp float warmth;
        uniform highp vec3 sunDirection;
        uniform lowp float nightOnly;
        void main() {
            highp vec3 n = normalize(earthPosition);
            // GIBS EPSG:3857 stops at +/-85.051129 degrees. Do not stretch
            // the last image row over the poles. Derive UV per fragment
            // so coarse sphere geometry does not distort close-up lights.
            if (abs(n.y) > 0.996272077) discard;
            highp float latitude = asin(clamp(n.y, -1.0, 1.0));
            highp vec2 worldUv = vec2(atan(n.x, n.z) / 6.283185307 + 0.5,
                0.5 - log(tan(0.785398163 + latitude / 2.0)) / 6.283185307);
            highp vec2 uv = (worldUv - bounds.xy) / (bounds.zw - bounds.xy);
            if (uv.x < 0.0 || uv.x > 1.0 || uv.y < 0.0 || uv.y > 1.0) discard;
            highp vec4 light = texture2D(lights, uv);
            // Adjust the resident original texture, never upload on slider changes.
            if (light.a > 0.0) {
                light *= pow(light.a, lightsGamma) / light.a;
            }
            highp float t = clamp(-dot(n, sunDirection) / TWILIGHT_SINE, 0.0, 1.0);
            highp float night = mix(1.0, t * t * (3.0 - 2.0 * t), nightOnly);
            light.rgb *= vec3(1.0, 1.0 - WARMTH_GREEN_LOSS * warmth, 1.0 - WARMTH_BLUE_LOSS * warmth);
            gl_FragColor = light * (opacity * night);
        }
    )")
        .replace(QLatin1String("TWILIGHT_SINE"), glslNumber(twilightSine()))
        .replace(QLatin1String("WARMTH_GREEN_LOSS"), glslNumber(kWarmthGreenLoss))
        .replace(QLatin1String("WARMTH_BLUE_LOSS"), glslNumber(kWarmthBlueLoss));
}

} // namespace AetherSDR::CityLightsShading
