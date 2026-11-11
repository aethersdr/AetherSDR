// SmartSDR 4.3 waveform container environments (#6947): request validation, the
// Flex capability and wire text, `env=` / `waveform restarted` decode, the
// RadioModel apply sequence and the Environment editor. Recording command sinks
// and injected status only: no sockets, radio or TX.
#include "TestSettingsProfile.h"
#include "RadioSetupControlFixture.h"
#include "AutomationSensitiveLineEdit.h"
#include "core/backends/flex/CommandParser.h"
#include "core/TailnetShimClient.h"
#include "gui/CanonMessageBox.h"
#include "gui/TailnetShimDialog.h"
#include "gui/WaveformEnvironmentDialog.h"
#include "models/FlexWaveformModel.h"

#include <QApplication>
#include <QCheckBox>
#include <QImage>
#include <QLabel>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLineEdit>
#include <QPushButton>
#include <QSignalSpy>
#include <QTest>
#include <QTimer>

#include <cstdio>
#include <optional>

namespace AetherSDR {
class WaveformEnvTestAccess {
public:
    static void confirmTimeout(RadioModel& radio, int ms) { radio.m_waveformEnvConfirmTimeoutMs = ms; }
    static void serial(RadioModel& radio, const QString& serial) { radio.m_lastInfo.serial = serial; }
    static void publish(RadioModel& radio) { radio.publishCapabilities(radio.isConnected()); }
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
        container(R"({"OLD":"1"})");
    }
    // The measured replies (FLEX-8600 4.3.16): R|0 with a message per verb.
    void accept() { backend->replyLast({RadioSetupReply::Kind::Accepted, 0, {}}); }
    void reply(qsizetype index, RadioSetupReply::Kind kind = RadioSetupReply::Kind::Accepted,
               const QString& message = {})
    {
        const RadioSetupCompletion callback = backend->replies.at(index);
        callback({kind, kind == RadioSetupReply::Kind::Accepted ? 0 : 0x50000015, message});
    }
    void container(const QByteArray& json, const QString& name = QStringLiteral("wf-x"))
    {
        Kvs kvs{{QStringLiteral("name"), name}, {QStringLiteral("version"), QStringLiteral("1")}};
        if (!json.isEmpty()) { kvs.insert(QStringLiteral("env"), b64(json)); }
        status(radio, QStringLiteral("waveform container"), kvs);
    }
    void restarted(const QString& result = QStringLiteral("ok"))
    {
        status(radio, QStringLiteral("waveform restarted"),
               {{QStringLiteral("name"), QStringLiteral("wf-x")}, {QStringLiteral("result"), result}});
    }
};

// A capable session also subscribes to the radio's NTP sources (#6953); these
// checks count the environment commands only.
QStringList envCommands(const ApplyRig& rig)
{
    QStringList commands;
    for (const QString& command : rig.backend->commands) {
        if (!command.startsWith(QLatin1String("sub ntp_sources"))) { commands.append(command); }
    }
    return commands;
}

using Result = std::optional<WaveformEnvApplyResult>;
auto into(Result& result) { return [&result](const WaveformEnvApplyResult& r) { result = r; }; }

