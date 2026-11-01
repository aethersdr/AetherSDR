// The Remote Access sign-in QR code (src/gui/QrCodeImage.cpp): the image a
// phone scans must have the quiet zone and finder patterns the QR standard
// requires, in exactly the two colours given, and fail cleanly for text too
// long to encode.

#include "gui/QrCodeImage.h"

#include <QGuiApplication>

#include <iostream>

namespace {

bool expect(bool condition, const char* message)
{
    if (!condition) {
        std::cerr << "FAIL: " << message << '\n';
    }
    return condition;
}

}  // namespace

int main(int argc, char** argv)
{
    qputenv("QT_QPA_PLATFORM", "offscreen");
    QGuiApplication app(argc, argv);
    using namespace AetherSDR;
    bool ok = true;
    const QColor dark(Qt::black), light(Qt::white);
    constexpr int px = 3;

    const QString url = QStringLiteral("https://login.tailscale.com/a/0123456789abcdef");
    const QImage img = qrCodeImage(url, px, dark, light);
    ok &= expect(!img.isNull(), "a sign-in URL encodes");
    // A version-v code is 17 + 4v modules, plus a four-module quiet zone on
    // each side. A 47-byte URL at medium correction needs version 4.
    const int modules = img.width() / px;
    ok &= expect(img.width() == img.height() && img.width() % px == 0, "square, whole modules");
    ok &= expect(modules == 17 + 4 * 4 + 8, "version 4 with its quiet zone");

    auto module = [&](int x, int y) { return img.pixelColor(x * px + px / 2, y * px + px / 2); };
    bool quiet = true;
    for (int i = 0; i < modules; ++i) {
        for (int q = 0; q < 4; ++q) {
            quiet &= module(i, q) == light && module(q, i) == light
                     && module(i, modules - 1 - q) == light && module(modules - 1 - q, i) == light;
        }
    }
    ok &= expect(quiet, "the quiet zone is light all round");

    // Finder pattern, top-left: a dark 7x7 ring, a light ring, a dark 3x3 core.
    auto finderAt = [&](int ox, int oy) {
        bool f = true;
        for (int y = 0; y < 7; ++y) {
            for (int x = 0; x < 7; ++x) {
                const bool ring = x == 0 || y == 0 || x == 6 || y == 6;
                const bool core = x >= 2 && x <= 4 && y >= 2 && y <= 4;
                f &= module(ox + x, oy + y) == ((ring || core) ? dark : light);
            }
        }
        return f;
    };
    ok &= expect(finderAt(4, 4), "top-left finder pattern");
    ok &= expect(finderAt(modules - 4 - 7, 4), "top-right finder pattern");
    ok &= expect(finderAt(4, modules - 4 - 7), "bottom-left finder pattern");

    bool twoColours = true;
    for (int y = 0; y < img.height(); y += 2) {
        for (int x = 0; x < img.width(); x += 2) {
            const QColor c = img.pixelColor(x, y);
            twoColours &= c == dark || c == light;
        }
    }
    ok &= expect(twoColours, "only the two given colours");

    ok &= expect(qrCodeImage(QString(5000, QLatin1Char('x')), px, dark, light).isNull(),
                 "text too long for a QR code gives a null image");
    ok &= expect(qrCodeImage(url, 0, dark, light).isNull(), "a zero module size gives a null image");

    std::cout << (ok ? "qr_code_image_test: PASS\n" : "qr_code_image_test: FAILED\n");
    return ok ? 0 : 1;
}
