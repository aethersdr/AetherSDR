// Production VFO/model/Flex encoder; no default socket, firmware peer or RF.
#include "TestSettingsProfile.h"
#include "SeamThreadAffinityProbe.h"
#include "core/AutomationServer.h"
#include "core/backends/flex/FlexBackend.h"
#include "core/backends/sim/SimBackend.h"
#include "gui/VfoWidget.h"
#include "models/RadioModel.h"
#include "models/SliceModel.h"

#include <QApplication>
#include <QAbstractItemModel>
#include <QComboBox>
#include <QLabel>
#include <QList>
#include <QJsonDocument>
#include <QJsonObject>
#include <QMetaObject>
#include <QPair>
#include <QStringList>
#include <QWidget>
#include <QPushButton>
#include <QSignalSpy>
#include <QVBoxLayout>
#include <QTest>
#include <cstdio>
#include <functional>
#include <memory>
#include <thread>

namespace AetherSDR {
class AutomationServerTestAccess {
public:
    static QJsonObject request(AutomationServer& server, const QJsonObject& request)
    {
        return server.handleLine(QJsonDocument(request).toJson(QJsonDocument::Compact), nullptr);
    }
};
class RadioModelSliceLifecycleTestAccess {
public:
    static void identity(RadioModel& radio, const QString& model, const QString& version)
    {
        radio.m_model = model;
        radio.m_version = version;
        emit radio.infoChanged();
    }
    static void own(RadioModel& radio, bool owned)
    {
        if (owned) { radio.m_ownedSliceIds.insert(0); }
        else { radio.m_ownedSliceIds.remove(0); }
        emit radio.slotOccupancyChanged(0);
    }
    static void license(RadioModel& radio, const QString& enabled)
    {
        radio.onStatusReceived(QStringLiteral("license feature"), {
            {QStringLiteral("name"), QStringLiteral("noise_reduction")},
            {QStringLiteral("enabled"), enabled}, {QStringLiteral("reason"), QStringLiteral("LICENSE_FILE")}});
    }
    static void expireSubscription(RadioModel& radio)
    {
        radio.m_licenseSubscription = QStringLiteral("SmartSDR");
        radio.m_licenseExpirationDate = QStringLiteral("01/01/2000");
    }
    static void disconnectSession(RadioModel& radio)
    {
        radio.m_intentionalDisconnect = true;
        radio.onDisconnected();
    }
    static void reclaim(RadioModel& radio)
    {
        radio.stageSessionModelsForReconnect();
        identity(radio, QStringLiteral("FLEX-8600"), QStringLiteral("4.3.16.42034"));
        own(radio, true);
        FlexBackend* previous = radio.m_flexBackend;
        radio.m_flexBackend = qobject_cast<FlexBackend*>(radio.backend());
        radio.handleSliceStatus(0, {{QStringLiteral("in_use"), QStringLiteral("1")},
            {QStringLiteral("RF_frequency"), QStringLiteral("14.2")},
            {QStringLiteral("mode"), QStringLiteral("USB")}}, false);
        radio.m_flexBackend = previous;
    }
};
}
using namespace AetherSDR;
namespace {
int failures = 0;
void check(bool passed, const char* message)
{
    std::printf("[%s] %s\n", passed ? "PASS" : "FAIL", message);
    failures += !passed;
}
class ConnectedFlex final : public FlexBackend {
public:
    bool connected = true;
    bool isConnected() const override { return connected; }
};
struct Fixture {
    RadioModel radio;
    ConnectedFlex* backend;
    QStringList commands;
    VfoWidget vfo;
    Fixture()
    {
        auto owned = std::make_unique<ConnectedFlex>();
        backend = owned.get();
        radio.setBackendForTest(std::move(owned), QStringLiteral("flex"));
        backend->setModelProvider([this] { return radio.model(); });
        backend->setFirmwareVersionProvider([this] { return radio.softwareVersion(); });
        backend->setDspResetLicenseProvider([this]() -> std::optional<bool> {
            const LicenseFeatureState license = radio.licenseFeature(QStringLiteral("NOISE_REDUCTION"));
            return license.seen ? std::optional<bool>(license.enabled) : std::nullopt;
        });
        backend->setSliceCommandSink([this](const QString& command) { commands.append(command); });
        RadioModelSliceLifecycleTestAccess::identity(radio, QStringLiteral("FLEX-8600"), QStringLiteral("4.3.16.42034"));
        SliceDelta delta;
        delta.panId = QStringLiteral("0x40000000");
        delta.mode = QStringLiteral("USB");
        delta.inUse = true;
        delta.nr = true; delta.nrLevel = 73;
        delta.nb = true; delta.nbLevel = 61;
        delta.apf = false; delta.apfLevel = 42;
        emit backend->sliceChanged(0, delta);
        RadioModelSliceLifecycleTestAccess::own(radio, true);
        vfo.setRadioModel(&radio);
        vfo.setSlice(slice());
        commands.clear();
    }
    SliceModel* slice() { return radio.slice(0); }
    QComboBox* target() { return vfo.findChild<QComboBox*>(QStringLiteral("dspParameterResetTarget")); }
    QPushButton* reset() { return vfo.findChild<QPushButton*>(QStringLiteral("dspParameterResetButton")); }
    void choose(DspParameterResetTarget target)
    { this->target()->setCurrentIndex(this->target()->findData(static_cast<int>(target))); }
    bool request(DspParameterResetTarget target, SliceModel* source = nullptr)
    { return radio.requestDspParameterReset(source ? source : slice(), radio.receiveControlSession(), target); }
    void status()
    { backend->decodeSliceStatus(0, {{"nr_level", "50"}, {"nb_level", "50"}, {"apf_level", "30"}}); }
};
void encodingAndGates()
{
    Fixture f;
    using Target = DspParameterResetTarget;
    check(f.commands.isEmpty(), "construction, status and UI binding do not reset");
    check(!f.request(Target::Nr) && f.commands.isEmpty(), "unknown entitlement refuses individual advanced reset");
    check(f.request(Target::All) && f.commands == QStringList{"slice reset 0 dsp_param=all"},
          "All is one Basic command, never an individual-target loop");
    check(f.slice()->nrLevel() == 73 && f.slice()->nbLevel() == 61 && f.slice()->apfLevel() == 42
              && f.slice()->nrOn() && f.slice()->nbOn() && !f.slice()->apfOn(),
          "reset command neither guesses levels nor changes enabled state");
    f.commands.clear();
    check(f.request(Target::Nrs) && f.commands == QStringList{"slice reset 0 dsp_param=speex_nr"},
          "NRS level-only reset requires no advanced entitlement");
    RadioModelSliceLifecycleTestAccess::license(f.radio, QStringLiteral("1"));
    RadioModelSliceLifecycleTestAccess::expireSubscription(f.radio);
    const QList<QPair<Target, QString>> cases = {{Target::Nr, "nr"}, {Target::Anf, "anf"},
        {Target::Nrl, "lms_nr"}, {Target::Anfl, "lms_anf"}, {Target::Nrf, "nrf"},
        {Target::Nrs, "speex_nr"}, {Target::Anft, "anft"}};
    for (const auto& [target, token] : cases) {
        f.commands.clear();
        check(f.request(target) && f.commands == QStringList{QStringLiteral("slice reset 0 dsp_param=") + token},
              "whitelisted target emits exact FlexLib encoding with retained entitlement");
    }
    f.commands.clear();
    for (int invalid : {-1, 8, 100}) {
        check(!f.request(static_cast<Target>(invalid))
            && f.backend->requestDspParameterReset(0, static_cast<Target>(invalid)) == ReceiveDispatch::Unsupported,
            "invalid enums fail at model and Flex boundaries");
    }
    for (int id : {-1, 4, 2147483647}) {
        check(f.backend->requestDspParameterReset(id, Target::All) == ReceiveDispatch::Unsupported
                  && f.commands.isEmpty(), "invalid or unavailable slice id cannot be encoded");
    }
    for (const QString& version : {QString(), QStringLiteral("unknown"), QStringLiteral("4.2.20.41343"),
            QStringLiteral("4.3"), QStringLiteral("4.3.15"), QStringLiteral("4.3.16junk"),
            QStringLiteral("4.3.16.42034junk"), QStringLiteral(" 4.3.16")}) {
        RadioModelSliceLifecycleTestAccess::identity(f.radio, "FLEX-8600", version);
        check(!f.request(Target::All) && f.backend->requestDspParameterReset(0, Target::All) == ReceiveDispatch::Unsupported
                  && f.commands.isEmpty(), "old, missing and malformed firmware refuse without side effects");
    }
    for (const QString& version : {QStringLiteral("4.3.16"), QStringLiteral("4.3.16.42034"), QStringLiteral("5.0.0")}) {
        RadioModelSliceLifecycleTestAccess::identity(f.radio, "FLEX-8600", version);
        check(f.request(Target::All), "firmware boundary and later versions support reset");
    }
    f.commands.clear();
    RadioModelSliceLifecycleTestAccess::identity(f.radio, "FLEX-6700", "4.3.16");
    check(!f.request(Target::Nrs) && !f.request(Target::Nrf) && f.commands.isEmpty(),
          "6000-series rejects NRS and NRF reset");
    check(f.request(Target::Nrl), "6000-series supports licensed LMS reset");
    f.commands.clear();
    RadioModelSliceLifecycleTestAccess::identity(f.radio, "UNKNOWN", "4.3.16");
    check(!f.request(Target::All) && f.commands.isEmpty(), "unknown hardware fails closed");
    RadioModelSliceLifecycleTestAccess::identity(f.radio, "FLEX-8600", "4.3.16");
    RadioModelSliceLifecycleTestAccess::license(f.radio, QStringLiteral("0"));
    check(!f.request(Target::Nr) && !f.request(Target::Anft) && f.commands.isEmpty(),
          "explicit license denial refuses advanced resets");
    check(f.request(Target::All) && f.request(Target::Nrs), "license denial leaves Basic All and NRS usable");
    f.commands.clear();
    RadioModelSliceLifecycleTestAccess::license(f.radio, QStringLiteral("1"));
    for (const QString& mode : {QStringLiteral("CW"), QStringLiteral("RTTY"), QStringLiteral("DIGU"),
            QStringLiteral("DIGL"), QStringLiteral("FDVU"), QStringLiteral("FDVL")}) {
        SliceDelta delta; delta.mode = mode; emit f.backend->sliceChanged(0, delta);
        check(!f.request(Target::Anf) && !f.request(Target::Anfl) && !f.request(Target::Anft),
              "notch target reset respects mode restrictions");
        check(f.request(Target::All), "All remains available in supported non-FM modes");
    }
    f.commands.clear();
    for (const QString& mode : {QStringLiteral("FM"), QStringLiteral("NFM"), QStringLiteral("DFM"),
            QStringLiteral("DSTR"), QStringLiteral("UNKNOWN"), QString()}) {
        SliceDelta delta; delta.mode = mode; emit f.backend->sliceChanged(0, delta);
        f.commands.clear(); // isolate reset from existing mode-dependent UI controls
        check(!f.request(Target::All) && !f.request(Target::Nrs) && f.commands.isEmpty(),
              "FM and unknown modes cannot reset parameters");
    }
}
void productionProviders()
{
    RadioModel radio;
    RadioModelSliceLifecycleTestAccess::identity(radio, QStringLiteral("FLEX-8600"), QStringLiteral("4.3.16"));
    check(radio.rebuildBackendForTest(QStringLiteral("flex")), "production setup creates Flex without connecting");
    auto* backend = qobject_cast<FlexBackend*>(radio.backend());
    check(backend != nullptr, "production setup selects Flex backend");
    if (!backend) { return; }
    QStringList commands;
    backend->setSliceCommandSink([&](const QString& command) { commands.append(command); });
    check(backend->requestDspParameterReset(0, DspParameterResetTarget::Nr) == ReceiveDispatch::Unsupported,
          "production license provider starts unknown");
    RadioModelSliceLifecycleTestAccess::license(radio, QStringLiteral("1"));
    check(backend->requestDspParameterReset(0, DspParameterResetTarget::Nr) == ReceiveDispatch::Dispatched
              && commands == QStringList{"slice reset 0 dsp_param=nr"},
          "production setup wires live normalized license and firmware providers");
    commands.clear();
    RadioModelSliceLifecycleTestAccess::license(radio, QStringLiteral("0"));
    check(backend->requestDspParameterReset(0, DspParameterResetTarget::Nr) == ReceiveDispatch::Unsupported
              && commands.isEmpty(), "production provider observes license revocation");
    RadioModelSliceLifecycleTestAccess::identity(radio, QStringLiteral("FLEX-8600"), QStringLiteral("4.3.15"));
    check(backend->requestDspParameterReset(0, DspParameterResetTarget::All) == ReceiveDispatch::Unsupported
              && commands.isEmpty(), "production firmware provider observes an older radio");
}

void identityAndSession()
{
    Fixture f;
    using Target = DspParameterResetTarget;
    SliceModel stale(0);
    check(!f.request(Target::All, &stale) && f.commands.isEmpty(), "different object with reused id is refused");
    RadioModelSliceLifecycleTestAccess::own(f.radio, false);
    check(!f.request(Target::All) && f.commands.isEmpty(), "unconfirmed or foreign ownership is refused");
    RadioModelSliceLifecycleTestAccess::own(f.radio, true);
    f.slice()->invalidateFrequencyObservation();
    check(!f.request(Target::All) && !f.reset()->isEnabled(), "missing radio mode observation cannot authorize reset");
    f.backend->decodeSliceStatus(0, {{"mode", "USB"}});
    check(f.reset()->isEnabled(), "same-value mode status restores availability after invalidation");
    f.slice()->setMode(QStringLiteral("CW"));
    f.commands.clear();
    check(!f.request(Target::All), "unconfirmed optimistic mode cannot authorize reset");
    f.backend->decodeSliceStatus(0, {{"mode", "CW"}});
    check(f.reset()->isEnabled(), "radio confirmation re-enables reset for current mode");
    f.backend->decodeSliceStatus(0, {{"mode", "USB"}});
    f.commands.clear();
    bool dispatched = true;
    SliceModel* slice = f.slice();
    const quint64 session = f.radio.receiveControlSession();
    std::thread offThread([&] { dispatched = f.radio.requestDspParameterReset(slice, session, Target::All); });
    offThread.join();
    check(!dispatched && f.commands.isEmpty(), "off-owner-thread request is rejected synchronously");
    f.backend->connected = false;
    emit f.radio.connectionStateChanged(false);
    check(!f.request(Target::All) && !f.reset()->isEnabled() && f.commands.isEmpty(), "disconnect disables UI and model dispatch");
    f.backend->connected = true;
    RadioModelSliceLifecycleTestAccess::disconnectSession(f.radio);
    QSignalSpy added(&f.radio, &RadioModel::sliceAdded);
    RadioModelSliceLifecycleTestAccess::reclaim(f.radio);
    check(f.radio.slice(0) == slice && added.isEmpty(), "production status reclaims exact object without another sliceAdded");
    check(!f.radio.requestDspParameterReset(slice, session, Target::All) && f.commands.isEmpty(),
          "old session cannot act even if the exact slice object is reclaimed");
    f.reset()->click();
    check(f.commands == QStringList{"slice reset 0 dsp_param=all"}, "new deliberate click works after same-object reclaim without rebinding VFO");
    f.commands.clear();
    QMetaObject::invokeMethod(f.reset(), "pressed", Qt::DirectConnection);
    RadioModelSliceLifecycleTestAccess::disconnectSession(f.radio);
    RadioModelSliceLifecycleTestAccess::reclaim(f.radio);
    QMetaObject::invokeMethod(f.reset(), "clicked", Qt::DirectConnection, Q_ARG(bool, false));
    check(f.commands.isEmpty(), "held press cannot cross a reclaimed session");
    QMetaObject::invokeMethod(f.reset(), "pressed", Qt::DirectConnection);
    f.choose(Target::Nrs);
    QMetaObject::invokeMethod(f.reset(), "clicked", Qt::DirectConnection, Q_ARG(bool, false));
    check(f.commands.isEmpty(), "target changed during a press cannot reset another target");
    const quint64 beforeReplacement = f.radio.receiveControlSession();
    f.radio.setBackendForTest(std::make_unique<ConnectedFlex>(), QStringLiteral("flex"));
    check(beforeReplacement != f.radio.receiveControlSession(), "backend replacement also retires control session");
    SimBackend sim;
    check(!sim.capabilities().dspParameterReset
              && sim.requestDspParameterReset(0, Target::All) == ReceiveDispatch::Unsupported,
          "unsupported backends neither advertise nor inherit a working reset verb");
}
void userAndStatus()
{
    Fixture f;
    test::SeamThreadAffinityProbe probe(f.backend);
    test::attachAllSeamSignals(probe);
    using Target = DspParameterResetTarget;
    RadioModelSliceLifecycleTestAccess::license(f.radio, QStringLiteral("1"));
    f.choose(Target::Nr);
    check(f.commands.isEmpty(), "changing selection alone never resets");
    f.reset()->click();
    check(f.commands == QStringList{"slice reset 0 dsp_param=nr"}, "explicit production Reset click dispatches once");
    f.commands.clear();
    f.status();
    check(f.commands.isEmpty() && f.slice()->nrLevel() == 50 && f.slice()->nbLevel() == 50
              && f.slice()->apfLevel() == 30 && f.slice()->nrOn() && f.slice()->nbOn() && !f.slice()->apfOn(),
          "production status decoder reconciles levels, preserves enables and emits no command");
    RadioModelSliceLifecycleTestAccess::license(f.radio, QStringLiteral("0"));
    check(!f.reset()->isEnabled() && !f.reset()->toolTip().isEmpty()
              && !f.reset()->accessibleDescription().isEmpty(), "denied target remains present, disabled with accessible reason");
    f.reset()->click();
    check(f.commands.isEmpty(), "disabled production Reset has no effect");
    f.choose(Target::All);
    check(f.reset()->isEnabled(), "All remains enabled after advanced license denial");
    f.slice()->setExternalReceiveAudioReplacementMute(true, false);
    check(!f.reset()->isEnabled(), "external receiver replacement immediately disables reset");
    f.slice()->setExternalReceiveAudioReplacementMute(false, false);
    check(f.reset()->isEnabled(), "returning to radio receive immediately restores reset availability");
    f.commands.clear();
    f.reset()->setFocus();
    QTest::keyClick(f.reset(), Qt::Key_Space);
    check(f.commands == QStringList{"slice reset 0 dsp_param=all"}, "keyboard activation reaches the same operator path");
    check(probe.violations().isEmpty() && probe.afterDisconnect().isEmpty(),
          "exercised Flex seam signals honor owner-thread and disconnect boundaries");
}

void targetAvailability()
{
    Fixture f;
    using Target = DspParameterResetTarget;
    const auto enabled = [&f](Target target) {
        const int row = f.target()->findData(static_cast<int>(target));
        return f.target()->model()->flags(f.target()->model()->index(row, 0)).testFlag(Qt::ItemIsEnabled);
    };
    const auto reason = [&f](Target target, int role) {
        return f.target()->itemData(f.target()->findData(static_cast<int>(target)), role).toString();
    };
    check(f.target()->count() == 8 && enabled(Target::All) && enabled(Target::Nrs) && !enabled(Target::Nr),
          "unknown entitlement dims advanced choices while retaining all eight targets");
    check(!reason(Target::Nr, Qt::ToolTipRole).isEmpty()
              && !reason(Target::Nr, Qt::AccessibleDescriptionRole).isEmpty(),
          "unavailable target exposes a tooltip and screen-reader reason");
    f.target()->setFocus();
    QTest::keyClick(f.target(), Qt::Key_Down);
    check(f.target()->currentData().toInt() == static_cast<int>(Target::Nrs) && f.commands.isEmpty(),
          "keyboard selection skips unavailable targets without resetting");
    RadioModelSliceLifecycleTestAccess::license(f.radio, QStringLiteral("1"));
    check(enabled(Target::Nr), "license grant restores advanced menu choices");
    f.choose(Target::Nr);
    RadioModelSliceLifecycleTestAccess::license(f.radio, QStringLiteral("0"));
    check(!enabled(Target::Nr) && !f.reset()->isEnabled()
              && f.target()->accessibleDescription().contains(reason(Target::Nr, Qt::AccessibleDescriptionRole)),
          "revocation dims the selected target and explains the current restriction");
    RadioModelSliceLifecycleTestAccess::license(f.radio, QStringLiteral("1"));
    RadioModelSliceLifecycleTestAccess::identity(f.radio, "FLEX-6700", "4.3.16");
    check(!enabled(Target::Nrs) && !enabled(Target::Nrf) && enabled(Target::Nr),
          "hardware restrictions disable only unsupported targets");
    f.backend->decodeSliceStatus(0, {{"mode", "CW"}});
    check(!enabled(Target::Anf) && !enabled(Target::Anfl) && !enabled(Target::Anft) && enabled(Target::All),
          "mode changes refresh individual menu availability");
    f.backend->decodeSliceStatus(0, {{"mode", "FM"}});
    check(!enabled(Target::All) && !f.reset()->isEnabled() && f.commands.isEmpty(),
          "unsupported mode disables every target without a reset");
}
void invalidTargetSelection()
{
    Fixture f;
    f.vfo.show();
    for (QPushButton* button : f.vfo.findChildren<QPushButton*>()) {
        if (button->text() == QStringLiteral("DSP")) {
            button->click();
            break;
        }
    }
    AutomationServer bridge;
    bridge.setRadioModel(&f.radio);
    const auto invoke = [&bridge](const QString& target, const QString& action, const QString& value) {
        return AutomationServerTestAccess::request(bridge,
            {{QStringLiteral("cmd"), QStringLiteral("invoke")},
             {QStringLiteral("target"), target}, {QStringLiteral("action"), action},
             {QStringLiteral("value"), value}});
    };
    check(!bridge.isRunning() && !bridge.txAllowed(), "bridge dispatcher proof uses no socket or TX permission");
    const QJsonObject cleared = invoke(QStringLiteral("dspParameterResetTarget"),
                                      QStringLiteral("setCurrentIndex"), QStringLiteral("-1"));
    check(cleared.value(QStringLiteral("ok")).toBool() && f.target()->currentIndex() == -1,
          "production bridge can clear the reset target");
    check(!f.reset()->isEnabled() && !f.reset()->accessibleDescription().isEmpty(),
          "empty reset selection disables Reset with an accessible reason");
    const QJsonObject refused = invoke(QStringLiteral("dspParameterResetButton"),
                                      QStringLiteral("click"), QString());
    check(!refused.value(QStringLiteral("ok")).toBool() && f.commands.isEmpty(),
          "bridge refuses Reset with no selected target and sends no All command");
    QMetaObject::invokeMethod(f.reset(), "pressed", Qt::DirectConnection);
    QMetaObject::invokeMethod(f.reset(), "clicked", Qt::DirectConnection, Q_ARG(bool, false));
    check(f.commands.isEmpty(), "direct signal delivery cannot arm an empty target");
    f.commands.clear();
    for (const QVariant& invalid : {QVariant(), QVariant(QStringLiteral("invalid")), QVariant(99)}) {
        f.choose(DspParameterResetTarget::Nrs);
        const int row = f.target()->currentIndex();
        const QVariant saved = f.target()->itemData(row);
        f.target()->setItemData(row, invalid);
        emit f.radio.infoChanged();
        check(!f.reset()->isEnabled(), "invalid selected item data disables Reset");
        QMetaObject::invokeMethod(f.reset(), "pressed", Qt::DirectConnection);
        QMetaObject::invokeMethod(f.reset(), "clicked", Qt::DirectConnection, Q_ARG(bool, false));
        check(f.commands.isEmpty(), "invalid selected item data cannot dispatch a reset");
        f.commands.clear();
        f.target()->setItemData(row, saved);
        emit f.radio.infoChanged();
    }
    f.choose(DspParameterResetTarget::All);
    QMetaObject::invokeMethod(f.reset(), "pressed", Qt::DirectConnection);
    f.target()->setCurrentIndex(-1);
    QMetaObject::invokeMethod(f.reset(), "clicked", Qt::DirectConnection, Q_ARG(bool, false));
    check(f.commands.isEmpty(), "clearing a valid target after press cancels the pending reset");
    f.commands.clear();
    f.choose(DspParameterResetTarget::All);
    QMetaObject::invokeMethod(f.reset(), "pressed", Qt::DirectConnection);
    f.target()->setCurrentIndex(-1);
    f.choose(DspParameterResetTarget::All);
    QMetaObject::invokeMethod(f.reset(), "clicked", Qt::DirectConnection, Q_ARG(bool, false));
    check(f.commands.isEmpty(), "reselecting All cannot resurrect a cancelled press");
    f.commands.clear();
    f.choose(DspParameterResetTarget::All);
    f.reset()->click();
    check(f.commands == QStringList{"slice reset 0 dsp_param=all"},
          "a fresh explicit All selection and click still dispatch exactly once");
}
int bridgeFixture(QApplication& app)
{
    Fixture f;
    RadioModelSliceLifecycleTestAccess::license(f.radio, QStringLiteral("1"));
    QWidget window;
    window.setObjectName(QStringLiteral("dspResetFixture"));
    window.setWindowTitle(QStringLiteral("DSP reset — synthetic status fixture (no radio)"));
    auto* layout = new QVBoxLayout(&window);
    auto* title = new QLabel(QStringLiteral("Synthetic fixture: real VFO/model/Flex encoder. No radio or RF."));
    title->setWordWrap(true);
    layout->addWidget(title);
    layout->addWidget(&f.vfo);
    auto* log = new QLabel(QStringLiteral("[]"));
    log->setObjectName(QStringLiteral("fixtureCommandLog"));
    log->setWordWrap(true);
    layout->addWidget(log);
    f.backend->setSliceCommandSink([&](const QString& command) {
        f.commands.append(command);
        log->setText(f.commands.join(QStringLiteral("\n")));
    });
    auto fixtureButton = [&](const QString& name, const QString& text, const std::function<void()>& action) {
        auto* button = new QPushButton(text, &window);
        button->setObjectName(name);
        layout->addWidget(button);
        QObject::connect(button, &QPushButton::clicked, &window, action);
    };
    fixtureButton("fixtureApplyStatus", "Fixture: apply radio-reported reset levels", [&] { f.status(); });
    fixtureButton("fixtureDenyLicense", "Fixture: deny advanced entitlement", [&] {
        RadioModelSliceLifecycleTestAccess::license(f.radio, "0");
    });
    fixtureButton("fixtureDisconnect", "Fixture: disconnect", [&] {
        f.backend->connected = false; emit f.radio.connectionStateChanged(false);
    });
    fixtureButton("fixtureRetireSession", "Fixture: retire session (same slice object)", [&] {
        RadioModelSliceLifecycleTestAccess::disconnectSession(f.radio);
        RadioModelSliceLifecycleTestAccess::reclaim(f.radio);
    });
    window.show();
    for (QPushButton* button : f.vfo.findChildren<QPushButton*>()) {
        if (button->text() == QStringLiteral("DSP")) { button->click(); break; }
    }
    AutomationServer bridge;
    bridge.setRadioModel(&f.radio);
    if (!bridge.start(QStringLiteral("aethersdr-dsp-reset-fixture"))) {
        f.vfo.setParent(nullptr);
        return 77;
    }
    std::printf("DSP_RESET_FIXTURE_SOCKET=%s\n", qPrintable(bridge.fullServerName()));
    std::fflush(stdout);
    const int result = app.exec();
    f.vfo.setParent(nullptr);
    return result;
}
}
int main(int argc, char** argv)
{
    TestSettingsProfile profile(QStringLiteral("dsp-parameter-reset"));
    QApplication app(argc, argv);
    if (app.arguments().contains(QStringLiteral("--bridge-fixture"))) { return bridgeFixture(app); }
    encodingAndGates();
    productionProviders();
    identityAndSession();
    userAndStatus();
    targetAvailability();
    invalidTargetSelection();
    return failures ? 1 : 0;
}
