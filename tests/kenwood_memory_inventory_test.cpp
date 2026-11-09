// Native memory operations are unavailable on this backend.
// Requests and disabled controls must never start memory I/O.
#include "TestSettingsProfile.h"
#include "core/backends/kenwood/KenwoodBackend.h"
#include <QCoreApplication>
#include <QList>
#include <QMap>
#include <QPointer>
#include <QStringList>
#include <QVariantMap>
#include <QtTest>
#include <functional>
#include <memory>

namespace AetherSDR::kenwood {
struct KenwoodBackendTestAccess {
    static void authenticate(KenwoodBackend& b) {
        b.m_clock.start(); b.m_protocol.start("memory-fixture", "fixture-only", false, 0);
        b.m_protocol.receive("##CN1;", 1); b.m_protocol.receive("##ID1;", 2); b.ready();
    }
    static void observe(KenwoodBackend& b, const QByteArray& body) { b.adopt(body); }
    static void poll(KenwoodBackend& b, qint64 now) { b.pollMemoryInventory(now); }
    static void sameBatch(KenwoodBackend& b, const QByteArray& body) { b.adopt(body,b.m_controlIngressSequence); }
    static void batch(KenwoodBackend& b,const QList<QByteArray>& bodies) {
        const quint64 ingress=++b.m_controlIngressSequence;
        for (const QByteArray& body:bodies) { b.adopt(body,ingress); }
    }
};
}
namespace {
using namespace AetherSDR;
using Backend = kenwood::KenwoodBackend;
struct Fixture {
    QList<QByteArray> wires;
    QMap<quint64, QVariant> results;
    QMap<quint64, QString> errors;
    quint64 request = 1;
    std::unique_ptr<Backend> backend;
    Fixture() {
        backend = std::make_unique<Backend>(Backend::Transport{[this](const QByteArray& bytes) { wires.append(bytes); return true; }, {}});
        QObject::connect(backend.get(), &IRadioBackend::extensionResult, backend.get(), [this](quint64 id, const QVariant& value) { results[id] = value; });
        QObject::connect(backend.get(), &IRadioBackend::extensionError, backend.get(), [this](quint64 id, const QString& value) { errors[id] = value; });
    }
    void observe(const QByteArray& body) { kenwood::KenwoodBackendTestAccess::observe(*backend, body); }
    quint64 call(const QString& verb, const QVariant& args = {}) { const quint64 id = request++; backend->invokeExtension("kenwood", verb, id, args); return id; }
    void tick() { kenwood::KenwoodBackendTestAccess::poll(*backend, 100); }
};
}
class KenwoodMemoryExclusionTest : public QObject {
    Q_OBJECT
private slots:
    void nativeMemoryIsNotAdvertised_data() {
        QTest::addColumn<bool>("connected");
        QTest::newRow("disconnected") << false;
        QTest::newRow("connected") << true;
    }
    void nativeMemoryIsNotAdvertised() {
        QFETCH(bool, connected);
        Fixture f;
        if (connected) { kenwood::KenwoodBackendTestAccess::authenticate(*f.backend); f.observe("ID024"); }
        const QVariantMap descriptor = f.backend->capabilities().extensions.value("kenwood").toMap();
        QVERIFY(!descriptor.contains("nativeMemoryReadVersion"));
        QVERIFY(!descriptor.contains("nativeMemoryReadVerbs"));
        QCOMPARE(descriptor.value("nativeMemoryReadUnavailableReason"), QVariant(QStringLiteral("Native memory support is disabled in this TS-890S build.")));
        f.wires.clear();
        const QList<QVariant> arguments{QVariant{}, QVariantMap{}, QVariantMap{{"context", "stale"}, {"channel", 7}}, QVariant(QStringLiteral("malformed"))};
        for (const QString& verb : {QStringLiteral("memory.read.get"), QStringLiteral("memory.read.refresh"), QStringLiteral("memory.read.channel"), QStringLiteral("memory.read.quick"), QStringLiteral("memory.read.cancel")}) {
            for (const QVariant& argument : arguments) {
                const quint64 request = f.call(verb, argument);
                QCOMPARE(f.errors.value(request), QStringLiteral("Native memory support is disabled in this TS-890S build."));
                QVERIFY(!f.results.contains(request));
                QVERIFY(f.wires.isEmpty());
            }
        }
        f.tick();
        QVERIFY(f.wires.isEmpty());
        const quint64 other = f.call("transverter.get");
        QVERIFY(f.results.contains(other));
        QVERIFY(!f.errors.contains(other));
        QVERIFY(f.results.value(other).toMap().contains("frequency"));
        QVERIFY(f.wires.isEmpty());
        QVERIFY(descriptor.contains("nativeTransverterVersion"));
    }
};
int main(int argc,char** argv) { TestSettingsProfile profile("kenwood-memory-read"); if (!profile.isValid()) { return 1; } QCoreApplication app(argc,argv); KenwoodMemoryExclusionTest test; return QTest::qExec(&test,argc,argv); }
#include "kenwood_memory_inventory_test.moc"
