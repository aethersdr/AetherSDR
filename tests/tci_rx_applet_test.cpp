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
    settings.setValue("TciRxGain1", "10.00");
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
    check(receive->gain() == 10.0f, "saved +20 dB receive boost is restored");
    check(receive->toolTip().contains("+20.0 dB"), "receive gain is displayed in dB");
    float emitted = 0.0f;
    QObject::connect(&applet, &TciApplet::tciRxGainChanged, &applet,
                     [&](int channel, float gain) { if (channel == 1) { emitted = gain; } });
    QTest::keyClick(receive, Qt::Key_Right);
    check(std::fabs(receive->gain() - std::pow(10.0f, 21.0f / 20.0f)) < 0.0001f,
          "receive keyboard steps use one dB");
    check(emitted == receive->gain(), "receive boost reaches the server-facing signal");
    check(settings.value("TciRxGain1").toString().toFloat() == receive->gain(), "applet saves dB gain without precision loss");
    receive->setGain(TciServer::kMaxRxGain);
    QTest::keyClick(receive, Qt::Key_Right);
    check(std::fabs(receive->gain() - TciServer::kMaxRxGain) < 0.0001f, "receive boost stops at +30 dB");
    receive->setGain(0.0f);
    check(receive->toolTip().contains("Muted"), "zero gain is explicitly muted");
    receive->setGain(10.0f);
    applet.setTciRxLevel(1, std::pow(10.0f, -30.0f / 20.0f));
    QTest::qWait(250);
    check(std::fabs(receive->level() - 0.5f) < 0.005f, "-30 dBFS RMS occupies half the meter regardless of gain");
    transmit->setGain(10.0f);
    check(transmit->gain() == 1.0f, "TCI transmit still cannot be boosted");
    MeterSlider legacy;
    legacy.setGain(0.5f);
    QTest::keyClick(&legacy, Qt::Key_Right);
    check(std::fabs(legacy.gain() - 0.55f) < 0.0001f, "legacy percentage faders keep their keyboard behavior");
    std::printf("TCI RX applet: %d failures\n", failures);
    return failures ? 1 : 0;
}