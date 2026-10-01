// The VFO flag's DSP tab ends with two client-side launchers, AetherRX and
// AetherTX. They share a row and must always be the same width, whatever
// the mode-dependent set of radio-side toggles above them leaves free on
// that row: a fresh row gives each two columns, a half-used row gives each
// one. Before this, AetherTX always spanned the rightmost two columns, so in
// USB (ANFT on the same row) it was twice as wide as AetherRX.
#include "TestSettingsProfile.h"
#include "core/AppSettings.h"
#include "core/backends/SliceDelta.h"
#include "gui/VfoWidget.h"
#include "models/SliceModel.h"

#include <QApplication>
#include <QPushButton>
#include <QtTest>

#include <algorithm>
#include <climits>

using namespace AetherSDR;

namespace {

void setMode(SliceModel& slice, const QString& mode)
{
    SliceDelta delta;
    delta.mode = mode;
    slice.applyChanges(delta);
}

} // namespace

class VfoDspLauncherWidthTest : public QObject
{
    Q_OBJECT

private:
    // `fullFirstRow`: the mode shows at least four radio-side toggles, so the
    // grid's first row is full and marks the grid's true right edge. With
    // fewer, the launcher row is the widest thing in the grid and only the
    // weaker "nothing sticks out past them" check applies.
    void checkSameWidth(const QString& mode, bool fullFirstRow)
    {
        SliceModel slice(0);
        setMode(slice, mode);
        VfoWidget vfo;
        vfo.setSlice(&slice);
        vfo.show();
        QVERIFY(QTest::qWaitForWindowExposed(&vfo));
        // The tab panel is closed until a tab is chosen; open DSP the way the
        // operator does, through its tab button.
        QPushButton* dspTab = nullptr;
        for (QPushButton* b : vfo.findChildren<QPushButton*>()) {
            if (b->text() == QStringLiteral("DSP")) { dspTab = b; break; }
        }
        QVERIFY(dspTab);
        dspTab->click();
        QCoreApplication::processEvents();
        QCoreApplication::processEvents();

        auto* rx = vfo.findChild<QPushButton*>(QStringLiteral("aetherDspBtn"));
        auto* tx = vfo.findChild<QPushButton*>(QStringLiteral("aetherVoiceBtn"));
        QVERIFY(rx);
        QVERIFY(tx);
        QVERIFY(rx->isVisible());
        QVERIFY(tx->isVisible());
        // Same row, RX first, and neither wider than the other. A one-pixel
        // difference is the grid splitting an odd width between two equal
        // spans, not one launcher spanning more columns than the other,
        // which was a whole cell (about 60 px) before.
        QCOMPARE(tx->geometry().top(), rx->geometry().top());
        QVERIFY(tx->geometry().left() > rx->geometry().right());
        QVERIFY2(qAbs(tx->width() - rx->width()) <= 1,
                 qPrintable(QStringLiteral("AetherRX %1 px vs AetherTX %2 px")
                                .arg(rx->width()).arg(tx->width())));
        // And the pair fill their row to the grid's right edge: AetherTX ends
        // where the grid's first toggle row ends (that row is always full),
        // with no empty cell after it.
        int firstTop = INT_MAX;
        QList<QPushButton*> toggles;
        for (QPushButton* b : vfo.findChildren<QPushButton*>()) {
            if (b == rx || b == tx || !b->isVisible()) continue;
            if (!b->objectName().startsWith(QStringLiteral("dsp"))) continue;
            toggles.append(b);
            firstTop = std::min(firstTop, b->mapTo(&vfo, QPoint(0, 0)).y());
        }
        int gridRight = 0;
        for (QPushButton* b : toggles) {
            if (b->mapTo(&vfo, QPoint(0, 0)).y() == firstTop)
                gridRight = std::max(gridRight, b->mapTo(&vfo, b->rect().topRight()).x());
        }
        QVERIFY(gridRight > 0);
        const int txRight = tx->mapTo(&vfo, tx->rect().topRight()).x();
        QVERIFY(txRight >= gridRight);
        if (fullFirstRow) QCOMPARE(txRight, gridRight);
    }

private slots:
    // USB shows ANF/ANFL/ANFT, so the launchers share their row with ANFT.
    void sameWidthInUsb() { checkSameWidth(QStringLiteral("USB"), true); }
    // CW hides the notch trio and shows APF, which changes what is left on
    // the launchers' row.
    void sameWidthInCw()  { checkSameWidth(QStringLiteral("CW"), true); }
    // DIGU hides the notch trio and APF both, leaving under a full row.
    void sameWidthInDigu() { checkSameWidth(QStringLiteral("DIGU"), false); }
};

int main(int argc, char** argv)
{
    TestSettingsProfile profile(QStringLiteral("vfo-dsp-launcher-width"));
    if (!profile.isValid()) {
        return 1;
    }
    QApplication app(argc, argv);
    AppSettings::instance().load();
    VfoDspLauncherWidthTest test;
    return QTest::qExec(&test, argc, argv);
}

#include "vfo_dsp_launcher_width_test.moc"
