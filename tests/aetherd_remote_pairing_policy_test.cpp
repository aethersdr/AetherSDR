// Socket-free advertisement parsing and desktop pairing and persistence rules.
#include "TestSettingsProfile.h"
#include "core/AppSettings.h"
#include "core/backends/aetherd/RemoteEngineBackend.h"
#include "core/backends/aetherd/RemoteEngineDiscovery.h"
#include "core/backends/aetherd/RemoteEnginePairing.h"
#include "core/backends/aetherd/RemoteEngineStore.h"
#include "core/control/ControlCredentials.h"
#include "core/control/ControlPairing.h"

#include <QCoreApplication>
#include <QJsonDocument>
#include <QJsonObject>

#include <cstdio>

using namespace AetherSDR;
using namespace AetherSDR::aetherd;
using namespace AetherSDR::control;

namespace {

int failures = 0;

void check(bool condition, const char* message)
{
    std::printf("[%s] %s\n", condition ? "PASS" : "FAIL", message);
    failures += !condition;
}

const QString kFingerprint = QStringLiteral("ab").repeated(32);

QByteArray reply(const QJsonObject& object) { return QJsonDocument(object).toJson(QJsonDocument::Compact); }

void advertisements()
{
    const QHostAddress sender(QStringLiteral("192.168.50.216"));
    const QJsonObject good{{"aetherd", 1}, {"name", "g2-alarm"},
        {"radio", QJsonObject{{"model", "ANAN-G2"}, {"family", "anan"}, {"connected", true}}},
        {"wss", "wss://192.168.50.216:8443"}, {"certificateSha256", kFingerprint}};
    const auto advert = RemoteEngineDiscovery::parseReply(reply(good), sender);
    check(advert && advert->name == "g2-alarm" && advert->model == "ANAN-G2" && advert->radioConnected
              && advert->port == 8443 && advert->certificateSha256 == kFingerprint,
          "an advertisement names the engine, its radio and its listener");
    check(RemoteEngineDiscovery::parseReply(reply(good), QHostAddress("::ffff:192.168.50.216")).has_value(),
          "an IPv4-mapped sender is the same machine");
    check(!RemoteEngineDiscovery::parseReply(reply(good), QHostAddress("192.168.50.99")),
          "a reply naming another machine's listener is refused (no redirect)");
    QJsonObject bad = good;
    bad["certificateSha256"] = "xyz";
    check(!RemoteEngineDiscovery::parseReply(reply(bad), sender), "a malformed fingerprint is refused");
    bad = good;
    bad["wss"] = "ws://192.168.50.216:8443";
    check(!RemoteEngineDiscovery::parseReply(reply(bad), sender), "a plain ws:// listener is never advertised");
    bad["wss"] = "wss://192.168.50.216:8443#fragment";
    check(!RemoteEngineDiscovery::parseReply(reply(bad), sender), "an advertised URL cannot have a fragment");
    bad = good;
    bad["aetherd"] = 2;
    check(!RemoteEngineDiscovery::parseReply(reply(bad), sender), "another version is ignored");
    bad = good;
    bad["name"] = QString(2000, u'x');
    check(!RemoteEngineDiscovery::parseReply(reply(bad), sender), "a reply over 1 KiB is ignored");
}

void storeRules()
{
    check(RemoteEngineStore::validLanUrl("wss://192.168.50.216:8443"), "a LAN address is wss://<IP>:<port>");
    check(!RemoteEngineStore::validLanUrl("ws://192.168.50.216:8443")
              && !RemoteEngineStore::validLanUrl("wss://g2.local:8443")
              && !RemoteEngineStore::validLanUrl("wss://192.168.50.216")
              && !RemoteEngineStore::validLanUrl("wss://192.168.50.216:8443/x")
              && !RemoteEngineStore::validLanUrl("wss://u@192.168.50.216:8443"),
          "never ws://, a host name, a missing port, a path or user info");
    check(RemoteEngineStore::validTailnetUrl("ws://100.119.109.89:8443")
              && !RemoteEngineStore::validTailnetUrl("wss://100.119.109.89:8443"),
          "a tailnet address is ws://<IP>:<port>");
    RemoteEngineRecord record{kFingerprint, "g2-alarm", "wss://192.168.50.216:8443", {}, QString(64, u'7')};
    RemoteEngineStore::save(record);
    const auto stored = RemoteEngineStore::record(kFingerprint);
    check(stored && stored->name == "g2-alarm" && stored->token.isEmpty(), "the stored record carries no token");
    const QString settings = AppSettings::instance().value("RemoteEngines").toString();
    check(!settings.isEmpty() && !settings.contains(QString(64, u'7')), "the token never reaches settings");
    const auto session = RemoteEngineCredentials::session(kFingerprint);
    check(session && session->token == QString(64, u'7'), "saving primes the connect path's session cache");
    check(!RemoteEngineStore::setTailnetUrl(kFingerprint, "wss://100.119.109.89:8443")
              && RemoteEngineStore::setTailnetUrl(kFingerprint, "ws://100.119.109.89:8443")
              && RemoteEngineStore::record(kFingerprint)->tailnetUrl == "ws://100.119.109.89:8443"
              && RemoteEngineCredentials::session(kFingerprint)->tailnetUrl == "ws://100.119.109.89:8443",
          "a tailnet address joins an engine already paired");
    RadioConnectRequest request;
    request.serial = kFingerprint + RemoteEngineCredentials::kTailnetSuffix;
    check(RemoteEngineBackend::recordFor(request).has_value() && RemoteEngineCredentials::tailnetSerial(request.serial)
              && RemoteEngineCredentials::engineIdOf(request.serial) == kFingerprint,
          "the tailnet entry resolves to the same engine");
    record.lanUrl = QStringLiteral("wss://192.168.50.217:8443");
    record.token = QString(64, u'8');
    RemoteEngineStore::save(record);
    const auto repaired = RemoteEngineStore::record(kFingerprint);
    const auto repairedSession = RemoteEngineCredentials::session(kFingerprint);
    check(repaired && repaired->lanUrl == record.lanUrl && repaired->token.isEmpty()
              && repaired->tailnetUrl == "ws://100.119.109.89:8443"
              && repairedSession && repairedSession->token == record.token
              && repairedSession->tailnetUrl == repaired->tailnetUrl,
          "re-pairing refreshes the LAN URL and token while keeping the Tailnet address");
    RemoteEngineStore::forget(kFingerprint);
    check(!RemoteEngineStore::record(kFingerprint) && !RemoteEngineCredentials::session(kFingerprint),
          "forget removes the record and the session token");
}

void pairingName()
{
    const QString fits = QString(30, u'a') + QStringLiteral("é");
    const QString split = QString(31, u'a') + QStringLiteral("é");
    check(RemoteEnginePairing::boundedClientName(fits) == fits,
          "a complete UTF-8 character at the byte limit is kept");
    const QString bounded = RemoteEnginePairing::boundedClientName(split);
    check(bounded == QString(31, u'a') && ControlCredentials::validName(bounded),
          "truncating a multibyte hostname leaves a daemon-valid name");
    check(RemoteEnginePairing::boundedClientName(QStringLiteral("name\nwith control")) == "AetherSDR",
          "a disallowed character falls back to a valid name");
    check(RemoteEnginePairing::codeMatchesCertificate(
              "123456" + ControlPairing::checkDigits("123456", kFingerprint), kFingerprint),
          "a code carries its certificate's check digits");
}

} // namespace

int main(int argc, char* argv[])
{
    TestSettingsProfile profile(QStringLiteral("aetherd-remote-pairing-policy"));
    if (!profile.isValid()) { return 1; }
    QCoreApplication app(argc, argv);
    AppSettings::instance().load();
    check(!RemoteEngineStore::persistentStoreAvailable(), "test profiles leave the OS keychain untouched");
    advertisements();
    storeRules();
    pairingName();
    return failures ? 1 : 0;
}
