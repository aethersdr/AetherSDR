// The remote-engine backend (family "aetherd") against an in-process aetherd
// over a real loopback wss:// connection (docs/aetherd-remote-backend.md,
// *Tests*). The engine's "radio" is a fake receive target that publishes the
// slice, panadapter and radio session the way RadioResourceAdapter does.
//  - rule 1: connecting sends hello and resource.subscribe only, verbs before
//    the first snapshot are dropped, and a real RadioModel connect pushes
//    nothing to the engine;
//  - rule 2: disconnecting sends nothing;
//  - rule 3: intents paced at 1/ms for 3 s stay inside the client bucket at
//    the real server limits, the session stays open and the slice ends at the
//    last value; a conflicting change is re-read and retried with the intent;
//  - rule 4: slice add/remove and a forced resync reach the seam;
//  - a wrong certificate fingerprint fails before hello; keying sends nothing.
// Binds 127.0.0.1 on an ephemeral port and a local socket in a private runtime
// directory. Needs the openssl tool; exits 77 (skipped) without it.
#include "TestSettingsProfile.h"
#include "core/RadioDiscovery.h"
#include "core/backends/aetherd/RemoteEngineBackend.h"
#include "core/backends/aetherd/RemoteEngineMapping.h"
#include "core/control/ControlEngine.h"
#include "core/control/LocalControlServer.h"
#include "core/control/ReceiveControlTarget.h"
#include "core/control/RemoteControlServer.h"
#include "core/control/SliceFrequencyTarget.h"
#include "models/PanadapterModel.h"
#include "models/RadioModel.h"
#include "models/SliceModel.h"

#include <QCoreApplication>
#include <QElapsedTimer>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QProcess>
#include <QSignalSpy>
#include <QStandardPaths>
#include <QTemporaryDir>
#include <QThread>
#include <QUuid>
#include <cstdio>
#include <functional>

using namespace AetherSDR;
using namespace AetherSDR::control;
using AetherSDR::aetherd::RemoteEngineBackend;
using AetherSDR::aetherd::RemoteEngineCredentials;
using AetherSDR::aetherd::RemoteEngineRecord;