void applySequence()
{
    const QString clear = QStringLiteral("waveform env clear");
    {
        // The measured order: `restarted` arrives before the recreate's reply,
        // the next container line still carries the OLD environment, and the new
        // one arrives in a later broadcast.
        ApplyRig rig;
        Result result;
        QSignalSpy staging(&rig.radio, &RadioModel::waveformEnvironmentStagingChanged);
        const bool started = rig.radio.applyWaveformEnvironment(QStringLiteral("wf-x"),
            {{QStringLiteral("API_KEY"), QStringLiteral("s3cret value")}, {QStringLiteral("MODE"), QStringLiteral("fast")}},
            nullptr, into(result));
        check(started && rig.radio.waveformEnvironmentApplyActive() && rig.radio.waveformEnvironmentStagingUncertain()
                  && staging.size() == 1 && rig.backend->commands == QStringList{clear},
              "an apply starts with a clear, marks staging in use and waits for the reply");
        check(waveformEnvironmentInstallBlocker(&rig.radio).contains(QLatin1String("Wait for")),
              "Docker installs wait while an apply is in flight");
        check(!rig.radio.applyWaveformEnvironment(QStringLiteral("wf-x"), {{QStringLiteral("A"), {}}}, nullptr, {}),
              "a second apply is refused while one is in flight");
        rig.accept();
        rig.accept();
        rig.accept();
        check(rig.backend->commands == QStringList{clear, QStringLiteral("waveform env API_KEY=s3cret value"),
                                                   QStringLiteral("waveform env MODE=fast"),
                                                   QStringLiteral("waveform recreate wf-x")},
              "each accepted step sends the next, ending with the recreate");
        const qsizetype recreateReply = rig.backend->replies.size() - 1;
        rig.restarted();
        check(!result && rig.backend->commands.size() == 5 && rig.backend->commands.last() == clear,
              "restarted ok sends the acknowledged cleanup clear and does not finish yet");
        rig.reply(recreateReply);
        rig.container(R"({"OLD":"1"})");
        check(!result, "the stale container line after the restart does not finish the apply");
        rig.accept();
        check(!result && rig.radio.waveformEnvironmentApplyActive(),
              "an acknowledged cleanup still waits for the new environment");
        rig.container(R"({"API_KEY":"s3cret value","MODE":"fast"})");
        check(result && result->outcome == Outcome::Recreated && result->environmentConfirmed
                  && result->stagingCleared && !rig.radio.waveformEnvironmentApplyActive()
                  && !rig.radio.waveformEnvironmentStagingUncertain(),
              "the broadcast carrying the requested set finishes Recreated, confirmed and cleaned up");
    }
    {
        ApplyRig rig;
        WaveformEnvTestAccess::confirmTimeout(rig.radio, 20);
        Result result;
        rig.radio.applyWaveformEnvironment(QStringLiteral("wf-x"), {{QStringLiteral("K"), QStringLiteral("v")}},
                                           nullptr, into(result));
        rig.accept(); rig.accept(); rig.accept();
        rig.restarted();
        rig.accept();
        rig.container(R"({"OLD":"1"})");
        QTest::qWait(80);
        check(result && result->outcome == Outcome::Recreated && !result->environmentConfirmed
                  && result->stagingCleared,
              "if the new environment never arrives the result says recreated but unconfirmed");
    }
    {
        ApplyRig rig;
        Result result;
        rig.radio.applyWaveformEnvironment(QStringLiteral("wf-x"), {{QStringLiteral("K"), QStringLiteral("v")}},
                                           nullptr, into(result));
        rig.accept(); rig.accept(); rig.accept();
        rig.restarted();
        rig.container(R"({"K":"v"})");
        rig.backend->replyLast({RadioSetupReply::Kind::Failed, 0x50000015, QStringLiteral("busy")});
        check(!waveformEnvironmentInstallBlocker(&rig.radio).isEmpty(),
              "Docker installs stay blocked after a cleanup that was not acknowledged");
        check(result && result->outcome == Outcome::Recreated && result->environmentConfirmed
                  && !result->stagingCleared && result->stagingMessage == QLatin1String("busy")
                  && !rig.radio.waveformEnvironmentApplyActive() && rig.radio.waveformEnvironmentStagingUncertain(),
              "a refused cleanup reports recreated-but-not-cleaned and keeps staging uncertain");
        Result again;
        check(!rig.radio.applyWaveformEnvironment(QStringLiteral("wf-x"), {}, nullptr, into(again)),
              "an empty environment is refused: the radio would keep the old one");
        std::optional<RadioControlReply> cleared;
        check(rig.radio.clearWaveformEnvironmentStaging(nullptr, [&](const RadioControlReply& r) { cleared = r; })
                  && rig.backend->commands.last() == clear && rig.radio.waveformEnvironmentStagingUncertain(),
              "Clear Staged Variables sends a clear and stays uncertain until it is acknowledged");
        check(rig.radio.waveformEnvironmentStagingUncertain()
                  && waveformEnvironmentInstallBlocker(&rig.radio).contains(QLatin1String("Clear Staged Variables")),
              "until then Docker installs stay blocked and say how to clear it");
        rig.accept();
        check(cleared && cleared->kind == RadioControlReply::Kind::Accepted
                  && !rig.radio.waveformEnvironmentStagingUncertain(),
              "an acknowledged clear makes staging known clear again");
        check(waveformEnvironmentInstallBlocker(&rig.radio).isEmpty(), "and installs are allowed again");
    }
    {
        ApplyRig rig;
        Result result;
        rig.radio.applyWaveformEnvironment(QStringLiteral("wf-x"), {{QStringLiteral("K"), QStringLiteral("v")}},
                                           nullptr, into(result));
        rig.accept(); rig.accept(); rig.accept();
        rig.backend->accept = false;  // the sink refuses the cleanup outright
        rig.restarted();
        rig.container(R"({"K":"v"})");
        check(result && result->outcome == Outcome::Recreated && !result->stagingCleared
                  && rig.radio.waveformEnvironmentStagingUncertain(),
              "a cleanup that cannot be dispatched is reported, not assumed");
    }
    {
        ApplyRig rig;
        Result result;
        rig.radio.applyWaveformEnvironment(QStringLiteral("wf-x"), {{QStringLiteral("K"), QStringLiteral("v")}},
                                           nullptr, into(result));
        rig.accept();
        rig.backend->replyLast({RadioSetupReply::Kind::Failed, 0x50000015, QStringLiteral("Invalid value")});
        check(!result && rig.backend->commands.last() == clear
                  && !rig.backend->commands.contains(QStringLiteral("waveform recreate wf-x")),
              "a refused stage stops before the recreate and sends the cleanup");
        rig.accept();
        check(result && result->outcome == Outcome::Refused && result->stagingCleared
                  && result->message.contains(QLatin1String("variable K"))
                  && !result->message.contains(QLatin1String("=v")) && !rig.radio.waveformEnvironmentStagingUncertain(),
              "it then finishes Refused, naming the key and not the value, with staging cleared");
    }
    {
        ApplyRig rig;
        Result result;
        rig.radio.applyWaveformEnvironment(QStringLiteral("wf-x"), {{QStringLiteral("K"), QString()}},
                                           nullptr, into(result));
        rig.accept(); rig.accept();
        check(rig.backend->commands.at(1) == QLatin1String("waveform env K="),
              "an empty value stages as K= (the way to empty a variable)");
        rig.restarted(QStringLiteral("error"));
        rig.accept();
        check(result && result->outcome == Outcome::RecreateFailed && result->stagingCleared,
              "result=error finishes RecreateFailed after the cleanup");
    }
    {
        ApplyRig rig;
        WaveformEnvTestAccess::confirmTimeout(rig.radio, 20);
        Result result;
        rig.radio.applyWaveformEnvironment(QStringLiteral("wf-x"), {{QStringLiteral("K"), QStringLiteral("v")}},
                                           nullptr, into(result));
        rig.accept(); rig.accept(); rig.accept();
        QTest::qWait(80);
        check(!result && rig.backend->commands.last() == clear,
              "no restarted status in time sends the cleanup");
        rig.accept();
        check(result && result->outcome == Outcome::NoConfirmation && result->stagingCleared,
              "and finishes NoConfirmation once it is acknowledged");
    }
    {
        ApplyRig rig;
        for (const WaveformEnvironment& bad : {
                 WaveformEnvironment{},
                 WaveformEnvironment{{QStringLiteral("A"), QStringLiteral("1")}, {QStringLiteral("A"), QStringLiteral("2")}},
                 WaveformEnvironment{{QStringLiteral("A B"), QStringLiteral("1")}},
                 WaveformEnvironment{{QStringLiteral("A"), QStringLiteral("1\n2")}},
                 WaveformEnvironment{{QStringLiteral("A"), QStringLiteral("a  b")}},
                 WaveformEnvironment{{QStringLiteral("A"), QStringLiteral(" a")}},
                 WaveformEnvironment{{QStringLiteral("A"), QString(513, QLatin1Char('v'))}}}) {
            check(!rig.radio.applyWaveformEnvironment(QStringLiteral("wf-x"), bad, nullptr, {})
                      && rig.backend->commands.isEmpty() && !rig.radio.waveformEnvironmentApplyActive()
                      && !rig.radio.waveformEnvironmentStagingUncertain(),
                  "an empty, duplicate, invalid, multi-line, spaced or overlong set is refused before anything is sent");
        }
        check(!rig.radio.applyWaveformEnvironment(QStringLiteral("wf x"), {{QStringLiteral("A"), {}}}, nullptr, {}),
              "an unsafe container name is refused");
    }
    {
        ApplyRig rig;
        rig.backend->setFirmwareVersionProvider([] { return QStringLiteral("4.2.20.41343"); });
        check(!rig.radio.applyWaveformEnvironment(QStringLiteral("wf-x"), {{QStringLiteral("A"), {}}}, nullptr, {})
                  && rig.backend->commands.isEmpty(),
              "older firmware refuses the apply and sends nothing");
    }
    {
        ApplyRig rig;
        Result result;
        rig.radio.applyWaveformEnvironment(QStringLiteral("wf-x"), {{QStringLiteral("A"), {}}}, nullptr, into(result));
        rig.accept();
        rig.accept();
        rig.backend->live = false;
        RadioSetupControlTestAccess::disconnected(rig.radio);
        check(result && result->outcome == Outcome::Cancelled && !result->stagingCleared
                  && !rig.radio.waveformEnvironmentApplyActive() && !rig.radio.waveformEnvironmentStagingUncertain(),
              "a session that ends mid-apply finishes Cancelled; with no radio connected nothing is blocked");
        rig.backend->live = true;
        check(rig.radio.waveformEnvironmentStagingUncertain(),
              "back on the same radio its staging is uncertain");
    }
}

