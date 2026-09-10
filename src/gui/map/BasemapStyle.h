#pragma once

#include <QColor>
#include <QImage>
#include <array>

namespace AetherSDR::BasemapStyle {
inline constexpr char kBackgroundToken[] = "color.map.darkBackground";
inline constexpr char kDetailToken[] = "color.map.darkDetail";
// Identical luminance weights and ramp in the globe shader. Reversing
// luminance darkens pale terrain while lifting dark printed text.
inline constexpr float kRedWeight = 0.2126F;
inline constexpr float kGreenWeight = 0.7152F;
inline constexpr float kBlueWeight = 0.0722F;

// Night remains a shadow even when the surrounding app uses a light theme.
inline QColor nightColor(const QColor& background)
{
    return background.darker(150);
}

inline QImage darkImage(const QImage& original, const QColor& background,
                        const QColor& detail)
{
    QImage image = original.convertToFormat(QImage::Format_ARGB32);
    std::array<QRgb, 256> ramp;
    for (int i = 0; i < 256; ++i) {
        const float amount = 1.0F - i / 255.0F;
        const auto channel = [amount](int low, int high) {
            return qRound(low + (high - low) * amount);
        };
        ramp[i] = qRgb(channel(background.red(), detail.red()),
                       channel(background.green(), detail.green()),
                       channel(background.blue(), detail.blue()));
    }
    for (int y = 0; y < image.height(); ++y) {
        auto* pixels = reinterpret_cast<QRgb*>(image.scanLine(y));
        for (int x = 0; x < image.width(); ++x) {
            const QRgb source = pixels[x];
            const int luminance = qRound(kRedWeight * qRed(source)
                + kGreenWeight * qGreen(source) + kBlueWeight * qBlue(source));
            pixels[x] = (ramp[luminance] & 0x00ffffffU)
                | (source & 0xff000000U);
        }
    }
    return image;
}
} // namespace AetherSDR::BasemapStyle
