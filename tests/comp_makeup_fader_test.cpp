// The compressor's makeup gain used to be a knob in the foot row. It is now a
// fader riding the Out meter: -12 dB at the bottom of the bar, through a 0 dB
// detent, to +24 dB at the top. That mapping is the whole contract — an
// operator reads their makeup off where the handle sits — and it is computed
// from paint-time geometry, which is exactly the kind of thing that silently
// drifts when the meter's padding or footer height changes. Nothing else
// checks it: the engine only ever sees the dB value that comes out.
//
// The bar is not the whole widget. Label, level readout and makeup readout all
// claim rows, so the test primes the geometry with a real paint (grab()) and
// then works in the coordinates the operator actually clicks.

#include "gui/ClientCompMeter.h"

#include <QSignalSpy>
#include <QtTest>

using namespace AetherSDR;

class CompMakeupFaderTest : public QObject {
    Q_OBJECT

private slots:
    void initTestCase();
    void defaultsToUnityGain();
    void topOfBarIsPlus24AndBottomIsMinus12();
    void detentSitsOneThirdUp();
    void clampsToTheEngineRange();
    void wheelStepsHalfADecibel();
    void doubleClickReturnsToUnity();
    void keyboardDrivesTheFader();
    void plainMetersIgnoreTheMouse();

private:
    // A meter sized like the one in StripCompPanel, painted once so the bar
    // geometry the hit-test reads is real.
    static void prime(ClientCompMeter& m)
    {
        m.setMode(ClientCompMeter::Mode::Level);
        m.setLabel(QStringLiteral("Out"));
        m.setTickSide(ClientCompMeter::TickSide::Right);
        m.setShowValueLabel(true);
        m.resize(72, 320);
        m.grab();
    }

    // The y of a given makeup dB, derived the same way paint derives it.
    static int yFor(ClientCompMeter& m, float db)
    {
        const int labelH  = 12;
        const int valueH  = 14;
        const int makeupH = 13;
        const int top     = labelH + 2;
        const int h       = m.height() - labelH - 4 - valueH - makeupH;
        const float norm  = (db - ClientCompMeter::kMakeupMinDb)
                          / (ClientCompMeter::kMakeupMaxDb - ClientCompMeter::kMakeupMinDb);
        return static_cast<int>(top + h - norm * h);
    }

    static void clickAt(ClientCompMeter& m, int y)
    {
        QTest::mousePress(&m, Qt::LeftButton, Qt::NoModifier, QPoint(m.width() / 2, y));
        QTest::mouseRelease(&m, Qt::LeftButton, Qt::NoModifier, QPoint(m.width() / 2, y));
    }
};

void CompMakeupFaderTest::initTestCase()
{
    QVERIFY2(qEnvironmentVariableIsSet("QT_QPA_PLATFORM"),
             "run offscreen; the test paints");
}

void CompMakeupFaderTest::defaultsToUnityGain()
{
    ClientCompMeter m;
    QCOMPARE(m.makeupDb(), 0.0f);
    QVERIFY(!m.makeupControlEnabled());
}

void CompMakeupFaderTest::topOfBarIsPlus24AndBottomIsMinus12()
{
    ClientCompMeter m;
    m.setMakeupControlEnabled(true);
    prime(m);

    clickAt(m, yFor(m, ClientCompMeter::kMakeupMaxDb));
    QVERIFY2(m.makeupDb() > 23.0f, qPrintable(QString::number(m.makeupDb())));

    clickAt(m, yFor(m, ClientCompMeter::kMakeupMinDb));
    QVERIFY2(m.makeupDb() < -11.0f, qPrintable(QString::number(m.makeupDb())));
}

void CompMakeupFaderTest::detentSitsOneThirdUp()
{
    ClientCompMeter m;
    m.setMakeupControlEnabled(true);
    prime(m);

    // 0 dB is a third of the way up a -12..+24 span. If the range or the
    // mapping is edited without the other, this is what catches it.
    clickAt(m, yFor(m, 0.0f));
    QVERIFY2(std::fabs(m.makeupDb()) < 0.6f, qPrintable(QString::number(m.makeupDb())));
}

void CompMakeupFaderTest::clampsToTheEngineRange()
{
    ClientCompMeter m;
    m.setMakeupControlEnabled(true);
    prime(m);

    m.setMakeupDb(999.0f);
    QCOMPARE(m.makeupDb(), ClientCompMeter::kMakeupMaxDb);
    m.setMakeupDb(-999.0f);
    QCOMPARE(m.makeupDb(), ClientCompMeter::kMakeupMinDb);

    // Dragging above the bar cannot exceed the top of the range either.
    clickAt(m, -50);
    QCOMPARE(m.makeupDb(), ClientCompMeter::kMakeupMaxDb);
}

void CompMakeupFaderTest::wheelStepsHalfADecibel()
{
    ClientCompMeter m;
    m.setMakeupControlEnabled(true);
    prime(m);
    m.setMakeupDb(0.0f);

    QSignalSpy spy(&m, &ClientCompMeter::makeupChanged);
    QWheelEvent up(QPointF(10, 10), m.mapToGlobal(QPoint(10, 10)), QPoint(), QPoint(0, 120),
                   Qt::NoButton, Qt::NoModifier, Qt::NoScrollPhase, false);
    QApplication::sendEvent(&m, &up);
    QCOMPARE(m.makeupDb(), 0.5f);
    QCOMPARE(spy.count(), 1);
}

void CompMakeupFaderTest::doubleClickReturnsToUnity()
{
    ClientCompMeter m;
    m.setMakeupControlEnabled(true);
    prime(m);
    m.setMakeupDb(18.0f);

    QTest::mouseDClick(&m, Qt::LeftButton, Qt::NoModifier,
                       QPoint(m.width() / 2, m.height() / 2));
    QCOMPARE(m.makeupDb(), 0.0f);
}

void CompMakeupFaderTest::keyboardDrivesTheFader()
{
    // The fader is the only way to reach makeup from this panel, so it has to
    // be operable without a mouse (issue #4896).
    ClientCompMeter m;
    m.setMakeupControlEnabled(true);
    prime(m);
    m.setMakeupDb(0.0f);
    QCOMPARE(m.focusPolicy(), Qt::StrongFocus);
    QVERIFY(!m.accessibleName().isEmpty());

    QTest::keyClick(&m, Qt::Key_Up);
    QCOMPARE(m.makeupDb(), 0.5f);
    QTest::keyClick(&m, Qt::Key_PageUp);
    QCOMPARE(m.makeupDb(), 3.5f);
    QTest::keyClick(&m, Qt::Key_Home);
    QCOMPARE(m.makeupDb(), 0.0f);
    QTest::keyClick(&m, Qt::Key_Down);
    QCOMPARE(m.makeupDb(), -0.5f);
}

void CompMakeupFaderTest::plainMetersIgnoreTheMouse()
{
    // Three other panels use this widget as a meter. A click there must stay
    // a click on a meter.
    ClientCompMeter m;
    prime(m);
    QSignalSpy spy(&m, &ClientCompMeter::makeupChanged);
    clickAt(m, m.height() / 2);
    QCOMPARE(spy.count(), 0);
    QCOMPARE(m.makeupDb(), 0.0f);
}

QTEST_MAIN(CompMakeupFaderTest)
#include "comp_makeup_fader_test.moc"
