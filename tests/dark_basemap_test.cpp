#include "gui/map/DarkBasemapLayer.h"

#include <QApplication>
#include <iostream>

namespace {
bool expect(bool condition, const char* message)
{
    if (!condition) {
        std::cerr << "FAIL: " << message << '\n';
    }
    return condition;
}
}

int main(int argc, char** argv)
{
    QApplication app(argc, argv);
    bool ok = true;
    const QColor background(24, 33, 43);
    const QColor detail(200, 216, 232);
    const QColor night = AetherSDR::BasemapStyle::nightColor(background);
    ok &= expect(night.red() < background.red() && night.green() < background.green()
                     && night.blue() < background.blue(),
                 "dark-map night shading must darken every background channel");
    QImage source(256, 2, QImage::Format_ARGB32);
    for (int x = 0; x < source.width(); ++x) {
        source.setPixel(x, 0, qRgba(x, x, x, 255));
        source.setPixel(x, 1, qRgba(x, x, x, x));
    }
    const QImage original = source.copy();
    const QImage dark = AetherSDR::BasemapStyle::darkImage(source, background, detail);
    ok &= expect(source == original, "styling cannot modify the cached source");
    ok &= expect(dark.pixelColor(255, 0) == background, "white paper becomes dark background");
    ok &= expect(dark.pixelColor(0, 0) == detail, "black printed text becomes light detail");
    ok &= expect(qGray(dark.pixel(0, 0)) > 4 * qGray(dark.pixel(255, 0)),
                 "text and background have distinct brightness");
    for (int x = 0; x < 256; ++x) {
        ok &= expect(qAlpha(dark.pixel(x, 1)) == x, "source alpha is preserved");
        if (x > 0) {
            ok &= expect(qGray(dark.pixel(x - 1, 0)) >= qGray(dark.pixel(x, 0)),
                         "grey ramp reverses continuously without threshold edges");
        }
    }
    AetherSDR::DarkBasemapTile tile(source);
    for (int i = 0; i < 3; ++i) {
        tile.setStyle(true, background, detail);
        ok &= expect(tile.getImage() == dark, "repeated styling cannot compound the transform");
        tile.setStyle(false, background, detail);
        ok &= expect(tile.getImage().cacheKey() == source.cacheKey(),
                     "disable restores the shared original without decoding or copying");
    }
    tile.setStyle(true, detail, background);
    ok &= expect(tile.getImage().pixelColor(255, 0) == detail,
                 "a palette change immediately recolours an existing tile");
    const QImage premultiplied = source.convertToFormat(QImage::Format_ARGB32_Premultiplied);
    const QImage translucent = AetherSDR::BasemapStyle::darkImage(premultiplied, background, detail);
    ok &= expect(qAlpha(translucent.pixel(128, 1)) == 128,
                 "premultiplied input retains transparency");
    ok &= expect(AetherSDR::BasemapStyle::darkImage({}, background, detail).isNull(),
                 "empty image remains empty");
    return ok ? 0 : 1;
}
