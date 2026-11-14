// Remote-client pairing (aetherd-remote-clients.md §3), socket-free: the code
// and its check digits, expiry, single use and the attempt limit; pairing.begin
// only on the local socket with control; pairing.complete only before hello on
// a TLS network session, answered through a deferred reply; the issued token
// then authenticates a network session with control and no transmit; v2 vault
// records carry the client's name; adding a credential retires nobody.
#include "core/control/ControlCredentialVault.h"
#include "core/control/ControlInputPump.h"
#include "core/control/ControlPairing.h"
#include "core/control/ControlService.h"
#ifdef HAVE_WEBSOCKETS
#include "core/control/RemoteControlServer.h"
#endif

#include <QCoreApplication>
#include <QJsonArray>
#include <QJsonDocument>
#include <QTimer>
#include <cstdio>

using namespace AetherSDR::control;

namespace {
int failures = 0;
void check(bool condition, const char* message)
{
    std::printf("[%s] %s\n", condition ? "PASS" : "FAIL", message);
    failures += !condition;
}
QString error(const QJsonObject& reply) { return reply.value("error").toObject().value("code").toString(); }
QJsonObject result(const QJsonObject& reply) { return reply.value("result").toObject(); }
const QString kFingerprint = QStringLiteral("ab").repeated(32);

bool advertises(const QJsonObject& caps, const QString& method)
{
    return caps.value("capabilities").toArray().contains(method);
}

// Mints like VaultPairingIssuer, minus the OS vault: the record goes straight
// into the live verifier. Completion is held until release(), so the test can
// observe the connection while the reply is owed.
class FakeIssuer final : public PairingIssuer {
public:
    explicit FakeIssuer(ControlCredentials& credentials) : m_credentials(credentials) {}
    void issue(const QString& clientName, Done done) override
    {
        names.append(clientName);
        m_done = std::move(done);
        if (immediate) { release(*immediate); }   // answers before issue() returns
    }
    std::optional<bool> immediate;
    void release(bool succeed)
    {
        Done done = std::move(m_done);
        if (!succeed) { done(std::nullopt); return; }
        QByteArray secret;
        auto record = ControlCredentials::generateRemote(ControlCredentials::Role::RemoteControl, &secret);
        record.name = names.last();
        check(m_credentials.add(record), "the issued record joins the live verifier");
        done(Issued{record.id, QString::fromLatin1(secret.toHex())});
    }
    [[nodiscard]] bool owed() const { return bool(m_done); }
    QStringList names;
private:
    ControlCredentials& m_credentials;
    Done m_done;
};

struct Harness {
    ControlResourceStore store;
    ControlCredentials credentials;
    ControlService service{&store};
    FakeIssuer issuer{credentials};
    qint64 now{1000};
    Harness(bool bindPairing = true)
    {
        check(credentials.replace({}) && service.bindCredentials(&credentials), "bind the verifier");
        if (bindPairing) {
            check(service.bindPairing({&issuer, kFingerprint, [this] { return now; }}),
                  "bind pairing before dispatch");
        }
    }
    QJsonObject request(ControlSession& session, const QString& method, QJsonObject params = {},
                        bool withSession = true)
    {
        QJsonObject r{{"v", 1}, {"id", "t"}, {"method", method}, {"params", params}};
        if (withSession) { r.insert("sessionId", session.sessionId()); }
        return r;
    }
    ServiceReply send(ControlSession& session, const QJsonObject& r)
    {
        return service.handle(QJsonDocument(r).toJson(QJsonDocument::Compact), &session);
    }
    QJsonObject invoke(ControlSession& session, const QString& method, QJsonObject params = {})
    {
        return send(session, request(session, method, params, method != "hello")).message;
    }
    QJsonObject hello(ControlSession& session, const QString& token = {})
    {
        QJsonObject p{{"versions", QJsonArray{1}}};
        if (!token.isEmpty()) { p.insert("auth", QJsonObject{{"scheme", "bearer"}, {"token", token}}); }
        return invoke(session, "hello", p);
    }
    QString openPairing()
    {
        ControlSession local(&store, 1 << 20, SessionAuthorization::ObserverController);
        (void)hello(local);
        return result(invoke(local, "pairing.begin")).value("code").toString();
    }
};

// A network session behind a production input pump with scripted transport.
struct Wire {
    ControlSession session;
    QByteArray input;
    QList<QJsonObject> written;
    int closes{0};
    std::unique_ptr<ControlInputPump> pump;
    Wire(Harness& h, bool tls)
        : session(&h.store, 1 << 20, SessionAuthorization::NetworkScoped)
    {
        if (tls) { session.allowPairing(); }
        pump = std::make_unique<ControlInputPump>(h.service, session,
            [this](qint64 maximum) { const QByteArray out = input.left(maximum); input.remove(0, out.size()); return out; },
            [this](const QJsonObject& message) { written.append(message); return true; },
            [this](bool) { ++closes; });
    }
    void feed(const QJsonObject& request)
    {
        input.append(QJsonDocument(request).toJson(QJsonDocument::Compact));
        input.append('\n');
        pump->readAvailable();
    }
};

void drain()
{
    for (int i = 0; i < 4; ++i) { QCoreApplication::processEvents(); }
}
} // namespace

