#include "TestSettingsProfile.h"
#include "core/AppSettings.h"
#include "core/TciServer.h"
#include "core/backends/IRadioBackend.h"
#include "core/backends/RadioCapabilities.h"
#include "gui/TciApplet.h"
#include "models/RadioModel.h"

#include <QApplication>
#include <QCheckBox>
#include <cstdio>
#include <memory>

using namespace AetherSDR;

namespace {

// A connected radio that declares the `hl2` extension namespace, or not.
class DeclaringBackend final : public IRadioBackend {
public:
    RadioCapabilities caps;
    explicit DeclaringBackend(bool hl2)
    {
        if (hl2) {
            caps.extensionNamespaces << QStringLiteral("hl2");
        }
    }
    RadioCapabilities capabilities() const override { return caps; }
    bool isConnected() const override { return true; }
    void connectRadio(const RadioConnectRequest&) override {}
    void disconnectRadio() override {}
    void invokeExtension(const QString&, const QString&, quint64, const QVariant&) override {}
    void setSliceFrequency(int, double) override {}
    void setSliceMode(int, const QString&) override {}
    void setSliceFilter(int, int, int) override {}
    void setSliceAgc(int, const QString&, int) override {}
    void setPanCenter(const QString&, double, PanCenterIntent) override {}
    void setKeying(bool, const TxCoordinator::Operation&,
                   const TxCoordinator::Completion&) override {}
};

} // namespace

int main(int argc, char** argv)
{
    TestSettingsProfile profile(QStringLiteral("tci-applet"));
    QApplication app(argc, argv);
    auto& settings = AppSettings::instance();
    settings.load();
    const bool hadRecording = settings.contains(QStringLiteral("TciRecording"));
    const QVariant savedRecording = settings.value(QStringLiteral("TciRecording"));
    settings.remove(QStringLiteral("TciRecording"));
    settings.remove(QStringLiteral("TciExtensions"));
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

    // "Allow TCI clients to change HL2 hardware": shown only while the radio
    // offers the HL2 group, off by default, and the server's own setting.
    auto* hl2 = applet.findChild<QCheckBox*>(QStringLiteral("tciAllowHl2HardwareChanges"));
    check(hl2 && hl2->accessibleName() == QStringLiteral("Allow TCI clients to change HL2 hardware")
              && hl2->text().contains(QStringLiteral("Allow TCI clients to change"))
              && hl2->text().contains(QStringLiteral("HL2 hardware"))
              && !hl2->accessibleDescription().isEmpty() && !hl2->toolTip().isEmpty(),
          "the HL2 hardware control is labelled, named and described");
    if (hl2) { hl2->ensurePolished(); }
    check(hl2 && hl2->sizeHint().width() <= 240,
          "the HL2 hardware control fits the default 248-pixel applet without clipping");
    check(hl2 && hl2->isHidden() && !hl2->isChecked(),
          "without a radio that offers the HL2 group it is hidden, and off");
    {
        RadioModel other;
        other.setBackendForTest(std::make_unique<DeclaringBackend>(false), QStringLiteral("test"));
        TciServer otherServer(&other);
        TciApplet otherApplet;
        otherApplet.setRadioModel(&other);
        otherApplet.setTciServer(&otherServer);
        auto* row = otherApplet.findChild<QCheckBox*>(QStringLiteral("tciAllowHl2HardwareChanges"));
        check(other.isConnected() && row && row->isHidden(),
              "a connected radio that does not declare hl2 does not show it");
    }
    {
        RadioModel hl2Radio;
        auto owner = std::make_unique<DeclaringBackend>(true);
        DeclaringBackend* backend = owner.get();
        hl2Radio.setBackendForTest(std::move(owner), QStringLiteral("test"));
        TciServer hl2Server(&hl2Radio);
        TciApplet hl2Applet;
        hl2Applet.setRadioModel(&hl2Radio);
        hl2Applet.setTciServer(&hl2Server);
        auto* row = hl2Applet.findChild<QCheckBox*>(QStringLiteral("tciAllowHl2HardwareChanges"));
        check(row && !row->isHidden() && !row->isChecked() && !hl2Server.hl2HardwareChangesAllowed(),
              "a radio that declares hl2 shows it, off by default");
        if (row) { row->setChecked(true); }
        check(hl2Server.hl2HardwareChangesAllowed(), "checking it allows HL2 hardware changes");
        TciServer restoredHl2(&hl2Radio);
        check(restoredHl2.hl2HardwareChangesAllowed(), "and the setting persists");
        hl2Server.setHl2HardwareChangesAllowed(false);
        check(row && !row->isChecked(), "it mirrors the server setting");
        backend->caps.extensionNamespaces.clear();
        emit hl2Radio.capabilitiesChanged(true, backend->caps);
        check(row && row->isHidden(), "it hides when the radio stops declaring hl2");
    }
    settings.remove(QStringLiteral("TciExtensions"));

    if (hadRecording) {
        settings.setValue(QStringLiteral("TciRecording"), savedRecording);
    } else {
        settings.remove(QStringLiteral("TciRecording"));
    }
    settings.save();
    std::printf("TCI applet: %d failures\n", failures);
    return failures == 0 ? 0 : 1;
}
