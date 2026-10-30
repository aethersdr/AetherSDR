#include "TestSettingsProfile.h"
#include "core/AppSettings.h"
#include "core/TciServer.h"
#include "gui/MeterSlider.h"
#include "gui/TciApplet.h"
#include "models/RadioModel.h"

#include <QApplication>
#include <QCheckBox>
#include <QImage>
#include <QTest>
#include <cmath>
#include <cstdio>
#include <initializer_list>
#include <limits>

using namespace AetherSDR;

int main(int argc, char** argv)
{
    TestSettingsProfile profile("tci-applet");
    QApplication app(argc, argv);
    auto& settings = AppSettings::instance();
    settings.load();
    settings.setValue("TciRxGain1", "0.50");
    settings.save();
    int failures = 0;
    const auto check = [&failures](bool passed, const char* name) {
        if (!passed) {
            ++failures;
            std::fprintf(stderr, "FAIL: %s\n", name);
        }
    };

    TciApplet applet;
    applet.setMaxDaxChannels(TciApplet::kChannels);
    auto* microphone = applet.findChild<MeterSlider*>(QStringLiteral("tciMicrophoneGain"));
    check(microphone != nullptr, "MIC meter exists");
    if (!microphone) {
        return 1;
    }
    microphone->setEnabled(true);
    microphone->setFixedSize(160, 16);
    microphone->setLevel(0.25f);
    int receivers = 0;
    for (auto* meter : applet.findChildren<MeterSlider*>()) {
        if (meter->accessibleName().startsWith(QStringLiteral("TCI RX "))) {
            if (meter->accessibleName() == QStringLiteral("TCI RX 1 gain")) {
                check(meter->gain() == 0.5f, "saved RX attenuation is restored");
                QTest::keyClick(meter, Qt::Key_Right);
                check(std::abs(meter->gain() - 0.55f) < 0.0001f,
                      "RX keyboard steps remain five percent");
                check(settings.value("TciRxGain1").toString().toFloat() == meter->gain(),
                      "RX percentage gain is persisted");
            }
            meter->setFixedSize(160, 16);
            applet.setTciRxLevel(++receivers, 0.25f);
        }
    }
    check(receivers == TciApplet::kChannels, "all RX meters are tested");
    QTest::qWait(1500);

    for (float gain : {0.25f, 0.5f, 1.0f}) {
        microphone->setGain(gain);
        const QImage expected = microphone->grab().toImage();
        for (auto* meter : applet.findChildren<MeterSlider*>()) {
            if (meter->accessibleName().startsWith(QStringLiteral("TCI RX "))) {
                meter->setGain(gain);
                const float expectedLevel = (20.0f * std::log10(0.25f * gain) + 60.0f) / 60.0f;
                check(std::abs(meter->level() - expectedLevel) < 0.001f,
                      "RX applies gain exactly once before logarithmic scaling");
                check(meter->grab().toImage() == expected,
                      "equal source RMS paints identical RX and MIC bars at every gain");
            }
        }
    }

    const float calibrationRms = std::pow(10.0f, -30.0f / 20.0f);
    microphone->setGain(1.0f);
    microphone->setLevel(calibrationRms);
    for (int channel = 1; channel <= TciApplet::kChannels; ++channel) {
        applet.setTciRxLevel(channel, calibrationRms);
    }
    QTest::qWait(1500);
    for (auto* meter : applet.findChildren<MeterSlider*>()) {
        if (meter == microphone || meter->accessibleName().startsWith(QStringLiteral("TCI RX "))) {
            meter->setGain(1.0f);
            check(std::abs(meter->level() - 0.5f) < 0.005f,
                  "-30 dBFS RMS occupies half of every RX and MIC meter");
            meter->setGain(0.5f);
            check(std::abs(meter->level() - (30.0f + 20.0f * std::log10(0.5f)) / 60.0f) < 0.005f,
                  "gain changes immediately apply once before display scaling");
            meter->setGain(0.0f);
            check(meter->level() == 0.0f, "mute immediately empties the meter without another callback");
            meter->setGain(10.0f);
            check(meter->gain() == 1.0f, "meter faders remain attenuation-only");
            meter->setGain(std::numeric_limits<float>::quiet_NaN());
            check(meter->gain() == 1.0f, "non-finite meter gain is rejected");
        }
    }
    for (const QString& saved : {QStringLiteral("nan"), QStringLiteral("inf"), QStringLiteral("-inf")}) {
        settings.setValue("TciRxGain1", saved);
        settings.save();
        TciServer server(nullptr);
        TciApplet invalidSettings;
        for (auto* meter : invalidSettings.findChildren<MeterSlider*>()) {
            if (meter->accessibleName() == QStringLiteral("TCI RX 1 gain")) {
                check(meter->gain() == 0.5f && meter->gain() == server.rxChannelGain(1),
                      "invalid saved RX gains use the same fallback in applet and server");
            }
        }
        server.setRxChannelGain(1, std::numeric_limits<float>::quiet_NaN());
        check(server.rxChannelGain(1) == 0.5f, "server rejects non-finite RX gain");
    }
    {
        RadioModel model;
        TciServer server(&model);
        TciApplet capabilityApplet;
        capabilityApplet.setRadioModel(&model);
        capabilityApplet.setTciServer(&server);
        auto* checkbox = capabilityApplet.findChild<QCheckBox*>(QStringLiteral("tciMicrophoneRecording"));
        auto* lineOut = capabilityApplet.findChild<QCheckBox*>(QStringLiteral("tciLineOutIncludeTx"));
        check(lineOut && !lineOut->isEnabled() && !lineOut->isChecked(), "Line Out TX is opt-in and unavailable without a connected radio");
        check(checkbox && !checkbox->isEnabled(), "mic option is unavailable without a connected radio");
        RadioCapabilities caps;
        emit model.capabilitiesChanged(true, caps);
        check(lineOut && lineOut->isEnabled(), "Line Out does not require the separate microphone capture capability");
        if (lineOut) {
            lineOut->setChecked(true);
            check(server.lineOutIncludeTx() && !server.microphoneRecordingEnabled(), "Line Out checkbox changes only its own TX policy");
            TciServer restored(&model);
            check(restored.lineOutIncludeTx(), "Line Out TX preference persists independently");
            server.setLineOutIncludeTx(false);
            check(!lineOut->isChecked(), "Line Out checkbox mirrors controller policy changes");
        }
        check(checkbox && !checkbox->isEnabled() && !checkbox->accessibleDescription().isEmpty(),
              "unsupported backend dims the mic option with an accessible reason");
        caps.microphoneCapture = RadioCapabilities::MicrophoneCapture{};
        emit model.capabilitiesChanged(true, caps);
        check(checkbox && checkbox->isEnabled(), "mic option is enabled only by an explicit capture capability");
    }
    std::printf("TCI applet: %d failures\n", failures);
    return failures == 0 ? 0 : 1;
}
