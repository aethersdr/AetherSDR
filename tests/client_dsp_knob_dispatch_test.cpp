// Client DSP knob dispatch (#6724): each applet's knob reaches its engine
// through a captured member-function pointer called on a local object
// operand. Turning the first knob must land on the matching engine setter,
// and turning it with no engine attached must be a no-op.

#include "TestSettingsProfile.h"
#include "core/AppSettings.h"
#include "core/AudioEngine.h"
#include "core/ClientComp.h"
#include "core/ClientGate.h"
#include "core/ClientReverb.h"
#include "core/ClientTube.h"
#include "gui/ClientCompApplet.h"
#include "gui/ClientCompKnob.h"
#include "gui/ClientGateApplet.h"
#include "gui/StripTubePanel.h"
#include "gui/ClientReverbApplet.h"
#include "gui/ClientTubeApplet.h"
#include "gui/StripReverbPanel.h"

#include <QApplication>
#include <QAccessible>
#include <QLineEdit>
#include <QPushButton>
#include <QSignalSpy>
#include <QTimer>
#include <QtTest>
#include <limits>

using namespace AetherSDR;

namespace {

// The first knob each applet builds is the one its first wire() call binds.
ClientCompKnob* firstKnob(QWidget& w)
{
    const auto knobs = w.findChildren<ClientCompKnob*>();
    return knobs.isEmpty() ? nullptr : knobs.first();
}

struct AccessibilityCapture {
    static inline QObject* target = nullptr;
    static inline int values = 0;
    static inline int textUpdates = 0;
    QAccessible::UpdateHandler previous = QAccessible::installUpdateHandler(capture);
    bool wasActive = QAccessible::isActive();

    explicit AccessibilityCapture(QObject* object)
    {
        target = object;
        values = 0;
        textUpdates = 0;
        QAccessible::setActive(true);
    }
    ~AccessibilityCapture()
    {
        QAccessible::installUpdateHandler(previous);
        QAccessible::setActive(wasActive);
        target = nullptr;
    }
    static void capture(QAccessibleEvent* event)
    {
        if (event->object() == target && event->type() == QAccessible::ValueChanged) {
            ++values;
        }
        if (event->type() == QAccessible::TextUpdated
            || event->type() == QAccessible::TextInserted
            || event->type() == QAccessible::TextRemoved) {
            ++textUpdates;
        }
    }
};

class CountingCompKnob : public ClientCompKnob {
public:
    using ClientCompKnob::ClientCompKnob;
    int paints = 0;

protected:
    void paintEvent(QPaintEvent* event) override
    {
        ++paints;
        ClientCompKnob::paintEvent(event);
    }
};

}  // namespace

class ClientDspKnobDispatchTest : public QObject {
    Q_OBJECT

private slots:
    void unchangedKnobValuesDoNotRepaint()
    {
        CountingCompKnob knob;
        knob.setValue(0.5f);
        knob.show();
        QTRY_VERIFY(knob.paints > 0);
        QTest::qWait(100); // let native window exposure finish
        knob.paints = 0;
        for (int i = 0; i < 100; ++i) {
            knob.setValue(0.5f);
        }
        QTest::qWait(40);
        QCOMPARE(knob.paints, 0);
        knob.setValue(0.7f);
        QTRY_COMPARE(knob.paints, 1);
    }

    void knobAccessibilityAndKeyboard()
    {
        ClientCompKnob knob;
        knob.setInlineEditEnabled(false);
        knob.setLabel(QStringLiteral("Drive"));
        knob.setRange(0.0f, 24.0f);
        knob.setLabelFormat([](float value) {
            return QString::number(value, 'f', 1) + QStringLiteral(" dB");
        });
        knob.setValue(6.0f);
        QAccessibleInterface* accessible = QAccessible::queryAccessibleInterface(&knob);
        QVERIFY(accessible);
        QCOMPARE(accessible->role(), QAccessible::Dial);
        QCOMPARE(accessible->text(QAccessible::Name), QStringLiteral("Drive"));
        QCOMPARE(accessible->text(QAccessible::Value), QStringLiteral("6.0 dB"));
        QAccessibleValueInterface* value = accessible->valueInterface();
        QVERIFY(value);
        QCOMPARE(value->currentValue().toFloat(), 6.0f);
        QCOMPARE(value->minimumValue().toFloat(), 0.0f);
        QCOMPARE(value->maximumValue().toFloat(), 24.0f);
        QVERIFY(qAbs(value->minimumStepSize().toFloat() - 0.24f) < 0.0001f);
        QSignalSpy changes(&knob, &ClientCompKnob::valueChanged);
        value->setCurrentValue(9.0f);
        QCOMPARE(knob.value(), 9.0f);
        QCOMPARE(changes.count(), 1);
        value->setCurrentValue(9.0f);
        value->setCurrentValue(QStringLiteral("invalid"));
        value->setCurrentValue(std::numeric_limits<float>::quiet_NaN());
        QCOMPARE(changes.count(), 1);
        QAccessibleActionInterface* action = accessible->actionInterface();
        QVERIFY(action);
        QVERIFY(action->actionNames().contains(QAccessibleActionInterface::increaseAction()));
        action->doAction(QAccessibleActionInterface::increaseAction());
        QVERIFY(qAbs(knob.value() - 9.24f) < 0.0001f);
        action->doAction(QAccessibleActionInterface::decreaseAction());
        QVERIFY(qAbs(knob.value() - 9.0f) < 0.0001f);
        QTest::keyClick(&knob, Qt::Key_Right);
        QVERIFY(qAbs(knob.value() - 9.24f) < 0.0001f);
        QTest::keyClick(&knob, Qt::Key_Down, Qt::ShiftModifier);
        QVERIFY(qAbs(knob.value() - 9.18f) < 0.0001f);
        QTest::keyClick(&knob, Qt::Key_Home);
        QCOMPARE(knob.value(), 0.0f);
        QTest::keyClick(&knob, Qt::Key_End);
        QCOMPARE(knob.value(), 24.0f);
        knob.setEnabled(false);
        value->setCurrentValue(3.0f);
        action->doAction(QAccessibleActionInterface::decreaseAction());
        QCOMPARE(knob.value(), 24.0f);
    }

