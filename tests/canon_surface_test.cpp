// CanonApplet, CanonPan and CanonCanvas (RFC #6226): the canon inside the
// main window. Each paints the same ground as CanonWindow with square
// corners, so every corner pixel is opaque where CanonWindow's are clear. Its
// close button only asks the owner to close it, and the window-only shortcuts
// (Escape, QKeySequence::Close) leave it alone.

#include "TestSettingsProfile.h"
#include "core/ThemeManager.h"
#include "gui/CanonSurface.h"
#include "gui/CanonWindow.h"

#include <QApplication>
#include <QHBoxLayout>
#include <QImage>
#include <QKeyEvent>
#include <QKeySequence>
#include <QPixmap>
#include <QTest>
#include <QToolButton>
#include <QVBoxLayout>
#include <QWidget>
#include <cstdio>

using namespace AetherSDR;

static int g_failures = 0;

#define EXPECT_TRUE(cond) do { \
    if (!(cond)) { \
        std::fprintf(stderr, "FAIL %s:%d  expected (%s) to be true\n", \
                     __FILE__, __LINE__, #cond); \
        ++g_failures; \
    } \
} while (0)

namespace {

constexpr QSize kSize(240, 160);

QList<QPoint> corners(const QImage& img)
{
    return { {0, 0}, {img.width() - 1, 0},
             {0, img.height() - 1}, {img.width() - 1, img.height() - 1} };
}

// The ground's corners are opaque, the hairline sits on the outer pixels, and
// the close button is at the top right, closing nothing on its own.
void checkSurface(CanonSurface* surface, const char* name)
{
    std::fprintf(stderr, "-- %s\n", name);
    QWidget host;
    host.resize(kSize + QSize(40, 40));
    surface->setParent(&host);
    surface->setGeometry(QRect(QPoint(20, 20), kSize));
    host.show();
    EXPECT_TRUE(QTest::qWaitForWindowExposed(&host));

    // Rendered into a clear image without the window background: grab()
    // would paint the parent behind the corners and hide a rounded one.
    QImage img(surface->size(), QImage::Format_ARGB32_Premultiplied);
    img.fill(Qt::transparent);
    surface->render(&img, QPoint(), QRegion(), QWidget::DrawChildren);
    img = img.convertToFormat(QImage::Format_ARGB32);
    EXPECT_TRUE(!img.isNull());
    for (const QPoint& c : corners(img)) {
        EXPECT_TRUE(qAlpha(img.pixel(c)) == 255);
    }
    // The hairline is lighter than the ground just inside it.
    const QColor edge = img.pixelColor(img.width() / 2, img.height() - 1);
    const QColor inner = img.pixelColor(img.width() / 2, img.height() - 3);
    EXPECT_TRUE(edge.lightness() > inner.lightness());

    QToolButton* close = surface->closeButton();
    EXPECT_TRUE(close && close->isVisible());
    EXPECT_TRUE(surface->isCloseButtonVisible());
    EXPECT_TRUE(close->geometry().right() > surface->width() * 3 / 4);
    EXPECT_TRUE(close->geometry().top() < surface->height() / 4);
    EXPECT_TRUE(close->accessibleName() == QStringLiteral("Close"));

    int requested = 0;
    QObject::connect(surface, &CanonSurface::closeRequested, [&] { ++requested; });
    QTest::mouseClick(close, Qt::LeftButton);
    EXPECT_TRUE(requested == 1);
    EXPECT_TRUE(surface->isVisible());   // the owner decides, not the surface

    // Escape and Ctrl+W belong to windows: the surface stays put.
    surface->setFocus();
    QTest::keyClick(surface, Qt::Key_Escape);
    QTest::keySequence(&host, QKeySequence(QKeySequence::Close));
    EXPECT_TRUE(surface->isVisible());
    EXPECT_TRUE(requested == 1);

    surface->setCloseButtonVisible(false);
    EXPECT_TRUE(!close->isVisible());
    EXPECT_TRUE(!surface->isCloseButtonVisible());
    surface->setCloseButtonVisible(true);
    EXPECT_TRUE(close->isVisible());

    // The body fills the surface inside the hairline.
    EXPECT_TRUE(surface->bodyWidget()->geometry()
                == QRect(QPoint(CanonSurface::kInset, CanonSurface::kInset),
                         kSize - QSize(2 * CanonSurface::kInset, 2 * CanonSurface::kInset)));

    // A resize moves the close button with the corner.
    surface->resize(kSize + QSize(100, 0));
    EXPECT_TRUE(close->geometry().right() > kSize.width());
}

} // namespace

int main(int argc, char** argv)
{
    TestSettingsProfile settingsProfile(QStringLiteral("aether-canon-surface-test"));
    if (!settingsProfile.isValid()) {
        std::fprintf(stderr, "FAIL could not create isolated settings profile\n");
        return 1;
    }
    QApplication app(argc, argv);

    checkSurface(new CanonApplet, "CanonApplet");
    checkSurface(new CanonPan, "CanonPan");
    checkSurface(new CanonCanvas, "CanonCanvas");

    // An owner's header can take the close button (the panadapter's): it
    // joins the row's layout and a resize no longer pulls it to the corner.
    {
        std::fprintf(stderr, "-- close button in a header row\n");
        QWidget host;
        host.resize(kSize + QSize(40, 40));
        auto* pan = new CanonPan(&host);
        pan->setGeometry(QRect(QPoint(20, 20), kSize));
        auto* body = new QVBoxLayout(pan->bodyWidget());
        body->setContentsMargins(0, 0, 0, 0);
        auto* header = new QWidget(pan->bodyWidget());
        header->setFixedHeight(32);
        auto* row = new QHBoxLayout(header);
        row->setContentsMargins(8, 3, 4, 3);
        row->addStretch();
        body->addWidget(header);
        body->addStretch();
        pan->placeCloseButtonIn(row);
        host.show();
        EXPECT_TRUE(QTest::qWaitForWindowExposed(&host));

        QToolButton* close = pan->closeButton();
        EXPECT_TRUE(close->parentWidget() == header);
        EXPECT_TRUE(row->indexOf(close) == row->count() - 1);
        pan->resize(kSize + QSize(100, 0));
        QApplication::processEvents();
        EXPECT_TRUE(close->geometry().right() == header->width() - 1 - 4);
        EXPECT_TRUE(close->geometry().top() == 3);

        int requested = 0;
        QObject::connect(pan, &CanonSurface::closeRequested, [&] { ++requested; });
        QTest::mouseClick(close, Qt::LeftButton);
        EXPECT_TRUE(requested == 1);
        pan->setCloseButtonVisible(false);
        EXPECT_TRUE(!close->isVisible());
    }

    // The shared ground still rounds CanonWindow: its corners stay clear.
    {
        std::fprintf(stderr, "-- CanonWindow corners\n");
        CanonWindow window(QStringLiteral("Corners"));
        window.resize(kSize);
        const QImage img = window.grab().toImage().convertToFormat(QImage::Format_ARGB32);
        for (const QPoint& c : corners(img)) {
            EXPECT_TRUE(qAlpha(img.pixel(c)) == 0);
        }
        EXPECT_TRUE(qAlpha(img.pixel(img.width() / 2, img.height() / 2)) == 255);
    }

    if (g_failures == 0) {
        std::printf("canon_surface_test: all checks passed\n");
        return 0;
    }
    std::fprintf(stderr, "canon_surface_test: %d failure(s)\n", g_failures);
    return 1;
}
