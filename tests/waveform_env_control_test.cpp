// SmartSDR 4.3 waveform container environments (#6947): request validation, the
// Flex capability and wire text, `env=` / `waveform restarted` decode, the
// RadioModel apply sequence and the Environment editor. Recording command sinks
// and injected status only: no sockets, radio or TX.
#include "TestSettingsProfile.h"
#include "RadioSetupControlFixture.h"
#include "core/backends/flex/CommandParser.h"
#include "gui/WaveformEnvironmentDialog.h"
#include "models/FlexWaveformModel.h"

#include <QApplication>
#include <QCheckBox>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLineEdit>
#include <QPushButton>
#include <QSignalSpy>
#include <QTest>

#include <cstdio>
#include <optional>

namespace AetherSDR {
class WaveformEnvTestAccess {
public:
    static void confirmTimeout(RadioModel& radio, int ms) { radio.m_waveformEnvConfirmTimeoutMs = ms; }
};
} // namespace AetherSDR

using namespace AetherSDR;

namespace {
int failures = 0;
void check(bool ok, const char* claim)
{
    std::printf("[%s] %s\n", ok ? "PASS" : "FAIL", claim);
    failures += !ok;
}

using Kvs = QMap<QString, QString>;
using Action = WaveformEnvRequest::Action;
using Outcome = WaveformEnvApplyResult::Outcome;

// The padded base64 of {"API_KEY":"s3cret value","MODE":"fast"}.
const QString kEnvB64 = QStringLiteral("eyJBUElfS0VZIjoiczNjcmV0IHZhbHVlIiwiTU9ERSI6ImZhc3QifQ==");

QString b64(const QByteArray& json) { return QString::fromLatin1(json.toBase64()); }

void status(RadioModel& radio, const QString& object, const Kvs& kvs)
{
    QMetaObject::invokeMethod(&radio, "onStatusReceived", Qt::DirectConnection,
                              Q_ARG(QString, object), Q_ARG(Kvs, kvs));
}

void requestValidation()
{
    check(WaveformEnvRequest{Action::Clear, {}, {}, {}}.valid(), "a bare clear is valid");
    check(WaveformEnvRequest{Action::Stage, QStringLiteral("API_KEY"), QStringLiteral("a b = c"), {}}.valid()
              && WaveformEnvRequest{Action::Stage, QStringLiteral("_x9"), QString(), {}}.valid(),
          "a POSIX name with a spaced, '='-bearing or empty value stages");
    for (const QString& key : {QString(), QStringLiteral("9LIVES"), QStringLiteral("A B"),
                               QStringLiteral("A=B"), QStringLiteral("A-B"), QString(65, QLatin1Char('A'))}) {
        check(!WaveformEnvRequest{Action::Stage, key, QStringLiteral("v"), {}}.valid(),
              "an empty, digit-led, spaced, '='/'-' bearing or overlong name is refused");
    }
    for (const QString& value : {QStringLiteral("a\nwaveform remove_container x"), QStringLiteral("a\rb"),
                                 QStringLiteral("a\tb"), QString(513, QLatin1Char('v'))}) {
        check(!WaveformEnvRequest{Action::Stage, QStringLiteral("K"), value, {}}.valid(),
              "a value with a line break or control character, or over 512 chars, is refused");
    }
    check(WaveformEnvRequest{Action::Recreate, {}, {}, QStringLiteral("wf-x.1_a")}.valid()
              && !WaveformEnvRequest{Action::Recreate, {}, {}, QStringLiteral("wf x")}.valid()
              && !WaveformEnvRequest{Action::Recreate, {}, {}, QString()}.valid(),
          "recreate takes the waveform name token only");
    check(!WaveformEnvRequest{Action::Clear, QStringLiteral("K"), {}, {}}.valid()
              && !WaveformEnvRequest{Action::Recreate, QStringLiteral("K"), {}, QStringLiteral("x")}.valid(),
          "fields that do not belong to the action are refused");
}

void flexDeclarationAndEncoding()
{
    FlexBackend flex;
    QString model = QStringLiteral("FLEX-8600");
    QString firmware;
    QStringList commands;
    flex.setModelProvider([&] { return model; });
    flex.setFirmwareVersionProvider([&] { return firmware; });
    flex.setSetupCommandSink([&](const QString& command, const RadioSetupCompletion&) {
        commands.append(command);
        return true;
    });
    for (const QString& version : {QString(), QStringLiteral("4.2.20.41343"),
                                   QStringLiteral("4.3.15"), QStringLiteral("4.3.16beta")}) {
        firmware = version;
        check(!flex.capabilities().waveformEnvControl,
              "absent, older and malformed firmware declare no container environments");
    }
    check(flex.requestWaveformEnv({Action::Clear, {}, {}, {}}) == ReceiveDispatch::Unsupported
              && commands.isEmpty(),
          "without the capability nothing is sent");
    firmware = QStringLiteral("4.3.16.42034");
    check(flex.capabilities().waveformEnvControl.has_value(), "FLEX-8600 on 4.3.16 declares it");
    for (const QString& other : {QStringLiteral("FLEX-6700"), QStringLiteral("FLEX-6600M"), QString()}) {
        model = other;
        check(!flex.capabilities().waveformEnvControl,
              "6000-series (no Waveform Processor) and unknown models declare none");
    }
    model = QStringLiteral("AU-520M");
    check(flex.capabilities().waveformEnvControl.has_value(), "Aurora declares it");

    flex.requestWaveformEnv({Action::Clear, {}, {}, {}});
    flex.requestWaveformEnv({Action::Stage, QStringLiteral("CALL"), QStringLiteral("KK7GWY portable"), {}});
    flex.requestWaveformEnv({Action::Recreate, {}, {}, QStringLiteral("wf-x")});
    check(commands == QStringList{QStringLiteral("waveform env clear"),
                                  QStringLiteral("waveform env CALL=KK7GWY portable"),
                                  QStringLiteral("waveform recreate wf-x")},
          "FlexLib's three verbs, the value carried with its spaces");
    commands.clear();
    check(flex.requestWaveformEnv({Action::Stage, QStringLiteral("K"), QStringLiteral("a\nb"), {}})
                  == ReceiveDispatch::Unsupported
              && commands.isEmpty(),
          "an invalid request sends nothing");
}

void statusDecode()
{
    // The CommandParser splits a kv on the FIRST '=', so base64 padding survives.
    const ParsedMessage msg = CommandParser::parseLine(
        QStringLiteral("S0A1B2C3D|waveform container name=wf-x version=1.0 env=") + kEnvB64);
    check(msg.object == QLatin1String("waveform container")
              && msg.kvs.value(QStringLiteral("env")) == kEnvB64,
          "the env token keeps its '=' padding through the status parser");

    FlexWaveformModel model;
    QSignalSpy listed(&model, &FlexWaveformModel::waveformsChanged);
    QSignalSpy environment(&model, &FlexWaveformModel::environmentChanged);
    model.handleContainerStatus(msg.kvs);
    const auto entry = [&model]() -> FlexWaveformEntry { return model.waveforms().value(0); };
    check(entry().environment == QMap<QString, QString>{{QStringLiteral("API_KEY"), QStringLiteral("s3cret value")},
                                                         {QStringLiteral("MODE"), QStringLiteral("fast")}}
              && listed.size() == 1 && environment.isEmpty(),
          "a new container arrives with its decoded environment");

    Kvs kvs = msg.kvs;
    kvs[QStringLiteral("env")] = b64(R"({"MODE":"slow"})");
    model.handleContainerStatus(kvs);
    check(entry().environment.value(QStringLiteral("MODE")) == QLatin1String("slow")
              && entry().environment.size() == 1 && listed.size() == 1 && environment.size() == 1
              && environment.at(0).at(0).toString() == QLatin1String("wf-x"),
          "an env-only update replaces the environment without rebuilding the list");

    model.handleContainerStatus(kvs);
    check(environment.size() == 1 && listed.size() == 1, "an unchanged status announces nothing");

    kvs.remove(QStringLiteral("env"));
    model.handleContainerStatus(kvs);
    check(entry().environment.isEmpty() && environment.size() == 2,
          "a status without env= is an empty environment, as in FlexLib");

    kvs[QStringLiteral("env")] = b64(R"({"N":5,"B":true})");
    model.handleContainerStatus(kvs);
    check(entry().environment.value(QStringLiteral("N")) == QLatin1String("5")
              && entry().environment.value(QStringLiteral("B")) == QLatin1String("true"),
          "scalar values are taken as their text");

    for (const QString& bad : {QStringLiteral("not*base64"), b64("[1,2]"), b64("{\"A\":{\"B\":1}}"),
                               b64("{\"A\":"), b64("\"x\"")}) {
        kvs[QStringLiteral("env")] = bad;
        model.handleContainerStatus(kvs);
        check(entry().environment.isEmpty(), "malformed base64, non-object or nested JSON is empty");
    }

    kvs[QStringLiteral("version")] = QStringLiteral("2.0");
    kvs[QStringLiteral("env")] = b64(R"({"A":"1"})");
    model.handleContainerStatus(kvs);
    check(listed.size() == 2 && entry().version == QLatin1String("2.0"),
          "a version change still rebuilds the list");

    QSignalSpy restarted(&model, &FlexWaveformModel::restarted);
    model.handleRestarted({{QStringLiteral("name"), QStringLiteral("wf-x")}, {QStringLiteral("result"), QStringLiteral("ok")}});
    model.handleRestarted({{QStringLiteral("name"), QStringLiteral("wf-x")}, {QStringLiteral("result"), QStringLiteral("ERROR")}});
    model.handleRestarted({{QStringLiteral("name"), QStringLiteral("wf-x")}});
    model.handleRestarted({{QStringLiteral("result"), QStringLiteral("error")}});
    check(restarted.size() == 3 && restarted.at(0).at(1).toBool() && !restarted.at(1).at(1).toBool()
              && restarted.at(2).at(1).toBool(),
          "restarted: only result=error fails, a missing result succeeds, no name is ignored");
}

struct ApplyRig {
    RadioModel radio;
    RadioSetupTestBackend* backend{nullptr};
    ApplyRig()
    {
        backend = installRadioSetupBackend(radio);
        backend->setModelProvider([] { return QStringLiteral("FLEX-8600"); });
        backend->setFirmwareVersionProvider([] { return QStringLiteral("4.3.16.42034"); });
        status(radio, QStringLiteral("waveform container"),
               {{QStringLiteral("name"), QStringLiteral("wf-x")}, {QStringLiteral("version"), QStringLiteral("1")},
                {QStringLiteral("env"), b64(R"({"OLD":"1"})")}});
    }
    void accept() { backend->replyLast({RadioSetupReply::Kind::Accepted, 0, {}}); }
};

void applySequence()
{
    {
        ApplyRig rig;
        std::optional<WaveformEnvApplyResult> result;
        const bool started = rig.radio.applyWaveformEnvironment(QStringLiteral("wf-x"),
            {{QStringLiteral("API_KEY"), QStringLiteral("s3cret value")}, {QStringLiteral("MODE"), QStringLiteral("fast")}},
            nullptr, [&](const WaveformEnvApplyResult& r) { result = r; });
        check(started && rig.radio.waveformEnvironmentApplyActive()
                  && rig.backend->commands == QStringList{QStringLiteral("waveform env clear")},
              "an apply starts with a clear and waits for its reply");
        check(!rig.radio.applyWaveformEnvironment(QStringLiteral("wf-x"), {}, nullptr, {}),
              "a second apply is refused while one is in flight");
        rig.accept();
        rig.accept();
        rig.accept();
        check(rig.backend->commands == QStringList{QStringLiteral("waveform env clear"),
                                                   QStringLiteral("waveform env API_KEY=s3cret value"),
                                                   QStringLiteral("waveform env MODE=fast"),
                                                   QStringLiteral("waveform recreate wf-x")},
              "each accepted step sends the next, ending with the recreate");
        rig.accept();
        check(!result && rig.radio.waveformEnvironmentApplyActive(),
              "an accepted recreate still waits for the radio's restarted status");
        status(rig.radio, QStringLiteral("waveform restarted"),
               {{QStringLiteral("name"), QStringLiteral("other")}, {QStringLiteral("result"), QStringLiteral("ok")}});
        check(!result, "another waveform's restart is not this one");
        status(rig.radio, QStringLiteral("waveform restarted"),
               {{QStringLiteral("name"), QStringLiteral("wf-x")}, {QStringLiteral("result"), QStringLiteral("ok")}});
        check(result && result->outcome == Outcome::Recreated && !rig.radio.waveformEnvironmentApplyActive()
                  && rig.backend->commands.last() == QLatin1String("waveform env clear"),
              "result=ok finishes Recreated and clears the stage so it cannot reach a later deploy");
        check(rig.radio.flexWaveformModel().waveforms().value(0).environment.value(QStringLiteral("OLD"))
                  == QLatin1String("1"),
              "nothing local claims the new environment: only the radio's next status will");
    }
    {
        ApplyRig rig;
        std::optional<WaveformEnvApplyResult> result;
        rig.radio.applyWaveformEnvironment(QStringLiteral("wf-x"), {{QStringLiteral("K"), QStringLiteral("v")}},
                                           nullptr, [&](const WaveformEnvApplyResult& r) { result = r; });
        rig.accept();
        rig.backend->replyLast({RadioSetupReply::Kind::Failed, 0x50000015, QStringLiteral("Invalid value")});
        check(result && result->outcome == Outcome::Refused
                  && result->message.contains(QLatin1String("variable K"))
                  && !result->message.contains(QLatin1String("=v"))
                  && !rig.backend->commands.contains(QStringLiteral("waveform recreate wf-x"))
                  && rig.backend->commands.last() == QLatin1String("waveform env clear"),
              "a refused stage stops before the recreate, names the key not the value, and clears");
    }
    {
        ApplyRig rig;
        std::optional<WaveformEnvApplyResult> result;
        rig.radio.applyWaveformEnvironment(QStringLiteral("wf-x"), {}, nullptr,
                                           [&](const WaveformEnvApplyResult& r) { result = r; });
        rig.accept();
        check(rig.backend->commands == QStringList{QStringLiteral("waveform env clear"),
                                                   QStringLiteral("waveform recreate wf-x")},
              "an empty environment clears and recreates");
        // The status may beat the command reply.
        status(rig.radio, QStringLiteral("waveform restarted"),
               {{QStringLiteral("name"), QStringLiteral("wf-x")}, {QStringLiteral("result"), QStringLiteral("error")}});
        check(result && result->outcome == Outcome::RecreateFailed, "result=error finishes RecreateFailed");
        rig.accept();
        check(result->outcome == Outcome::RecreateFailed, "the late reply changes nothing");
    }
    {
        ApplyRig rig;
        WaveformEnvTestAccess::confirmTimeout(rig.radio, 20);
        std::optional<WaveformEnvApplyResult> result;
        rig.radio.applyWaveformEnvironment(QStringLiteral("wf-x"), {}, nullptr,
                                           [&](const WaveformEnvApplyResult& r) { result = r; });
        rig.accept();
        rig.accept();
        QTest::qWait(80);
        check(result && result->outcome == Outcome::NoConfirmation
                  && rig.backend->commands.last() == QLatin1String("waveform env clear")
                  && !rig.radio.waveformEnvironmentApplyActive(),
              "no restarted status in time finishes NoConfirmation and still clears");
    }
    {
        ApplyRig rig;
        for (const WaveformEnvironment& bad : {
                 WaveformEnvironment{{QStringLiteral("A"), QStringLiteral("1")}, {QStringLiteral("A"), QStringLiteral("2")}},
                 WaveformEnvironment{{QStringLiteral("A B"), QStringLiteral("1")}},
                 WaveformEnvironment{{QStringLiteral("A"), QStringLiteral("1\n2")}}}) {
            check(!rig.radio.applyWaveformEnvironment(QStringLiteral("wf-x"), bad, nullptr, {})
                      && rig.backend->commands.isEmpty() && !rig.radio.waveformEnvironmentApplyActive(),
                  "a duplicate, invalid name or multi-line value is refused before anything is sent");
        }
        check(!rig.radio.applyWaveformEnvironment(QStringLiteral("wf x"), {}, nullptr, {}),
              "an unsafe container name is refused");
    }
    {
        ApplyRig rig;
        rig.backend->setFirmwareVersionProvider([] { return QStringLiteral("4.2.20.41343"); });
        check(!rig.radio.applyWaveformEnvironment(QStringLiteral("wf-x"), {}, nullptr, {})
                  && rig.backend->commands.isEmpty(),
              "older firmware refuses the apply and sends nothing");
    }
    {
        ApplyRig rig;
        std::optional<WaveformEnvApplyResult> result;
        rig.radio.applyWaveformEnvironment(QStringLiteral("wf-x"), {}, nullptr,
                                           [&](const WaveformEnvApplyResult& r) { result = r; });
        rig.accept();
        rig.accept();
        rig.backend->live = false;
        RadioSetupControlTestAccess::disconnected(rig.radio);
        check(result && result->outcome == Outcome::Cancelled && !rig.radio.waveformEnvironmentApplyActive(),
              "a session that ends while waiting finishes Cancelled");
    }
}

void editor()
{
    WaveformEnvironment collected;
    check(WaveformEnvironmentDialog::collectEnvironment(
              {{QStringLiteral(" API_KEY "), QStringLiteral("x y")}, {QString(), QString()}}, &collected).isEmpty()
              && collected == WaveformEnvironment{{QStringLiteral("API_KEY"), QStringLiteral("x y")}},
          "the editor trims names and skips blank rows");
    check(!WaveformEnvironmentDialog::collectEnvironment({{QString(), QStringLiteral("v")}}, nullptr).isEmpty()
              && !WaveformEnvironmentDialog::collectEnvironment(
                      {{QStringLiteral("A"), {}}, {QStringLiteral("A"), {}}}, nullptr).isEmpty()
              && !WaveformEnvironmentDialog::collectEnvironment({{QStringLiteral("1A"), {}}}, nullptr).isEmpty(),
          "a value without a name, a duplicate or an invalid name gets a reason");

    ApplyRig rig;
    WaveformEnvironmentDialog dialog(&rig.radio, QStringLiteral("wf-x"));
    const auto edits = [&dialog] { return dialog.findChildren<QLineEdit*>(); };
    check(edits().size() == 2 && edits().at(0)->text() == QLatin1String("OLD")
              && edits().at(1)->text() == QLatin1String("1"),
          "the rows start from the environment the radio reports");
    check(edits().at(1)->echoMode() == QLineEdit::Password, "values are masked by default");
    QCheckBox* show = dialog.findChild<QCheckBox*>();
    show->setChecked(true);
    check(edits().at(1)->echoMode() == QLineEdit::Normal, "Show values reveals them");

    QPushButton* apply = nullptr;
    for (QPushButton* button : dialog.findChildren<QPushButton*>()) {
        if (button->objectName() == QLatin1String("wfEnvApply")) { apply = button; }
    }
    check(apply && apply->isEnabled(), "Apply & Recreate is available on 4.3.16");
    std::optional<WaveformEnvironment> handed;
    dialog.setApplyHandler([&](const QString& name, const WaveformEnvironment& environment) {
        handed = environment;
        return name == QLatin1String("wf-x");
    });
    edits().at(1)->setText(QStringLiteral("2"));
    apply->click();
    check(handed && *handed == WaveformEnvironment{{QStringLiteral("OLD"), QStringLiteral("2")}}
              && !apply->isEnabled() && !edits().at(0)->isEnabled(),
          "Apply hands over the edited rows and locks the editor while the radio works");
    status(rig.radio, QStringLiteral("waveform container"),
           {{QStringLiteral("name"), QStringLiteral("wf-x")}, {QStringLiteral("version"), QStringLiteral("1")},
            {QStringLiteral("env"), b64(R"({"OLD":"2","NEW":"3"})")}});
    dialog.showApplyResult({Outcome::Recreated, {}});
    QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
    check(edits().size() == 4 && apply->isEnabled(),
          "after Recreated the rows show what the radio now reports");

    rig.backend->setFirmwareVersionProvider([] { return QStringLiteral("4.2.20.41343"); });
    emit rig.radio.capabilitiesChanged(true, rig.radio.backendCapabilities());
    check(!apply->isEnabled() && !apply->accessibleDescription().isEmpty()
              && apply->accessibleDescription() == apply->toolTip(),
          "on older firmware Apply is dimmed with the reason in its tooltip and accessible description");
}
} // namespace

int main(int argc, char** argv)
{
    TestSettingsProfile settings(QStringLiteral("waveform-env-control-test"));
    QApplication app(argc, argv);
    requestValidation();
    flexDeclarationAndEncoding();
    statusDecode();
    applySequence();
    editor();
    std::printf("%s (%d failure%s)\n", failures ? "FAILED" : "PASSED", failures, failures == 1 ? "" : "s");
    return failures ? 1 : 0;
}