namespace {
int failures = 0;
void check(bool condition, const QString& message)
{
    std::printf("[%s] %s\n", condition ? "PASS" : "FAIL", qPrintable(message));
    failures += !condition;
}
bool waitUntil(const std::function<bool()>& predicate, int timeoutMs = 5000)
{
    QElapsedTimer timer;
    timer.start();
    while (!predicate() && timer.elapsed() < timeoutMs) {
        QCoreApplication::processEvents(QEventLoop::AllEvents, 5);
    }
    return predicate();
}
void spin(int ms) { waitUntil([] { return false; }, ms); }

// The engine side: one slice on one pan, published like RadioResourceAdapter.
class FakeRadio final : public SliceFrequencyTarget {
public:
    explicit FakeRadio(ControlResourceStore& store) : m_store(store) { m_clock.start(); }
    bool available() const override { return true; }
    std::optional<ProtocolError> setFrequency(int sliceId, qint64 hz) override
    {
        if (sliceId != 0) { return ProtocolError{"resource.not_found", "no slice", {}, false}; }
        callTimesMs.append(m_clock.elapsed());
        frequencyHz = hz;
        publishSlice();
        return std::nullopt;
    }
    void publishSession()
    {
        (void)m_store.upsert({"radioSession", {}, "radio-1"}, QJsonObject{
            {"id", "radio-1"}, {"connected", true}, {"family", "anan"},
            {"identity", QJsonObject{{"model", "ANAN-G2"}, {"manufacturer", "Apache Labs"}}},
            {"capabilities", QJsonObject{
                {"maxSlices", 2}, {"maxPanadapters", 1},
                {"sliceFrequencyControl", QJsonObject{{"authority", "engine"}, {"minimumHz", 10'000}, {"maximumHz", 61'440'000}}},
                {"receiveModeControl", QJsonObject{{"authority", "engine"}, {"modes", QJsonArray{"USB", "LSB", "CW"}}}},
                {"receiveDspControl", QJsonObject{{"authority", "engine"}, {"maximumRitOffsetHz", 9999},
                    {"features", QJsonObject{{"agcThreshold", QJsonArray{"USB", "LSB", "CW"}}}}}}}},
            {"receiveObservation", QJsonObject{{"radioSpeaker", QJsonObject{{"known", false}}}}}});
    }
    void publishPan()
    {
        (void)m_store.upsert({"panadapter", "radio-1", "0x40000000"}, QJsonObject{
            {"id", "0x40000000"}, {"centerHz", 14'100'000}, {"bandwidthHz", 192'000},
            {"bandwidthLimitsHz", QJsonObject{{"minimum", 48'000}, {"maximum", 1'536'000}}},
            {"receive", QJsonObject{{"antenna", "ANT1"}, {"rfGain", -6}}}});
    }
    void publishSlice(const QString& id = "0")
    {
        (void)m_store.upsert({"slice", "radio-1", id}, QJsonObject{
            {"id", id}, {"letter", id == "0" ? "A" : "B"}, {"panadapterId", "0x40000000"},
            {"frequencyHz", frequencyHz}, {"mode", "USB"},
            {"filter", QJsonObject{{"lowHz", 100}, {"highHz", 2900}}}, {"active", id == "0"},
            {"audio", QJsonObject{{"gain", 50}, {"pan", 50}, {"muted", false}}},
            {"receive", QJsonObject{{"agc", QJsonObject{{"mode", "med"}, {"threshold", 70}, {"offLevel", 10}}}}}});
    }
    qint64 frequencyHz{14'074'000};
    QList<qint64> callTimesMs;
private:
    ControlResourceStore& m_store;
    QElapsedTimer m_clock;
};

struct Seam {
    QList<int> slices;
    QList<int> removed;
    QList<double> frequencies;
    QStringList pans;
    int connected{0};
    int disconnected{0};
    QStringList errors;
    explicit Seam(IRadioBackend* backend)
    {
        QObject::connect(backend, &IRadioBackend::sliceChanged, [this](int id, const SliceDelta& d) {
            slices.append(id);
            if (d.frequency) { frequencies.append(*d.frequency); }
        });
        QObject::connect(backend, &IRadioBackend::sliceRemoved, [this](int id) { removed.append(id); });
        QObject::connect(backend, &IRadioBackend::panCenterBandwidthChanged,
                         [this](const QString& id, double, double) { pans.append(id); });
        QObject::connect(backend, &IRadioBackend::connected, [this] { ++connected; });
        QObject::connect(backend, &IRadioBackend::disconnected, [this] { ++disconnected; });
        QObject::connect(backend, &IRadioBackend::connectionError, [this](const QString& e) { errors.append(e); });
    }
};

RadioConnectRequest requestFor(const QString& engineId, quint16 port)
{
    RadioConnectRequest request;
    request.host = QStringLiteral("127.0.0.1");
    request.port = port;
    request.serial = engineId;
    return request;
}
} // namespace

int main(int argc, char* argv[])
{
    TestSettingsProfile profile(QStringLiteral("aetherd-remote-engine-loopback"));
    if (!profile.isValid()) { return 1; }
    QTemporaryDir runtime;
    qputenv("XDG_RUNTIME_DIR", runtime.path().toLocal8Bit());
    QFile::setPermissions(runtime.path(), QFileDevice::ReadOwner | QFileDevice::WriteOwner | QFileDevice::ExeOwner);
    QCoreApplication app(argc, argv);

    const QString openssl = QStandardPaths::findExecutable("openssl");
    if (openssl.isEmpty()) {
        std::printf("SKIP: no openssl tool for a throwaway certificate\n");
        return 77;
    }
    QTemporaryDir tls;
    const QString cert = tls.filePath("cert.pem");
    const QString key = tls.filePath("key.pem");
    const int made = QProcess::execute(openssl, {"req", "-x509", "-newkey", "ec", "-pkeyopt", "ec_paramgen_curve:prime256v1",
        "-nodes", "-days", "1", "-subj", "/CN=aetherd-test", "-keyout", key, "-out", cert});
    QString why;
    const auto identity = RemoteControlServer::loadTlsIdentity(cert, key, &why);
    if (made != 0 || !identity) { std::printf("FAIL: no throwaway certificate (%s)\n", qPrintable(why)); return 1; }
    const QString fingerprint = RemoteControlServer::certificateSha256(*identity);

    ControlEngine engine;
    ControlCredentials credentials;
    FakeRadio radio(engine.resources);
    radio.publishSession();
    radio.publishPan();
    radio.publishSlice();
    QByteArray secret;
    auto record = ControlCredentials::generateRemote(ControlCredentials::Role::RemoteControl, &secret);
    record.name = QStringLiteral("loopback-desktop");
    QByteArray secondSecret;
    auto second = ControlCredentials::generateRemote(ControlCredentials::Role::RemoteControl, &secondSecret);
    second.name = QStringLiteral("loopback-second");
    LocalControlServer local(engine, nullptr, {}, true);
    RemoteControlServer remote(engine, RemoteControlServer::Mode::Secure, nullptr);
    if (!credentials.replace({}) || !credentials.add(record) || !credentials.add(second)
        || !local.bindCredentials(&credentials) || !local.bindFrequencyTarget(&radio)) {
        std::printf("FAIL: cannot compose the engine\n");
        return 1;
    }
    const bool localUp = local.listen("remote-engine-" + QUuid::createUuid().toString(QUuid::Id128),
                                      LocalControlServer::ListenMode::ReserveEndpoint);
    if (!remote.setTlsConfiguration(*identity)) { std::printf("FAIL: TLS refused\n"); return 1; }
    if (!localUp || !remote.listen({QHostAddress::LocalHost, 0}, &why)) {
        std::printf("SKIP: cannot bind the test listeners (%s)\n", qPrintable(why));
        return 77;
    }
    if (!local.startServing() || !remote.startServing()) { std::printf("FAIL: not serving\n"); return 1; }
    const quint16 port = remote.serverPort();
    const QString url = QStringLiteral("wss://127.0.0.1:%1").arg(port);
    RemoteEngineCredentials::setSession({fingerprint, "loopback", url, {}, QString::fromLatin1(secret.toHex())});

    // ---- rule 1 and the snapshot -------------------------------------------
    {
        RemoteEngineBackend backend;
        Seam seam(&backend);
        bool droppedBeforeSnapshot = true;
        QObject::connect(&backend, &IRadioBackend::connected, [&] {
            // The model pushes state the moment it hears connected(); none of it may leave.
            backend.setSliceFrequency(0, 7'000'000.0);
            backend.setSliceMode(0, "CW");
            droppedBeforeSnapshot = backend.sentMethodsForTest().size() <= 1;
        });
        backend.connectRadio(requestFor(fingerprint, port));
        backend.setSliceFrequency(0, 3'500'000.0);   // before the socket is even open
        check(waitUntil([&] { return !seam.slices.isEmpty(); }), "connect: the snapshot reaches the seam");
        spin(300);
        check(seam.connected == 1 && backend.isConnected(), "connect: connected() once");
        check(backend.sentMethodsForTest() == QStringList{"resource.subscribe"},
              QStringLiteral("rule 1: after hello, only resource.subscribe went out (%1)")
                  .arg(backend.sentMethodsForTest().join(',')));
        check(droppedBeforeSnapshot && radio.callTimesMs.isEmpty() && radio.frequencyHz == 14'074'000,
              "rule 1: verbs before the first snapshot are dropped, not replayed");
        check(!seam.frequencies.isEmpty() && std::abs(seam.frequencies.last() - 14.074) < 1e-9,
              "the slice arrives at the engine's frequency, in MHz");
        check(seam.pans.contains("0x40000000"), "the pan arrives before or with its slice");
        const RadioCapabilities caps = backend.capabilities();
        check(caps.family == "aetherd" && caps.model == "ANAN-G2" && !caps.canTransmit
                  && caps.agcModes == QStringList{"med"},
              "capabilities mirror the engine, receive-only, AGC mode as running");
        check(backend.clientLimitsForTest() == aetherd::RemoteEngineScheduler::Limits{50.0, 50.0},
              "the client bucket is 50/s, burst 50, against the advertised 100/s, burst 200");

        // ---- rule 3: paced intents against the real server limits ----------
        const qsizetype callsBefore = radio.callTimesMs.size();
        QElapsedTimer paced;
        paced.start();
        qint64 last = 0;
        int sentIntents = 0;
        while (paced.elapsed() < 3000) {
            last = 14'000'000 + paced.elapsed();
            backend.setSliceFrequency(0, double(last));
            ++sentIntents;
            QCoreApplication::processEvents(QEventLoop::AllEvents, 1);
            QThread::usleep(1000);
        }
        check(waitUntil([&] { return radio.frequencyHz == last; }), "rule 3: the slice ends at the last intent");
        spin(200);
        check(backend.isConnected() && seam.disconnected == 0, "rule 3: the session stayed open throughout");
        const QList<qint64> calls = radio.callTimesMs.mid(callsBefore);
        bool withinBucket = true;
        for (qsizetype i = 0; i < calls.size(); ++i) {
            for (qsizetype j = i; j < calls.size(); ++j) {
                const double allowed = 50.0 + 50.0 * double(calls.at(j) - calls.at(i)) / 1000.0 + 2.0;
                if (double(j - i + 1) > allowed) { withinBucket = false; }
            }
        }
        check(withinBucket && calls.size() > 100 && calls.size() < 250,
              QStringLiteral("rule 3: %1 intents became %2 requests, inside burst 50 + 50/s in every window")
                  .arg(sentIntents).arg(calls.size()));

        // ---- rule 3: a conflicting change is re-read and retried -----------
        const int gets = backend.sentMethodsForTest().count("resource.get");
        backend.setSliceFrequency(0, 21'074'000.0);
        radio.frequencyHz = 21'000'000;   // another client moves the slice first
        radio.publishSlice();
        check(waitUntil([&] { return radio.frequencyHz == 21'074'000; }),
              "rule 3: after a conflict the intent is retried and lands");
        check(backend.sentMethodsForTest().count("resource.get") == gets + 1,
              "rule 3: the conflict cost one re-read");

        // ---- rule 4: slices come and go, and a resync rebuilds -------------
        radio.publishSlice("1");
        check(waitUntil([&] { return seam.slices.contains(1); }), "rule 4: a new slice id reaches the model");
        (void)engine.resources.remove({"slice", "radio-1", "1"});
        check(waitUntil([&] { return seam.removed.contains(1); }), "rule 4: a removed slice is retired");
        backend.receiveTextForTest(QString::fromUtf8(QJsonDocument(QJsonObject{
            {"v", 1}, {"event", "resource.changed"}, {"sequence", 1'000'000},
            {"resource", QJsonObject{{"type", "slice"}, {"radioSession", "radio-1"}, {"id", "9"}}},
            {"revision", 1}, {"value", QJsonObject{{"id", "9"}, {"panadapterId", "0x40000000"}, {"frequencyHz", 1'000'000}}}})
            .toJson(QJsonDocument::Compact)));
        check(seam.slices.contains(9), "a stale slice the engine no longer has (injected)");
        const int subscribes = backend.sentMethodsForTest().count("resource.subscribe");
        backend.receiveTextForTest(QString::fromUtf8(QJsonDocument(QJsonObject{
            {"v", 1}, {"event", "resource.resyncRequired"}, {"sequence", 1'000'001},
            {"subscriptionsInvalidated", true}}).toJson(QJsonDocument::Compact)));
        check(waitUntil([&] { return seam.removed.contains(9); }),
              "rule 4: resync takes a fresh snapshot, which retires what the engine lacks");
        check(backend.sentMethodsForTest().count("resource.subscribe") == subscribes + 1,
              "rule 4: the resync cost one subscribe");

        // ---- TX absent, then rule 2 ----------------------------------------
        const qsizetype sentBefore = backend.sentMethodsForTest().size();
        backend.setKeying(true, {}, {});
        backend.setKeying(false, {}, {});
        backend.disconnectRadio();
        spin(300);
        check(backend.sentMethodsForTest().size() == sentBefore,
              "keying and disconnect send nothing (rule 2; never radio.disconnect)");
        check(seam.disconnected == 1 && !backend.isConnected(), "rule 2: disconnected() exactly once");
        check(waitUntil([&] { return remote.sessionCount() == 0; }), "rule 2: the engine sees the socket close");
        check(engine.resources.get({"radioSession", {}, "radio-1"}).has_value(),
              "rule 2: the engine still holds its radio");
    }

    // ---- the pin ------------------------------------------------------------
    {
        const QString wrong = QString(64, u'0');
        RemoteEngineCredentials::setSession({wrong, "impostor", url, {}, QString::fromLatin1(secret.toHex())});
        RemoteEngineBackend backend;
        Seam seam(&backend);
        backend.connectRadio(requestFor(wrong, port));
        check(waitUntil([&] { return !seam.errors.isEmpty(); }), "a wrong fingerprint fails the connect");
        spin(200);
        check(!backend.helloSentForTest() && seam.connected == 0,
              "a wrong fingerprint fails before hello: the token never leaves");
        check(!seam.errors.isEmpty() && seam.errors.first().contains("certificate changed"),
              "the failure names the certificate change");
        RemoteEngineCredentials::clearSession(wrong);
    }
    {
        RemoteEngineBackend backend;
        Seam seam(&backend);
        backend.connectRadio(requestFor(QStringLiteral("never-paired"), port));
        check(!seam.errors.isEmpty() && seam.errors.first().contains("not paired"),
              "an engine with no session token fails at once, without blocking");
    }

    // ---- rule 1 through a real RadioModel, and no echo to a second client ---
    {
        RadioModel model;
        RadioInfo info;
        info.family = QStringLiteral("aetherd");
        info.address = QHostAddress::LocalHost;
        info.port = port;
        info.serial = fingerprint;
        info.model = QStringLiteral("ANAN-G2");
        model.connectToRadio(info);
        auto* mirror = qobject_cast<RemoteEngineBackend*>(model.backend());
        check(mirror != nullptr, "RadioModel builds the aetherd family through makeBackend()");
        check(waitUntil([&] { return model.isConnected() && !model.slices().isEmpty(); }, 8000),
              "RadioModel connects and shows the engine's slice");
        spin(1500);
        QStringList panIds;
        for (PanadapterModel* pan : model.panadapters()) { panIds.append(pan->panId()); }
        // The model keys a seam family's pans by its own neutral ids; what
        // matters is one pan, with the engine's slice on it.
        check(panIds.size() == 1 && !model.slices().isEmpty()
                  && model.slices().first()->panId() == panIds.first(),
              QStringLiteral("the model holds the engine's one pan and its slice sits on it (pans: %1)")
                  .arg(panIds.join(',')));
        const QStringList afterConnect = mirror ? mirror->sentMethodsForTest() : QStringList{};
        check(afterConnect == QStringList{"resource.subscribe"},
              QStringLiteral("rule 1 through RadioModel: nothing but the subscribe (%1)").arg(afterConnect.join(',')));

        RemoteEngineCredentials::setSession({fingerprint, "loopback", url, {}, QString::fromLatin1(secondSecret.toHex())});
        RemoteEngineBackend other;
        Seam otherSeam(&other);
        other.connectRadio(requestFor(fingerprint, port));
        check(waitUntil([&] { return !otherSeam.slices.isEmpty(); }), "a second client connects");
        other.setSliceFrequency(0, 10'136'000.0);
        check(waitUntil([&] {
                  return !model.slices().isEmpty() && std::abs(model.slices().first()->frequency() - 10.136) < 1e-9;
              }),
              "a change from the second client is mirrored into the model");
        spin(500);
        check(mirror && mirror->sentMethodsForTest() == afterConnect,
              "the mirrored change makes the model's backend send nothing (no echo)");
        other.disconnectRadio();
        model.disconnectFromRadio();
        spin(300);
    }
    RemoteEngineCredentials::clearAll();
    std::printf("%s (%d failure%s)\n", failures ? "FAILED" : "PASSED", failures, failures == 1 ? "" : "s");
    return failures ? 1 : 0;
}
