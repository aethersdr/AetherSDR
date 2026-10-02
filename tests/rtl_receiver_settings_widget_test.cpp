#include "TestSettingsProfile.h"
#include "core/AppSettings.h"
#include "gui/RtlReceiverSettingsWidget.h"
#include "models/RadioModel.h"

#include <QApplication>
#include <QCheckBox>
#include <QDialog>
#include <QDialogButtonBox>
#include <QElapsedTimer>
#include <QEventLoop>
#include <QKeyEvent>
#include <QLabel>
#include <QPushButton>
#include <QSpinBox>
#include <QThread>
#include <QVBoxLayout>
#include <cstdio>

using namespace AetherSDR;
namespace {
int failures = 0;
void check(bool value, const char* message)
{
    if (!value) { ++failures; std::fprintf(stderr, "FAIL: %s\n", message); }
}
void pumpFor(int milliseconds)
{
    QElapsedTimer elapsed;
    elapsed.start();
    while (elapsed.elapsed() < milliseconds) {
        QApplication::processEvents(QEventLoop::AllEvents, 10);
        QThread::msleep(1);
    }
}
template<typename Predicate>
bool waitFor(Predicate predicate)
{
    QElapsedTimer elapsed;
    elapsed.start();
    while (!predicate() && elapsed.elapsed() < 1500) { pumpFor(1); }
    return predicate();
}
void press(QWidget& target, int key, const QString& text = {})
{
    QKeyEvent down(QEvent::KeyPress, key, Qt::NoModifier, text);
    QKeyEvent up(QEvent::KeyRelease, key, Qt::NoModifier, text);
    QApplication::sendEvent(&target, &down);
    QApplication::sendEvent(&target, &up);
}
// Injected extension seam, no firmware simulator, socket or device access.
class ExtensionBackend final : public IRadioBackend {
public:
    RadioCapabilities caps;
    bool connectedState = true;
    bool synchronousPpm = false;
    int controlRequests = 0;
    quint64 pendingId = 0;
    QVariant requested;
    QString verb;
    QVariantMap accepted{{"serial", "widget-device"}, {"applied", true},
        {"ppm", 7}, {"requestedPpm", 7}, {"dcSuppression", false}, {"receiveMetersEnabled", true}, {"pending", false}, {"saved", true}};
    ExtensionBackend()
    {
        caps.extensionNamespaces = {QStringLiteral("rtl")};
        caps.extensions["rtl"] = QVariantMap{{"settingsVersion", 1}, {"receiveMetersControl", true}};
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
        ++controlRequests;
        pendingId = id; verb = name; requested = arg;
        accepted["pending"] = true;
        if (name == QLatin1String("ppm.set")) { accepted["requestedPpm"] = arg; }
        emit extensionStatus("rtl", "settings", accepted);
        if (synchronousPpm && name == QLatin1String("ppm.set")) { confirm(); }
    }
    void confirm()
    {
        // Copy before emitting: a result may synchronously reenter settings.get.
        const quint64 id = pendingId;
        const QVariant value = requested;
        accepted[verb == QLatin1String("ppm.set") ? "ppm"
            : verb == QLatin1String("receive_meters.set") ? "receiveMetersEnabled" : "dcSuppression"] = value;
        accepted["pending"] = false;
        emit extensionStatus("rtl", "settings", accepted);
        emit extensionResult(id, value);
    }
    void refuse(const QString& reason)
    {
        accepted["pending"] = false;
        emit extensionError(pendingId, reason);
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
    // Match the settings window's Close-only dialog, including its default
    // button routing; Return must commit PPM without closing this parent.
    QDialog dialog;
    auto* layout = new QVBoxLayout(&dialog);
    RtlReceiverSettingsWidget widget(model, &dialog);
    layout->addWidget(&widget);
    auto* buttons = new QDialogButtonBox(QDialogButtonBox::Close);
    layout->addWidget(buttons);
    QObject::connect(buttons, &QDialogButtonBox::rejected, &dialog, &QDialog::close);
    auto* ppm = widget.findChild<QSpinBox*>(QStringLiteral("rtlPpmRequest"));
    auto* apply = widget.findChild<QPushButton*>(QStringLiteral("rtlPpmApply"));
    auto* dc = widget.findChild<QCheckBox*>(QStringLiteral("rtlDcSuppression"));
    auto* meters = widget.findChild<QCheckBox*>(QStringLiteral("rtlReceiveMeters"));
    check(meters != nullptr, "RTL receiver settings expose the receive-meter switch");
    if (!meters) { return 1; }
    auto* applied = widget.findChild<QLabel*>(QStringLiteral("rtlCorrectionsApplied"));
    auto* status = widget.findChild<QLabel*>(QStringLiteral("rtlCorrectionsStatus"));
    if (!ppm || !apply || !dc || !applied || !status) { return 1; }
    dialog.show(); pumpFor(1);
    check(ppm->value() == 7 && !dc->isChecked() && apply->isEnabled(), "lazy widget immediately loads accepted settings");
    check(ppm->minimum() == -1000 && ppm->maximum() == 1000 && !ppm->keyboardTracking(), "integer PPM entry exposes hardware bounds and waits for complete entry");
    check(!apply->autoDefault(), "Enter in PPM entry cannot also trigger an automatic Apply button");
    pumpFor(260);
    check(source->controlRequests == 0, "initial confirmed state never schedules a PPM request");

    ppm->setValue(18); apply->click();
    const quint64 prior = source->pendingId;
    check(source->verb == QLatin1String("ppm.set") && source->requested.toInt() == 18
        && applied->text().contains("7 ppm") && status->text().contains("Applying"), "requested correction cannot masquerade as applied");
    const int firstRequest = source->controlRequests;
    apply->click(); pumpFor(260);
    check(source->controlRequests == firstRequest, "Apply flush cancels debounce and duplicate pending Apply sends nothing");
    ppm->setValue(19); apply->click();
    check(source->pendingId != prior, "replacement request has a fresh correlation id");
    emit source->extensionError(prior, "superseded");
    emit source->extensionResult(prior, 18);
    check(!status->text().contains("refused") && status->text().contains("Applying")
        && ppm->value() == 19 && applied->text().contains("7 ppm"), "stale request error and result cannot replace the latest request");
    source->confirm();
    check(applied->text().contains("19 ppm") && !status->text().contains("Applying"), "confirmed reply settles the applied value");
    const int settled = source->controlRequests;
    apply->click(); pumpFor(260);
    check(source->controlRequests == settled, "Apply of the settled value is a no-op");

    ppm->setValue(20); apply->click();
    ppm->setValue(21); source->confirm();
    check(ppm->value() == 21 && applied->text().contains("20 ppm"), "completion cannot erase a newer unsent edit");
    const int beforeNewer = source->controlRequests;
    check(waitFor([&] { return source->controlRequests > beforeNewer; })
        && source->controlRequests == beforeNewer + 1 && source->requested.toInt() == 21,
        "newer edit still debounces after the previous request completes");
    source->confirm();

    const int beforeBurst = source->controlRequests;
    ppm->setValue(30); pumpFor(40);
    ppm->setValue(31); pumpFor(40);
    ppm->setValue(32);
    check(source->controlRequests == beforeBurst, "rapid PPM edits do not dispatch an intermediate value");
    check(waitFor([&] { return source->controlRequests > beforeBurst; })
        && source->controlRequests == beforeBurst + 1 && source->requested.toInt() == 32,
        "visible PPM burst automatically submits only its final value without Apply");
    source->confirm();

    const int beforeArrow = source->controlRequests;
    ppm->setFocus();
    press(*ppm, Qt::Key_Up); press(*ppm, Qt::Key_Up);
    check(ppm->value() == 34 && source->controlRequests == beforeArrow,
        "real arrow keys edit PPM before the debounce expires");
    check(waitFor([&] { return source->controlRequests > beforeArrow; })
        && source->controlRequests == beforeArrow + 1 && source->requested.toInt() == 34,
        "arrow-key edits automatically apply while the settings window stays open");
    source->confirm();
    const int beforeTyping = source->controlRequests;
    ppm->selectAll();
    press(*ppm, Qt::Key_Minus, QStringLiteral("-"));
    press(*ppm, Qt::Key_4, QStringLiteral("4"));
    press(*ppm, Qt::Key_2, QStringLiteral("2"));
    pumpFor(260);
    check(ppm->text().contains(QStringLiteral("-42")) && ppm->value() == 34
        && source->controlRequests == beforeTyping, "uncommitted typed digits never send partial corrections");
    press(*ppm, Qt::Key_Return);
    check(dialog.isVisible(), "Enter commits PPM without activating the parent dialog Close button");
    if (!dialog.isVisible()) { return 1; }
    check(waitFor([&] { return source->controlRequests > beforeTyping; })
        && source->controlRequests == beforeTyping + 1 && source->requested.toInt() == -42,
        "Enter commits typed PPM and automatic application uses the complete number");
    source->confirm(); pumpFor(260);
    check(source->controlRequests == beforeTyping + 1, "typed commit and Enter cannot submit twice");

    ppm->setValue(18); apply->click();
    const quint64 reversedRequest = source->pendingId;
    const int beforeReverse = source->controlRequests;
    ppm->setValue(-42);
    check(waitFor([&] { return source->controlRequests > beforeReverse; })
        && source->controlRequests == beforeReverse + 1 && source->requested.toInt() == -42,
        "returning to confirmed PPM supersedes a different in-flight value");
    emit source->extensionResult(reversedRequest, 18);
    emit source->extensionError(reversedRequest, "superseded");
    check(ppm->value() == -42 && status->text().contains("Applying")
        && !status->text().contains("refused"), "old completion cannot settle a pending reversal");
    source->confirm();

    ppm->setValue(20); apply->click();
    ppm->setValue(21);
    source->refuse(QStringLiteral("readback failed"));
    const int beforeNewerAfterError = source->controlRequests;
    check(ppm->value() == 21 && applied->text().contains("-42 ppm")
        && status->text().contains("readback failed"), "refusal cannot overwrite a newer unsent edit or applied state");
    check(waitFor([&] { return source->controlRequests > beforeNewerAfterError; })
        && source->controlRequests == beforeNewerAfterError + 1 && source->requested.toInt() == 21,
        "failure of an older request preserves automatic application of a newer operator edit");
    source->refuse(QStringLiteral("latest readback failed"));
    const int beforeRetry = source->controlRequests;
    pumpFor(260);
    check(ppm->value() == 21 && applied->text().contains("-42 ppm")
        && status->text().contains("latest readback failed") && source->controlRequests == beforeRetry,
        "the rejected current value remains available but never automatically retries itself");
    apply->click();
    check(source->controlRequests == beforeRetry + 1 && source->requested.toInt() == 21,
        "operator can explicitly retry the retained edit after refusal");
    source->confirm();
    dc->click();
    check(source->verb == QLatin1String("dc_suppression.set") && source->requested.toBool()
        && !dc->isChecked() && applied->text().endsWith("off"), "DC checkbox remains confirmed while DSP change is pending");
    source->confirm();
    check(dc->isChecked() && applied->text().endsWith("on"), "DC checkbox follows DSP adoption");
    check(dc->accessibleDescription().isEmpty(), "checked DC control does not also announce an inactive state");
    source->accepted["saved"] = false;
    source->accepted["saveReason"] = "Applied for this session; saving failed.";
    emit source->extensionStatus("rtl", "settings", source->accepted);
    check(status->text().contains("saving failed") && !status->text().contains("corrections saved"), "persistence failure does not claim durable settings");

    ppm->setValue(40); apply->click();
    const int beforeHide = source->controlRequests;
    ppm->setValue(41); widget.hide();
    check(ppm->value() == 40, "hiding discards only the unsent edit and retains the submitted value");
    pumpFor(260);
    check(source->controlRequests == beforeHide, "hidden page never dispatches its discarded debounce");
    source->confirm();
    check(ppm->value() == 40 && applied->text().contains("40 ppm"), "submitted PPM can complete while the page is hidden");
    widget.show(); pumpFor(260);
    check(source->controlRequests == beforeHide, "showing the page does not replay completed or discarded intent");
    ppm->setValue(41); widget.hide();
    check(ppm->value() == 40, "hiding an unsent edit resets entry to the confirmed value");
    ppm->setValue(42); pumpFor(260);
    check(source->controlRequests == beforeHide, "programmatic value change on a hidden page cannot start live calibration");
    emit source->extensionStatus("rtl", "settings", source->accepted);
    widget.show(); pumpFor(260);
    check(source->controlRequests == beforeHide, "reopening never applies a hidden value change");

    ppm->setValue(43);
    source->connectedState = false;
    emit model.connectionStateChanged(false);
    pumpFor(260);
    check(source->controlRequests == beforeHide && !apply->isEnabled() && !dc->isChecked()
        && applied->text().contains("unavailable"), "disconnect retires queued edits and confirmed controls");
    emit source->extensionResult(prior, 18);
    emit source->extensionError(prior, "old session");
    check(applied->text().contains("unavailable") && !status->text().contains("old session"),
        "disconnected page ignores stale completion and error");
    source->accepted["ppm"] = -4;
    source->accepted["requestedPpm"] = -4;
    source->accepted["dcSuppression"] = false;
    source->connectedState = true;
    emit model.connectionStateChanged(true);
    pumpFor(260);
    check(ppm->value() == -4 && !dc->isChecked() && apply->isEnabled()
        && source->controlRequests == beforeHide, "reconnect seeds current device without replaying old intent");
    ppm->setValue(44);
    source->caps.extensionNamespaces.clear();
    emit model.capabilitiesChanged(true, source->caps);
    pumpFor(260);
    check(source->controlRequests == beforeHide && !apply->isEnabled()
        && !apply->accessibleDescription().isEmpty(), "capability loss cancels debounce and dims controls with an accessible reason");
    source->caps.extensionNamespaces = {QStringLiteral("rtl")};
    emit model.capabilitiesChanged(true, source->caps);
    check(ppm->value() == -4, "capability restoration seeds confirmed PPM without stale edits");

    ppm->setValue(45);
    auto replacement = std::make_unique<ExtensionBackend>();
    ExtensionBackend* next = replacement.get();
    next->accepted["ppm"] = -8;
    next->accepted["requestedPpm"] = -8;
    model.setBackendForTest(std::move(replacement), QStringLiteral("test"));
    source = next;
    emit model.connectionStateChanged(true);
    emit source->extensionResult(prior, 18);
    emit source->extensionError(prior, "prior backend");
    pumpFor(260);
    check(ppm->value() == -8 && source->controlRequests == 0 && !status->text().contains("prior backend"),
        "backend rebind drops debounce and old request identities");

    source->synchronousPpm = true;
    ppm->setValue(9);
    check(waitFor([&] { return source->controlRequests != 0; }) && source->controlRequests == 1
        && ppm->value() == 9 && applied->text().contains("9 ppm") && !status->text().contains("Applying"),
        "synchronous extension completion settles a debounced request without reentrancy loss");
    pumpFor(260); apply->click();
    check(source->controlRequests == 1, "synchronous completion cannot leave a duplicate timer or pending request");
    ppm->setValue(10); apply->click(); pumpFor(260);
    check(source->controlRequests == 2 && applied->text().contains("10 ppm") && !status->text().contains("Applying"),
        "synchronous immediate Apply cancels debounce before invoking the extension");
    check(meters->isChecked() && meters->isEnabled(), "receive meters default to confirmed enabled state");
    meters->click();
    check(source->verb == QLatin1String("receive_meters.set") && source->requested.metaType().id() == QMetaType::Bool
        && !source->requested.toBool() && meters->isChecked(), "meter toggle waits for accepted readback");
    source->confirm();
    check(!meters->isChecked() && !status->text().contains("Applying"), "confirmed meter disable clears pending state");
    meters->click(); source->refuse("test refusal");
    check(!meters->isChecked(), "refused enable cannot masquerade as enabled");
    meters->click(); source->confirm();
    check(meters->isChecked(), "confirmed re-enable restores the switch");
    const int beforeDestroy = source->controlRequests;
    {
        auto closing = std::make_unique<RtlReceiverSettingsWidget>(model);
        closing->show(); pumpFor(1);
        auto* closingPpm = closing->findChild<QSpinBox*>(QStringLiteral("rtlPpmRequest"));
        if (!closingPpm) { return 1; }
        closingPpm->setValue(11);
    }
    pumpFor(260);
    check(source->controlRequests == beforeDestroy, "destroying the settings page cancels its unsubmitted timer");
    return failures ? 1 : 0;
}