    void editorRefreshIsIdempotent()
    {
        ClientCompKnob knob;
        knob.setValue(0.5f);
        QLineEdit* editor = knob.findChild<QLineEdit*>();
        QVERIFY(editor);
        editor->selectAll();
        const QString selected = editor->selectedText();
        QSignalSpy changes(&knob, &ClientCompKnob::valueChanged);
        for (int i = 0; i < 100; ++i) {
            knob.setValue(0.5f);
        }
        QCOMPARE(editor->selectedText(), selected);
        QCOMPARE(changes.count(), 0);
        knob.setInlineEditEnabled(false);
        const QString before = editor->text();
        knob.setValue(0.7f);
        QCOMPARE(knob.value(), 0.7f);
        QCOMPARE(editor->text(), before);
        knob.setInlineEditEnabled(true);
        QCOMPARE(editor->text(), QStringLiteral("0.70"));
        knob.setLabelFormat([](float value) { return QString::number(value * 100) + "%"; });
        QCOMPARE(editor->text(), QStringLiteral("70%"));
    }

    void tubePollingLeavesHiddenEditorsAlone_data()
    {
        QTest::addColumn<bool>("receive");
        QTest::newRow("transmit") << false;
        QTest::newRow("receive") << true;
    }

    void tubePollingLeavesHiddenEditorsAlone()
    {
        QFETCH(bool, receive);
        AudioEngine engine;
        ClientTubeApplet applet(receive ? ClientTubeApplet::Side::Rx
                                       : ClientTubeApplet::Side::Tx);
        applet.setAudioEngine(&engine);
        const auto knobs = applet.findChildren<ClientCompKnob*>();
        QCOMPARE(knobs.size(), 5);
        for (ClientCompKnob* knob : knobs) {
            QLineEdit* editor = knob->findChild<QLineEdit*>();
            QVERIFY(editor);
            QVERIFY(editor->isHidden());
            editor->setText(QStringLiteral("untouched"));
        }
        ClientTube* tube = receive ? engine.clientTubeRx() : engine.clientTubeTx();
        AccessibilityCapture capture(&applet);
        tube->setDriveDb(12.0f);
        QTest::qWait(150); // exercise the production 33 ms timer
        qInfo() << "Tube poll text events:" << AccessibilityCapture::textUpdates;
        QCOMPARE(AccessibilityCapture::textUpdates, 0);
        QCOMPARE(knobs.first()->value(), 12.0f);
        for (ClientCompKnob* knob : knobs) {
            QCOMPARE(knob->findChild<QLineEdit*>()->text(), QStringLiteral("untouched"));
        }
    }

    void settledAccessibilityAnnouncements()
    {
        ClientCompKnob knob;
        knob.setInlineEditEnabled(false);
        knob.setLabel(QStringLiteral("Drive"));
        knob.setRange(0.0f, 24.0f);
        knob.show();
        knob.activateWindow();
        knob.setFocus();
        QTRY_VERIFY(knob.hasFocus());
        AccessibilityCapture capture(&knob);
        if (!QAccessible::isActive()) {
            QSKIP("Platform plugin has no accessibility backend; run with cocoa/windows for announcement checks");
        }
        for (int i = 1; i <= 10; ++i) {
            knob.setValue(static_cast<float>(i));
            QTest::qWait(33);
        }
        QCOMPARE(AccessibilityCapture::values, 0);
        // Unchanged model polling must neither announce nor starve delivery.
        QTimer poll;
        connect(&poll, &QTimer::timeout, &knob, [&] { knob.setValue(knob.value()); });
        poll.start(33);
        QTRY_COMPARE(AccessibilityCapture::values, 1);
        QTest::qWait(350);
        QCOMPARE(AccessibilityCapture::values, 1);
        QCOMPARE(AccessibilityCapture::textUpdates, 0);
        knob.setValue(11.0f);
        knob.clearFocus();
        QTest::qWait(350);
        QCOMPARE(AccessibilityCapture::values, 1);
    }

