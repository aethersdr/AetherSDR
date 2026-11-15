// The panadapter's canon header lies over the top of the spectrum (#6386).
//
// Input: a press, double-click or wheel on the title, grip or the button
// strip's margin stays in the header and never reaches the spectrum behind it
// (a double-click on the title once retuned the slice). The empty middle is
// the spectrum's own, so input there still reaches it.
//
// Placement: the title sits at the left at its natural width, the buttons at
// the right, and place() reports the width the visible buttons take so the
// spectrum's top-right readouts end left of them.
//
// dBm arrows: the arrow row starts below the header but never leaves the FFT
// area at a short spectrum/waterfall split, where it once sat over the divider
// and its hit row went unhandled.

#include "TestSettingsProfile.h"
#include "gui/PanadapterHeader.h"
#include "gui/SpectrumChromeGeometry.h"

#include <QApplication>
#include <QHBoxLayout>
#include <QLabel>
#include <QMouseEvent>
#include <QPushButton>
#include <QTest>
#include <QToolButton>
#include <QWheelEvent>
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

// Stands in for the spectrum: counts the input that reaches it.
class Host : public QWidget {
public:
    int presses{0};
    int doubleClicks{0};
    int wheels{0};

protected:
    void mousePressEvent(QMouseEvent*) override { ++presses; }
    void mouseDoubleClickEvent(QMouseEvent*) override { ++doubleClicks; }
    void wheelEvent(QWheelEvent*) override { ++wheels; }
};

void sendWheel(QWidget* w, const QPoint& local)
{
    QWheelEvent ev(QPointF(local), QPointF(w->mapToGlobal(local)), QPoint(),
                   QPoint(0, 120), Qt::NoButton, Qt::NoModifier,
                   Qt::NoScrollPhase, false);
    QApplication::sendEvent(w, &ev);
}

// Every kind of input at `local` on `target`; returns what reached the host.
int inputReachingHost(Host& host, QWidget* target, const QPoint& local)
{
    host.presses = host.doubleClicks = host.wheels = 0;
    QTest::mouseClick(target, Qt::LeftButton, Qt::NoModifier, local);
    QTest::mouseDClick(target, Qt::LeftButton, Qt::NoModifier, local);
    sendWheel(target, local);
    return host.presses + host.doubleClicks + host.wheels;
}

} // namespace

int main(int argc, char** argv)
{
    TestSettingsProfile settingsProfile(QStringLiteral("aether-panadapter-header-test"));
    if (!settingsProfile.isValid()) {
        std::fprintf(stderr, "FAIL could not create isolated settings profile\n");
        return 1;
    }
    QApplication app(argc, argv);

    Host host;
    host.resize(600, 300);
    const PanadapterHeader header = PanadapterHeader::build(&host);
    header.title->setText(QStringLiteral("Slice A"));
    // The owner adds the close button to the row, as PanadapterApplet does.
    auto* close = new QToolButton;
    close->setFixedSize(18, 18);
    header.buttonRow->addWidget(close);
    close->hide();
    host.show();
    EXPECT_TRUE(QTest::qWaitForWindowExposed(&host));

    // ── Placement ─────────────────────────────────────────────────────────
    std::fprintf(stderr, "-- placement\n");
    EXPECT_TRUE(header.place(host.width()) == 0);   // no button showing
    header.popOut->show();
    header.maximize->show();
    close->show();
    const int reserve = header.place(host.width());
    EXPECT_TRUE(reserve > 3 * 18);
    EXPECT_TRUE(header.titleBar->geometry().topLeft() == QPoint(0, 0));
    EXPECT_TRUE(header.titleBar->height() == PanadapterHeader::kHeight);
    EXPECT_TRUE(header.titleBar->width() < host.width() / 2);
    EXPECT_TRUE(header.buttons->geometry().right() == host.width() - 1);
    EXPECT_TRUE(header.buttons->width() == reserve);
    EXPECT_TRUE(header.buttons->height() == PanadapterHeader::kHeight);

    // ── Input ─────────────────────────────────────────────────────────────
    std::fprintf(stderr, "-- input\n");
    const QPoint titleCenter = header.title->rect().center();
    EXPECT_TRUE(inputReachingHost(host, header.title, titleCenter) == 0);
    const QWidget* grip = header.titleBar->findChild<QLabel*>(QStringLiteral("panHeaderGrip"));
    EXPECT_TRUE(grip != nullptr);
    if (grip) {
        EXPECT_TRUE(inputReachingHost(host, const_cast<QWidget*>(grip),
                                      grip->rect().center()) == 0);
    }
    // The title strip's own margin, and the button strip's right margin.
    EXPECT_TRUE(inputReachingHost(host, header.titleBar, QPoint(2, 2)) == 0);
    EXPECT_TRUE(inputReachingHost(host, header.buttons,
                                  QPoint(header.buttons->width() - 2, 2)) == 0);
    // The empty middle between the pieces is the spectrum's own.
    const QPoint middle(host.width() / 2, PanadapterHeader::kHeight / 2);
    EXPECT_TRUE(host.childAt(middle) == nullptr);
    EXPECT_TRUE(inputReachingHost(host, &host, middle) == 3);
    // A button still takes its own click.
    int clicks = 0;
    QObject::connect(header.maximize, &QPushButton::clicked, [&] { ++clicks; });
    QTest::mouseClick(header.maximize, Qt::LeftButton);
    EXPECT_TRUE(clicks == 1);

    // ── dBm arrow row ─────────────────────────────────────────────────────
    std::fprintf(stderr, "-- dBm arrow row\n");
    constexpr int arrowH = 14;
    EXPECT_TRUE(dbmArrowRowTop(0, 200, arrowH) == 0);                 // no header
    EXPECT_TRUE(dbmArrowRowTop(PanadapterHeader::kHeight, 200, arrowH)
                == PanadapterHeader::kHeight);                        // below it
    // A 10% split of a 298 px floating pan: 27 px of FFT area. The row stays
    // inside it rather than over the divider.
    const int shortTop = dbmArrowRowTop(PanadapterHeader::kHeight, 27, arrowH);
    EXPECT_TRUE(shortTop + arrowH <= 27);
    EXPECT_TRUE(shortTop == 13);
    EXPECT_TRUE(dbmArrowRowTop(PanadapterHeader::kHeight, 10, arrowH) == 0);

    if (g_failures == 0) {
        std::printf("panadapter_header_test: all checks passed\n");
        return 0;
    }
    std::fprintf(stderr, "panadapter_header_test: %d failure(s)\n", g_failures);
    return 1;
}