int main(int argc, char* argv[])
{
    QCoreApplication app(argc, argv);

    // ---- the code ----
    check(ControlPairing::checkDigits("123456", kFingerprint) == "4053"
              && ControlPairing::checkDigits("000000", QStringLiteral("0123456789abcdef").repeated(4)) == "8991"
              && ControlPairing::checkDigits("987654", QStringLiteral("FF").repeated(32)) == "2535",
          "check digits match an independent SHA-256 computation (fingerprint case-insensitive)");
    check(ControlPairing::digitsOf("123-456-7890") == QString("1234567890")
              && ControlPairing::digitsOf("123 456 7890") == QString("1234567890")
              && ControlPairing::digitsOf("1234567890") == QString("1234567890"),
          "dashes and spaces are accepted as separators");
    check(!ControlPairing::digitsOf("123-456-789") && !ControlPairing::digitsOf("123-456-78901")
              && !ControlPairing::digitsOf("123x456-7890") && !ControlPairing::digitsOf(QString::fromUtf8("١٢٣٤٥٦٧٨٩٠")),
          "a wrong length, a letter or non-ASCII digits are not a code");
    {
        ControlPairing pairing;
        pairing.randomSixForTest = [] { return QStringLiteral("123456"); };
        const auto opened = pairing.begin(kFingerprint, 0);
        check(opened.code == "123-456-4053" && opened.expiresInMs == ControlPairing::kLifetimeMs
                  && opened.certificateSha256 == kFingerprint,
              "begin() shows six random digits plus the four check digits");
        check(pairing.attempt("123-456-4054", 1) == ControlPairing::Attempt::Wrong
                  && pairing.attempt("1234564053", 2) == ControlPairing::Attempt::Accepted,
              "a wrong code is refused, the right one accepted");
        check(pairing.attempt("123-456-4053", 3) == ControlPairing::Attempt::NotOpen,
              "a code is single use");
        (void)pairing.begin(kFingerprint, 0);
        check(pairing.attempt("123-456-4053", ControlPairing::kLifetimeMs) == ControlPairing::Attempt::Expired
                  && !pairing.isOpen(0),
              "the code expires after five minutes and the pairing closes");
        (void)pairing.begin(kFingerprint, 0);
        for (int i = 0; i < ControlPairing::kMaxAttempts - 1; ++i) { (void)pairing.attempt("000-000-0000", 1); }
        check(pairing.isOpen(1), "four wrong codes leave the pairing open");
        (void)pairing.attempt("000-000-0000", 1);
        check(!pairing.isOpen(1) && pairing.attempt("123-456-4053", 1) == ControlPairing::Attempt::NotOpen,
              "the fifth wrong code ends the pairing, even for the right code after it");
        pairing.randomSixForTest = [] { return QStringLiteral("987654"); };
        (void)pairing.begin(kFingerprint, 0);
        check(pairing.attempt("987-654-" + ControlPairing::checkDigits("987654", kFingerprint), 1)
                  == ControlPairing::Attempt::Accepted,
              "a new begin() replaces the open pairing");
    }
    {
        ControlPairing pairing;
        QSet<QString> codes;
        for (int i = 0; i < 20; ++i) { codes.insert(pairing.begin(kFingerprint, 0).code); }
        check(codes.size() > 15, "production codes are random");
    }

    // ---- pairing.begin ----
    {
        Harness h;
        ControlSession local(&h.store, 1 << 20, SessionAuthorization::ObserverController);
        const QJsonObject caps = result(h.hello(local));
        check(advertises(caps, "pairing.begin"), "a local controller is advertised pairing.begin");
        const QJsonObject opened = result(h.invoke(local, "pairing.begin"));
        check(opened.value("code").toString().size() == 12 && opened.value("expiresInMs").toInteger() == 300000
                  && opened.value("certificateSha256").toString() == kFingerprint,
              "pairing.begin returns code, expiresInMs and the certificate fingerprint");
        check(error(h.invoke(local, "pairing.begin", {{"x", 1}})) == "request.invalid_params",
              "pairing.begin takes no parameters");

        ControlSession observer(&h.store, 1 << 20, SessionAuthorization::Observer);
        check(!advertises(result(h.hello(observer)), "pairing.begin")
                  && error(h.invoke(observer, "pairing.begin")) == "auth.grant_denied",
              "a local observer cannot start pairing");
    }
    {
        Harness h(false);
        ControlSession local(&h.store, 1 << 20, SessionAuthorization::ObserverController);
        check(!advertises(result(h.hello(local)), "pairing.begin")
                  && error(h.invoke(local, "pairing.begin")) == "capability.unavailable",
              "without a TLS listener there is no pairing");
    }

    // ---- pairing.complete, end to end through the input pump ----
    {
        Harness h;
        const QString code = h.openPairing();
        Wire wire(h, true);
        wire.feed(h.request(wire.session, "pairing.complete", {{"code", code}, {"clientName", " Shack laptop "}}, false));
        check(wire.written.isEmpty() && wire.pump->awaitingReply() && h.issuer.owed(),
              "a right code defers the reply while the credential is written");
        wire.feed(h.request(wire.session, "hello", {{"versions", QJsonArray{1}}}, false));
        check(wire.written.isEmpty(), "input waits while a deferred reply is owed");
        h.issuer.release(true);
        drain();
        check(wire.written.size() == 1 && wire.closes == 1 && !wire.pump->awaitingReply(),
              "the deferred reply is written once and the connection closes; queued input is not run");
        const QJsonObject issued = result(wire.written.value(0));
        const QString token = issued.value("token").toString();
        check(token.size() == 64 && issued.value("credentialId").toString().size() == 32
                  && h.issuer.names == QStringList{"Shack laptop"},
              "the reply carries credentialId and token; the name is trimmed");

        ControlSession remote(&h.store, 1 << 20, SessionAuthorization::NetworkScoped);
        const QJsonObject caps = result(h.hello(remote, token));
        check(caps.value("grants").toArray() == QJsonArray{"observe", "control"},
              "the paired token authenticates a network session with observe and control");
        bool anyTx = false;
        for (const QJsonValue& v : caps.value("capabilities").toArray()) { anyTx |= v.toString().startsWith("tx"); }
        check(!advertises(caps, "pairing.begin") && !anyTx,
              "a paired client can neither pair others nor transmit");
        check(error(h.invoke(remote, "pairing.begin")) == "auth.grant_denied",
              "pairing.begin is refused on the network");
        ControlSession localTry(&h.store, 1 << 20, SessionAuthorization::ObserverController);
        check(error(h.hello(localTry, token)) == "auth.invalid",
              "a paired token does not work on the local socket");
    }
    {
        Harness h;
        const QString code = h.openPairing();
        Wire wire(h, true);
        wire.feed(h.request(wire.session, "pairing.complete", {{"code", code}, {"clientName", "x"}}, false));
        h.issuer.release(false);
        drain();
        check(wire.written.size() == 1 && error(wire.written.value(0)) == "engine.failed" && wire.closes == 1,
              "a credential that could not be stored is reported, and the connection closes");
    }
    for (const bool succeed : {false, true}) {
        Harness h;
        h.issuer.immediate = succeed;
        const QString code = h.openPairing();
        Wire wire(h, true);
        wire.feed(h.request(wire.session, "pairing.complete", {{"code", code}, {"clientName", "x"}}, false));
        check(wire.written.size() == 1 && wire.closes == 1 && !wire.pump->awaitingReply()
                  && (succeed ? result(wire.written.value(0)).value("token").toString().size() == 64
                              : error(wire.written.value(0)) == "engine.failed"),
              succeed ? "an issuer that succeeds before issue() returns is answered at once"
                      : "an issuer that refuses before issue() returns is answered at once, not left hanging");
    }
    {
        // A credential-bound local session (the radio's panel, say) outlives
        // a completed pairing: adding a record retires nobody.
        Harness h;
        const auto panel = ControlCredentials::generate(ControlCredentials::Role::Client);
        check(h.credentials.replace({panel}), "load the panel's local identity");
        ControlSession local(&h.store, 1 << 20, SessionAuthorization::ObserverController);
        check(error(h.hello(local, QString::fromLatin1(panel.secret.toHex()))).isEmpty(),
              "the panel session binds its credential");
        const QString code = result(h.invoke(local, "pairing.begin")).value("code").toString();
        Wire wire(h, true);
        wire.feed(h.request(wire.session, "pairing.complete", {{"code", code}, {"clientName", "x"}}, false));
        h.issuer.release(true);
        drain();
        check(!result(wire.written.value(0)).value("token").toString().isEmpty() && !local.isRevoked()
                  && error(h.invoke(local, "capabilities.get")).isEmpty(),
              "the credential-bound local session survives the pairing");
    }
    {
        Harness h;
        const QString code = h.openPairing();
        auto wire = std::make_unique<Wire>(h, true);
        wire->feed(h.request(wire->session, "pairing.complete", {{"code", code}, {"clientName", "x"}}, false));
        wire.reset();   // the client went away mid-write
        h.issuer.release(true);
        check(true, "a reply owed to a connection that ended is dropped without a crash");
    }
    {
        Harness h;
        const QString code = h.openPairing();
        QString wrong = code;
        wrong[11] = wrong[11] == u'0' ? u'1' : u'0';
        Wire wire(h, true);
        wire.feed(h.request(wire.session, "pairing.complete", {{"code", wrong}, {"clientName", "x"}}, false));
        check(error(wire.written.value(0)) == "auth.invalid" && wire.closes == 1 && !h.issuer.owed(),
              "a wrong code is refused and the connection closes: one attempt per connection");

        Wire tailnet(h, false);
        tailnet.feed(h.request(tailnet.session, "pairing.complete", {{"code", code}, {"clientName", "x"}}, false));
        check(error(tailnet.written.value(0)) == "capability.unavailable" && !h.issuer.owed(),
              "pairing.complete is refused on the plain tailnet listener");

        Wire withSession(h, true);
        withSession.feed(h.request(withSession.session, "pairing.complete", {{"code", code}, {"clientName", "x"}}, true));
        check(error(withSession.written.value(0)) == "protocol.invalid_envelope" && !h.issuer.owed(),
              "pairing.complete carries no sessionId");

        for (const QJsonObject& bad : {QJsonObject{{"code", code}}, QJsonObject{{"code", code}, {"clientName", ""}},
                                       QJsonObject{{"code", code}, {"clientName", QString(33, u'a')}},
                                       QJsonObject{{"code", code}, {"clientName", "a\nb"}},
                                       QJsonObject{{"code", 1}, {"clientName", "x"}},
                                       QJsonObject{{"code", "abc"}, {"clientName", "x"}},
                                       QJsonObject{{"code", "123-456-789"}, {"clientName", "x"}},
                                       QJsonObject{{"code", code}, {"clientName", "x"}, {"extra", 1}}}) {
            Wire w(h, true);
            w.feed(h.request(w.session, "pairing.complete", bad, false));
            check(error(w.written.value(0)) == "request.invalid_params" && w.closes == 1,
                  "malformed pairing.complete parameters are refused");
        }
        check(!h.issuer.owed(), "no malformed attempt reached the issuer");

        ControlSession local(&h.store, 1 << 20, SessionAuthorization::ObserverController);
        const QJsonObject localReply = h.send(local, h.request(local, "pairing.complete",
            {{"code", code}, {"clientName", "x"}}, false)).message;
        check(error(localReply) == "protocol.invalid_envelope" && !h.issuer.owed(),
              "pairing.complete is not a request on the local socket");

        Wire right(h, true);
        right.feed(h.request(right.session, "pairing.complete", {{"code", code}, {"clientName", "x"}}, false));
        check(right.pump->awaitingReply(), "malformed attempts did not use up the pairing");
    }
    {
        Harness h;
        Wire wire(h, true);
        wire.feed(h.request(wire.session, "pairing.complete", {{"code", "123-456-7890"}, {"clientName", "x"}}, false));
        check(error(wire.written.value(0)) == "capability.unavailable", "with no open pairing nothing is accepted");
    }

    // ---- the verifier and the vault ----
    {
        ControlCredentials credentials;
        const auto local = ControlCredentials::generate(ControlCredentials::Role::Client);
        check(credentials.replace({local}), "load one local identity");
        const auto principal = credentials.verify(QJsonObject{{"scheme", "bearer"},
                                                              {"token", QString::fromLatin1(local.secret.toHex())}});
        QByteArray token;
        auto remote = ControlCredentials::generateRemote(ControlCredentials::Role::RemoteControl, &token);
        remote.name = "Laptop";
        check(credentials.add(remote) && principal.valid() && credentials.size() == 2,
              "add() keeps every existing principal valid");
        check(!credentials.add(remote), "add() refuses a duplicate");
        auto named = ControlCredentials::generate(ControlCredentials::Role::Client);
        named.name = "x";
        check(!ControlCredentials::validRecords({named}), "a local role carries no name");
    }
    {
        QByteArray token;
        auto remote = ControlCredentials::generateRemote(ControlCredentials::Role::RemoteObserve, &token);
        remote.name = QString::fromUtf8("Tablette \xc3\xa9t\xc3\xa9");
        const auto local = ControlCredentials::generate(ControlCredentials::Role::GrantAdmin);
        const auto bytes = ControlCredentialVault::encode({local, remote});
        check(bytes && bytes->startsWith("AETHTX02") && bytes->size() == 9 + 2 * 97, "v2 records are 97 bytes");
        const auto decoded = bytes ? ControlCredentialVault::decode(*bytes) : std::nullopt;
        check(decoded && decoded->size() == 2 && decoded->at(1).name == remote.name
                  && decoded->at(0).name.isEmpty() && decoded->at(1).secret == remote.secret,
              "v2 round-trips the client's name (UTF-8) and the digest");
        QByteArray corrupt = *bytes;
        corrupt[9 + 97 + 65 + 20] = 'x';   // after the name's zero padding begins
        check(!ControlCredentialVault::decode(corrupt), "bytes after a name's padding are corrupt");
        const auto v1 = ControlCredentialVault::encode({local});
        check(v1 && v1->startsWith("AETHTX01") && v1->size() == 9 + 65 && ControlCredentialVault::decode(*v1),
              "a vault without remote records is still v1");
        QString longName(32, u'a');
        check(ControlCredentials::validName(longName) && !ControlCredentials::validName(longName + u'a')
                  && ControlCredentials::validName(QString(16, QChar(0x00e9)))
                  && !ControlCredentials::validName(QString(16, QChar(0x00e9)) + u'a'),
              "a name is at most 32 bytes of UTF-8");
    }

#ifdef HAVE_WEBSOCKETS
    // ---- listener rules that need no socket ----
    {
        const auto v4 = RemoteControlServer::parseEndpoint("192.0.2.7:8443");
        const auto v6 = RemoteControlServer::parseEndpoint("[2001:db8::7]:8443");
        check(v4 && v4->address == QHostAddress("192.0.2.7") && v4->port == 8443
                  && v6 && v6->address == QHostAddress("2001:db8::7"),
              "IPv4 and bracketed IPv6 endpoints parse");
        check(!RemoteControlServer::parseEndpoint("radio.local:8443") && !RemoteControlServer::parseEndpoint("192.0.2.7")
                  && !RemoteControlServer::parseEndpoint("192.0.2.7:0") && !RemoteControlServer::parseEndpoint("192.0.2.7:70000")
                  && !RemoteControlServer::parseEndpoint("2001:db8::7:8443") && !RemoteControlServer::parseEndpoint("[192.0.2.7]:1")
                  && !RemoteControlServer::parseEndpoint("192.0.2.7:+80") && !RemoteControlServer::parseEndpoint("[fe80::1%eth0]:1"),
              "host names, missing or out-of-range ports, unbracketed IPv6 and scoped addresses are refused");
        ControlEngine engine;
        QString why;
        RemoteControlServer tailnet(engine, RemoteControlServer::Mode::Tailnet);
        check(!tailnet.listen({QHostAddress::AnyIPv4, 1}, &why) && why.contains("wildcard")
                  && !tailnet.listen({QHostAddress::AnyIPv6, 1}, &why),
              "a wildcard address is refused");
        check(!tailnet.listen({QHostAddress::LocalHost, 1}, &why) && why.contains("WireGuard")
                  && !tailnet.isListening(),
              "the tailnet listener refuses an address outside WireGuard and binds nothing");
        RemoteControlServer secure(engine, RemoteControlServer::Mode::Secure);
        check(!secure.listen({QHostAddress::LocalHost, 1}, &why) && why.contains("TLS") && !secure.isListening(),
              "the secure listener refuses to start without a certificate");
        check(RemoteControlServer::wireGuardInterfaceFor(QHostAddress::LocalHost).isEmpty(),
              "loopback is not a WireGuard address");
    }
#endif

    std::printf("%s (%d failure%s)\n", failures ? "FAILED" : "PASSED", failures, failures == 1 ? "" : "s");
    return failures ? 1 : 0;
}
