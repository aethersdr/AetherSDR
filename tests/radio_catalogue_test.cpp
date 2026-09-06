#include "core/control/RadioCatalogue.h"
#include "core/control/ControlService.h"

#include <QCoreApplication>
#include <QEvent>
#include <QJsonArray>
#include <QJsonDocument>

#include <cstdio>
#include <utility>

using namespace AetherSDR;
using namespace AetherSDR::control;

namespace {
const ResourceAddress kCatalogue{QStringLiteral("radioCatalogue"), {}, {}};

bool check(bool condition, const char* message)
{
    if (!condition) { std::fprintf(stderr, "%s\n", message); }
    return condition;
}

class InjectedSource final : public RadioDiscoverySource {
public:
    QStringList enabledSources() const override { return {QStringLiteral("sim")}; }
    void start() override { ++starts; }
    void stop() override { ++stops; }
    int starts{0};
    int stops{0};
};

struct Fixture {
    ControlResourceStore store;
    ControlService service{&store};
    std::unique_ptr<InjectedSource> owned{std::make_unique<InjectedSource>()};
    InjectedSource* source{owned.get()};
    RadioCatalogue catalogue{std::move(owned), &store};
    QJsonObject value() const { return store.get(kCatalogue)->value; }
    QJsonArray entries() const { return value().value(QStringLiteral("entries")).toArray(); }
    quint64 revision() const { return store.get(kCatalogue)->revision; }
};

DiscoveredRadio radio(QString family = QStringLiteral("sim"), QString serial = QStringLiteral("one"))
{
    DiscoveredRadio result;
    result.family = std::move(family);
    result.serial = std::move(serial);
    result.name = QStringLiteral("Test radio");
    result.model = QStringLiteral("Model");
    result.transport = QStringLiteral("sim");
    return result;
}

ServiceReply invoke(Fixture& f, ControlSession& session, const QString& method,
                    const QJsonObject& params)
{
    QJsonObject request{{QStringLiteral("v"), 1}, {QStringLiteral("id"), QStringLiteral("test")},
                        {QStringLiteral("method"), method}, {QStringLiteral("params"), params}};
    if (method != QStringLiteral("hello")) {
        request.insert(QStringLiteral("sessionId"), session.sessionId());
    }
    return f.service.handle(QJsonDocument(request).toJson(QJsonDocument::Compact), &session);
}

bool testIdentityAndLifecycle()
{
    Fixture f;
    f.source->radioChanged(radio());
    if (!check(f.entries().isEmpty() && f.source->starts == 0, "construction must not discover")) {
        return false;
    }
    f.catalogue.start();
    f.catalogue.start();
    DiscoveredRadio first = radio(QStringLiteral("hl2"));
    f.source->radioChanged(first);
    const quint64 firstRevision = f.revision();
    const QString firstId = f.entries().first().toObject().value(QStringLiteral("id")).toString();
    f.source->radioChanged(first);
    if (!check(f.revision() == firstRevision && f.source->starts == 1,
               "duplicate announcements/start must not churn revision or restart discovery")) {
        return false;
    }
    f.source->radioChanged(radio(QStringLiteral("flex")));
    if (!check(f.entries().size() == 2
                   && f.entries().first().toObject().value(QStringLiteral("family")) == QStringLiteral("flex")
                   && f.entries().first().toObject().value(QStringLiteral("id")) != firstId,
               "same serial in two families must remain distinct and deterministically sorted")) {
        return false;
    }
    first.nickname = QStringLiteral("Renamed");
    first.transport = QStringLiteral("lan");
    first.address = QStringLiteral("192.0.2.1");
    first.port = 1024;
    f.source->radioChanged(first);
    if (!check(f.entries().last().toObject().value(QStringLiteral("id")) == firstId
                   && f.entries().last().toObject().value(QStringLiteral("nickname")) == first.nickname
                   && f.revision() > firstRevision,
               "address/display updates must preserve identity and publish a new canonical value")) {
        return false;
    }
    f.source->radioLost(QStringLiteral("flex"), first.serial);
    if (!check(f.entries().size() == 1, "loss must remove only the matching family/serial")) {
        return false;
    }
    // Queue a late source update, then stop before it is dispatched.
    QMetaObject::invokeMethod(f.source, [&f] { f.source->radioChanged(radio()); }, Qt::QueuedConnection);
    f.catalogue.stop();
    const quint64 stoppedRevision = f.revision();
    f.catalogue.start();
    f.catalogue.stop();
    QCoreApplication::sendPostedEvents(nullptr, QEvent::MetaCall);
    return check(f.entries().isEmpty() && !f.value().value(QStringLiteral("running")).toBool()
                     && f.revision() == stoppedRevision && f.source->starts == 1,
                 "stop must clear entries, ignore queued callbacks and be terminal");
}

bool testBounds()
{
    Fixture f;
    f.catalogue.start();
    for (int invalid = 0; invalid < 10; ++invalid) {
        DiscoveredRadio value = radio();
        switch (invalid) {
        case 0: value.serial.clear(); break;
        case 1: value.family = QStringLiteral("bad/family"); break;
        case 2: value.nickname = QString(129, QLatin1Char('x')); break;
        case 3: value.serial.append(QChar(0x1f)); break;
        case 4: value.transport = QStringLiteral("lan"); value.address = QStringLiteral("not-an-IP"); value.port = 1; break;
        case 5: value.address = QStringLiteral("192.0.2.1"); break;
        case 6: value.serial = QString(QChar(0xd800)); break;
        case 7: value.model = QString(QChar(0xdc00)); break;
        case 8: value.transport = QStringLiteral("unknown"); break;
        case 9: value.transport = QStringLiteral("lan"); value.address = QStringLiteral("192.0.2.1"); break;
        }
        f.source->radioChanged(value);
    }
    if (!check(f.entries().isEmpty(), "invalid identities/display text/endpoints must not publish")) {
        return false;
    }
    for (qsizetype i = 0; i < RadioCatalogue::kMaxEntries + 1; ++i) {
        DiscoveredRadio value = radio(QString(16, QLatin1Char('a')),
                                      QString(125, QChar(0x800)) + QString::number(i).rightJustified(3, QLatin1Char('0')));
        value.name = value.model = value.nickname = value.version = QString(128, QChar(0x800));
        f.source->radioChanged(value);
    }
    if (!check(f.entries().size() == RadioCatalogue::kMaxEntries
                   && f.value().value(QStringLiteral("limited")).toBool()
                   && QJsonDocument(f.store.get(kCatalogue)->toJson()).toJson(QJsonDocument::Compact).size()
                       < ProtocolLimits::kMaxMessageBytes,
               "capacity must be bounded, disclosed, and fit the protocol frame limit")) {
        return false;
    }
    f.source->radioLost(QString(16, QLatin1Char('a')), QString(125, QChar(0x800)) + QStringLiteral("000"));
    f.source->radioChanged(radio(QStringLiteral("sim"), QStringLiteral("new")));
    return check(f.entries().size() == RadioCatalogue::kMaxEntries
                     && f.value().value(QStringLiteral("limited")).toBool(),
                 "freed capacity must accept new observations without hiding prior incompleteness");
}

bool testProtocolAndIsolation()
{
    Fixture f;
    f.catalogue.start();
    ControlSession observer(&f.store, 256 * 1024, SessionAuthorization::Observer);
    ControlSession second(&f.store, 256 * 1024, SessionAuthorization::Observer);
    ControlSession denied(&f.store, 256 * 1024, SessionAuthorization::AuthenticatedWithoutGrants);
    for (ControlSession* session : {&observer, &second, &denied}) {
        const QJsonObject welcome = invoke(f, *session, QStringLiteral("hello"),
            {{QStringLiteral("versions"), QJsonArray{1}}}).message.value(QStringLiteral("result")).toObject();
        if (!check(welcome.value(QStringLiteral("capabilities")).toArray().contains(QStringLiteral("radioCatalogue.read"))
                       == (session != &denied), "catalogue capability must require observe permission")) {
            return false;
        }
    }
    const QJsonObject params{{QStringLiteral("resources"), QJsonArray{kCatalogue.toJson()}}};
    for (ControlSession* session : {&observer, &second}) {
        const QJsonObject baseline = invoke(f, *session, QStringLiteral("resource.subscribe"), params)
                                         .message.value(QStringLiteral("result")).toObject();
        if (!check(baseline.value(QStringLiteral("resources")).toArray().size() == 1,
                   "catalogue subscription must include its atomic baseline")) { return false; }
    }
    const ServiceReply noGrant = invoke(f, denied, QStringLiteral("resource.subscribe"), params);
    if (!check(noGrant.message.value(QStringLiteral("error")).toObject().value(QStringLiteral("code"))
                   == QStringLiteral("auth.grant_denied"), "denied clients must not subscribe to discovery")) {
        return false;
    }
    observer.revokeAuthorization();
    f.source->radioChanged(radio());
    const QList<QByteArray> frames = second.takePendingFrames();
    if (!check(observer.takePendingFrames().isEmpty() && denied.takePendingFrames().isEmpty()
                   && frames.size() == 1
                   && QJsonDocument::fromJson(frames.first()).object().value(QStringLiteral("value"))
                       .toObject().value(QStringLiteral("entries")).toArray().size() == 1,
               "catalogue deltas must reach only active observers")) { return false; }
    const QJsonObject read = invoke(f, second, QStringLiteral("resource.get"),
        {{QStringLiteral("resource"), kCatalogue.toJson()}}).message.value(QStringLiteral("result")).toObject();
    QJsonObject invalidSelector = kCatalogue.toJson();
    invalidSelector.insert(QStringLiteral("id"), QStringLiteral("not-a-singleton"));
    const ServiceReply invalid = invoke(f, second, QStringLiteral("resource.get"),
        {{QStringLiteral("resource"), invalidSelector}});
    return check(read.value(QStringLiteral("value")).toObject() == f.value()
                     && invalid.message.contains(QStringLiteral("error")),
                 "get must return the canonical catalogue and reject non-singleton selectors");
}
} // namespace

int main(int argc, char* argv[])
{
    QCoreApplication app(argc, argv);
    return testIdentityAndLifecycle() && testBounds() && testProtocolAndIsolation() ? 0 : 1;
}
