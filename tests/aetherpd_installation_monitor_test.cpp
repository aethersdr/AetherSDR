// Socket-free installation observation: our API, injected replies and clock.
#include "AetherPDTestNetwork.h"
#include "core/AetherPDInstallationMonitor.h"

#include <QCryptographicHash>
#include <QJsonDocument>
#include <QJsonObject>
#include <QtEndian>

#include <array>
#include <limits>

using namespace AetherSDR::aetherpd;
using Monitor = AetherSDR::AetherPDInstallationMonitor;
using MonitorState = Monitor::State;
using Reason = Monitor::Reason;

namespace {
QByteArray approvedPlan()
{
    QByteArray result("PDUPGR01");
    for (const char digest : {char(1), char(9)}) {
        QByteArray part(140, '\0');
        part.replace(0, 8, "PDAPRV02");
        qToBigEndian<quint32>(1, part.data() + 8);
        part.replace(16, 32, QByteArray(32, digest));
        part.replace(48, 12, "4.3.16.42034");
        part.replace(76, 12, "fixture-8600");
        result += part;
    }
    return result + QByteArray(32, 2) + QByteArray(32, 3);
}
QJsonObject observation(const InstallationPlan& plan, bool committed = true)
{
    return {{"state", committed ? "committed" : "rolled_back"}, {"installation_committed", committed},
            {"upgrade_plan_blake2b_256", plan.planDigest},
            {"selected_release_blake2b_256", committed ? plan.candidateDigest : plan.previousDigest},
            {"selected_release_key", committed ? plan.candidateKey : plan.previousKey},
            {"profile", int(plan.profile)}, {"firmware", plan.firmware}, {"serial", plan.serial}};
}
QByteArray encode(const QJsonObject& json) { return QJsonDocument(json).toJson(QJsonDocument::Compact); }
const QHostAddress address(QStringLiteral("192.168.50.100"));
const QString token(43, u'a');

bool parserChecks()
{
    bool ok = true;
    const QByteArray bytes = approvedPlan();
    const std::optional<InstallationPlan> plan = parseInstallationPlan(bytes);
    if (!check(plan.has_value(), "valid native-format approval parsed")) { return false; }
    ok &= check(bytes.size() == 352 && plan->profile == 1 && plan->firmware == QStringLiteral("4.3.16.42034")
        && plan->serial == QStringLiteral("fixture-8600"), "exact approval layout");
    for (qsizetype size = 0; size < bytes.size(); ++size) {
        ok &= check(!parseInstallationPlan(bytes.first(size)), "truncated approval refused");
    }
    ok &= check(!parseInstallationPlan(bytes + 'x'), "trailing approval bytes refused");
    for (const int offset : {0, 8, 16, 20, 48 + 8, 139 + 8, 148, 156, 160, 224, 287}) {
        QByteArray bad = bytes;
        bad[offset] = char(0xff);
        ok &= check(!parseInstallationPlan(bad), "invalid or mismatched approval identity refused");
    }
    for (const int offset : {24, 164, 288, 320}) {
        QByteArray bad = bytes;
        bad.replace(offset, 32, QByteArray(32, '\0'));
        ok &= check(!parseInstallationPlan(bad), "zero release or key refused");
    }
    QByteArray same = bytes;
    same.replace(164, 32, bytes.mid(24, 32));
    ok &= check(!parseInstallationPlan(same), "unchanged release refused");
    for (const bool committed : {true, false}) {
        const std::optional<InstallationObservation> observed = parseInstallationObservation(encode(observation(*plan, committed)));
        ok &= check(observed && matchInstallation(*plan, *observed)
                    == (committed ? InstallationMatch::Committed : InstallationMatch::RolledBack), "exact terminal observation matched");
    }
    QFile nativePlan(QStringLiteral(AETHERPD_INSTALLATION_TESTDATA_DIR "/approved.hex"));
    ok &= check(nativePlan.open(QIODevice::ReadOnly) && QByteArray::fromHex(nativePlan.readAll().trimmed()) == bytes,
                "approval matches native publication fixture bytes");
    for (const QString& state : {QStringLiteral("committed"), QStringLiteral("rolled_back")}) {
        QFile fixture(QStringLiteral(AETHERPD_INSTALLATION_TESTDATA_DIR "/") + state + QStringLiteral(".json"));
        ok &= check(fixture.open(QIODevice::ReadOnly), "native-to-Go JSON fixture readable");
        const std::optional<InstallationObservation> native = parseInstallationObservation(fixture.readAll());
        ok &= check(native && matchInstallation(*plan, *native)
                    == (state == QStringLiteral("committed") ? InstallationMatch::Committed : InstallationMatch::RolledBack),
                    "native publisher through Go API encoding matches Qt observation and exact BLAKE2b plan hash");
    }
    const QJsonObject good = observation(*plan);
    for (const QString& key : good.keys()) {
        QJsonObject bad = good;
        bad.remove(key);
        ok &= check(!parseInstallationObservation(encode(bad)), "missing terminal field refused");
        bad = good; bad[key] = QJsonValue::Null;
        ok &= check(!parseInstallationObservation(encode(bad)), "null terminal field refused");
    }
    for (const auto& [key, value] : std::array<std::pair<QString, QJsonValue>, 11>{{
        {"installation_committed", false}, {"state", "unavailable"}, {"profile", 1.5}, {"profile", 3},
        {"firmware", "4.3.16"}, {"serial", "wrong\nserial"}, {"upgrade_plan_blake2b_256", QString(64, u'0')},
        {"selected_release_key", QString(64, u'Z')}, {"selected_release_blake2b_256", QString(63, u'a')},
        {"extra", true}, {"installation_committed", "true"}}}) {
        QJsonObject bad = good; bad[key] = value;
        ok &= check(!parseInstallationObservation(encode(bad)), "invalid terminal field refused");
    }
    QByteArray duplicate = encode(good); duplicate.chop(1); duplicate += ",\"state\":\"committed\"}";
    ok &= check(!parseInstallationObservation(duplicate), "duplicate terminal field refused");
    duplicate = encode(good); duplicate.chop(1); duplicate += ",\"st\\u0061te\":\"committed\"}";
    ok &= check(!parseInstallationObservation(duplicate), "escaped duplicate terminal field refused");
    for (const QString& key : {QStringLiteral("profile"), QStringLiteral("firmware"), QStringLiteral("serial"),
                               QStringLiteral("selected_release_key"), QStringLiteral("selected_release_blake2b_256")}) {
        QJsonObject bad = good;
        if (key == QStringLiteral("profile")) { bad[key] = 2; }
        else if (key == QStringLiteral("firmware")) { bad[key] = "4.3.16.42035"; }
        else if (key == QStringLiteral("serial")) { bad[key] = "another-radio"; }
        else { bad[key] = QString(64, u'f'); }
        const std::optional<InstallationObservation> observed = parseInstallationObservation(encode(bad));
        ok &= check(observed && matchInstallation(*plan, *observed) == InstallationMatch::Mismatch, "wrong target/release/key never matches");
    }
    QJsonObject prior = good; prior[QStringLiteral("upgrade_plan_blake2b_256")] = QString(64, u'e');
    const std::optional<InstallationObservation> old = parseInstallationObservation(encode(prior));
    ok &= check(old && matchInstallation(*plan, *old) == InstallationMatch::OtherPlan, "previous plan remains pending");
    return ok;
}

bool firstInstallationChecks()
{
    bool ok = true;
    QFile fixture(QStringLiteral(AETHERPD_INSTALLATION_TESTDATA_DIR "/first-approved.hex"));
    if (!check(fixture.open(QIODevice::ReadOnly), "Python first-install plan fixture readable")) { return false; }
    const QByteArray bytes = QByteArray::fromHex(fixture.readAll().trimmed());
    const QByteArray publisherKey(32, char(3));
    for (const QString& identityToken : {token, QString(64, u'a')}) {
        QByteArray identityPlan = bytes;
        identityPlan.replace(148, 32, QCryptographicHash::hash(identityToken.toLatin1(), QCryptographicHash::Sha256));
        Network identityNetwork; Monitor identityMonitor(identityNetwork, [] { return 0; });
        ok &= check(identityMonitor.beginFirstInstallation(address, identityToken, identityPlan, publisherKey),
                    "base64url and first-install hex identity tokens accepted");
        identityMonitor.cancel();
    }
    for (const QString& invalidToken : {QString(63, u'a'), QString(65, u'a'), QString(64, u'g'),
                                       QString(64, u'A'), QString(64, u'_'), QString(63, u'a') + u'\n'}) {
        QByteArray identityPlan = bytes;
        identityPlan.replace(148, 32, QCryptographicHash::hash(invalidToken.toLatin1(), QCryptographicHash::Sha256));
        Network identityNetwork; Monitor identityMonitor(identityNetwork, [] { return 0; });
        ok &= check(!identityMonitor.beginFirstInstallation(address, invalidToken, identityPlan, publisherKey)
            && identityNetwork.sent.empty(), "malformed token refused even when its hash matches approval");
    }
    const auto plan = parseFirstInstallationPlan(bytes, publisherKey);
    if (!check(plan.has_value(), "first-install plan parsed with independent publisher key")) { return false; }
    ok &= check(plan->firstInstall && plan->previousDigest.isEmpty() && plan->previousKey.isEmpty()
        && plan->candidateKey == QString::fromLatin1(publisherKey.toHex()), "fresh plan has no predecessor");
    ok &= check(!parseInstallationPlan(bytes) && !installationAttempt(bytes, QByteArray(32, 'x'))
        && !parseFirstInstallationPlan(approvedPlan(), publisherKey), "upgrade and fresh plans never convert implicitly");
    for (qsizetype size = 0; size < bytes.size(); ++size) {
        ok &= check(!parseFirstInstallationPlan(bytes.first(size), publisherKey), "truncated first plan refused");
    }
    ok &= check(!parseFirstInstallationPlan(bytes + 'x', publisherKey)
        && !parseFirstInstallationPlan(bytes, QByteArray(32, '\0'))
        && !parseFirstInstallationPlan(bytes, QByteArray(31, 'x')), "invalid length and publisher key refused");
    for (const int offset : {0, 8, 16, 19, 20, 56, 83, 84, 147, 180, 243, 276, 307}) {
        QByteArray bad = bytes; bad[offset] = char(0xff);
        ok &= check(!parseFirstInstallationPlan(bad, publisherKey), "malformed first-install target/hostname/padding refused");
    }
    for (const int offset : {24, 148, 244}) {
        QByteArray bad = bytes; bad.replace(offset, 32, QByteArray(32, '\0'));
        ok &= check(!parseFirstInstallationPlan(bad, publisherKey), "zero release/identity/attempt refused");
    }
    QFile response(QStringLiteral(AETHERPD_INSTALLATION_TESTDATA_DIR "/first-committed.json"));
    if (!check(response.open(QIODevice::ReadOnly), "first commitment fixture readable")) { return false; }
    const QByteArray committed = response.readAll();
    const auto observed = parseInstallationObservation(committed);
    if (!check(observed && observed->firstInstall && matchInstallation(*plan, *observed) == InstallationMatch::Committed,
               "Python plan digest and first commitment agree")) { return false; }
    const QJsonObject good = QJsonDocument::fromJson(committed).object();
    for (const QString& key : good.keys()) {
        QJsonObject bad = good; bad.remove(key);
        ok &= check(!parseInstallationObservation(encode(bad)), "first commitment requires every field");
    }
    for (const QString& key : {QStringLiteral("selected_release_key"), QStringLiteral("selected_release_blake2b_256")}) {
        QJsonObject bad = good; bad[key] = QString(64, u'f');
        const auto wrong = parseInstallationObservation(encode(bad));
        ok &= check(wrong && matchInstallation(*plan, *wrong) == InstallationMatch::Mismatch,
                    "first commitment never trusts substituted publisher or release");
    }
    QJsonObject bad = good; bad[QStringLiteral("profile")] = 2;
    ok &= check(!parseInstallationObservation(encode(bad)), "unsupported fresh profile refused");
    bad = good; bad[QStringLiteral("installation_committed")] = false; bad[QStringLiteral("state")] = "rolled_back";
    ok &= check(!parseInstallationObservation(encode(bad)), "fresh plan cannot roll back to an absent predecessor");
    bad = good; bad[QStringLiteral("upgrade_plan_blake2b_256")] = plan->planDigest;
    ok &= check(!parseInstallationObservation(encode(bad)), "ambiguous fresh and upgrade commitment refused");
    bad = good; bad[QStringLiteral("upgrade_plan_blake2b_256")] = bad.take(QStringLiteral("first_install_plan_blake2b_256"));
    const auto wrongKind = parseInstallationObservation(encode(bad));
    ok &= check(wrongKind && matchInstallation(*plan, *wrongKind) == InstallationMatch::OtherPlan,
                "upgrade commitment cannot satisfy fresh install even with matching digest");
    qint64 clock = 0; Network net; Monitor monitor(net, [&] { return clock; });
    ok &= check(!monitor.beginFirstInstallation(address, QString(43, u'b'), bytes, publisherKey)
        && !monitor.beginFirstInstallation(address, token, bytes, QByteArray()) && net.sent.empty(),
        "mismatched identity token and missing publisher refuse before network access");
    ok &= check(monitor.beginFirstInstallation(address, token, bytes, publisherKey), "fresh monitor began");
    net.last->complete(503, "{}");
    clock = 500; monitor.poll(); net.last->complete(200, encode(bad));
    ok &= check(monitor.state() == MonitorState::Waiting, "other transaction kind stays pending");
    bad = good; bad[QStringLiteral("first_install_plan_blake2b_256")] = QString(64, u'e');
    clock = 1000; monitor.poll(); net.last->complete(200, encode(bad));
    ok &= check(monitor.state() == MonitorState::Waiting, "earlier fresh attempt stays pending");
    clock = 1500; monitor.poll(); net.last->complete(200, committed);
    ok &= check(monitor.state() == MonitorState::Committed, "exact fresh attempt confirmed after restart");
    for (const Network::Sent& sent : net.sent) {
        ok &= check(sent.method == QNetworkAccessManager::GetOperation && sent.body.isEmpty(),
                    "fresh observation never submits installation or RF commands");
    }
    return ok;
}

bool monitorChecks()
{
    bool ok = true;
    const QByteArray bytes = approvedPlan();
    const std::optional<InstallationPlan> plan = parseInstallationPlan(bytes);
    if (!plan) { return false; }
    const QByteArray committed = encode(observation(*plan));
    {
        const auto first = installationAttempt(bytes, QByteArray(32, char(4)));
        const auto second = installationAttempt(bytes, QByteArray(32, char(5)));
        if (!check(first && second, "distinct attempts encoded")) { return false; }
        const auto firstPlan = parseInstallationPlan(*first);
        const auto secondPlan = parseInstallationPlan(*second);
        if (!check(firstPlan && secondPlan, "attempt plans parsed")) { return false; }
        QFile attemptFixture(QStringLiteral(AETHERPD_INSTALLATION_TESTDATA_DIR "/approved-attempt.hex"));
        ok &= check(attemptFixture.open(QIODevice::ReadOnly)
            && QByteArray::fromHex(attemptFixture.readAll().trimmed()) == *second, "Python and Qt encode identical attempts");
        for (const int phase : {4, 7}) {
            QFile mirror(QStringLiteral(AETHERPD_INSTALLATION_TESTDATA_DIR "/attempt-%1.hex").arg(phase));
            ok &= check(mirror.open(QIODevice::ReadOnly), "native attempt mirror fixture readable");
            const QByteArray raw = QByteArray::fromHex(mirror.readAll().trimmed());
            ok &= check(raw.size() == 204 && raw.startsWith("PDVIEW01")
                && raw.mid(16, 32).toHex() == secondPlan->planDigest.toLatin1(), "native publication and Qt bind identical attempt digest");
        }
        ok &= check(firstPlan->candidateDigest == secondPlan->candidateDigest
            && firstPlan->previousDigest == secondPlan->previousDigest
            && firstPlan->planDigest != secondPlan->planDigest, "same releases have distinct transaction identities");
        ok &= check(!installationAttempt(bytes, QByteArray(32, '\0'))
            && !installationAttempt(bytes, QByteArray(31, 'x'))
            && !installationAttempt("bad", QByteArray(32, 'x')), "invalid attempt inputs refused");
        for (const int offset : {0, 8, 16, 20, 88, 147, 148, 156, 160, 287}) {
            QByteArray bad = *second; bad[offset] = char(0xff);
            ok &= check(!parseInstallationPlan(bad), "malformed attempt identity/padding refused");
        }
        QByteArray zero = *second; zero.replace(56, 32, QByteArray(32, '\0'));
        ok &= check(!parseInstallationPlan(zero), "zero attempt refused");
        qint64 clock = 0;
        Network net;
        Monitor monitor(net, [&] { return clock; });
        ok &= check(monitor.begin(address, token, *second), "new attempt monitor began");
        net.last->complete(200, encode(observation(*firstPlan, false)));
        ok &= check(monitor.state() == MonitorState::Waiting, "same-package prior rollback cannot finish new attempt");
        clock = 500; monitor.poll();
        net.last->complete(200, encode(observation(*firstPlan, true)));
        ok &= check(monitor.state() == MonitorState::Waiting, "same-package prior commit cannot finish new attempt");
        clock = 1000; monitor.poll();
        net.last->complete(200, encode(observation(*secondPlan)));
        ok &= check(monitor.state() == MonitorState::Committed, "only current attempt commits");
    }
    {
        qint64 clock = 0;
        Network net;
        Monitor monitor(net, [&] { return clock; });
        int finished = 0;
        QObject::connect(&monitor, &Monitor::finished, &monitor, [&](MonitorState, Reason) { ++finished; });
        ok &= check(!monitor.begin(QHostAddress(QStringLiteral("8.8.8.8")), token, bytes)
                    && !monitor.begin(address, QStringLiteral("bad-token"), bytes)
                    && !monitor.begin(address, token, QByteArray("bad")) && net.sent.empty(), "invalid inputs never disclose token");
        ok &= check(monitor.begin(address, token, bytes, 5000), "monitor began");
        ok &= check(!monitor.begin(address, token, bytes), "cannot replace active transaction");
        net.last->complete(0, "", QNetworkReply::RemoteHostClosedError);
        monitor.poll();
        ok &= check(net.sent.size() == 1, "no immediate retry loop");
        clock = 500; monitor.poll(); net.last->complete(503, R"({"state":"unavailable","installation_committed":false})");
        clock = 1000; monitor.poll(); net.last->complete(404, "{}");
        clock = 1500; monitor.poll();
        QJsonObject stale = observation(*plan); stale[QStringLiteral("upgrade_plan_blake2b_256")] = QString(64, u'e');
        net.last->complete(200, encode(stale));
        ok &= check(monitor.state() == MonitorState::Waiting && finished == 0, "old successful transaction is not this commit");
        clock = 2000; monitor.poll(); net.last->complete(200, committed);
        ok &= check(monitor.state() == MonitorState::Committed && finished == 1, "restart followed by exact commit confirmed");
        const size_t count = net.sent.size(); clock = 10000; monitor.poll();
        ok &= check(net.sent.size() == count && !monitor.begin(address, token, bytes), "terminal observation never restarts");
        for (const Network::Sent& sent : net.sent) {
            ok &= check(sent.method == QNetworkAccessManager::GetOperation && sent.body.isEmpty()
                && sent.request.url() == QUrl(QStringLiteral("http://192.168.50.100:48992/v1/aetherpd/installation"))
                && sent.request.rawHeader("Authorization") == "Bearer " + token.toLatin1()
                && sent.request.attribute(QNetworkRequest::RedirectPolicyAttribute).toInt() == QNetworkRequest::ManualRedirectPolicy
                && sent.request.attribute(QNetworkRequest::CacheLoadControlAttribute).toInt() == QNetworkRequest::AlwaysNetwork
                && !sent.request.attribute(QNetworkRequest::CacheSaveControlAttribute).toBool(), "all retries fixed uncached GET without redirects");
        }
    }
    for (const auto& [code, reason] : std::array<std::pair<int, Reason>, 4>{{{401, Reason::Unauthorized}, {403, Reason::Unauthorized}, {302, Reason::InvalidReply}, {500, Reason::InvalidReply}}}) {
        qint64 clock = 0; Network net; Monitor monitor(net, [&] { return clock; });
        monitor.begin(address, token, bytes); net.last->complete(code, committed);
        clock = 5000; monitor.poll();
        ok &= check(monitor.state() == MonitorState::Unconfirmed && monitor.reason() == reason && net.sent.size() == 1, "HTTP failures neither succeed nor install");
    }
    for (const bool rollback : {true, false}) {
        qint64 clock = 0; Network net; Monitor monitor(net, [&] { return clock; });
        monitor.begin(address, token, bytes);
        QJsonObject result = observation(*plan, !rollback);
        if (!rollback) { result[QStringLiteral("serial")] = "wrong-radio"; }
        net.last->complete(200, encode(result));
        ok &= check(monitor.state() == (rollback ? MonitorState::RolledBack : MonitorState::Unconfirmed)
                    && monitor.reason() == (rollback ? Reason::None : Reason::Mismatch), "rollback and wrong-radio result distinguished");
    }
    for (const QByteArray& bad : {QByteArray("{}"), QByteArray(2049, ' ')}) {
        qint64 clock = 0; Network net; Monitor monitor(net, [&] { return clock; });
        monitor.begin(address, token, bytes); net.last->complete(200, bad);
        ok &= check(monitor.state() == MonitorState::Unconfirmed && monitor.reason() == Reason::InvalidReply, "malformed or oversized reply refuses");
    }
    {
        qint64 clock = 0; Network net; Monitor monitor(net, [&] { return clock; });
        monitor.begin(address, token, bytes, 6000); Reply* old = net.last;
        clock = 2500; monitor.poll();
        ok &= check(old->aborted && monitor.state() == MonitorState::Waiting, "stalled request abandoned without ending observation");
        clock = 3000; monitor.poll(); old->complete(200, committed);
        ok &= check(monitor.state() == MonitorState::Waiting, "late reply from aborted request ignored");
        net.last->complete(200, committed, QNetworkReply::RemoteHostClosedError);
        ok &= check(monitor.state() == MonitorState::Waiting, "valid-looking truncated response not proof");
        clock = 3500; monitor.poll(); clock = 6000; net.last->complete(200, committed);
        ok &= check(monitor.state() == MonitorState::Unconfirmed && monitor.reason() == Reason::Deadline, "late terminal response cannot extend deadline");
    }
    {
        qint64 clock = 0; Network net; Monitor monitor(net, [&] { return clock; });
        monitor.begin(address, token, bytes, 1000); clock = 1000; monitor.poll();
        ok &= check(net.last->aborted && monitor.state() == MonitorState::Unconfirmed, "absolute deadline aborts pending request");
    }
    {
        qint64 clock = 0; Network net; Monitor monitor(net, [&] { return clock; });
        monitor.begin(address, token, bytes); Reply* old = net.last; monitor.cancel(); old->complete(200, committed);
        clock = 5000; monitor.poll();
        ok &= check(old->aborted && monitor.state() == MonitorState::Canceled && net.sent.size() == 1, "local cancel sends no remote cancellation or installation");
    }
    {
        qint64 clock = 10; Network net; Monitor monitor(net, [&] { return clock; });
        monitor.begin(address, token, bytes); clock = 9; monitor.poll();
        ok &= check(monitor.state() == MonitorState::Unconfirmed && monitor.reason() == Reason::InvalidClock, "clock regression fails closed");
    }
    {
        Network net;
        Monitor monitor(net, [] { return std::numeric_limits<qint64>::max(); });
        ok &= check(!monitor.begin(address, token, bytes) && net.sent.empty(), "clock overflow refuses before request");
    }
    {
        qint64 clock = 0; std::unique_ptr<Network> net = std::make_unique<Network>();
        Monitor monitor(*net, [&] { return clock; });
        monitor.begin(address, token, bytes); net.reset();
        ok &= check(monitor.state() == MonitorState::Unconfirmed && monitor.reason() == Reason::TransportDestroyed, "transport destruction observed");
    }
    {
        qint64 clock = 0; Network net;
        std::unique_ptr<Monitor> monitor = std::make_unique<Monitor>(net, [&] { return clock; });
        monitor->begin(address, token, bytes); monitor.reset();
        ok &= check(net.last && net.last->aborted, "destruction abandons only its read");
    }
    {
        qint64 clock = 0; Network net;
        std::unique_ptr<Monitor> monitor = std::make_unique<Monitor>(net, [&] { return clock; });
        QObject::connect(monitor.get(), &Monitor::finished, &net, [&](MonitorState, Reason) { monitor.reset(); });
        monitor->begin(address, token, bytes); net.last->complete(200, committed);
        ok &= check(!monitor, "completion observer may destroy monitor");
    }
    {
        qint64 clock = 0; Network net;
        std::unique_ptr<Monitor> monitor = std::make_unique<Monitor>(net, [&] { return clock; });
        monitor->begin(address, token, bytes);
        QObject::connect(net.last, &QNetworkReply::finished, &net, [&] { monitor.reset(); });
        monitor->cancel();
        ok &= check(!monitor, "abort observer may destroy monitor during cancellation");
    }
    return ok;
}
} // namespace
int main(int argc, char** argv)
{
    QCoreApplication app(argc, argv);
    const bool parser = parserChecks();
    const bool monitor = monitorChecks();
    const bool first = firstInstallationChecks();
    return parser && monitor && first ? 0 : 1;
}
