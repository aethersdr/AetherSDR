#include "TestSettingsProfile.h"
#include "core/AppSettings.h"
#include "core/TciServer.h"
#include "gui/MeterSlider.h"
#include "gui/TciApplet.h"
#include <QApplication>
#include <QTest>
#include <cmath>
#include <cstdio>

using namespace AetherSDR;

int main(int argc, char** argv)
{
    TestSettingsProfile profile("tci-rx-applet");
    QApplication app(argc, argv);
    auto& settings = AppSettings::instance();
    settings.load();
    settings.setValue("TciRxGain1", "0.50");
    settings.setValue("TciRxGain2", "0.50");
    settings.save();
    int failures = 0;
    const auto check = [&](bool ok, const char* name) {
        if (!ok) { ++failures; std::fprintf(stderr, "FAIL: %s\n", name); }
    };
    TciApplet applet;
    MeterSlider* receive = nullptr;
    MeterSlider* transmit = nullptr;
    for (MeterSlider* meter : applet.findChildren<MeterSlider*>()) {
        if (meter->accessibleName() == "TCI RX 1 gain") { receive = meter; }
        if (meter->accessibleName() == "TCI TX gain") { transmit = meter; }
        if (meter->accessibleName() == "TCI RX 2 gain") {
            check(meter->gain() == 0.5f, "existing attenuation is unchanged");
        }
    }
    check(receive && transmit, "TCI receive and transmit meters exist");
    if (!receive || !transmit) { return 1; }
    check(receive->gain() == 0.5f, "saved receive attenuation is restored");
    check(receive->toolTip().contains("0-100%"), "receive gain remains a percentage attenuator");
    float emitted = 0.0f;
    QObject::connect(&applet, &TciApplet::tciRxGainChanged, &applet,
                     [&](int channel, float gain) { if (channel == 1) { emitted = gain; } });
    QTest::keyClick(receive, Qt::Key_Right);
    check(std::fabs(receive->gain() - 0.55f) < 0.0001f, "receive keyboard steps remain five percent");
    check(emitted == receive->gain(), "receive attenuation reaches the server-facing signal");
    check(settings.value("TciRxGain1").toString().toFloat() == receive->gain(), "applet saves percentage gain");
    receive->setGain(1.0f);
    QTest::keyClick(receive, Qt::Key_Right);
    check(receive->gain() == 1.0f, "receive gain stops at unity");
    receive->setGain(10.0f);
    check(receive->gain() == 1.0f, "receive control cannot boost audio");
    applet.setTciRxLevel(1, std::pow(10.0f, -30.0f / 20.0f));
    QTest::qWait(250);
    check(std::fabs(receive->level() - 0.5f) < 0.005f, "-30 dBFS RMS occupies half the meter at unity");
    receive->setGain(0.5f);
    check(std::fabs(receive->level() - (30.0f + 20.0f * std::log10(0.5f)) / 60.0f) < 0.005f,
          "fader changes immediately apply gain once before logarithmic scaling");
    receive->setGain(0.0f);
    check(receive->level() == 0.0f, "zero gain immediately clears the meter without another callback");
    receive->setGain(1.0f);
    applet.setTciRxLevel(1, 0.0f);
    QTest::qWait(1500);
    check(receive->level() == 0.0f, "silence settles to an empty logarithmic meter");
    transmit->setGain(10.0f);
    check(transmit->gain() == 1.0f, "TCI transmit still cannot be boosted");
    MeterSlider legacy;
    legacy.setGain(0.5f);
    QTest::keyClick(&legacy, Qt::Key_Right);
    check(std::fabs(legacy.gain() - 0.55f) < 0.0001f, "legacy percentage faders keep their keyboard behavior");
    for (const QString& saved : {QStringLiteral("nan"), QStringLiteral("inf"), QStringLiteral("-inf")}) {
        settings.setValue("TciRxGain1", saved);
        settings.save();
        TciServer server(nullptr);
        TciApplet invalidSettings;
        for (MeterSlider* meter : invalidSettings.findChildren<MeterSlider*>()) {
            if (meter->accessibleName() == "TCI RX 1 gain") {
                check(meter->gain() == 0.5f && meter->gain() == server.rxChannelGain(1),
                      "non-finite saved RX gains use the same fallback in applet and server");
            }
        }
    }
    std::printf("TCI RX applet: %d failures\n", failures);
    return failures ? 1 : 0;
}