    void compThreshold()
    {
        AudioEngine engine;
        ClientCompApplet applet;
        applet.setAudioEngine(&engine);
        ClientCompKnob* knob = firstKnob(applet);
        QVERIFY(knob);
        emit knob->valueChanged(-23.0f);
        QCOMPARE(engine.clientCompTx()->thresholdDb(), -23.0f);
    }

    // The gate applet is AetherRX's only: AetherTX has no gate.
    void gateThreshold()
    {
        AudioEngine engine;
        ClientGateApplet applet;
        applet.setAudioEngine(&engine);
        ClientCompKnob* knob = firstKnob(applet);
        QVERIFY(knob);
        emit knob->valueChanged(-47.0f);
        QCOMPARE(engine.clientGateRx()->thresholdDb(), -47.0f);
    }

    // RN2, which replaced the TX gate, is on the Tube page, TX only.
    void tubePageRn2()
    {
        AudioEngine engine;
        StripTubePanel panel(&engine);
        auto* rn2 = panel.findChild<QPushButton*>(QStringLiteral("aetherTxRn2"));
        QVERIFY(rn2);
        panel.showForTx();
        QVERIFY(rn2->isVisibleTo(&panel));
        rn2->click();
        QCOMPARE(engine.rn2TxEnabled(), rn2->isChecked());
        QVERIFY(engine.rn2TxEnabled());
        rn2->click();
        QVERIFY(!engine.rn2TxEnabled());
        // A change made elsewhere (a profile, the gate migration) shows at
        // once, without waiting for the page's poll.
        engine.setRn2TxEnabled(true);
        QVERIFY(rn2->isChecked());
        engine.setRn2TxEnabled(false);
        QVERIFY(!rn2->isChecked());
        panel.showForRx();
        QVERIFY(!rn2->isVisibleTo(&panel));
    }

    void tubeDrive()
    {
        AudioEngine engine;
        ClientTubeApplet applet;
        applet.setAudioEngine(&engine);
        ClientCompKnob* knob = firstKnob(applet);
        QVERIFY(knob);
        QAccessibleInterface* accessible = QAccessible::queryAccessibleInterface(knob);
        QVERIFY(accessible && accessible->valueInterface());
        accessible->valueInterface()->setCurrentValue(7.0f);
        QCOMPARE(engine.clientTubeTx()->driveDb(), 7.0f);
        QTest::keyClick(knob, Qt::Key_Home);
        QCOMPARE(engine.clientTubeTx()->driveDb(), 0.0f);
    }

    void reverbAppletSize()
    {
        AudioEngine engine;
        ClientReverbApplet applet;
        applet.setAudioEngine(&engine);
        ClientCompKnob* knob = firstKnob(applet);
        QVERIFY(knob);
        emit knob->valueChanged(0.7f);
        QCOMPARE(engine.clientReverbTx()->size(), 0.7f);
    }

    void stripReverbSize()
    {
        AudioEngine engine;
        StripReverbPanel panel(&engine);
        ClientCompKnob* knob = firstKnob(panel);
        QVERIFY(knob);
        emit knob->valueChanged(0.3f);
        QCOMPARE(engine.clientReverbTx()->size(), 0.3f);
    }

    void knobsWithoutEngineAreNoOps()
    {
        ClientCompApplet comp;
        ClientGateApplet gate;
        ClientTubeApplet tube;
        ClientReverbApplet reverb;
        StripReverbPanel strip(nullptr);
        for (QWidget* w : {static_cast<QWidget*>(&comp), static_cast<QWidget*>(&gate),
                           static_cast<QWidget*>(&tube),
                           static_cast<QWidget*>(&reverb), static_cast<QWidget*>(&strip)}) {
            ClientCompKnob* knob = firstKnob(*w);
            QVERIFY(knob);
            emit knob->valueChanged(knob->value());
        }
    }
};

int main(int argc, char** argv)
{
    TestSettingsProfile profile(QStringLiteral("client-dsp-knob-dispatch"));
    if (!profile.isValid()) {
        return 1;
    }
    QApplication app(argc, argv);
    AppSettings::instance().load();
    ClientDspKnobDispatchTest test;
    return QTest::qExec(&test, argc, argv);
}

#include "client_dsp_knob_dispatch_test.moc"
