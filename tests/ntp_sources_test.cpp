// SmartSDR 4.3 NTP time sources (#6953): validation, the Flex capability and
// wire text, `ntp_sources` status decode, RadioModel's subscription, list and
// remove-and-create edit, and the Radio Setup Time page. Recording command sinks
// and injected status only: no sockets, radio or TX.
#include "TestSettingsProfile.h"
#include "RadioSetupControlFixture.h"
#include "core/backends/flex/CommandParser.h"
#include "gui/NtpSourcesPage.h"

#include <QApplication>
#include <QLabel>
#include <QLineEdit>
#include <QPushButton>
#include <QSignalSpy>
#include <QTableWidget>

#include <cstdio>
#include <optional>

namespace AetherSDR {
class NtpSourcesTestAccess {
public:
    static void publish(RadioModel& radio) { radio.publishCapabilities(radio.isConnected()); }
    static void apply(RadioModel& radio, const NtpSourceDelta& d) { radio.applyNtpSourceChanges(d); }
    static void message(RadioModel& radio, const QString& line, int generationsAgo = 0)
    {
        radio.handleMessageReceived(CommandParser::parseLine(line),
                                    radio.m_gpsSessionGeneration - quint64(generationsAgo));
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
    for (const QString& other : {QStringLiteral("FLEX-6700"), QStringLiteral("FLEX-6400M"), QString()}) {
        model = other;
        check(!flex.capabilities().ntpSourceControl, "6000-series and unknown models declare none");
    }
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
    flex.decodeNtpSourceStatus(body);
    const NtpSourceDelta d = seen.value(0);
    check(seen.size() == 1 && d.id == QLatin1String("default-pool") && !d.removed
              && d.address == QStringLiteral("2.debian.pool.ntp.org") && d.directive == QStringLiteral("pool")
              && d.enabled == true && d.selected == true && d.prefer == false
              && d.state == QStringLiteral("selected") && d.reach == 255 && !d.stratum,
          "the measured line decodes every field it carries and nothing it does not");
    check(d.description == QStringLiteral("Debian NTP pool"), "the quoted description keeps its spaces");

    flex.decodeNtpSourceStatus(QStringLiteral("id=x description=\"removed later\" stratum=2"));
    check(seen.size() == 2 && !seen.at(1).removed && seen.at(1).description == QStringLiteral("removed later")
              && seen.at(1).stratum == 2,
          "only the word right after the id means removal");
    flex.decodeNtpSourceStatus(QStringLiteral("id=x removed"));
    check(seen.size() == 3 && seen.at(2).removed && seen.at(2).id == QLatin1String("x"), "a bare removed is a removal");
    flex.decodeNtpSourceStatus(QStringLiteral("id=x reach=abc enabled=yes"));
    check(seen.size() == 4 && !seen.at(3).reach && !seen.at(3).enabled, "malformed numbers and flags are dropped");
    flex.decodeNtpSourceStatus(QStringLiteral("address=y enabled=1"));
    flex.decodeNtpSourceStatus(QStringLiteral("id= enabled=1"));
    flex.decodeNtpSourceStatus(QStringLiteral("id=z"));
    check(seen.size() == 4, "a line without a leading id, with an empty id or with nothing after it is ignored");
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
    NtpSourcesTestAccess::message(radio, QStringLiteral("S0|ntp_sources id=stale address=old.example enabled=1"), 1);
    check(radio.ntpSources().size() == 1, "a status queued from a previous session adds no row");
    FlexBackend flex;
    check(!flex.decodeRawStatus(QStringLiteral("gps lat=1")) && flex.decodeRawStatus(QStringLiteral("ntp_sources id=a removed")),
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
    check(!six.backend->commands.contains(QStringLiteral("sub ntp_sources all")),
          "a FLEX-6000 on 4.3.16 is never sent the subscription");
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
    QTableWidget* table() { return find<QTableWidget>("ntpSourcesTable"); }
    void select(int row) { table()->selectRow(row); }
};

void page()
{
    {
        Rig plain;
        plain.firmware = QStringLiteral("4.2.20.41343");
        NtpSourcesPage p(plain.radio);
        auto* reason = p.findChild<QLabel*>(QStringLiteral("ntpUnavailableReason"));
        auto* table = p.findChild<QTableWidget*>(QStringLiteral("ntpSourcesTable"));
        check(reason && !reason->isHidden() && reason->text().contains(QLatin1String("4.3.16"))
                  && table && !table->isEnabled() && !table->isHidden(),
              "without the capability the page stays visible, dimmed, with the reason");
    }
    PageRig rig;
    auto* add = rig.find<QPushButton>("ntpAddSource");
    auto* edit = rig.find<QPushButton>("ntpEditSource");
    auto* remove = rig.find<QPushButton>("ntpRemoveSource");
    auto* count = rig.find<QLabel>("ntpSourceCount");
    check(rig.table()->isEnabled() && add->isEnabled() && !edit->isEnabled() && !remove->isEnabled()
              && count->text() == QLatin1String("0 of 10 sources"),
          "a capable radio enables the table and Add; Edit and Remove wait for a selection");

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
    check(rig.table()->rowCount() == 1 && rig.table()->item(0, 3)->text().contains(QLatin1String("In use"))
              && rig.table()->item(0, 5)->text() == QLatin1String("8/8")
              && rig.table()->item(0, 6)->checkState() == Qt::Checked,
          "the measured source shows as in use, in words, with 8/8 reach");

    rig.select(0);
    check(edit->isEnabled() && remove->isEnabled(), "a selection enables Edit and Remove");
    NtpSourceDelta reach;
    reach.id = QStringLiteral("default-pool");
    reach.reach = 0x0F;
    NtpSourcesTestAccess::apply(rig.radio, reach);
    check(rig.table()->item(0, 5)->text() == QLatin1String("4/8") && rig.table()->currentRow() == 0
              && !rig.table()->selectedItems().isEmpty(),
          "a live status refreshes the row in place and keeps the selection");

    // Disabling the only enabled source asks first; Cancel sends nothing.
    rig.answer = false;
    rig.table()->item(0, 6)->setCheckState(Qt::Unchecked);
    check(rig.asked.size() == 1 && rig.asked.last().contains(QLatin1String("only enabled"))
              && !rig.backend->commands.contains(QStringLiteral("ntp_sources disable id=default-pool"))
              && rig.table()->item(0, 6)->checkState() == Qt::Checked,
          "disabling the last enabled source warns, and Cancel leaves the radio's state shown");
    rig.answer = true;
    rig.table()->item(0, 6)->setCheckState(Qt::Unchecked);
    check(rig.backend->commands.contains(QStringLiteral("ntp_sources disable id=default-pool"))
              && rig.table()->item(0, 6)->checkState() == Qt::Checked,
          "a confirmed toggle sends the request, and the box follows the radio, not the click");
    check(!(rig.table()->item(0, 6)->flags() & Qt::ItemIsUserCheckable) && !remove->isEnabled(),
          "while the radio has not answered, the row cannot be changed again");
    rig.accept();

    remove->click();
    check(rig.asked.last().contains(QLatin1String("only enabled"))
              && rig.backend->commands.last() == QStringLiteral("ntp_sources remove id=default-pool"),
          "removing the last enabled source warns, then sends the remove");

    // Add: live validation, never sending an invalid definition.
    add->click();
    auto* id = rig.find<QLineEdit>("ntpSourceId");
    auto* address = rig.find<QLineEdit>("ntpSourceAddress");
    auto* save = rig.find<QPushButton>("ntpSourceSave");
    auto* issue = rig.find<QLabel>("ntpSourceIssue");
    id->setText(QStringLiteral("default-pool"));
    address->setText(QStringLiteral("time.nist.gov"));
    check(!save->isEnabled() && issue->text().contains(QLatin1String("already exists")),
          "a duplicate id is refused with the reason");
    id->setText(QStringLiteral("nist"));
    address->setText(QStringLiteral("time nist"));
    check(!save->isEnabled() && issue->text().contains(QLatin1String("address")),
          "an invalid address is refused with the reason");
    address->setText(QStringLiteral("time.nist.gov"));
    check(save->isEnabled() && issue->isHidden(), "a valid definition can be saved");
    const qsizetype before = rig.backend->commands.size();
    save->click();
    check(rig.backend->commands.size() == before + 1
              && rig.backend->commands.last() == QStringLiteral("ntp_sources create id=nist address=time.nist.gov directive=server")
              && rig.table()->rowCount() == 1,
          "Add sends the create and adds no row until the radio reports it");

    // Full: Add is dimmed with the reason.
    for (int i = 0; i < 9; ++i) { rig.source(QStringLiteral("s%1").arg(i)); }
    check(rig.radio.ntpSources().size() == 10 && !add->isEnabled() && add->toolTip().contains(QLatin1String("at most 10"))
              && count->text() == QLatin1String("10 of 10 sources"),
          "at ten sources Add is disabled with the reason");
}

void pageSafeguards()
{
    auto* message = static_cast<QLabel*>(nullptr);
    {
        // Two enabled sources, two unticks before either reply: the second asks.
        PageRig rig;
        rig.source(QStringLiteral("a"));
        rig.source(QStringLiteral("b"));
        rig.table()->item(0, 6)->setCheckState(Qt::Unchecked);
        rig.table()->item(1, 6)->setCheckState(Qt::Unchecked);
        check(rig.asked.size() == 1 && rig.asked.last().contains(QLatin1String("only enabled"))
                  && rig.backend->commands.count(QStringLiteral("ntp_sources disable id=b")) == 1,
              "a disable still in flight counts toward the last-enabled-source warning");
    }
    {
        // The session changes while the operator is asked: nothing is sent.
        PageRig rig;
        rig.source(QStringLiteral("a"));
        rig.duringConfirm = [&rig] { RadioSetupControlTestAccess::replaceConnectionGeneration(rig.radio); };
        rig.select(0);
        rig.find<QPushButton>("ntpRemoveSource")->click();
        message = rig.find<QLabel>("ntpSourcesMessage");
        check(rig.asked.size() == 1 && rig.backend->commands.isEmpty()
                  && message->text().contains(QLatin1String("Nothing was sent")),
              "a remove confirmed after the session changed is not sent");
        rig.asked.clear();
        message->clear();
        rig.table()->item(0, 6)->setCheckState(Qt::Unchecked);
        check(rig.asked.size() == 1 && rig.backend->commands.isEmpty()
                  && message->text().contains(QLatin1String("Nothing was sent")),
              "nor is a disable");
    }
    {
        PageRig rig;
        rig.source(QStringLiteral("a"));
        rig.select(0);
        rig.find<QPushButton>("ntpEditSource")->click();
        auto* save = rig.find<QPushButton>("ntpSourceSave");
        auto* issue = rig.find<QLabel>("ntpSourceIssue");
        check(!save->isEnabled() && issue->text().contains(QLatin1String("Nothing has changed")),
              "Save Changes with nothing changed sends no remove and create");
        rig.find<QLineEdit>("ntpSourceDescription")->setText(QStringLiteral("Shack GPS\n"));
        check(!save->isEnabled() && issue->text().contains(QLatin1String("one line")),
              "a pasted description ending in a newline cannot be saved");
        rig.find<QLineEdit>("ntpSourceDescription")->setText(QStringLiteral("Shack GPS"));
        check(save->isEnabled(), "a real change can be saved");
        save->click();
        check(rig.backend->commands == QStringList{QStringLiteral("ntp_sources remove id=a")},
              "and starts the edit");
    }
    {
        // Another client fills the list while the Add form is open.
        PageRig rig;
        rig.find<QPushButton>("ntpAddSource")->click();
        rig.find<QLineEdit>("ntpSourceId")->setText(QStringLiteral("new"));
        rig.find<QLineEdit>("ntpSourceAddress")->setText(QStringLiteral("time.nist.gov"));
        auto* save = rig.find<QPushButton>("ntpSourceSave");
        check(save->isEnabled(), "the form is valid");
        for (int i = 0; i < 10; ++i) { rig.source(QStringLiteral("s%1").arg(i)); }
        check(!save->isEnabled() && rig.find<QLabel>("ntpSourceIssue")->text().contains(QLatin1String("at most 10")),
              "a list filled by another client disables Save with the reason");
    }
    {
        PageRig rig;
        auto* add = rig.find<QPushButton>("ntpAddSource");
        auto* id = rig.find<QLineEdit>("ntpSourceId");
        auto* address = rig.find<QLineEdit>("ntpSourceAddress");
        auto* save = rig.find<QPushButton>("ntpSourceSave");
        message = rig.find<QLabel>("ntpSourcesMessage");
        add->click();
        id->setText(QStringLiteral("nist"));
        address->setText(QStringLiteral("time.nist.gov"));
        save->click();
        add->click();
        id->setText(QStringLiteral("nist"));
        check(!save->isEnabled() && rig.find<QLabel>("ntpSourceIssue")->text().contains(QLatin1String("being added")),
              "an id whose create is still in flight cannot be added twice");
        rig.find<QPushButton>("ntpSourceCancel")->click();
        rig.refuse(QString());
        check(message->text() == QLatin1String("The radio did not add nist: no reason given"),
              "a refusal without a reason does not end in a bare colon");
        add->click();
        id->setText(QStringLiteral("nist2"));
        address->setText(QStringLiteral("time.nist.gov"));
        save->click();
        rig.backend->replyLast({RadioSetupReply::Kind::Failed, -1, QStringLiteral("no radio response")});
        check(message->text().contains(QLatin1String("did not answer")),
              "a timeout says the radio did not answer, not that it refused");
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
    replaceSequence();
    page();
    pageSafeguards();
    std::printf("%s (%d failure%s)\n", failures ? "FAILED" : "ok", failures, failures == 1 ? "" : "s");
    return failures ? 1 : 0;
}
