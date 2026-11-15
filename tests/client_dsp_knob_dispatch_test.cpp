// Client DSP knob dispatch (#6724): each applet's knob reaches its engine
// through a captured member-function pointer called on a local object
// operand. Turning the first knob must land on the matching engine setter,
// and turning it with no engine attached must be a no-op.

#include "TestSettingsProfile.h"
#include "core/AppSettings.h"
#include "core/AudioEngine.h"
#include "core/ClientCfc.h"
#include "core/ClientLeveler.h"
#include "core/ClientComp.h"
#include "core/ClientGate.h"
#include "core/ClientReverb.h"
#include "core/ClientTube.h"
#include "gui/ClientCfcApplet.h"
#include "gui/ClientCfcCurveWidget.h"
#include "gui/ClientLevelerApplet.h"
#include "gui/ClientCompApplet.h"
#include "gui/ClientCompKnob.h"
#include "gui/ClientGateApplet.h"
#include "gui/StripTubePanel.h"
#include "gui/ClientReverbApplet.h"
#include "gui/ClientTubeApplet.h"
#include "gui/StripCfcPanel.h"
#include "gui/StripLevelerPanel.h"
#include "gui/StripReverbPanel.h"

#include <QApplication>
#include <QPushButton>
#include <QtTest>

using namespace AetherSDR;

namespace {

// The first knob each applet builds is the one its first wire() call binds.
ClientCompKnob* firstKnob(QWidget& w)
{
    const auto knobs = w.findChildren<ClientCompKnob*>();
    return knobs.isEmpty() ? nullptr : knobs.first();
}

}  // namespace

class ClientDspKnobDispatchTest : public QObject {
    Q_OBJECT

private slots:
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
        emit knob->valueChanged(7.0f);
        QCOMPARE(engine.clientTubeTx()->driveDb(), 7.0f);
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

    void cfcAppletPrecomp()
    {
        AudioEngine engine;
        ClientCfcApplet applet;
        applet.setAudioEngine(&engine);
        ClientCompKnob* knob = firstKnob(applet);
        QVERIFY(knob);
        emit knob->valueChanged(6.0f);
        QCOMPARE(engine.clientCfcTx()->precompDb(), 6.0f);
    }

    void stripCfcPrecomp()
    {
        AudioEngine engine;
        StripCfcPanel panel(&engine);
        ClientCompKnob* knob = firstKnob(panel);
        QVERIFY(knob);
        emit knob->valueChanged(4.5f);
        QCOMPARE(engine.clientCfcTx()->precompDb(), 4.5f);
    }

    // Double-clicking empty curve space adds a band to the engine's profile.
    void stripCfcCurveAddsABand()
    {
        AudioEngine engine;
        StripCfcPanel panel(&engine);
        auto* curve = panel.findChild<ClientCfcCurveWidget*>();
        QVERIFY(curve);
        curve->resize(560, 220);
        const auto before = engine.clientCfcTx()->bands().size();
        QTest::mouseDClick(curve, Qt::LeftButton, {}, QPoint(curve->width() / 2, 40));
        QCOMPARE(engine.clientCfcTx()->bands().size(), before + 1);
    }

    void levelerAppletMaxGain()
    {
        AudioEngine engine;
        ClientLevelerApplet applet;
        applet.setAudioEngine(&engine);
        ClientCompKnob* knob = firstKnob(applet);
        QVERIFY(knob);
        emit knob->valueChanged(13.0f);
        QCOMPARE(engine.clientLevelerTx()->maxGainDb(), 13.0f);
    }

    void stripLevelerMaxGain()
    {
        AudioEngine engine;
        StripLevelerPanel panel(&engine);
        ClientCompKnob* knob = firstKnob(panel);
        QVERIFY(knob);
        emit knob->valueChanged(9.0f);
        QCOMPARE(engine.clientLevelerTx()->maxGainDb(), 9.0f);
    }

    void knobsWithoutEngineAreNoOps()
    {
        ClientCompApplet comp;
        ClientGateApplet gate;
        ClientTubeApplet tube;
        ClientReverbApplet reverb;
        StripReverbPanel strip(nullptr);
        ClientCfcApplet cfc;
        StripCfcPanel cfcStrip(nullptr);
        ClientLevelerApplet lvlr;
        StripLevelerPanel lvlrStrip(nullptr);
        for (QWidget* w : {static_cast<QWidget*>(&lvlr), static_cast<QWidget*>(&lvlrStrip),
                           static_cast<QWidget*>(&comp), static_cast<QWidget*>(&gate),
                           static_cast<QWidget*>(&tube),
                           static_cast<QWidget*>(&reverb), static_cast<QWidget*>(&strip),
                           static_cast<QWidget*>(&cfc), static_cast<QWidget*>(&cfcStrip)}) {
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
