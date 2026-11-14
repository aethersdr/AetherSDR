#include "TestSettingsProfile.h"
#include "core/AppSettings.h"
#include "core/TciServer.h"
#include "gui/TciApplet.h"
#include "models/RadioModel.h"

#include <QApplication>
#include <QCheckBox>
#include <cstdio>

using namespace AetherSDR;

int main(int argc, char** argv)
{
    TestSettingsProfile profile(QStringLiteral("tci-applet"));
    QApplication app(argc, argv);
    auto& settings = AppSettings::instance();
    settings.load();
    const bool hadRecording = settings.contains(QStringLiteral("TciRecording"));
    const QVariant savedRecording = settings.value(QStringLiteral("TciRecording"));
    settings.remove(QStringLiteral("TciRecording"));
    settings.save();

    int failures = 0;
    const auto check = [&failures](bool passed, const char* name) {
        if (!passed) {
            ++failures;
            std::fprintf(stderr, "FAIL: %s\n", name);
        }
    };

    RadioModel model;
    TciServer server(&model);
    TciApplet applet;
    auto* lineOut = applet.findChild<QCheckBox*>(QStringLiteral("tciLineOutIncludeTx"));
    auto* lineOutConsent = applet.findChild<QCheckBox*>(QStringLiteral("tciLineOutPcAudioOffConsent"));
    const QString consentWarning = lineOutConsent
        ? lineOutConsent->accessibleDescription() : QString();
    applet.setRadioModel(&model);
    applet.setTciServer(&server);
    check(lineOut && lineOut->text() == QStringLiteral("Include TX (Line Out)"),
          "Line Out TX control names its destination");
    check(lineOut && !lineOut->isChecked(), "Line Out TX inclusion defaults off");
    check(lineOut && !lineOut->isEnabled() && !lineOut->accessibleDescription().isEmpty(),
          "Line Out TX control is unavailable with an accessible reason when disconnected");
    check(lineOutConsent
          && lineOutConsent->text().contains(QStringLiteral("Line Out with PC Audio off"))
          && lineOutConsent->text().contains(QStringLiteral("mutes radio outputs")),
          "PC Audio-off Line Out consent is distinct from TX inclusion and names its radio-output effect");
    if (lineOutConsent) { lineOutConsent->ensurePolished(); }
    check(lineOutConsent && lineOutConsent->sizeHint().width() <= 240,
          "the consent warning fits the default 248-pixel applet without clipping");
    check(lineOutConsent && !lineOutConsent->isChecked()
          && consentWarning.contains(QStringLiteral("mutes"))
          && consentWarning.contains(QStringLiteral("speaker"))
          && consentWarning.contains(QStringLiteral("headphone"))
          && consentWarning.contains(QStringLiteral("line-out")),
          "PC Audio-off Line Out consent defaults off and names every muted radio output");

    server.setLineOutIncludeTx(true);
    check(server.lineOutIncludeTx() && lineOut && lineOut->isChecked(),
          "Line Out TX control mirrors the server setting");
    TciServer restored(&model);
    check(restored.lineOutIncludeTx(), "Line Out TX preference persists independently");
    server.setLineOutPcAudioOffConsent(true);
    check(server.lineOutPcAudioOffConsent() && lineOutConsent && lineOutConsent->isChecked(),
          "PC Audio-off Line Out consent mirrors the server setting");
    TciServer restoredConsent(&model);
    check(restoredConsent.lineOutPcAudioOffConsent(),
          "PC Audio-off Line Out consent persists independently");
    server.setLineOutPcAudioOffConsent(false);
    check(lineOutConsent && !lineOutConsent->isChecked(),
          "PC Audio-off Line Out consent mirrors controller updates");
    server.setLineOutIncludeTx(false);
    check(lineOut && !lineOut->isChecked(), "Line Out TX control mirrors controller updates");

    if (hadRecording) {
        settings.setValue(QStringLiteral("TciRecording"), savedRecording);
    } else {
        settings.remove(QStringLiteral("TciRecording"));
    }
    settings.save();
    std::printf("TCI applet: %d failures\n", failures);
    return failures == 0 ? 0 : 1;
}
