// SmartSDR 4.3 NTP time sources (#6953): validation, the Flex capability and
// wire text, `ntp_sources` status decode, RadioModel's subscription, list and
// remove-and-create edit, and the Radio Setup Time page. Recording command sinks
// and injected status only: no sockets, radio or TX.
#include "TestSettingsProfile.h"
#include "RadioSetupControlFixture.h"
#include "core/AudioEngine.h"
#include "core/QsoRecorder.h"
#include "core/AutomationServer.h"
#include "core/backends/flex/CommandParser.h"
#include "core/backends/flex/RadioConnection.h"
#include "gui/NtpSourcesPage.h"

#include <QApplication>
#include <QByteArray>
#include <QJsonArray>
#include <QJsonObject>
#include <QLabel>
#include <QLineEdit>
#include <QPushButton>
#include <QSignalSpy>
#include <QTabBar>
#include <QCheckBox>
#include <QGroupBox>

#include <cstdio>
#include <optional>

namespace AetherSDR {
class AutomationServerTestAccess {
public:
    static QJsonObject request(AutomationServer& server, const QByteArray& line)
    {
        return server.handleLine(line, nullptr);
    }
};

class NtpSourcesTestAccess {
public:
    static void gpsdo(RadioModel& radio, bool present, const QString& status, bool locked = false) {
        radio.m_gpsdoPresent = present;
        radio.m_gpsStatus = status;
        radio.m_oscLocked = locked;
        emit radio.oscillatorChanged();
    }
    static void publish(RadioModel& radio) { radio.publishCapabilities(radio.isConnected()); }
    static void apply(RadioModel& radio, const NtpSourceDelta& d) { radio.applyNtpSourceChanges(d); }
    // Through the production transport signal and status ingress, tagged with
    // the live session's epoch, or (previousSession) with the epoch a new
    // connect has just retired.
    static void message(RadioModel& radio, const QString& line, bool previousSession = false)
    {
        if (previousSession) { radio.beginStatusSession(); }
        emit radio.connection()->lineReceived(CommandParser::parseLine(line),
                                              radio.m_statusEpoch - (previousSession ? 1 : 0));
        QCoreApplication::sendPostedEvents();
    }
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

using Action = NtpSourceRequest::Action;
using Outcome = RadioModel::NtpSourceReplaceResult::Outcome;

// Measured on a FLEX-8600, 4.3.16.42034, after `sub ntp_sources all`.
const QString kMeasured = QStringLiteral(
    "S76C604D3|ntp_sources id=default-pool address=2.debian.pool.ntp.org enabled=1 directive=pool "
    "selected=1 prefer=0 state=selected reach=255 description=\"Debian NTP pool\"");

NtpSourceDefinition def(const QString& id, const QString& address = QStringLiteral("time.nist.gov"),
                        const QString& directive = QStringLiteral("server"),
                        const QString& description = {}, bool prefer = false)
{
    return {id, address, directive, description, prefer};
}

void validation()
{
    check(NtpSourceRules::idValid(QStringLiteral("a")) && NtpSourceRules::idValid(QStringLiteral("nist-1"))
              && NtpSourceRules::idValid(QString(64, QLatin1Char('a'))),
          "ids of letters, digits and inner hyphens up to 64 characters are valid");
    for (const QString& bad : {QString(), QStringLiteral("-a"), QStringLiteral("a-"), QStringLiteral("a b"),
                               QStringLiteral("a_b"), QStringLiteral("a\"b"), QStringLiteral("a\n"),
                               QString(65, QLatin1Char('a'))}) {
        check(!NtpSourceRules::idValid(bad),
              "an empty, edge-hyphen, spaced, underscored, quoted, newline-ended or long id is refused");
    }
    check(NtpSourceRules::addressValid(QStringLiteral("2.debian.pool.ntp.org"))
              && NtpSourceRules::addressValid(QStringLiteral("fe80::1%eth0"))
              && NtpSourceRules::addressValid(QStringLiteral("192.168.50.1")),
          "host names, IPv4 and scoped IPv6 addresses are valid");
    for (const QString& bad : {QString(), QStringLiteral("a b"), QStringLiteral("a\nsub x"), QStringLiteral("a\n"),
                               QStringLiteral("a=b"), QString(254, QLatin1Char('a'))}) {
        check(!NtpSourceRules::addressValid(bad),
              "an empty, spaced, multi-line, newline-ended, '=' or overlong address is refused");
    }
    check(NtpSourceRules::descriptionValid(QString()) && NtpSourceRules::descriptionValid(QStringLiteral("Debian NTP pool"))
              && NtpSourceRules::descriptionValid(QString(62, QLatin1Char('d'))),
          "an empty or spaced description up to 62 characters is valid");
    for (const QString& bad : {QStringLiteral("a\"b"), QStringLiteral("a=b"), QStringLiteral("Shack GPS\n"),
                               QString(63, QLatin1Char('d'))}) {
        check(!NtpSourceRules::descriptionValid(bad),
              "a quoted, '=', newline-ended or 63-character description is refused");
    }
    check(NtpSourceRules::directiveValid(QStringLiteral("server")) && NtpSourceRules::directiveValid(QStringLiteral("pool"))
              && NtpSourceRules::directiveValid(QStringLiteral("peer"))
              && !NtpSourceRules::directiveValid(QStringLiteral("Server"))
              && !NtpSourceRules::directiveValid(QStringLiteral("refclock")),
          "only server, pool and peer, lowercase, are directives");
    check(NtpSourceRequest{Action::Subscribe, {}}.valid()
              && !NtpSourceRequest{Action::Subscribe, def(QStringLiteral("x"))}.valid()
              && NtpSourceRequest{Action::Remove, {QStringLiteral("x"), {}, {}, {}, false}}.valid()
              && !NtpSourceRequest{Action::Enable, {QStringLiteral("x y"), {}, {}, {}, false}}.valid()
              && !NtpSourceRequest{Action::Create, def(QStringLiteral("x"), QStringLiteral("a b"))}.valid(),
          "each action carries only what it needs, validated");
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
    for (const QString& version : {QString(), QStringLiteral("4.2.20.41343"), QStringLiteral("4.3.15")}) {
        firmware = version;
        check(!flex.capabilities().ntpSourceControl, "absent or older firmware declares no NTP sources");
    }
    check(flex.requestNtpSource({Action::Subscribe, {}}) == ReceiveDispatch::Unsupported && commands.isEmpty(),
          "without the capability not even the subscription is sent");
    firmware = QStringLiteral("4.3.16.42034");
    check(flex.capabilities().ntpSourceControl && flex.capabilities().ntpSourceControl->maxSources == 10,
          "FLEX-8600 on 4.3.16 declares ten NTP sources");
    model = QStringLiteral("AU-520M");
    check(flex.capabilities().ntpSourceControl.has_value(), "Aurora declares them");
    // #7127: a FLEX-6300 on 4.3.16.42034 publishes ntp_sources, and FlexLib
    // gates them on no platform, so the 6000 series declares them too.
    for (const QString& sixK : {QStringLiteral("FLEX-6300"), QStringLiteral("FLEX-6700"),
                                QStringLiteral("FLEX-6400M")}) {
        model = sixK;
        check(flex.capabilities().ntpSourceControl && flex.capabilities().ntpSourceControl->maxSources == 10,
              "6000-series models on 4.3.16 declare ten NTP sources");
    }
    model = QString();
    check(!flex.capabilities().ntpSourceControl, "an unknown model declares none");
    model = QStringLiteral("FLEX-6300");
    firmware = QStringLiteral("4.3.15");
    check(!flex.capabilities().ntpSourceControl, "a 6000-series radio on older firmware declares none");
    firmware = QStringLiteral("4.3.16.42034");
    model = QStringLiteral("FLEX-8400");
    flex.requestNtpSource({Action::Subscribe, {}});
    flex.requestNtpSource({Action::Create, def(QStringLiteral("nist"))});
    flex.requestNtpSource({Action::Create, def(QStringLiteral("lan"), QStringLiteral("192.168.50.1"),
                                               QStringLiteral("peer"), QStringLiteral("Shack GPS"), true)});
    flex.requestNtpSource({Action::Remove, {QStringLiteral("nist"), {}, {}, {}, false}});
    flex.requestNtpSource({Action::Enable, {QStringLiteral("nist"), {}, {}, {}, false}});
    flex.requestNtpSource({Action::Disable, {QStringLiteral("nist"), {}, {}, {}, false}});
    check(commands == QStringList{
              QStringLiteral("sub ntp_sources all"),
              QStringLiteral("ntp_sources create id=nist address=time.nist.gov directive=server"),
              QStringLiteral("ntp_sources create id=lan address=192.168.50.1 directive=peer description=\"Shack GPS\" prefer=1"),
              QStringLiteral("ntp_sources remove id=nist"),
              QStringLiteral("ntp_sources enable id=nist"),
              QStringLiteral("ntp_sources disable id=nist")},
          "FlexLib's wire text: description quoted only when present, prefer only when set");
    commands.clear();
    check(flex.requestNtpSource({Action::Create, def(QStringLiteral("x"), QStringLiteral("a\nsub y"))})
                  == ReceiveDispatch::Unsupported
              && flex.requestNtpSource({Action::Create, def(QStringLiteral("x"), QStringLiteral("time.nist.gov"),
                                                            QStringLiteral("server"), QStringLiteral("Shack GPS\n"))})
                  == ReceiveDispatch::Unsupported
              && commands.isEmpty(),
          "an invalid request, a final newline included, sends nothing");
    model = QStringLiteral("ML-9600");
    check(flex.capabilities().ntpSourceControl.has_value(), "the ML-9600, on the same platform, declares them");
}

void statusDecode()
{
    FlexBackend flex;
    QList<NtpSourceDelta> seen;
    QObject::connect(&flex, &IRadioBackend::ntpSourceChanged, [&seen](const NtpSourceDelta& d) { seen.append(d); });
    const ParsedMessage msg = CommandParser::parseLine(kMeasured);
    const QString body = msg.raw.mid(msg.raw.indexOf(QLatin1Char('|')) + 1).mid(12);
    flex.statusIngress()->decodeNtpSourceStatus(body);
    const NtpSourceDelta d = seen.value(0);
    check(seen.size() == 1 && d.id == QLatin1String("default-pool") && !d.removed
              && d.address == QStringLiteral("2.debian.pool.ntp.org") && d.directive == QStringLiteral("pool")
              && d.enabled == true && d.selected == true && d.prefer == false
              && d.state == QStringLiteral("selected") && d.reach == 255 && !d.stratum,
          "the measured line decodes every field it carries and nothing it does not");
    check(d.description == QStringLiteral("Debian NTP pool"), "the quoted description keeps its spaces");

    flex.statusIngress()->decodeNtpSourceStatus(QStringLiteral("id=x description=\"removed later\" stratum=2"));
    check(seen.size() == 2 && !seen.at(1).removed && seen.at(1).description == QStringLiteral("removed later")
              && seen.at(1).stratum == 2,
          "only the word right after the id means removal");
    flex.statusIngress()->decodeNtpSourceStatus(QStringLiteral("id=x removed"));
    check(seen.size() == 3 && seen.at(2).removed && seen.at(2).id == QLatin1String("x"), "a bare removed is a removal");
    flex.statusIngress()->decodeNtpSourceStatus(QStringLiteral("id=x reach=abc enabled=yes"));
    check(seen.size() == 4 && !seen.at(3).reach && !seen.at(3).enabled, "malformed numbers and flags are dropped");
    flex.statusIngress()->decodeNtpSourceStatus(QStringLiteral("address=y enabled=1"));
    flex.statusIngress()->decodeNtpSourceStatus(QStringLiteral("id= enabled=1"));
    flex.statusIngress()->decodeNtpSourceStatus(QStringLiteral("id=z"));
    check(seen.size() == 4, "a line without a leading id, with an empty id or with nothing after it is ignored");

    // Measured on a FLEX-6300, 4.3.16.42034 (#7127): the same shape, plus stratum.
    const ParsedMessage sixK = CommandParser::parseLine(QStringLiteral(
        "S17B07863|ntp_sources id=default-pool address=2.debian.pool.ntp.org enabled=1 directive=pool "
        "selected=1 prefer=0 state=selected stratum=1 reach=255 description=\"Debian NTP pool\""));
    flex.statusIngress()->decodeNtpSourceStatus(sixK.raw.mid(sixK.raw.indexOf(QLatin1Char('|')) + 1).mid(12));
    const NtpSourceDelta s = seen.value(4);
    check(seen.size() == 5 && s.id == QLatin1String("default-pool") && s.selected == true
              && s.state == QStringLiteral("selected") && s.stratum == 1 && s.reach == 255
              && s.description == QStringLiteral("Debian NTP pool"),
          "the FLEX-6300's line decodes, stratum included");
}

struct Rig {
    RadioModel radio;
    RadioSetupTestBackend* backend{nullptr};
    QString model{QStringLiteral("FLEX-8600")};
    QString firmware{QStringLiteral("4.3.16.42034")};
    Rig()
    {
        backend = installRadioSetupBackend(radio);
        backend->setModelProvider([this] { return model; });
        backend->setFirmwareVersionProvider([this] { return firmware; });
    }
    void accept() { backend->replyLast({RadioSetupReply::Kind::Accepted, 0, {}}); }
    // A FLEX-8600 on 4.3.16.42034 refuses with `R<seq>|E2000000|Failed to create
    // NTP source: <id>`, which the setup sink delivers as Failed with that code.
    void refuse(const QString& why = QStringLiteral("Failed to create NTP source: a"))
    {
        backend->replyLast({RadioSetupReply::Kind::Failed, static_cast<int>(0xE2000000u), why});
    }
    void source(const QString& id, bool enabled = true, const QString& address = QStringLiteral("time.nist.gov"))
    {
        NtpSourceDelta d;
        d.id = id;
        d.address = address;
        d.directive = QStringLiteral("server");
        d.enabled = enabled;
        NtpSourcesTestAccess::apply(radio, d);
    }
    void removed(const QString& id)
    {
        NtpSourceDelta d;
        d.id = id;
        d.removed = true;
        NtpSourcesTestAccess::apply(radio, d);
    }
};

void modelList()
{
    Rig rig;
    QSignalSpy changed(&rig.radio, &RadioModel::ntpSourcesChanged);
    rig.source(QStringLiteral("a"));
    rig.source(QStringLiteral("b"), false);
    check(rig.radio.ntpSources().size() == 2 && rig.radio.ntpSources().at(1).id == QLatin1String("b")
              && !rig.radio.ntpSources().at(1).enabled && changed.size() == 2,
          "sources appear in arrival order, as the radio reports them");
    rig.source(QStringLiteral("a"));
    check(changed.size() == 2, "an unchanged status announces nothing");
    NtpSourceDelta stats;
    stats.id = QStringLiteral("a");
    stats.stratum = 2;
    stats.reach = 0x7F;
    NtpSourcesTestAccess::apply(rig.radio, stats);
    check(rig.radio.ntpSources().at(0).stratum == 2 && rig.radio.ntpSources().at(0).reach == 0x7F
              && rig.radio.ntpSources().at(0).address == QLatin1String("time.nist.gov"),
          "a partial status updates only what it carries");
    NtpSourceDelta unknown;
    unknown.id = QStringLiteral("a");
    unknown.state = QStringLiteral("unknown");
    NtpSourcesTestAccess::apply(rig.radio, unknown);
    check(rig.radio.ntpSources().at(0).stratum == -1 && rig.radio.ntpSources().at(0).reach == -1,
          "state=unknown forgets stale stratum and reach, as FlexLib does");
    rig.removed(QStringLiteral("a"));
    rig.removed(QStringLiteral("nope"));
    check(rig.radio.ntpSources().size() == 1 && rig.radio.ntpSources().at(0).id == QLatin1String("b"),
          "the radio's removed status drops the row; an unknown id is ignored");
}

void productionRoute()
{
    // The real Flex backend and RadioModel's own wiring, fed the measured line
    // through the raw status path.
    RadioModel radio;
    check(radio.rebuildBackendForTest(QStringLiteral("flex")), "a Flex backend is built");
    NtpSourcesTestAccess::message(radio, kMeasured);
    const QList<NtpSourceState>& list = radio.ntpSources();
    check(list.size() == 1 && list.at(0).id == QLatin1String("default-pool")
              && list.at(0).description == QLatin1String("Debian NTP pool") && list.at(0).inUse()
              && list.at(0).reach == 255,
          "the measured status line reaches the model through the production route");
    NtpSourcesTestAccess::message(radio, QStringLiteral("S0|ntp_sources id=stale address=old.example enabled=1"), true);
    check(radio.ntpSources().size() == 1, "a status queued from a previous session adds no row");
    FlexBackend flex;
    check(!flex.statusIngress()->decodeRawStatus(QStringLiteral("gps lat=1")) && flex.statusIngress()->decodeRawStatus(QStringLiteral("ntp_sources id=a removed")),
          "the backend claims only the raw statuses it decodes");
}

void subscription()
{
    Rig rig;
    NtpSourcesTestAccess::publish(rig.radio);
    check(rig.backend->commands.count(QStringLiteral("sub ntp_sources all")) == 1,
          "a capable session subscribes once");
    NtpSourcesTestAccess::publish(rig.radio);
    check(rig.backend->commands.count(QStringLiteral("sub ntp_sources all")) == 1,
          "a second capability publish in the same session does not subscribe again");
    rig.source(QStringLiteral("a"));
    RadioSetupControlTestAccess::replaceConnectionGeneration(rig.radio);
    NtpSourcesTestAccess::publish(rig.radio);
    check(rig.backend->commands.count(QStringLiteral("sub ntp_sources all")) == 2 && rig.radio.ntpSources().isEmpty(),
          "a new session clears the old list and subscribes again");

    Rig old;
    old.firmware = QStringLiteral("4.2.20.41343");
    old.source(QStringLiteral("stale"));
    NtpSourcesTestAccess::publish(old.radio);
    check(!old.backend->commands.contains(QStringLiteral("sub ntp_sources all")) && old.radio.ntpSources().isEmpty(),
          "older firmware is never sent the subscription, and no list is kept");

    Rig six;
    six.model = QStringLiteral("FLEX-6700");
    NtpSourcesTestAccess::publish(six.radio);
    check(six.backend->commands.count(QStringLiteral("sub ntp_sources all")) == 1,
          "a FLEX-6000 on 4.3.16 is sent the subscription once (#7127)");
}

void bridgeReadback()
{
    Rig rig;
    AutomationServer server;
    server.setRadioModel(&rig.radio);
    server.setReadOnly(true);
    const auto read = [&server] {
        return AutomationServerTestAccess::request(server, QByteArray("get ntp"));
    };
    rig.firmware = QStringLiteral("4.2.20");
    const QJsonObject unavailable = read();
    check(unavailable.value("ok").toBool()
              && unavailable.value("ntp").toObject() == QJsonObject{
                  {"available", false}, {"maxSources", 0}, {"sources", QJsonArray{}}},
          "get ntp is available read-only and reports unsupported firmware without sources");

    rig.firmware = QStringLiteral("4.3.16.42034");
    NtpSourceDelta delta;
    delta.id = QStringLiteral("a");
    delta.address = QStringLiteral("pool.ntp.org");
    delta.directive = QStringLiteral("pool");
    delta.description = QStringLiteral("Shack time");
    delta.enabled = false;
    delta.selected = true;
    delta.prefer = true;
    delta.state = QStringLiteral("candidate");
    delta.stratum = 2;
    delta.reach = 15;
    NtpSourcesTestAccess::apply(rig.radio, delta);
    const QJsonObject source{
        {"id", "a"}, {"address", "pool.ntp.org"}, {"directive", "pool"},
        {"description", "Shack time"}, {"enabled", false}, {"selected", true},
        {"inUse", true}, {"prefer", true}, {"state", "candidate"},
        {"stratum", 2}, {"reach", 15}, {"updating", false}};
    check(read().value("ntp").toObject() == QJsonObject{
              {"available", true}, {"maxSources", 10}, {"sources", QJsonArray{source}}}
              && rig.backend->commands.isEmpty(),
          "get ntp reports every documented source field and sends no command");

    rig.radio.replaceNtpSource(def(QStringLiteral("a")), nullptr, {});
    delta.enabled = true;
    delta.selected = false;
    delta.state = QStringLiteral("selected");
    NtpSourcesTestAccess::apply(rig.radio, delta);
    const QJsonObject updating = read().value("ntp").toObject().value("sources").toArray().at(0).toObject();
    check(updating.value("updating").toBool() && updating.value("enabled").toBool()
              && !updating.value("selected").toBool() && updating.value("inUse").toBool()
              && !rig.radio.ntpSourceEnabledShown(rig.radio.ntpSources().at(0)),
          "get ntp exposes live enabled and in-use state while the page pins an edit's checkbox");
    rig.refuse();
    check(!read().value("ntp").toObject().value("sources").toArray().at(0).toObject().value("updating").toBool(),
          "get ntp clears updating when an edit ends");
    rig.removed(QStringLiteral("a"));
    check(read().value("ntp").toObject().value("sources").toArray().isEmpty(),
          "get ntp drops a source when the radio removes it");
}

using ReplaceResult = std::optional<RadioModel::NtpSourceReplaceResult>;
auto into(ReplaceResult& r) { return [&r](const RadioModel::NtpSourceReplaceResult& v) { r = v; }; }

void replaceSequence()
{
    const NtpSourceDefinition edited = def(QStringLiteral("a"), QStringLiteral("pool.ntp.org"),
                                           QStringLiteral("pool"), QStringLiteral("Public pool"), true);
    {
        Rig rig;
        rig.source(QStringLiteral("a"));
        ReplaceResult result;
        check(rig.radio.replaceNtpSource(edited, nullptr, into(result)) && rig.radio.ntpSourceReplaceActive("a")
                  && rig.backend->commands == QStringList{QStringLiteral("ntp_sources remove id=a")},
              "an edit starts with the remove and marks the source as updating");
        check(!rig.radio.replaceNtpSource(edited, nullptr, {}), "a second edit of the same source is refused");
        rig.accept();
        check(rig.backend->commands.last()
                  == QStringLiteral("ntp_sources create id=a address=pool.ntp.org directive=pool description=\"Public pool\" prefer=1"),
              "an accepted remove creates the new definition with the same id");
        rig.accept();
        check(result && result->outcome == Outcome::Replaced && result->message.isEmpty()
                  && !rig.radio.ntpSourceReplaceActive("a") && rig.backend->commands.size() == 2,
              "an enabled source is done once the create is accepted");
    }
    {
        Rig rig;
        rig.source(QStringLiteral("a"), false);
        ReplaceResult result;
        rig.radio.replaceNtpSource(edited, nullptr, into(result));
        rig.accept();
        rig.accept();
        check(!result && rig.backend->commands.last() == QStringLiteral("ntp_sources disable id=a"),
              "a created source starts enabled, so a disabled one is disabled again");
        rig.accept();
        check(result && result->outcome == Outcome::Replaced, "and the edit completes after that");
    }
    {
        Rig rig;
        rig.source(QStringLiteral("a"));
        ReplaceResult result;
        rig.radio.replaceNtpSource(edited, nullptr, into(result));
        rig.refuse(QStringLiteral("busy"));
        check(result && result->outcome == Outcome::Unchanged && result->message.contains("busy")
                  && rig.backend->commands.size() == 1,
              "a refused remove changes nothing and sends nothing more");
    }
    {
        Rig rig;
        rig.source(QStringLiteral("a"), true, QStringLiteral("old.example"));
        ReplaceResult result;
        rig.radio.replaceNtpSource(edited, nullptr, into(result));
        rig.accept();
        rig.refuse(QStringLiteral("unresolvable"));
        check(rig.backend->commands.last()
                  == QStringLiteral("ntp_sources create id=a address=old.example directive=server"),
              "a refused new definition re-creates the original");
        rig.accept();
        check(result && result->outcome == Outcome::Restored && result->message.contains("unresolvable"),
              "and reports that the old settings were kept, with the radio's reason");
    }
    {
        Rig rig;
        rig.source(QStringLiteral("a"), false, QStringLiteral("old.example"));
        ReplaceResult result;
        rig.radio.replaceNtpSource(edited, nullptr, into(result));
        rig.accept();
        rig.refuse(QStringLiteral("unresolvable"));
        rig.accept();
        check(!result && rig.backend->commands.last() == QStringLiteral("ntp_sources disable id=a"),
              "a restored source that was disabled is disabled again");
        rig.accept();
        check(result && result->outcome == Outcome::Restored, "and only then is the edit reported");
    }
    {
        Rig rig;
        rig.source(QStringLiteral("a"));
        ReplaceResult result;
        rig.radio.replaceNtpSource(edited, nullptr, into(result));
        rig.accept();
        rig.refuse(QStringLiteral("first"));
        rig.refuse(QStringLiteral("second"));
        check(result && result->outcome == Outcome::Lost && result->message.contains("first")
                  && result->message.contains("second") && !rig.radio.ntpSourceReplaceActive("a")
                  && rig.radio.ntpSources().isEmpty(),
              "when neither definition can be created the source is reported lost and its row dropped");
    }
    {
        Rig rig;
        rig.source(QStringLiteral("a"), false);
        ReplaceResult result;
        rig.radio.replaceNtpSource(edited, nullptr, into(result));
        rig.accept();
        rig.accept();
        rig.refuse(QStringLiteral("nope"));
        check(result && result->outcome == Outcome::Replaced && result->message.contains("left enabled"),
              "a refused re-disable is reported, not hidden");
    }
    {
        Rig rig;
        rig.source(QStringLiteral("a"));
        ReplaceResult result;
        rig.radio.replaceNtpSource(edited, nullptr, into(result));
        RadioSetupControlTestAccess::replaceConnectionGeneration(rig.radio);
        rig.accept();
        check(result && result->outcome == Outcome::Cancelled && rig.backend->commands.size() == 1,
              "a session that ends mid-edit cancels it and sends nothing to the new session");
    }
    {
        Rig rig;
        rig.source(QStringLiteral("a"), false);
        ReplaceResult result;
        rig.radio.replaceNtpSource(edited, nullptr, into(result));
        rig.accept();
        rig.accept();
        RadioSetupControlTestAccess::replaceConnectionGeneration(rig.radio);
        rig.accept();
        check(result && result->outcome == Outcome::Cancelled,
              "a session that ends during the re-disable is cancelled, not reported as updated");
    }
    {
        Rig rig;
        rig.source(QStringLiteral("a"));
        ReplaceResult result;
        rig.radio.replaceNtpSource(edited, nullptr, into(result));
        rig.backend->replyLast({RadioSetupReply::Kind::Failed, -1, QStringLiteral("no radio response")});
        check(result && result->outcome == Outcome::NoAnswer && result->message.contains("did not answer the remove")
                  && rig.backend->commands.size() == 1 && rig.radio.ntpSources().size() == 1,
              "an unanswered remove is reported as no answer, not a refusal, and nothing more is sent");
    }
    {
        Rig rig;
        rig.source(QStringLiteral("a"));
        ReplaceResult result;
        rig.radio.replaceNtpSource(edited, nullptr, into(result));
        rig.accept();
        rig.refuse(QStringLiteral("unresolvable"));
        rig.backend->replyLast({RadioSetupReply::Kind::Failed, -1, QStringLiteral("no radio response")});
        check(result && result->outcome == Outcome::NoAnswer && result->message.contains("re-create"),
              "an unanswered rollback is not reported as lost");
    }
    {
        // The radio's own status order during an edit (FlexLib Radio.cs:14469).
        Rig rig;
        rig.source(QStringLiteral("a"), false);
        rig.source(QStringLiteral("b"));
        ReplaceResult result;
        rig.radio.replaceNtpSource(edited, nullptr, into(result));
        rig.removed(QStringLiteral("a"));
        const QList<NtpSourceState>& list = rig.radio.ntpSources();
        check(list.size() == 2 && list.at(0).id == QLatin1String("a") && rig.radio.ntpSourceReplaceActive("a"),
              "the edit's own removed status keeps the row in place");
        QString refused;
        rig.radio.requestNtpSource({Action::Enable, {QStringLiteral("a"), {}, {}, {}, false}}, nullptr,
            [&refused](const RadioControlReply& r) { refused = r.message; });
        check(refused.contains(QLatin1String("being updated")) && rig.backend->commands.size() == 1,
              "another request for an id being edited is refused and not sent");
        rig.accept();
        rig.source(QStringLiteral("a"), true, QStringLiteral("pool.ntp.org"));
        check(list.at(0).id == QLatin1String("a") && list.at(0).address == QLatin1String("pool.ntp.org")
                  && list.at(0).enabled && !rig.radio.ntpSourceEnabledShown(list.at(0)),
              "the re-create updates the row in place, and Enabled holds its old state until the edit ends");
        rig.accept();
        rig.source(QStringLiteral("a"), false, QStringLiteral("pool.ntp.org"));
        rig.accept();
        check(result && result->outcome == Outcome::Replaced && !rig.radio.ntpSourceReplaceActive("a")
                  && list.size() == 2 && list.at(0).id == QLatin1String("a") && !list.at(0).enabled,
              "once the re-disable lands the row is the radio's again, still first");
    }
    {
        // The create reply can come before the re-create status.
        Rig rig;
        rig.source(QStringLiteral("a"));
        ReplaceResult result;
        rig.radio.replaceNtpSource(edited, nullptr, into(result));
        rig.removed(QStringLiteral("a"));
        rig.accept();
        rig.accept();
        check(result && result->outcome == Outcome::Replaced && rig.radio.ntpSourceReplaceActive("a")
                  && rig.radio.ntpSources().size() == 1,
              "an edit done before its re-create status keeps the row until the status lands");
        rig.source(QStringLiteral("a"), true, QStringLiteral("pool.ntp.org"));
        check(!rig.radio.ntpSourceReplaceActive("a") && rig.radio.ntpSources().size() == 1,
              "and the status ends it");
        rig.removed(QStringLiteral("a"));
        check(rig.radio.ntpSources().isEmpty(), "a later removed status is honored again");
    }
    {
        // A synchronous observer can end the session before the first step.
        Rig rig;
        rig.source(QStringLiteral("a"));
        bool once = false;
        QObject::connect(&rig.radio, &RadioModel::ntpSourcesChanged, &rig.radio, [&] {
            if (!once) { once = true; RadioSetupControlTestAccess::replaceConnectionGeneration(rig.radio); }
        });
        ReplaceResult result;
        check(rig.radio.replaceNtpSource(edited, nullptr, into(result)) && result
                  && result->outcome == Outcome::Cancelled && rig.backend->commands.isEmpty(),
              "a session replaced while the edit is announced sends nothing");
    }
    {
        Rig rig;
        check(!rig.radio.replaceNtpSource(edited, nullptr, {}) && rig.backend->commands.isEmpty(),
              "an edit of a source the radio does not list is refused before sending");
        rig.source(QStringLiteral("a"));
        const RadioControlSession before = rig.radio.controlSession();
        RadioSetupControlTestAccess::replaceConnectionGeneration(rig.radio);
        check(!rig.radio.replaceNtpSource(edited, nullptr, {}, before) && rig.backend->commands.isEmpty(),
              "an edit meant for another session is refused before sending");
        rig.source(QStringLiteral("a"));
        check(!rig.radio.replaceNtpSource(def(QStringLiteral("a"), QStringLiteral("a b")), nullptr, {})
                  && rig.backend->commands.isEmpty(),
              "an invalid new definition removes nothing");
    }
}

struct PageRig : Rig {
    NtpSourcesPage page{radio};
    QStringList asked;
    bool answer{true};
    std::function<void()> duringConfirm;
    PageRig()
    {
        page.setConfirm([this](const QString&, const QString& text, const QString&) {
            asked.append(text);
            if (duringConfirm) { duringConfirm(); }
            return answer;
        });
    }
    template <typename T> T* find(const char* name) { return page.findChild<T*>(QString::fromLatin1(name)); }
    QTabBar* tabs() { return find<QTabBar>("ntpServerTabs"); }
    void select(int slot) {
        tabs()->setCurrentIndex(slot);
    }
    QCheckBox* enabled() { return find<QCheckBox>("ntpSourceEnabled"); }
    void add(const QString& id, int slot = 1) {
        tabs()->setCurrentIndex(slot);
        find<QLineEdit>("ntpSourceId")->setText(id);
        find<QLineEdit>("ntpSourceAddress")->setText(QStringLiteral("other.example"));
        find<QPushButton>("ntpSourceSave")->click();
    }
    void requestSecondSource(Action action)
    {
        if (action == Action::Enable) {
            source(QStringLiteral("b"), false);
            select(1);
            enabled()->click();
        } else {
            add(QStringLiteral("b"));
        }
        select(0);
    }
};

void pagePendingPositiveRequests()
{
    for (const Action action : {Action::Enable, Action::Create}) {
        for (const bool accepted : {false, true}) {
            PageRig rig;
            rig.source(QStringLiteral("a"));
            rig.answer = false;
            rig.requestSecondSource(action);
            const RadioSetupCompletion reply = rig.backend->replies.back();
            if (accepted) { rig.accept(); }
            rig.enabled()->click();
            check(rig.asked.size() == 1 && rig.asked.last().contains(QLatin1String("only enabled"))
                      && !rig.backend->commands.contains(QStringLiteral("ntp_sources disable id=a")),
                  "pending/accepted enable or create needs status before waiving last-source warning");
            rig.find<QPushButton>("ntpRemoveSource")->click();
            check(rig.asked.size() == 2 && !rig.backend->commands.contains(QStringLiteral("ntp_sources remove id=a")),
                  "pending positive request cannot waive last-source removal warning");
            if (!accepted) {
                reply({RadioSetupReply::Kind::Failed, static_cast<int>(0xE2000000u), QStringLiteral("refused")});
            }
            check(rig.enabled()->isChecked(), "cancel preserves radio-reported enabled state");
        }
        PageRig rig;
        rig.source(QStringLiteral("a"));
        rig.requestSecondSource(action);
        rig.source(QStringLiteral("b"));
        rig.enabled()->click();
        check(rig.asked.isEmpty() && rig.backend->commands.last() == QStringLiteral("ntp_sources disable id=a"),
              "reported enabled source counts while its positive reply is pending");
    }
}

void page()
{
    {
        Rig plain;
        plain.firmware = QStringLiteral("4.2.20.41343");
        NtpSourcesPage p(plain.radio);
        auto* reason = p.findChild<QLabel*>(QStringLiteral("ntpUnavailableReason"));
        auto* tabs = p.findChild<QTabBar*>(QStringLiteral("ntpServerTabs"));
        check(reason && !reason->isHidden() && reason->text().contains(QLatin1String("4.3.16"))
                  && tabs && !tabs->isEnabled() && !tabs->isHidden(),
              "unsupported radio keeps tabs visible, dimmed, with reason");
    }
    PageRig rig;
    auto* remove = rig.find<QPushButton>("ntpRemoveSource");
    auto* count = rig.find<QLabel>("ntpSourceCount");
    auto* id = rig.find<QLineEdit>("ntpSourceId");
    auto* address = rig.find<QLineEdit>("ntpSourceAddress");
    auto* save = rig.find<QPushButton>("ntpSourceSave");
    auto* issue = rig.find<QLabel>("ntpSourceIssue");
    check(rig.tabs()->count() == 10 && rig.tabs()->isEnabled() && !remove->isEnabled()
              && count->text() == QLatin1String("0 of 10 sources") && !save->isEnabled(),
          "ten configuration tabs with empty validated editor and no source actions");
    NtpSourceDelta measured;
    measured.id = QStringLiteral("default-pool");
    measured.address = QStringLiteral("2.debian.pool.ntp.org");
    measured.directive = QStringLiteral("pool");
    measured.description = QStringLiteral("Debian NTP pool");
    measured.enabled = true;
    measured.selected = true;
    measured.state = QStringLiteral("selected");
    measured.reach = 255;
    NtpSourcesTestAccess::apply(rig.radio, measured);
    check(rig.find<QLabel>("ntpServiceState")->text() == QLatin1String("Reference selected")
              && rig.find<QLabel>("ntpReachRegister")->text().contains(QStringLiteral("377 · 8/8"))
              && rig.enabled()->isChecked() && id->text() == measured.id && id->isReadOnly(),
          "source populates its tab with live reference, octal reach and immutable ID");
    check(rig.find<QLabel>("ntpSourceStratum")->text() == QLatin1String("Not reported"),
          "unknown source stratum is never inferred from selection");
    address->setText(QStringLiteral("draft.example"));
    NtpSourceDelta reach;
    reach.id = measured.id;
    reach.reach = 15;
    NtpSourcesTestAccess::apply(rig.radio, reach);
    check(rig.find<QLabel>("ntpReachRegister")->text().contains(QStringLiteral("017 · 4/8"))
              && rig.find<QLabel>("ntpPoll1")->accessibleName().contains(QLatin1String("no reply"))
              && rig.find<QLabel>("ntpPoll8")->accessibleName().contains(QLatin1String("reply received"))
              && address->text() == QLatin1String("draft.example"),
          "poll strip orders bits oldest to newest and live status preserves edits");
    rig.tabs()->setCurrentIndex(1);
    rig.tabs()->setCurrentIndex(0);
    check(address->text() == QLatin1String("draft.example"), "switching tabs preserves unsaved per-server drafts");
    NtpSourcesTestAccess::gpsdo(rig.radio, true, QStringLiteral("Fine Lock"), true);
    check(rig.find<QWidget>("ntpGpsCard")->property("ntpGold").toBool()
              && rig.find<QLabel>("ntpGpsReference")->text() == QLatin1String("GPSDO locked")
              && !rig.find<QLabel>("ntpSourceStratum")->text().contains(QLatin1String("1")),
          "GPSDO lock gets gold chrome without inventing local stratum one");
    NtpSourcesTestAccess::gpsdo(rig.radio, false, QStringLiteral("Not Present"));
    check(!rig.find<QWidget>("ntpGpsCard")->property("ntpGold").toBool(), "gold clears when GPSDO is no longer present");
    rig.find<QPushButton>("ntpSourceReset")->click();
    check(address->text() == *measured.address && !save->isEnabled(), "Reset restores the radio definition");
    rig.answer = false;
    rig.enabled()->click();
    check(rig.asked.size() == 1 && rig.enabled()->isChecked() && rig.backend->commands.isEmpty(),
          "last enabled source asks; cancel sends nothing");
    rig.answer = true;
    rig.enabled()->click();
    check(rig.enabled()->isChecked() && !rig.enabled()->isEnabled() && !remove->isEnabled()
              && rig.backend->commands.last() == QStringLiteral("ntp_sources disable id=default-pool"),
          "toggle waits for radio status and locks pending actions");
    rig.accept();
    remove->click();
    check(rig.asked.last().contains(QLatin1String("only enabled"))
              && rig.backend->commands.last() == QStringLiteral("ntp_sources remove id=default-pool"),
          "removing last enabled source confirms before sending");
    rig.tabs()->setCurrentIndex(1);
    id->setText(QStringLiteral("default-pool"));
    address->setText(QStringLiteral("time.nist.gov"));
    check(!save->isEnabled() && issue->text().contains(QLatin1String("already exists")), "duplicate ID refused");
    id->setText(QStringLiteral("nist"));
    address->setText(QStringLiteral("time nist"));
    check(!save->isEnabled(), "invalid address refused");
    address->setText(QStringLiteral("time.nist.gov"));
    save->click();
    check(rig.radio.ntpSources().size() == 1
              && rig.backend->commands.last() == QStringLiteral("ntp_sources create id=nist address=time.nist.gov directive=server"),
          "empty slot creates source without inventing reported state");
}

void pageSafeguards()
{
    {
        PageRig rig;
        rig.source(QStringLiteral("a"));
        rig.source(QStringLiteral("b"));
        rig.enabled()->click();
        rig.select(1);
        rig.enabled()->click();
        check(rig.asked.size() == 1 && rig.asked.last().contains(QLatin1String("only enabled")),
              "pending disable counts toward last-source warning across tabs");
    }
    {
        PageRig rig;
        rig.source(QStringLiteral("a"));
        rig.duringConfirm = [&rig] { RadioSetupControlTestAccess::replaceConnectionGeneration(rig.radio); };
        rig.find<QPushButton>("ntpRemoveSource")->click();
        check(rig.backend->commands.isEmpty()
                  && rig.find<QLabel>("ntpSourcesMessage")->text().contains(QLatin1String("Nothing was sent")),
              "session change during remove confirmation sends nothing");
        rig.enabled()->click();
        check(rig.backend->commands.isEmpty(), "session change during disable confirmation sends nothing");
    }
    {
        PageRig rig;
        rig.source(QStringLiteral("a"));
        auto* save = rig.find<QPushButton>("ntpSourceSave");
        check(!save->isEnabled(), "unchanged definition sends no remove/create");
        rig.find<QLineEdit>("ntpSourceDescription")->setText(QStringLiteral("Shack GPS\n"));
        check(!save->isEnabled(), "description with newline refused");
        rig.find<QLineEdit>("ntpSourceDescription")->setText(QStringLiteral("Shack GPS"));
        save->click();
        check(rig.backend->commands == QStringList{QStringLiteral("ntp_sources remove id=a")},
              "saving modified definition starts guarded replacement");
    }
    {
        PageRig rig;
        rig.tabs()->setCurrentIndex(9);
        rig.find<QLineEdit>("ntpSourceId")->setText(QStringLiteral("new"));
        rig.find<QLineEdit>("ntpSourceAddress")->setText(QStringLiteral("time.nist.gov"));
        for (int i = 0; i < 10; ++i) { rig.source(QStringLiteral("s%1").arg(i)); }
        check(!rig.find<QPushButton>("ntpSourceSave")->isEnabled()
                  && rig.find<QLineEdit>("ntpSourceId")->text() == QLatin1String("s9")
                  && rig.find<QLabel>("ntpSourcesMessage")->text().contains(QLatin1String("draft was discarded")),
              "a full radio inventory replaces a draft with the reported source and an explicit notice");
    }
    {
        PageRig rig;
        rig.add(QStringLiteral("nist"));
        rig.tabs()->setCurrentIndex(2);
        rig.find<QLineEdit>("ntpSourceId")->setText(QStringLiteral("nist"));
        rig.find<QLineEdit>("ntpSourceAddress")->setText(QStringLiteral("time.nist.gov"));
        check(!rig.find<QPushButton>("ntpSourceSave")->isEnabled()
                  && rig.find<QLabel>("ntpSourceIssue")->text().contains(QLatin1String("being added")),
              "pending create cannot be duplicated from another tab");
        rig.refuse(QString());
        check(rig.find<QLabel>("ntpSourcesMessage")->text() == QLatin1String("The radio did not add nist: no reason given"),
              "refusal includes explicit missing-reason text");
        rig.add(QStringLiteral("nist2"), 3);
        rig.backend->replyLast({RadioSetupReply::Kind::Failed, -1, QStringLiteral("no radio response")});
        check(rig.find<QLabel>("ntpSourcesMessage")->text().contains(QLatin1String("did not answer")),
              "timeout is distinguished from refusal");
    }
    {
        PageRig rig;
        rig.source(QStringLiteral("a"));
        rig.source(QStringLiteral("b"));
        rig.select(1);
        NtpSourceDelta gone; gone.id = QStringLiteral("a"); gone.removed = true;
        NtpSourcesTestAccess::apply(rig.radio, gone);
        check(rig.tabs()->currentIndex() == 1
                  && rig.find<QLineEdit>("ntpSourceId")->text() == QLatin1String("b"),
              "removing another source preserves tab identity");
    }
}

void pageReviewRegressions()
{
    {
        PageRig rig;
        rig.source(QStringLiteral("a"));
        rig.select(1);
        rig.backend->live = false;
        NtpSourcesTestAccess::publish(rig.radio);
        RadioSetupControlTestAccess::replaceConnectionGeneration(rig.radio);
        rig.backend->live = true;
        NtpSourcesTestAccess::publish(rig.radio);
        rig.source(QStringLiteral("a"));
        rig.find<QLineEdit>("ntpSourceId")->setText(QStringLiteral("after-reconnect"));
        rig.find<QLineEdit>("ntpSourceAddress")->setText(QStringLiteral("clock.example"));
        check(rig.find<QPushButton>("ntpSourceSave")->isEnabled(),
              "reconnect refreshes the empty tab session without switching tabs or resetting");
    }
    {
        PageRig rig;
        rig.select(1);
        rig.find<QLineEdit>("ntpSourceAddress")->setText(QStringLiteral("draft.example"));
        rig.select(0);
        rig.select(1);
        rig.find<QLineEdit>("ntpSourceAddress")->clear();
        rig.select(0);
        rig.select(1);
        check(rig.find<QLineEdit>("ntpSourceAddress")->text().isEmpty(),
              "clearing an add draft does not resurrect its parked text");
    }
    {
        PageRig rig;
        rig.source(QStringLiteral("a"));
        rig.source(QStringLiteral("a"), true, QStringLiteral("changed.example"));
        check(rig.find<QLineEdit>("ntpSourceAddress")->text() == QLatin1String("changed.example")
                  && !rig.find<QPushButton>("ntpSourceSave")->isEnabled(),
              "an untouched form follows reported definition changes without inventing an edit");
        rig.select(1);
        rig.source(QStringLiteral("a"), true, QStringLiteral("changed-again.example"));
        rig.select(0);
        check(rig.find<QLineEdit>("ntpSourceAddress")->text() == QLatin1String("changed-again.example")
                  && !rig.find<QPushButton>("ntpSourceSave")->isEnabled(),
              "visiting a source does not save a stale pristine form as a draft");
    }
    for (bool parked : {false, true}) {
        PageRig rig;
        rig.source(QStringLiteral("a"));
        rig.select(1);
        // An address-only draft is still a draft, even before its ID is entered.
        rig.find<QLineEdit>("ntpSourceAddress")->setText(QStringLiteral("mine.example"));
        if (parked) { rig.select(0); }
        rig.source(QStringLiteral("theirs"), true, QStringLiteral("theirs.example"));
        rig.select(1);
        check(rig.find<QLineEdit>("ntpSourceAddress")->text() == QLatin1String("mine.example")
                  && !rig.find<QPushButton>("ntpRemoveSource")->isEnabled()
                  && !rig.enabled()->isEnabled()
                  && rig.tabs()->tabToolTip(2).contains(QLatin1String("theirs.example")),
              "another client skips both active and parked add drafts while a free slot exists");
    }
    for (bool parked : {false, true}) {
        PageRig rig;
        rig.source(QStringLiteral("a"));
        rig.source(QStringLiteral("b"));
        rig.select(1);
        rig.find<QLineEdit>("ntpSourceAddress")->setText(QStringLiteral("edited.example"));
        if (parked) { rig.select(0); }
        rig.removed(QStringLiteral("b"));
        rig.select(1);
        check(rig.find<QLineEdit>("ntpSourceId")->text().isEmpty()
                  && rig.find<QLineEdit>("ntpSourceAddress")->text().isEmpty()
                  && !rig.find<QPushButton>("ntpSourceSave")->isEnabled()
                  && rig.find<QLabel>("ntpSourcesMessage")->text().contains(QLatin1String("The radio no longer lists b")),
              "removed-source edit drafts are discarded with a notice and cannot become an add");
    }
    for (bool replyFirst : {false, true}) {
        PageRig rig;
        rig.add(QStringLiteral("mine"), 2);
        check(rig.find<QLineEdit>("ntpSourceId")->text() == QLatin1String("mine")
                  && rig.find<QLabel>("ntpSelectionState")->text().contains(QLatin1String("Adding"))
                  && !rig.find<QPushButton>("ntpSourceSave")->isEnabled(),
              "an unreported create shows its definition and a pending state");
        if (replyFirst) { rig.accept(); }
        rig.select(1);
        rig.find<QLineEdit>("ntpSourceId")->setText(QStringLiteral("mine"));
        rig.find<QLineEdit>("ntpSourceAddress")->setText(QStringLiteral("duplicate.example"));
        check(!rig.find<QPushButton>("ntpSourceSave")->isEnabled(),
              "an accepted but unreported create still prevents duplicate IDs");
        rig.source(QStringLiteral("mine"), true, QStringLiteral("other.example"));
        if (!replyFirst) { rig.accept(); }
        rig.select(2);
        check(rig.find<QLineEdit>("ntpSourceId")->text() == QLatin1String("mine")
                  && rig.find<QLineEdit>("ntpSourceId")->isReadOnly()
                  && rig.tabs()->tabToolTip(0).contains(QLatin1String("empty")),
              "create keeps its requested tab with either reply/status ordering");
    }
    {
        PageRig rig;
        for (int i = 0; i < 9; ++i) { rig.source(QStringLiteral("s%1").arg(i)); }
        rig.add(QStringLiteral("mine"), 9);
        rig.accept();
        rig.source(QStringLiteral("theirs"));
        check(rig.find<QLineEdit>("ntpSourceId")->text() == QLatin1String("theirs")
                  && rig.find<QLineEdit>("ntpSourceId")->isReadOnly()
                  && !rig.find<QLabel>("ntpSourcesMessage")->text().contains(QLatin1String("draft was discarded")),
              "a full reported inventory outranks an unreported create without calling it a discarded draft");
    }
    {
        PageRig rig;
        rig.add(QStringLiteral("refused"), 2);
        rig.refuse();
        check(rig.find<QLineEdit>("ntpSourceId")->text().isEmpty(),
              "failed create releases its reservation and returns to an empty form");
        rig.add(QStringLiteral("unreported"), 2);
        rig.accept();
        rig.backend->live = false;
        NtpSourcesTestAccess::publish(rig.radio);
        RadioSetupControlTestAccess::replaceConnectionGeneration(rig.radio);
        rig.backend->live = true;
        NtpSourcesTestAccess::publish(rig.radio);
        rig.select(2);
        rig.find<QLineEdit>("ntpSourceId")->setText(QStringLiteral("unreported"));
        rig.find<QLineEdit>("ntpSourceAddress")->setText(QStringLiteral("clock.example"));
        check(rig.find<QPushButton>("ntpSourceSave")->isEnabled(),
              "new session retires accepted but unreported create reservations");
    }
    {
        PageRig rig;
        rig.source(QStringLiteral("hostile"), true, QStringLiteral("<b>spoof</b><img src='file:///tmp/x'>"));
        check(!rig.tabs()->tabToolTip(0).contains(QLatin1String("<b>"))
                  && rig.tabs()->tabToolTip(0).contains(QLatin1String("&lt;b&gt;")),
              "radio addresses are escaped before they enter rich-text tooltips");
        check(rig.tabs()->accessibleTabName(0).contains(QLatin1String("hostile"))
                  && rig.tabs()->accessibleTabName(0).contains(QLatin1String("Server 1"))
                  && rig.tabs()->accessibleTabName(1).contains(QLatin1String("empty")),
              "screen readers receive source identity and state, including empty tabs");
        NtpSourcesTestAccess::gpsdo(rig.radio, true, QStringLiteral("Locked"), false);
        check(!rig.find<QWidget>("ntpGpsCard")->property("ntpGold").toBool(),
              "GPSDO text alone cannot override an unlocked oscillator flag");
    }
}

} // namespace

int main(int argc, char** argv)
{
    TestSettingsProfile settings(QStringLiteral("ntp-sources-test"));
    QApplication app(argc, argv);
    validation();
    flexDeclarationAndEncoding();
    statusDecode();
    modelList();
    productionRoute();
    subscription();
    bridgeReadback();
    replaceSequence();
    page();
    pageSafeguards();
    pageReviewRegressions();
    pagePendingPositiveRequests();
    std::printf("%s (%d failure%s)\n", failures ? "FAILED" : "ok", failures, failures == 1 ? "" : "s");
    return failures ? 1 : 0;
}