void recreateReplyTimeout()
{
    {
        // fencedSetupCompletion's local 30 s timeout is not the radio's answer.
        ApplyRig rig;
        Result result;
        rig.radio.applyWaveformEnvironment(QStringLiteral("wf-x"), {{QStringLiteral("K"), QStringLiteral("v")}},
                                           nullptr, into(result));
        rig.accept(); rig.accept();
        rig.backend->replyLast({RadioSetupReply::Kind::Failed, -1, QStringLiteral("no radio response")});
        check(!result && rig.radio.waveformEnvironmentApplyActive()
                  && rig.backend->commands.last() == QLatin1String("waveform recreate wf-x"),
              "a local reply timeout on the recreate keeps waiting for restarted");
        rig.restarted();
        rig.accept();
        rig.container(R"({"K":"v"})");
        check(result && result->outcome == Outcome::Recreated && result->environmentConfirmed,
              "and the radio's restarted status still finishes it Recreated");
    }
    {
        ApplyRig rig;
        WaveformEnvTestAccess::confirmTimeout(rig.radio, 20);
        Result result;
        rig.radio.applyWaveformEnvironment(QStringLiteral("wf-x"), {{QStringLiteral("K"), QStringLiteral("v")}},
                                           nullptr, into(result));
        rig.accept(); rig.accept();
        rig.backend->replyLast({RadioSetupReply::Kind::Failed, -1, QStringLiteral("no radio response")});
        QTest::qWait(80);
        rig.accept();
        check(result && result->outcome == Outcome::NoConfirmation,
              "with no restarted either, the NoConfirmation timer decides");
    }
    {
        ApplyRig rig;
        Result result;
        rig.radio.applyWaveformEnvironment(QStringLiteral("wf-x"), {{QStringLiteral("K"), QStringLiteral("v")}},
                                           nullptr, into(result));
        rig.accept(); rig.accept();
        rig.backend->replyLast({RadioSetupReply::Kind::Failed, 0x50000015, QStringLiteral("no such waveform")});
        rig.accept();
        check(result && result->outcome == Outcome::Refused && result->message.contains(QLatin1String("recreate")),
              "a genuine refusal of the recreate still ends the apply");
    }
}

