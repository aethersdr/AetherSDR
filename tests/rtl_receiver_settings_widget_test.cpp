#include "TestSettingsProfile.h"
#include "core/AppSettings.h"
#include "gui/RtlReceiverSettingsWidget.h"
#include "models/RadioModel.h"

#include <QApplication>
#include <QCheckBox>
#include <QLabel>
#include <QPushButton>
#include <QSpinBox>
#include <cstdio>

using namespace AetherSDR;
namespace {
int failures = 0;
void check(bool value, const char* message)
{
    if (!value) { ++failures; std::fprintf(stderr, "FAIL: %s\n", message); }
}
// Injected extension seam, no firmware simulator, socket or device access.
class ExtensionBackend final : public IRadioBackend {
public:
    RadioCapabilities caps;
    bool connectedState = true;
    quint64 pendingId = 0;
    QVariant requested;
    QString verb;
    QVariantMap accepted{{"serial", "widget-device"}, {"applied", true},
        {"ppm", 7}, {"dcSuppression", false}, {"pending", false}, {"saved", true}};
    ExtensionBackend()
    {
        caps.extensionNamespaces = {QStringLiteral("rtl")};
        caps.extensions["rtl"] = QVariantMap{{"settingsVersion", 1}};
    }
    RadioCapabilities capabilities() const override { return caps; }
    bool isConnected() const override { return connectedState; }
    void connectRadio(const RadioConnectRequest&) override {}
    void disconnectRadio() override { connectedState = false; }
    void setSliceFrequency(int, double) override {}
    void setSliceMode(int, const QString&) override {}
    void setSliceFilter(int, int, int) override {}
    void setSliceAgc(int, const QString&, int) override {}
    void setPanCenter(const QString&, double, PanCenterIntent) override {}
    void setKeying(bool, const TxCoordinator::Operation&, const TxCoordinator::Completion&) override {}
    void invokeExtension(const QString& ns, const QString& name, quint64 id, const QVariant& arg) override
    {
        check(ns == QLatin1String("rtl"), "widget uses the declared extension namespace");
        if (name == QLatin1String("settings.get")) { emit extensionResult(id, accepted); return; }
        pendingId = id; verb = name; requested = arg;
    }
    void confirm()
    {
        accepted[verb == QLatin1String("ppm.set") ? "ppm" : "dcSuppression"] = requested;
        emit extensionStatus("rtl", "settings", accepted);
        emit extensionResult(pendingId, requested);
    }
};
}
int main(int argc, char** argv)
{
    TestSettingsProfile profile(QStringLiteral("rtl-receiver-settings-widget"));
    if (!profile.isValid()) { return 1; }
    QApplication app(argc, argv); AppSettings::instance().load();
    RadioModel model;
    auto owned = std::make_unique<ExtensionBackend>();
    ExtensionBackend* source = owned.get();
    model.setBackendForTest(std::move(owned), QStringLiteral("test"));
    RtlReceiverSettingsWidget widget(model);
    auto* ppm = widget.findChild<QSpinBox*>(QStringLiteral("rtlPpmRequest"));
    auto* apply = widget.findChild<QPushButton*>(QStringLiteral("rtlPpmApply"));
    auto* dc = widget.findChild<QCheckBox*>(QStringLiteral("rtlDcSuppression"));
    auto* applied = widget.findChild<QLabel*>(QStringLiteral("rtlCorrectionsApplied"));
    auto* status = widget.findChild<QLabel*>(QStringLiteral("rtlCorrectionsStatus"));
    if (!ppm || !apply || !dc || !applied || !status) { return 1; }
    check(ppm->value() == 7 && !dc->isChecked() && apply->isEnabled(), "lazy widget immediately loads accepted settings");
    check(ppm->minimum() == -1000 && ppm->maximum() == 1000 && !ppm->keyboardTracking(), "integer PPM entry exposes hardware bounds and waits for complete entry");
    ppm->setValue(18); apply->click();
    const quint64 prior = source->pendingId;
    check(source->verb == QLatin1String("ppm.set") && source->requested.toInt() == 18
        && applied->text().contains("7 ppm") && status->text().contains("Applying"), "requested correction cannot masquerade as applied");
    ppm->setValue(19); apply->click();
    check(source->pendingId != prior, "replacement request has a fresh correlation id");
    emit source->extensionError(prior, "superseded");
    check(!status->text().contains("refused"), "stale request error cannot replace the latest request");
    source->confirm();
    check(applied->text().contains("19 ppm") && !status->text().contains("Applying"), "confirmed reply settles the applied value");
    ppm->setValue(20); apply->click();
    ppm->setValue(21); source->confirm();
    check(ppm->value() == 21 && applied->text().contains("20 ppm"), "completion cannot erase a newer unsent edit");
    ppm->setValue(19); apply->click(); source->confirm();
    dc->click();
    check(source->verb == QLatin1String("dc_suppression.set") && source->requested.toBool()
        && !dc->isChecked() && applied->text().endsWith("off"), "DC checkbox remains confirmed while DSP change is pending");
    source->confirm();
    check(dc->isChecked() && applied->text().endsWith("on"), "DC checkbox follows DSP adoption");
    ppm->setValue(-12); apply->click();
    emit source->extensionError(source->pendingId, "readback failed");
    check(applied->text().contains("19 ppm") && status->text().contains("readback failed"), "refusal preserves applied value and exposes reason");
    source->accepted["saved"] = false;
    source->accepted["saveReason"] = "Applied for this session; saving failed.";
    emit source->extensionStatus("rtl", "settings", source->accepted);
    check(status->text().contains("saving failed") && !status->text().contains("corrections saved"), "persistence failure does not claim durable settings");

    source->connectedState = false;
    emit model.connectionStateChanged(false);
    check(!apply->isEnabled() && !dc->isChecked() && applied->text().contains("unavailable"), "disconnect retires old values and pending controls");
    source->accepted["ppm"] = -4;
    source->accepted["dcSuppression"] = false;
    source->connectedState = true;
    emit model.connectionStateChanged(true);
    check(ppm->value() == -4 && !dc->isChecked() && apply->isEnabled(), "reconnect seeds the current device without old pending intent");
    source->caps.extensionNamespaces.clear();
    emit model.capabilitiesChanged(true, source->caps);
    check(!apply->isEnabled() && !apply->accessibleDescription().isEmpty(), "unavailable extension dims controls with an accessible reason");
    return failures ? 1 : 0;
}
