// Client DSP knob dispatch (#6724): each applet's knob reaches its engine
// through a captured member-function pointer called on a local object
// operand. Turning the first knob must land on the matching engine setter,
// and turning it with no engine attached must be a no-op.

#include "TestSettingsProfile.h"
#include "core/AppSettings.h"
#include "core/AudioEngine.h"
#include "core/ClientComp.h"
#include "core/ClientDeEss.h"
#include "core/ClientGate.h"
#include "core/ClientReverb.h"
#include "core/ClientTube.h"
#include "gui/ClientCompApplet.h"
#include "gui/ClientCompKnob.h"
#include "gui/ClientDeEssApplet.h"
#include "gui/ClientGateApplet.h"
#include "gui/ClientReverbApplet.h"
#include "gui/ClientTubeApplet.h"
#include "gui/StripReverbPanel.h"

#include <QApplication>
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

    void gateThreshold()
    {
        AudioEngine engine;
        ClientGateApplet applet;
        applet.setAudioEngine(&engine);
        ClientCompKnob* knob = firstKnob(applet);
        QVERIFY(knob);
        emit knob->valueChanged(-47.0f);
        QCOMPARE(engine.clientGateTx()->thresholdDb(), -47.0f);
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

    void deEssFrequency()
    {
        AudioEngine engine;
        ClientDeEssApplet applet;
        applet.setAudioEngine(&engine);
        ClientCompKnob* knob = firstKnob(applet);
        QVERIFY(knob);
        emit knob->valueChanged(5500.0f);
        QCOMPARE(engine.clientDeEssTx()->frequencyHz(), 5500.0f);
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
        ClientDeEssApplet deEss;
        ClientReverbApplet reverb;
        StripReverbPanel strip(nullptr);
        for (QWidget* w : {static_cast<QWidget*>(&comp), static_cast<QWidget*>(&gate),
                           static_cast<QWidget*>(&tube), static_cast<QWidget*>(&deEss),
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