void stagingFollowsTheRadio()
{
    const QString clear = QStringLiteral("waveform env clear");
    const auto leaveUncertain = [](ApplyRig& rig) {
        rig.radio.applyWaveformEnvironment(QStringLiteral("wf-x"), {{QStringLiteral("K"), QStringLiteral("v")}},
                                           nullptr, {});
        rig.accept(); rig.accept(); rig.accept();
        rig.restarted();
        rig.container(R"({"K":"v"})");
        rig.backend->replyLast({RadioSetupReply::Kind::Failed, 0x50000015, QStringLiteral("busy")});
    };
    const auto reconnect = [](ApplyRig& rig, const QString& serial) {
        rig.backend->live = false;
        RadioSetupControlTestAccess::disconnected(rig.radio);
        WaveformEnvTestAccess::serial(rig.radio, serial);
        RadioSetupControlTestAccess::replaceConnectionGeneration(rig.radio);
        RadioSetupControlTestAccess::closing(rig.radio, false);  // onConnected's edge
        rig.backend->live = true;
        WaveformEnvTestAccess::publish(rig.radio);
    };
    {
        ApplyRig rig;
        WaveformEnvTestAccess::serial(rig.radio, QStringLiteral("8600-A"));
        leaveUncertain(rig);
        check(rig.radio.waveformEnvironmentStagingUncertain() && !waveformEnvironmentInstallBlocker(&rig.radio).isEmpty(),
              "an unacknowledged cleanup blocks installs on that radio");
        const qsizetype before = envCommands(rig).size();
        reconnect(rig, QStringLiteral("8600-A"));
        WaveformEnvTestAccess::publish(rig.radio);
        check(envCommands(rig).size() == before + 1 && envCommands(rig).last() == clear,
              "a new session to the same radio sends exactly one clear, however often capabilities publish");
        rig.accept();
        check(!rig.radio.waveformEnvironmentStagingUncertain() && waveformEnvironmentInstallBlocker(&rig.radio).isEmpty(),
              "an accepted automatic clear releases the block");
    }
    {
        ApplyRig rig;
        WaveformEnvTestAccess::serial(rig.radio, QStringLiteral("8600-A"));
        leaveUncertain(rig);
        reconnect(rig, QStringLiteral("8600-A"));
        rig.backend->replyLast({RadioSetupReply::Kind::Failed, 0x50000015, QStringLiteral("busy")});
        check(rig.radio.waveformEnvironmentStagingUncertain() && !waveformEnvironmentInstallBlocker(&rig.radio).isEmpty(),
              "a refused automatic clear keeps the block (Clear Staged Variables stays the manual path)");
        std::optional<RadioControlReply> cleared;
        rig.radio.clearWaveformEnvironmentStaging(nullptr, [&](const RadioControlReply& r) { cleared = r; });
        rig.accept();
        check(cleared && !rig.radio.waveformEnvironmentStagingUncertain(), "the manual clear still releases it");
    }
    {
        ApplyRig rig;
        WaveformEnvTestAccess::serial(rig.radio, QStringLiteral("8600-A"));
        leaveUncertain(rig);
        const qsizetype before = envCommands(rig).size();
        reconnect(rig, QStringLiteral("8400-B"));
        check(envCommands(rig).size() == before && !rig.radio.waveformEnvironmentStagingUncertain()
                  && waveformEnvironmentInstallBlocker(&rig.radio).isEmpty(),
              "a different radio is neither blocked nor sent a clear");
        reconnect(rig, QStringLiteral("8600-A"));
        check(envCommands(rig).size() == before + 1 && envCommands(rig).last() == clear,
              "returning to the first radio still clears it");
    }
}

