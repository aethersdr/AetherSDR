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
    applet.setRadioModel(&model);
    applet.setTciServer(&server);
    auto* lineOut = applet.findChild<QCheckBox*>(QStringLiteral("tciLineOutIncludeTx"));
    check(lineOut && lineOut->text() == QStringLiteral("Line Out TX"),
          "Line Out TX control uses its plain feature name");
    check(lineOut && !lineOut->isChecked(), "Line Out TX inclusion defaults off");
    check(lineOut && !lineOut->isEnabled() && !lineOut->accessibleDescription().isEmpty(),
          "Line Out TX control is unavailable with an accessible reason when disconnected");

    server.setLineOutIncludeTx(true);
    check(server.lineOutIncludeTx() && lineOut && lineOut->isChecked(),
          "Line Out TX control mirrors the server setting");
    TciServer restored(&model);
    check(restored.lineOutIncludeTx(), "Line Out TX preference persists independently");
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
