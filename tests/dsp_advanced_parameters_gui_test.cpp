// Production widgets, bridge dispatcher and Flex command sink; no firmware peer.
#include "TestSettingsProfile.h"
#include "core/AutomationServer.h"
#include "core/backends/flex/FlexBackend.h"
#include "gui/VfoWidget.h"
#include "gui/DspAdvancedControls.h"
#include "models/RadioModel.h"
#include "models/SliceModel.h"

#include <QApplication>
#include <QComboBox>
#include <QJsonDocument>
#include <QLabel>
#include <QMetaObject>
#include <QMap>
#include <QStringList>
#include <QPushButton>
#include <QPointer>
#include <QSignalSpy>
#include <QSpinBox>
#include <QStackedWidget>
#include <QLayout>
#include <QTest>
#include <QVBoxLayout>
#include <cstdio>
#include <functional>
#include <memory>
#include <optional>
#include <utility>
#include <vector>

namespace AetherSDR {
class AutomationServerTestAccess {
public:
    static QJsonObject request(AutomationServer& server, const QJsonObject& request)
    { return server.handleLine(QJsonDocument(request).toJson(QJsonDocument::Compact), nullptr); }
};
class RadioModelSliceLifecycleTestAccess {
public:
    static void identity(RadioModel& radio, const QString& model, const QString& version)
    { radio.m_model = model; radio.m_version = version; emit radio.infoChanged(); }
    static void own(RadioModel& radio, bool owned)
    {
        if (owned) { radio.m_ownedSliceIds.insert(0); } else { radio.m_ownedSliceIds.remove(0); }
        emit radio.slotOccupancyChanged(0);
    }
    static void license(RadioModel& radio, const QString& enabled)
    {
        radio.onStatusReceived(QStringLiteral("license feature"), {{"name", "noise_reduction"},
            {"enabled", enabled}, {"reason", "LICENSE_FILE"}});
    }
    static void retire(RadioModel& radio)
    { radio.m_intentionalDisconnect = true; radio.onDisconnected(); }
    static void reclaim(RadioModel& radio)
    {
        radio.stageSessionModelsForReconnect();
        identity(radio, "FLEX-8600", "4.3.16.42034");
        own(radio, true);
        FlexBackend* previous = radio.m_flexBackend;
        radio.m_flexBackend = qobject_cast<FlexBackend*>(radio.backend());
        radio.handleSliceStatus(0, {{"in_use", "1"}, {"RF_frequency", "14.2"}, {"mode", "USB"}}, false);
        radio.m_flexBackend = previous;
    }
};
}
using namespace AetherSDR;
namespace {
int failures = 0;
void check(bool ok, const char* message)
{ std::printf("[%s] %s\n", ok ? "PASS" : "FAIL", message); failures += !ok; }
class ConnectedFlex final : public FlexBackend {
public:
    bool connected{true};
    bool isConnected() const override { return connected; }
};
const QMap<QString, QString> kStatus = {
    {"nr_delay", "25"}, {"nr_wlen", "256"}, {"nr_isdft_mode", "half"}, {"nr_adapt_mode", "auto"},
    {"anf_delay", "25"}, {"anf_wlen", "256"}, {"anf_isdft_mode", "half"}, {"anf_adapt_mode", "auto"},
    {"nrl_delay", "16"}, {"nrl_filter_size", "128"}, {"nrl_leakage_level", "50"},
    {"anfl_delay", "16"}, {"anfl_filter_size", "128"}, {"anfl_leakage_level", "0"},
    {"anft_min_db", "-110"}, {"anft_tone_db_diff", "12"}, {"nrf_wlen", "2048"}};
struct Fixture {
    RadioModel radio;
    ConnectedFlex* backend;
    QStringList commands;
    QStringList readbacks;
    QStringList traffic;
    std::vector<FlexBackend::CommandReply> replies;
    VfoWidget vfo;
    AutomationServer bridge;
    Fixture()
    {
        auto owned = std::make_unique<ConnectedFlex>(); backend = owned.get();
        radio.setBackendForTest(std::move(owned), "flex");
        backend->setModelProvider([this] { return radio.model(); });
        backend->setFirmwareVersionProvider([this] { return radio.softwareVersion(); });
        backend->setDspResetLicenseProvider([this]() -> std::optional<bool> {
            const LicenseFeatureState feature = radio.licenseFeature("noise_reduction");
            return feature.seen ? std::optional(feature.enabled) : std::nullopt;
        });
        backend->setSliceCommandSink([this](const QString& command) { commands.append(command); traffic.append(command); });
        installAnsweredWriter();
        RadioModelSliceLifecycleTestAccess::identity(radio, "FLEX-8600", "4.3.16.42034");
        backend->decodeSliceStatus(0, {{"pan", "0x40000000"}, {"mode", "USB"}, {"in_use", "1"}});
        RadioModelSliceLifecycleTestAccess::own(radio, true);
        RadioModelSliceLifecycleTestAccess::license(radio, "1");
        backend->decodeSliceStatus(0, kStatus);
        vfo.setRadioModel(&radio); vfo.setSlice(slice());
        bridge.setRadioModel(&radio);
        commands.clear(); readbacks.clear(); traffic.clear();
    }
    void installAnsweredWriter(const std::function<void()>& changed = {})
    {
        backend->setCommandSink(std::function<bool(const QString&, FlexBackend::CommandReply)>(
            [this, changed](const QString& command, FlexBackend::CommandReply reply) {
                traffic.append(command);
                if (command == QStringLiteral("sub slice all")) { readbacks.append(command); }
                else { commands.append(command); if (reply) { replies.push_back(std::move(reply)); } }
                if (changed) { changed(); }
                return true;
            }));
    }
    void acknowledge(int code = 0)
    {
        if (replies.empty()) { return; }
        FlexBackend::CommandReply reply = std::move(replies.front());
        replies.erase(replies.begin());
        reply(code, {});
        QApplication::processEvents();
    }
    SliceModel* slice() { return radio.slice(0); }
    QComboBox* algorithm() { return vfo.findChild<QComboBox*>("advancedDspAlgorithm"); }
    QWidget* editor(DspParameter parameter)
    { return vfo.findChild<QWidget*>(QString::fromLatin1(dspParameterSpec(parameter)->id) + "Editor"); }
    QPushButton* apply(DspParameter parameter)
    { return vfo.findChild<QPushButton*>(QString::fromLatin1(dspParameterSpec(parameter)->id) + "Apply"); }
    void choose(DspParameter parameter)
    { algorithm()->setCurrentIndex(algorithm()->findData(static_cast<int>(dspParameterSpec(parameter)->algorithm))); }
    void edit(DspParameter parameter, int value)
    {
        choose(parameter);
        if (auto* spin = qobject_cast<QSpinBox*>(editor(parameter))) { spin->setValue(value); }
        else if (auto* combo = qobject_cast<QComboBox*>(editor(parameter))) {
            combo->setCurrentIndex(combo->findData(value));
        }
    }
    QJsonObject invoke(const QString& target, const QString& action, const QJsonValue& value = {})
    {
        const QJsonObject result = AutomationServerTestAccess::request(bridge,
            {{"cmd", "invoke"}, {"target", target}, {"action", action}, {"value", value}});
        if (result.value("deferred").toBool()) { QTest::qWait(1); }
        return result;
    }
    QJsonObject snapshot()
    { return AutomationServerTestAccess::request(bridge, {{"cmd", "get"}, {"model", "slice"}, {"selector", "0"}}); }
};
void deliberateOneFieldAndReadback()
{
    Fixture f;
    check(f.commands.isEmpty(), "constructing and binding advanced controls sends nothing");
    check(f.slice()->dspParameters().size() == 17, "all 17 published advanced observations reach the model");
    f.vfo.show();
    for (QPushButton* button : f.vfo.findChildren<QPushButton*>()) {
        if (button->text() == "DSP") { button->click(); break; }
    }
    const QJsonObject before = f.snapshot();
    check(before.value("ok").toBool() && before.value("slice").toObject().value("advancedDsp").toObject().value("nrDelay").toInt() == 25,
        "production bridge reads confirmed advanced parameters");
    check(f.invoke("nrDelayEditor", "setValue", 26).value("ok").toBool() && f.commands.isEmpty(),
        "bridge edit alone sends no command");
    check(f.invoke("nrDelayApply", "click").value("ok").toBool()
        && f.commands == QStringList{"slice set 0 nr_delay=26"}, "Apply requests only the selected field once");
    check(f.slice()->dspParameter(DspParameter::NrDelay) == 25
        && qobject_cast<QSpinBox*>(f.editor(DspParameter::NrDelay))->value() == 25,
        "request does not masquerade as confirmed model or displayed state");
    f.acknowledge();
    check(f.readbacks == QStringList{"sub slice all"} && f.commands.size() == 1
        && f.slice()->dspParameter(DspParameter::NrDelay) == 25,
        "successful reply requests one readback without claiming a confirmed value");
    f.backend->decodeSliceStatus(0, {{"nr_delay", "26"}});
    check(f.slice()->dspParameter(DspParameter::NrDelay) == 26
        && qobject_cast<QSpinBox*>(f.editor(DspParameter::NrDelay))->value() == 26
        && f.commands.size() == 1, "separate status converges model and editor without feedback");
    f.commands.clear();
    for (const DspParameterSpec& spec : dspParameterSpecs()) {
        f.choose(spec.parameter);
        const int current = *f.slice()->dspParameter(spec.parameter);
        const int next = !spec.choices.isEmpty() ? (current + 1) % spec.choices.size()
            : spec.powerOfTwo ? (current == spec.minimum ? spec.minimum * 2 : spec.minimum)
            : current == spec.maximum ? spec.minimum : current + 1;
        f.edit(spec.parameter, next);
        check(f.apply(spec.parameter)->isEnabled() && f.commands.isEmpty(), "every algorithm offers an explicit one-field Apply");
        f.apply(spec.parameter)->click();
        check(f.commands.size() == 1 && f.slice()->dspParameter(spec.parameter) == current,
            "all parameter editors preserve confirmed observations while requesting");
        f.commands.clear();
    }
}
void profileResetAndStaleEdits()
{
    Fixture f;
    using P = DspParameter;
    f.edit(P::NrDelay, 26);
    QMetaObject::invokeMethod(f.apply(P::NrDelay), "pressed", Qt::DirectConnection);
    f.backend->decodeSliceStatus(0, {{"nr_delay", "25"}});
    QMetaObject::invokeMethod(f.apply(P::NrDelay), "clicked", Qt::DirectConnection, Q_ARG(bool, false));
    check(f.commands.isEmpty() && !f.apply(P::NrDelay)->isEnabled(), "same-value profile/reset echo retires an in-flight edit");
    f.edit(P::NrDelay, 26);
    QMetaObject::invokeMethod(f.apply(P::NrDelay), "pressed", Qt::DirectConnection);
    RadioModelSliceLifecycleTestAccess::license(f.radio, "0");
    QMetaObject::invokeMethod(f.apply(P::NrDelay), "clicked", Qt::DirectConnection, Q_ARG(bool, false));
    check(f.commands.isEmpty() && !f.editor(P::NrDelay)->isEnabled()
        && !f.editor(P::NrDelay)->accessibleDescription().isEmpty(), "license denial clears drafts and states an accessible reason");
    RadioModelSliceLifecycleTestAccess::license(f.radio, "1");
    f.apply(P::NrDelay)->click();
    check(f.commands.isEmpty(), "renewed entitlement does not replay a discarded edit");
    f.edit(P::NrDelay, 26);
    QMetaObject::invokeMethod(f.apply(P::NrDelay), "pressed", Qt::DirectConnection);
    f.vfo.setSlice(nullptr); f.vfo.setSlice(f.slice());
    QMetaObject::invokeMethod(f.apply(P::NrDelay), "clicked", Qt::DirectConnection, Q_ARG(bool, false));
    check(f.commands.isEmpty(), "slice rebind discards a pressed intent even for the same object");
    f.edit(P::NrDelay, 26);
    QMetaObject::invokeMethod(f.apply(P::NrDelay), "pressed", Qt::DirectConnection);
    f.vfo.setRadioModel(nullptr); f.vfo.setRadioModel(&f.radio);
    QMetaObject::invokeMethod(f.apply(P::NrDelay), "clicked", Qt::DirectConnection, Q_ARG(bool, false));
    check(f.commands.isEmpty(), "radio context rebind discards a pressed intent");
    f.edit(P::NrDelay, 26);
    QMetaObject::invokeMethod(f.apply(P::NrDelay), "pressed", Qt::DirectConnection);
    RadioModelSliceLifecycleTestAccess::retire(f.radio);
    RadioModelSliceLifecycleTestAccess::reclaim(f.radio);
    RadioModelSliceLifecycleTestAccess::license(f.radio, "1");
    f.backend->decodeSliceStatus(0, kStatus);
    QMetaObject::invokeMethod(f.apply(P::NrDelay), "clicked", Qt::DirectConnection, Q_ARG(bool, false));
    check(f.commands.isEmpty(), "session reclaim cannot revive an old press");
    f.edit(P::NrDelay, 27); f.apply(P::NrDelay)->click();
    check(f.commands == QStringList{"slice set 0 nr_delay=27"}, "fresh deliberate edit works after session reclaim");
    f.commands.clear();
    f.backend->decodeSliceStatus(0, {{"nr_delay", "41"}, {"anft_min_db", "-120"}, {"anft_tone_db_diff", "14"}});
    check(f.slice()->dspParameter(P::NrDelay) == 41 && f.slice()->dspParameter(P::AnftMinDb) == -120
        && f.commands.isEmpty(), "profile/reset reported values update independently without commands");
}
void visibilityMalformedSelectionAndKeyboard()
{
    Fixture f;
    using P = DspParameter;
    f.edit(P::NrWindow, 128);
    auto* combo = qobject_cast<QComboBox*>(f.editor(P::NrWindow));
    combo->setCurrentIndex(-1);
    f.apply(P::NrWindow)->click();
    check(f.commands.isEmpty(), "clearing a parameter choice cancels the pending edit");
    RadioModelSliceLifecycleTestAccess::identity(f.radio, "FLEX-6700", "4.3.16.42034");
    f.choose(P::NrfWindow);
    check(f.algorithm()->currentText() == "NRF" && !f.editor(P::NrfWindow)->isEnabled()
        && f.editor(P::NrfWindow)->accessibleDescription().contains("model", Qt::CaseInsensitive),
        "6700 can inspect a visibly unavailable NRF group with a hardware reason");
    f.choose(P::NrDelay);
    check(f.editor(P::NrDelay)->isEnabled(), "6700 retains entitled advanced NR controls");
    f.vfo.resize(260, 700); f.vfo.show();
    for (QPushButton* button : f.vfo.findChildren<QPushButton*>()) {
        if (button->text() == "DSP") { button->click(); break; }
    }
    auto* spin = qobject_cast<QSpinBox*>(f.editor(P::NrDelay));
    spin->setFocus();
    QTest::keyClick(spin, Qt::Key_Up);
    check(f.commands.isEmpty() && f.apply(P::NrDelay)->isEnabled(), "keyboard adjustment also waits for Apply");
    QTest::keyClick(f.apply(P::NrDelay), Qt::Key_Space);
    check(f.commands.size() == 1, "keyboard activation applies one explicit edit");
    f.commands.clear();
    RadioModelSliceLifecycleTestAccess::identity(f.radio, "FLEX-6700", "4.2.20.41343");
    check(!f.editor(P::NrDelay)->isEnabled() && !f.apply(P::NrDelay)->isEnabled(),
        "legacy firmware advanced editing stays disabled; no numeric enum mapping is invented");
    f.vfo.hide();
}
void lifetimeAndDynamicLayout()
{
    DspAdvancedControls surviving;
    auto radio = std::make_unique<RadioModel>();
    surviving.setContext(radio.get(), nullptr);
    radio.reset();
    check(!surviving.findChild<QSpinBox*>("nrDelayEditor")->isEnabled(),
        "parameter editor survives radio destruction with a retired availability registry");
    surviving.setContext(nullptr, nullptr);
    Fixture f;
    f.vfo.show();
    for (QPushButton* button : f.vfo.findChildren<QPushButton*>()) {
        if (button->text() == "DSP") { button->click(); break; }
    }
    QWidget* controls = f.vfo.findChild<QWidget*>("advancedDspControls");
    QWidget* page = controls->parentWidget();
    auto* stack = qobject_cast<QStackedWidget*>(page->parentWidget());
    check(stack, "advanced controls belong to the existing VFO tab stack");
    RadioModelSliceLifecycleTestAccess::license(f.radio, "0");
    QApplication::processEvents();
    const int needed = qMax(page->layout()->minimumSize().height(), page->layout()->sizeHint().height());
    check(stack && stack->height() >= needed,
        "late availability reason fits the open fixed-height DSP page");
    RadioModelSliceLifecycleTestAccess::license(f.radio, "1");
    QApplication::processEvents();
    const int settled = qMax(page->layout()->minimumSize().height(), page->layout()->sizeHint().height());
    std::printf("LAYOUT stack=%d needed=%d settled=%d\n", stack ? stack->height() : -1, needed, settled);
    check(stack && stack->height() == settled,
        "confirmed availability shrinks the page without reopening the tab");
    f.vfo.hide();
}
void reentrantPresentationObservers()
{
    Fixture f;
    QPointer<DspAdvancedControls> controls = new DspAdvancedControls;
    controls->setContext(&f.radio, f.slice());
    auto* spin = controls->findChild<QSpinBox*>("nrDelayEditor");
    auto* button = controls->findChild<QPushButton*>("nrDelayApply");
    spin->setValue(26);
    const QPointer<DspAdvancedControls> first = controls;
    QObject::connect(controls, &DspAdvancedControls::contentChanged, controls, [first] { delete first.data(); });
    button->click();
    check(!controls && f.commands.size() == 1,
        "synchronous presentation observer can delete controls after a deliberate request");
    delete controls.data();
    controls = new DspAdvancedControls;
    controls->setContext(&f.radio, f.slice());
    spin = controls->findChild<QSpinBox*>("nrDelayEditor");
    button = controls->findChild<QPushButton*>("nrDelayApply");
    spin->setValue(28);
    bool rebound = false;
    QObject::connect(controls, &DspAdvancedControls::contentChanged, controls, [&] {
        if (!rebound) { rebound = true; controls->setContext(nullptr, nullptr); }
    });
    button->click();
    check(rebound && controls->findChild<QLabel*>("advancedDspNotice")->text().startsWith("Values follow"),
        "a reentrant rebind cannot receive the previous binding's request notice");
    delete controls.data();
    controls = new DspAdvancedControls;
    controls->setContext(&f.radio, f.slice());
    spin = controls->findChild<QSpinBox*>("nrDelayEditor");
    button = controls->findChild<QPushButton*>("nrDelayApply");
    spin->setValue(29);
    f.backend->setCommandSink(std::function<bool(const QString&, FlexBackend::CommandReply)>(
        [&](const QString& command, FlexBackend::CommandReply) {
            f.commands.append(command); delete controls.data(); return true;
        }));
    button->click();
    check(!controls, "dispatch-side destruction cannot reach presentation updates afterward");
    delete controls.data();
    f.backend->setCommandSink(std::function<bool(const QString&, FlexBackend::CommandReply)>{});
}
int bridgeFixture(QApplication& app)
{
    Fixture f;
    QWidget window;
    window.setObjectName("advancedDspFixture");
    window.setWindowTitle("Advanced DSP — injected client proof (no radio)");
    auto* layout = new QVBoxLayout(&window);
    auto* title = new QLabel("Injected radio status · real VFO/model/Flex encoder · no radio or RF");
    title->setWordWrap(true); layout->addWidget(title); layout->addWidget(&f.vfo);
    auto* log = new QLabel("[]"); log->setObjectName("fixtureCommandLog"); log->setWordWrap(true); layout->addWidget(log);
    f.installAnsweredWriter([&] { log->setText(f.traffic.join("\n")); });
    f.backend->setSliceCommandSink([&](const QString& command) {
        f.commands.append(command); f.traffic.append(command); log->setText(f.traffic.join("\n"));
    });
    auto add = [&](const QString& id, const QString& text, const std::function<void()>& action) {
        auto* button = new QPushButton(text); button->setObjectName(id); layout->addWidget(button);
        QObject::connect(button, &QPushButton::clicked, &window, action);
    };
    add("fixtureAcknowledge", "Fixture: acknowledge parameter request", [&] { f.acknowledge(); });
    add("fixtureConfirm", "Fixture: report NR delay 26", [&] { f.backend->decodeSliceStatus(0, {{"nr_delay", "26"}}); });
    add("fixtureProfile", "Fixture: recall DSP profile", [&] {
        f.backend->decodeSliceStatus(0, {{"nr_delay", "41"}, {"nr_wlen", "128"}, {"nr_adapt_mode", "slow"}});
    });
    add("fixtureResetEcho", "Fixture: report reset values", [&] { f.backend->decodeSliceStatus(0, kStatus); });
    add("fixtureDenyLicense", "Fixture: deny entitlement", [&] { RadioModelSliceLifecycleTestAccess::license(f.radio, "0"); });
    add("fixtureAllowLicense", "Fixture: confirm entitlement", [&] { RadioModelSliceLifecycleTestAccess::license(f.radio, "1"); });
    add("fixture6700", "Fixture: FLEX-6700", [&] { RadioModelSliceLifecycleTestAccess::identity(f.radio, "FLEX-6700", "4.3.16.42034"); });
    add("fixtureDisconnect", "Fixture: disconnect", [&] { f.backend->connected = false; emit f.radio.connectionStateChanged(false); });
    window.show();
    for (QPushButton* button : f.vfo.findChildren<QPushButton*>()) {
        if (button->text() == "DSP") { button->click(); break; }
    }
    if (!f.bridge.start("aethersdr-advanced-dsp-fixture")) { f.vfo.setParent(nullptr); return 77; }
    std::printf("ADVANCED_DSP_FIXTURE_SOCKET=%s\n", qPrintable(f.bridge.fullServerName())); std::fflush(stdout);
    const int result = app.exec(); f.vfo.setParent(nullptr); return result;
}
}
int main(int argc, char** argv)
{
    std::setvbuf(stdout, nullptr, _IONBF, 0);
    TestSettingsProfile profile("dsp-advanced-parameters");
    QApplication app(argc, argv);
    if (app.arguments().contains("--bridge-fixture")) { return bridgeFixture(app); }
    deliberateOneFieldAndReadback(); profileResetAndStaleEdits(); visibilityMalformedSelectionAndKeyboard(); lifetimeAndDynamicLayout(); reentrantPresentationObservers();
    return failures ? 1 : 0;
}