QPushButton* buttonNamed(QWidget& root, const QString& objectName)
{
    for (QPushButton* button : root.findChildren<QPushButton*>()) {
        if (button->objectName() == objectName) { return button; }
    }
    return nullptr;
}
QPushButton* buttonText(QWidget& root, const QString& text)
{
    for (QPushButton* button : root.findChildren<QPushButton*>()) {
        if (button->text() == text) { return button; }
    }
    return nullptr;
}
QList<QLineEdit*> liveEdits(QWidget& root)
{
    QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
    return root.findChildren<QLineEdit*>();
}
QLineEdit* editNamed(QWidget& root, const QString& accessibleName)
{
    for (QLineEdit* edit : liveEdits(root)) {
        if (edit->accessibleName() == accessibleName) { return edit; }
    }
    return nullptr;
}

void editorFollowsModelState()
{
    {
        // An apply this editor did not start dims it and freezes its rows.
        ApplyRig rig;
        WaveformEnvironmentDialog other(&rig.radio, QStringLiteral("wf-x"));
        QPushButton* apply = buttonNamed(other, QStringLiteral("wfEnvApply"));
        rig.radio.applyWaveformEnvironment(QStringLiteral("wf-x"), {{QStringLiteral("NEW"), QStringLiteral("2")}},
                                           nullptr, {});
        QLineEdit* old = editNamed(other, QStringLiteral("Value of OLD"));
        bool reasonShown = false;
        for (QLabel* label : other.findChildren<QLabel*>()) {
            if (label->text().contains(QLatin1String("Another environment change"))) { reasonShown = true; }
        }
        check(!apply->isEnabled() && old && !old->isEnabled() && reasonShown
                  && apply->accessibleDescription().contains(QLatin1String("Another environment change")),
              "another editor's apply dims this one and says why");
        rig.accept(); rig.accept(); rig.accept();
        rig.restarted();
        rig.container(R"({"OLD":"1","TRANSIENT":"x"})");
        check(editNamed(other, QStringLiteral("Value of OLD")) && !editNamed(other, QStringLiteral("Value of TRANSIENT")),
              "it does not reload from mid-recreate statuses");
        rig.container(R"({"NEW":"2"})");
        rig.accept();
        bool stale = false;
        for (QLabel* label : other.findChildren<QLabel*>()) {
            if (label->isVisibleTo(&other) && label->text().contains(QLatin1String("Another environment change"))) {
                stale = true;
            }
        }
        check(editNamed(other, QStringLiteral("Value of NEW")) && apply->isEnabled() && !stale,
              "when the apply ends it catches up with the radio, re-enables and drops the reason");
    }
    {
        // The container disappears for a moment: edits survive.
        ApplyRig rig;
        WaveformEnvironmentDialog dialog(&rig.radio, QStringLiteral("wf-x"));
        QPushButton* apply = buttonNamed(dialog, QStringLiteral("wfEnvApply"));
        editNamed(dialog, QStringLiteral("Value of OLD"))->setText(QStringLiteral("typed"));
        status(rig.radio, QStringLiteral("waveform container"),
               {{QStringLiteral("name"), QStringLiteral("wf-x")}, {QStringLiteral("version"), QStringLiteral("1")},
                {QStringLiteral("removed"), QString()}});
        QLineEdit* old = editNamed(dialog, QStringLiteral("Value of OLD"));
        bool absentShown = false;
        for (QLabel* label : dialog.findChildren<QLabel*>()) {
            if (label->text().contains(QLatin1String("not installed on the radio right now"))) { absentShown = true; }
        }
        check(old && old->text() == QLatin1String("typed") && !old->isEnabled() && !apply->isEnabled() && absentShown,
              "while the container is absent the edited rows stay, dimmed, with the reason");
        rig.container(R"({"OLD":"1"})");
        old = editNamed(dialog, QStringLiteral("Value of OLD"));
        bool stale = false;
        for (QLabel* label : dialog.findChildren<QLabel*>()) {
            if (label->isVisibleTo(&dialog) && label->text().contains(QLatin1String("not installed"))) { stale = true; }
        }
        check(old && old->text() == QLatin1String("typed") && old->isEnabled() && apply->isEnabled() && !stale,
              "when it returns the typed value is still there and the absence message is gone");
    }
    {
        // A firmware reason that goes away takes its message with it.
        ApplyRig rig;
        QString firmware = QStringLiteral("4.2.20.41343");
        rig.backend->setFirmwareVersionProvider([&firmware] { return firmware; });
        WaveformEnvironmentDialog dialog(&rig.radio, QStringLiteral("wf-x"));
        const auto messageShown = [&dialog](const char* fragment) {
            for (QLabel* label : dialog.findChildren<QLabel*>()) {
                if (label->isVisibleTo(&dialog) && label->text().contains(QLatin1String(fragment))) { return true; }
            }
            return false;
        };
        dialog.show();
        QCoreApplication::processEvents();
        check(messageShown("SmartSDR v4.3.16"), "older firmware shows the reason");
        firmware = QStringLiteral("4.3.16.42034");
        WaveformEnvTestAccess::publish(rig.radio);
        check(!messageShown("SmartSDR v4.3.16") && buttonNamed(dialog, QStringLiteral("wfEnvApply"))->isEnabled(),
              "when the reason goes away its message is cleared");
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
    check(WaveformEnvironmentDialog::collectEnvironment({{QStringLiteral("A"), QStringLiteral("a  b")}}, nullptr)
                  .contains(QLatin1String("two spaces"))
              && WaveformEnvironmentDialog::collectEnvironment({{QStringLiteral("A"), QStringLiteral("a ")}}, nullptr)
                  .contains(QLatin1String("starts or ends with a space"))
              && WaveformEnvironmentDialog::collectEnvironment({{QStringLiteral("A"), QStringLiteral("a\tb")}}, nullptr)
                  .contains(QLatin1String("one line")),
          "repeated, edge and control whitespace are refused with a reason, never altered");
    check(WaveformEnvironmentDialog::collectEnvironment({}, nullptr)
              == WaveformEnvironmentDialog::emptyEnvironmentReason(),
          "an empty list is refused with the keeps-the-old-variables reason");
    check(!WaveformEnvRequest::keyValid(QStringLiteral("clear")) && !WaveformEnvRequest::keyValid(QStringLiteral("CLEAR"))
              && !WaveformEnvRequest::keyValid(QStringLiteral("Clear")) && WaveformEnvRequest::keyValid(QStringLiteral("CLEAR_X"))
              && WaveformEnvironmentDialog::collectEnvironment({{QStringLiteral("Clear"), QStringLiteral("1")}}, nullptr)
                     .contains(QLatin1String("reserves")),
          "the name clear is reserved in any case, with a reason in the editor");

    // A value and a name longer than AetherSDR sends, as another client could
    // have set them: shown intact, never shortened, refused on Apply.
    const QString longValue(600, QLatin1Char('t'));
    const QString longKey = QStringLiteral("K") + QString(69, QLatin1Char('x'));
    ApplyRig rig;
    rig.container(QJsonDocument(QJsonObject{{QStringLiteral("OLD"), QStringLiteral("1")},
                                            {QStringLiteral("TOKEN"), longValue},
                                            {longKey, QStringLiteral("v")}}).toJson(QJsonDocument::Compact));
    WaveformEnvironmentDialog dialog(&rig.radio, QStringLiteral("wf-x"));
    std::optional<WaveformEnvironment> handed;
    dialog.setApplyHandler([&](const QString&, const WaveformEnvironment& environment) {
        handed = environment;
        return true;
    });
    QPushButton* apply = buttonNamed(dialog, QStringLiteral("wfEnvApply"));
    QLineEdit* token = editNamed(dialog, QStringLiteral("Value of TOKEN"));
    QLineEdit* longName = editNamed(dialog, QStringLiteral("Name of ") + longKey);
    check(token && token->text() == longValue && longName && longName->text() == longKey,
          "a long reported value and name load intact");
    editNamed(dialog, QStringLiteral("Value of OLD"))->setText(QStringLiteral("2"));
    apply->click();
    check(!handed && token->text() == longValue
              && dialog.findChildren<QLabel*>().size() > 0,
          "editing another row and applying refuses visibly and leaves the long value unchanged");
    bool reasonShown = false;
    for (QLabel* label : dialog.findChildren<QLabel*>()) {
        if (label->isVisibleTo(&dialog) && label->text().contains(QLatin1String("characters"))) { reasonShown = true; }
    }
    check(reasonShown, "the length refusal is shown on the dialog");

    // Values are plain text on screen, yet every value edit is marked sensitive.
    const auto allSensitive = [&dialog] {
        bool all = true;
        int values = 0;
        for (QLineEdit* edit : liveEdits(dialog)) {
            if (edit->accessibleName().startsWith(QLatin1String("Value of"))) {
                ++values;
                all = all && edit->property("aetherSensitiveValue").toBool()
                      && automationLineEditValue(edit) != edit->text();
            }
        }
        return all && values > 0;
    };
    check(token->echoMode() == QLineEdit::Normal && allSensitive()
              && automationLineEditValue(token) == QLatin1String("<hidden>"),
          "values are shown as plain text, yet the automation dump hides every one");
    check(dialog.findChild<QCheckBox*>() == nullptr, "there is no Show values switch");
    dialog.show();
    QCoreApplication::processEvents();
    const QImage revealed = token->grab().toImage();
    QImage masked;
    {
        AutomationSensitiveGrabMask mask(&dialog);
        check(token->echoMode() == QLineEdit::Password, "an automation grab masks a plain-text value");
        masked = token->grab().toImage();
    }
    check(!masked.isNull() && masked != revealed && token->echoMode() == QLineEdit::Normal,
          "the grab's pixels differ from the shown text and plain display is restored after");

    // Added rows: sensitive too, with an accessible name that does not move.
    buttonText(dialog, QStringLiteral("Add Variable"))->click();
    QCoreApplication::processEvents();
    QLineEdit* newName = editNamed(dialog, QStringLiteral("Name of new variable 1"));
    QLineEdit* newValue = editNamed(dialog, QStringLiteral("Value of new variable 1"));
    check(newName && newValue && newValue->property("aetherSensitiveValue").toBool()
              && newValue->echoMode() == QLineEdit::Normal,
          "an added row's value is plain text and marked sensitive");
    newName->setText(QStringLiteral("AE_APP"));
    check(editNamed(dialog, QStringLiteral("Value of new variable 1")) == newValue
              && newValue->accessibleDescription().contains(QLatin1String("AE_APP")),
          "typing the name keeps the row's accessible name and updates its description");

    // Remove every row: Apply is dimmed and says why, visibly and accessibly.
    while (QPushButton* remove = [&dialog]() -> QPushButton* {
               for (QPushButton* b : dialog.findChildren<QPushButton*>()) {
                   if (b->text() == QLatin1String("Remove") && b->isVisibleTo(&dialog)) { return b; }
               }
               return nullptr;
           }()) {
        remove->click();
        QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
    }
    bool hintShown = false;
    for (QLabel* label : dialog.findChildren<QLabel*>()) {
        if (label->isVisibleTo(&dialog) && label->text() == WaveformEnvironmentDialog::emptyEnvironmentReason()) {
            hintShown = true;
        }
    }
    check(!apply->isEnabled() && hintShown
              && apply->accessibleDescription() == WaveformEnvironmentDialog::emptyEnvironmentReason()
              && apply->toolTip() == apply->accessibleDescription(),
          "with no rows Apply is dimmed and the empty-staging reason is shown and announced");

    // Revert reloads the radio's rows, still sensitive.
    buttonText(dialog, QStringLiteral("Revert"))->click();
    check(editNamed(dialog, QStringLiteral("Value of TOKEN"))
              && editNamed(dialog, QStringLiteral("Value of TOKEN"))->property("aetherSensitiveValue").toBool()
              && editNamed(dialog, QStringLiteral("Value of TOKEN"))->text() == longValue,
          "reloaded rows are sensitive and intact");

    // A recreate whose new environment has not arrived yet is not shown as confirmed.
    WaveformEnvApplyResult unconfirmed;
    unconfirmed.outcome = Outcome::Recreated;
    unconfirmed.stagingCleared = true;
    dialog.showApplyResult(unconfirmed);
    bool saysUnconfirmed = false;
    for (QLabel* label : dialog.findChildren<QLabel*>()) {
        if (label->text().contains(QLatin1String("has not yet reported"))) { saysUnconfirmed = true; }
    }
    check(saysUnconfirmed, "recreated without the new environment says so instead of claiming it");

    WaveformEnvApplyResult notCleaned;
    notCleaned.outcome = Outcome::Recreated;
    notCleaned.environmentConfirmed = true;
    notCleaned.stagingMessage = QStringLiteral("busy");
    dialog.showApplyResult(notCleaned);
    bool saysCleanup = false;
    for (QLabel* label : dialog.findChildren<QLabel*>()) {
        if (label->text().contains(QLatin1String("Clearing the radio's staged variables failed (busy)"))) {
            saysCleanup = true;
        }
    }
    check(saysCleanup, "a failed cleanup is reported separately from the recreate");

    // While staging is uncertain the editor offers Clear Staged Variables.
    rig.radio.applyWaveformEnvironment(QStringLiteral("wf-x"), {{QStringLiteral("A"), {}}}, nullptr, {});
    rig.backend->live = false;
    RadioSetupControlTestAccess::disconnected(rig.radio);
    rig.backend->live = true;
    rig.backend->accept = false;  // the reconnect's automatic clear is refused
    WaveformEnvTestAccess::publish(rig.radio);
    rig.backend->accept = true;
    QPushButton* clearStaging = buttonText(dialog, QStringLiteral("Clear Staged Variables"));
    check(rig.radio.waveformEnvironmentStagingUncertain() && clearStaging && clearStaging->isVisibleTo(&dialog),
          "uncertain staging shows Clear Staged Variables");

    editorFollowsModelState();

    ApplyRig older;
    older.backend->setFirmwareVersionProvider([] { return QStringLiteral("4.2.20.41343"); });
    WaveformEnvironmentDialog old(&older.radio, QStringLiteral("wf-x"));
    QPushButton* oldApply = buttonNamed(old, QStringLiteral("wfEnvApply"));
    check(!oldApply->isEnabled() && !oldApply->accessibleDescription().isEmpty()
              && oldApply->accessibleDescription() == oldApply->toolTip(),
          "on older firmware Apply is dimmed with the reason in its tooltip and accessible description");
}
// Recreating the tailnet shim's container asks first, and Cancel is the default.
void shimRecreateAsks()
{
    const QString shim = QString::fromLatin1(kTailnetShimContainerName);
    for (const bool confirm : {false, true}) {
        ApplyRig rig;
        rig.container(R"({"OLD":"1"})", shim);
        WaveformEnvironmentDialog dialog(&rig.radio, shim);
        int handed = 0;
        dialog.setApplyHandler([&](const QString&, const WaveformEnvironment&) { ++handed; return true; });
        QString question;
        bool cancelDefault = false;
        QTimer::singleShot(0, [&] {
            for (QWidget* w : QApplication::topLevelWidgets()) {
                auto* box = qobject_cast<CanonMessageBox*>(w);
                if (!box || !box->isVisible()) { continue; }
                for (QLabel* label : box->findChildren<QLabel*>()) { question += label->text() + QLatin1Char('\n'); }
                for (QPushButton* b : box->findChildren<QPushButton*>()) {
                    if (b->text() == QLatin1String("Cancel")) { cancelDefault = b->isDefault(); }
                    if (b->text() == (confirm ? QLatin1String("Recreate") : QLatin1String("Cancel"))) { b->click(); }
                }
            }
        });
        buttonNamed(dialog, QStringLiteral("wfEnvApply"))->click();
        check(question.contains(QLatin1String("off your tailnet"))
                  && question.contains(TailnetShimDialog::offTailnetWarning()) && cancelDefault,
              "recreating the shim's container asks, names the tailnet and defaults to Cancel");
        check(handed == (confirm ? 1 : 0), confirm ? "Recreate in the question applies"
                                                   : "Cancel in the question recreates nothing");
    }
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
    recreateReplyTimeout();
    stagingFollowsTheRadio();
    editor();
    shimRecreateAsks();
    std::printf("%s (%d failure%s)\n", failures ? "FAILED" : "PASSED", failures, failures == 1 ? "" : "s");
    return failures ? 1 : 0;
}
