// Socket-free native scope contract tests. Literal wire/domain expectations
// come from the PC reference; observed source data is not firmware evidence.
#include "TestSettingsProfile.h"
#include "core/backends/kenwood/KenwoodBackend.h"
#include <QCoreApplication>
#include <QByteArray>
#include <QList>
#include <QMetaType>
#include <QString>
#include <QVariantList>
#include <QVariantMap>
#include <QMap>
#include <QPointer>
#include <QtTest>
#include <QtMath>
#include <functional>
#include <cstring>
#include <memory>

namespace AetherSDR::kenwood {
struct KenwoodBackendTestAccess {
    static void authenticate(KenwoodBackend& b) {
        b.m_clock.start(); b.m_protocol.start("scope-user", "scope-password", false, 0);
        b.m_protocol.receive("##CN1;", 1); b.m_protocol.receive("##ID1;", 2); b.ready();
    }
    static void observe(KenwoodBackend& b, const QByteArray& bytes) { b.adopt(bytes); }
    static void pollFrameAge(KenwoodBackend& b, qint64 age) { b.pollScopeFrameFreshness(b.m_nativeScopeLastFrameMs + age); }
    static void sameBatch(KenwoodBackend& b, const QByteArray& bytes) { b.adopt(bytes, b.m_controlIngressSequence); }
    static void batch(KenwoodBackend& b, const QList<QByteArray>& messages) {
        const quint64 ingress = ++b.m_controlIngressSequence;
        for (const QByteArray& message : messages) { b.adopt(message, ingress); }
    }
};
}
namespace {
using namespace AetherSDR;
using Backend = kenwood::KenwoodBackend;
struct Row { const char* id; const char* command; int minimum; int maximum; int digits; };
// All finite raw domains, independent of implementation tables.
const QList<Row> kRows{
    {"display", "BS0", 0, 1, 1}, {"display_type", "BS1", 0, 4, 1},
    {"mode", "BS3", 0, 2, 1}, {"span", "BS4", 0, 7, 1},
    {"fixed_range", "BS5", 0, 3, 1}, {"pause", "BS6", 0, 1, 1},
    {"marker", "BS7", 0, 1, 1}, {"attenuation", "BS8", 0, 3, 1},
    {"max_hold", "BS9", 0, 1, 1}, {"average", "BSA", 0, 3, 1},
    {"waterfall_speed", "BSB", 1, 4, 1}, {"reference", "BSC", 0, 60, 3},
    {"audio_attenuation", "BSG", 0, 3, 1}, {"audio_span", "BSH", 0, 1, 1},
    {"oscilloscope_level", "BSI", 0, 3, 1}, {"oscilloscope_sweep", "BSJ", 0, 5, 1},
    {"shift", "BSK", 0, 8, 1}, {"audio_pause", "BSN", 0, 1, 1},
    {"expand", "BSO", 0, 1, 1}, {"bandscope_output", "DD0", 0, 3, 1},
    {"filter_output", "DD1", 0, 1, 1}};
QVariantMap findRow(const QVariantMap& state, const QString& id) {
    for (const QVariant& item : state.value("controls").toList()) {
        const QVariantMap row = item.toMap(); if (row.value("id").toString() == id) { return row; }
    }
    return {};
}
struct Fixture {
    QList<QByteArray> wires;
    QVariantMap state, frame;
    QMap<quint64, QVariant> results;
    QMap<quint64, QString> errors;
    std::function<void(const QVariantMap&)> onStatus;
    std::function<void(const QByteArray&)> onWrite;
    bool accept = true;
    quint64 request = 1;
    std::unique_ptr<Backend> backend;
    Fixture() {
        backend = std::make_unique<Backend>(Backend::Transport{[this](const QByteArray& bytes) {
            wires.append(bytes); const bool result = accept; const auto callback = onWrite;
            if (callback) { callback(bytes); } return result;
        }, {}});
        QObject::connect(backend.get(), &IRadioBackend::extensionStatus, backend.get(),
            [this](const QString&, const QString& kind, const QVariantMap& value) {
                if (kind == "scopeControls") { state = value; if (onStatus) { onStatus(value); } }
                if (kind == "scopeFilterFrame") { frame = value; }
            });
        QObject::connect(backend.get(), &IRadioBackend::extensionResult, backend.get(),
            [this](quint64 id, const QVariant& value) { results[id] = value; if (value.toMap().contains("controls")) { state = value.toMap(); } });
        QObject::connect(backend.get(), &IRadioBackend::extensionError, backend.get(),
            [this](quint64 id, const QString& value) { errors[id] = value; });
    }
    void observe(const QByteArray& bytes) { kenwood::KenwoodBackendTestAccess::observe(*backend, bytes); }
    quint64 call(const QString& verb, const QVariant& argument = {}) {
        const quint64 id = request++; backend->invokeExtension("kenwood", verb, id, argument); return id;
    }
    void ready(char mode = '2') {
        kenwood::KenwoodBackendTestAccess::authenticate(*backend);
        for (const QByteArray& bytes : {QByteArray("ID024"), QByteArray("XV0"), QByteArray("FR0"), QByteArray("FT0"), QByteArray("FA00014074000"),
                QByteArray("OM0") + mode, QByteArray("SF000014074000") + mode, QByteArray("TS0"), QByteArray("RX")}) { observe(bytes); }
        call("scope.controls.get");
    }
    void qualifyGeometry(int mode = 0, int span = 0, int fixed = 0, int shift = 4, int expand = 0) {
        observe("BS3" + QByteArray::number(mode)); observe("BS4" + QByteArray::number(span));
        observe("BS5" + QByteArray::number(fixed)); observe("BSK" + QByteArray::number(shift)); observe("BSO" + QByteArray::number(expand));
        observe("BSM01400000014200000");
    }
    void load() {
        observe("AI2");
        for (const Row& row : kRows) {
            const int value = QByteArray(row.command) == "BSK" ? 4 : row.minimum;
            observe(QByteArray(row.command) + QByteArray::number(value).rightJustified(row.digits, '0'));
        }
        observe("BSL0");
        for (int slot = 0; slot <= 3; ++slot) { observe("BSM" + QByteArray::number(slot) + "1400000014200000"); }
        observe("DD01"); observe("DD11");
    }
    QVariantMap row(const QString& id) const { return findRow(state, id); }
    QVariantMap range(int slot) const {
        for (const QVariant& item : state.value("ranges").toList()) { if (item.toMap().value("slot").toInt() == slot) { return item.toMap(); } }
        return {};
    }
    QVariantMap args(const QString& id) const { return {{"context", state.value("context")}, {"id", id}, {"revision", row(id).value("revision")}}; }
    quint64 set(const QString& id, const QVariant& value) { QVariantMap a = args(id); a["value"] = value; return call("scope.controls.set", a); }
};
}
class KenwoodScopeControlsTest final : public QObject {
    Q_OBJECT
private slots:
    void descriptorUnknownAndReadOnlyQueries() {
        Fixture f; f.ready();
        QCOMPARE(f.state.value("controls").toList().size(), 22);
        QCOMPARE(f.state.value("ranges").toList().size(), 3);
        QVERIFY(f.state.value("statusRevision").typeId() == QMetaType::QString);
        QVERIFY(!f.row("reference").contains("value"));
        for (const Row& row : kRows) { QVERIFY2(f.wires.contains(QByteArray(row.command) + ";"), row.command); }
        QVERIFY(f.wires.contains("BSL;")); QVERIFY(f.wires.contains("AI;")); QVERIFY(f.wires.contains("BSM;"));
        f.wires.clear(); f.call("scope.controls.get");
        for (const QByteArray& wire : f.wires) {
            QVERIFY2(wire.endsWith(';'), wire.constData());
            QVERIFY2(!wire.startsWith("TX") && !wire.startsWith("PS") && !wire.startsWith("BSM1")
                && !wire.startsWith("BSM2") && !wire.startsWith("BSM3"), wire.constData());
        }
        f.load(); QVERIFY(f.row("overflow").contains("value")); QVERIFY(!f.row("overflow").value("editable").toBool());
    }
    void everyFiniteChoiceInEveryMode() {
        for (char mode : QByteArray("12345679ABCDEF")) {
            Fixture f; f.ready(mode);
            for (const Row& row : kRows) {
                for (int value = row.minimum; value <= row.maximum; ++value) {
                    f.load(); if (QByteArray(row.command) == "BS5") { f.qualifyGeometry(1); }
                    f.wires.clear();
                    const int before = f.row(row.id).value("value").toInt();
                    f.set(row.id, value);
                    const QByteArray expected = QByteArray(row.command) + QByteArray::number(value).rightJustified(row.digits, '0') + ';';
                    QVERIFY2(f.wires.contains(expected), qPrintable(QString("%1 mode%2 raw%3").arg(row.id).arg(QChar(mode)).arg(value)));
                    QCOMPARE(f.wires.count(expected), 1);
                    QCOMPARE(f.row(row.id).value("value").toInt(), before);
                    QVERIFY(f.row(row.id).value("pending").toBool());
                    f.observe(expected.chopped(1));
                    QCOMPARE(f.row(row.id).value("value").toInt(), value);
                    QVERIFY(!f.row(row.id).value("pending").toBool());
                }
            }
        }
    }
    void strictTypesRangesSerialChoicesAndReadOnly() {
        Fixture f; f.ready(); f.load();
        for (const Row& row : kRows) {
            for (const QVariant& value : QList<QVariant>{QVariant(true), QVariant("1"), QVariant(1.0), QVariant(row.minimum - 1), QVariant(row.maximum + 1)}) {
                f.wires.clear(); QVERIFY(f.errors.contains(f.set(row.id, value))); QVERIFY(f.wires.isEmpty());
            }
        }
        for (int serial : {4, 5}) { f.wires.clear(); QVERIFY(f.errors.contains(f.set("bandscope_output", serial))); QVERIFY(f.wires.isEmpty()); }
        f.wires.clear(); QVERIFY(f.errors.contains(f.set("filter_output", 2))); QVERIFY(f.errors.contains(f.set("overflow", 1))); QVERIFY(f.wires.isEmpty());
        const QVariantMap old = f.args("reference"); f.observe("TS1"); f.wires.clear();
        QVariantMap a = old; a["value"] = 41; QVERIFY(f.errors.contains(f.call("scope.controls.set", a))); QVERIFY(f.wires.isEmpty());
    }
    void malformedRepliesCannotReplaceObservedValues() {
        Fixture f; f.ready(); f.load();
        for (const QByteArray& bad : {QByteArray("BSC061"), QByteArray("BSC-01"), QByteArray("BSC00"), QByteArray("BSC0000"),
                QByteArray("BSJ6"), QByteArray("BSK9"), QByteArray("BSB0"), QByteArray("BS49"), QByteArray("DD06"), QByteArray("DD13")}) { f.observe(bad); }
        QCOMPARE(f.row("reference").value("value").toInt(), 0);
        QCOMPARE(f.row("oscilloscope_sweep").value("value").toInt(), 0);
        QCOMPARE(f.row("shift").value("value").toInt(), 4);
    }
    void resetsActionsAndTaggedPairs() {
        Fixture f; f.ready(); f.load(); f.wires.clear();
        const quint64 spanReset = f.call("scope.controls.reset", f.args("span")); QVERIFY(f.wires.contains("BS49;"));
        QCOMPARE(f.results.value(spanReset), QVariant(true));
        f.observe("BS42"); QCOMPARE(f.row("span").value("value").toInt(), 2);
        f.wires.clear(); const quint64 clear = f.call("scope.controls.action", QVariantMap{{"context", f.state.value("context")}, {"id", "clear"}}); QVERIFY(f.wires.contains("BSD;"));
        QVERIFY(f.results.value(clear).toMap().value("sent").toBool());
        QVERIFY(!f.results.value(clear).toMap().value("verified").toBool());
        f.wires.clear(); QVERIFY(f.errors.contains(f.call("scope.controls.action", QVariantMap{{"context", f.state.value("context")}, {"id", "marker_center"}}))); QVERIFY(f.wires.isEmpty());
        f.qualifyGeometry(1, 2);
        f.wires.clear(); f.call("scope.controls.action", QVariantMap{{"context", f.state.value("context")}, {"id", "marker_center"}}); QVERIFY(f.wires.contains("BSE;"));
        for (int slot = 1; slot <= 3; ++slot) {
            f.load(); f.observe("BS31"); f.observe("BSM01400000014200000");
            const QVariantMap stored = f.range(slot);
            QVariantMap a{{"context", f.state.value("context")}, {"slot", slot}, {"revision", stored.value("revision")},
                {"lowHz", QVariant::fromValue(qint64(14010000))}, {"highHz", QVariant::fromValue(qint64(14190000))}};
            f.wires.clear(); f.call("scope.ranges.set", a);
            QVERIFY(f.wires.contains("BSM" + QByteArray::number(slot) + "1401000014190000;"));
            QVERIFY(!f.range(slot).contains("lowHz"));
            QVERIFY(!f.range(slot).value("pending").toBool());
            QVERIFY(f.range(slot).value("reason").toString().contains("not verified", Qt::CaseInsensitive));
            f.observe("BSM01402000014180000");
            QVERIFY(!f.range(slot).contains("lowHz"));
            f.observe("BSM" + QByteArray::number(slot) + "1401000014190000");
            QCOMPARE(f.range(slot).value("lowHz").toLongLong(), qint64(14010000));
            a.remove("lowHz"); a.remove("highHz"); a["revision"] = f.range(slot).value("revision");
            f.wires.clear(); f.call("scope.ranges.reset", a);
            QVERIFY(f.wires.contains("BSM" + QByteArray::number(slot) + "9999999999999999;"));
        }
        f.wires.clear(); QVariantMap bad{{"context", f.state.value("context")}, {"slot", 0}, {"revision", "0"},
            {"lowHz", QVariant::fromValue(qint64(14000000))}, {"highHz", QVariant::fromValue(qint64(14100000))}};
        QVERIFY(f.errors.contains(f.call("scope.ranges.set", bad))); QVERIFY(f.wires.isEmpty());
    }
    void pendingReentryIngressAndWithdrawal() {
        Fixture f; f.ready(); f.load(); bool nested = false;
        f.onStatus = [&](const QVariantMap& value) {
            if (!nested && findRow(value, "reference").value("pending").toBool()) {
                nested = true; f.call("scope.controls.get"); f.observe("BSC000");
            }
        };
        f.wires.clear(); f.set("reference", 41); QVERIFY(nested); QVERIFY(f.wires.contains("BSC041;"));
        QCOMPARE(f.wires.first(), QByteArray("BSC041;"));
        QVERIFY(f.wires.contains("BSC;"));
        kenwood::KenwoodBackendTestAccess::sameBatch(*f.backend, "BSC041");
        QVERIFY(f.row("reference").value("pending").toBool()); QCOMPARE(f.row("reference").value("value").toInt(), 0);
        f.observe("BSC041"); QVERIFY(!f.row("reference").value("pending").toBool());
        f.onStatus = {}; f.load(); bool retired = false;
        f.onStatus = [&](const QVariantMap& value) {
            if (!retired && findRow(value, "reference").value("pending").toBool()) { retired = true; f.observe("TS1"); }
        };
        f.wires.clear(); f.set("reference", 42); QVERIFY(retired); QVERIFY(!f.wires.contains("BSC042;"));
        QVERIFY(!f.row("reference").value("editable").toBool());
    }
    void rejectedArgumentsNeverDispatchAndReadCanRetry() {
        Fixture f; f.ready(); f.load();
        for (const QString& key : {QString("context"), QString("id"), QString("revision")}) {
            QVariantMap a = f.args("reference"); a["value"] = 1; a[key] = 1;
            f.wires.clear(); QVERIFY(f.errors.contains(f.call("scope.controls.set", a))); QVERIFY(f.wires.isEmpty());
        }
        QVariantMap extra = f.args("reference"); extra["value"] = 1; extra["extra"] = true;
        f.wires.clear(); QVERIFY(f.errors.contains(f.call("scope.controls.set", extra))); QVERIFY(f.wires.isEmpty());
        f.accept = false; const quint64 rejected = f.set("reference", 1);
        QVERIFY(f.errors.contains(rejected)); QVERIFY(f.wires.contains("BSC001;"));
        QCOMPARE(f.row("reference").value("value").toInt(), 0);
        f.accept = true; f.wires.clear(); f.call("scope.controls.get");
        QVERIFY(!f.wires.contains("BSC001;"));
        f.observe("BSC002"); QCOMPARE(f.row("reference").value("value").toInt(), 2);
    }
    void replacementTicketAndPairTypeBoundaries() {
        Fixture f; f.ready(); f.load(); bool replaced = false;
        f.onStatus = [&](const QVariantMap& state) {
            if (!replaced && findRow(state, "reference").value("pending").toBool()) {
                replaced = true; f.observe("BSC041"); f.set("reference", 42);
            }
        };
        f.wires.clear(); f.set("reference", 41);
        QVERIFY(replaced); QVERIFY(!f.wires.contains("BSC041;")); QCOMPARE(f.wires.count("BSC042;"), 1);
        QVERIFY(f.row("reference").value("pending").toBool());
        f.onStatus = {}; f.observe("BSC042"); QVERIFY(!f.row("reference").value("pending").toBool());
        f.load(); f.observe("BS31"); f.observe("BSM01400000014200000");
        const QVariantMap base{{"context", f.state.value("context")}, {"slot", 1}, {"revision", f.range(1).value("revision")},
            {"lowHz", QVariant::fromValue(qint64(14000000))}, {"highHz", QVariant::fromValue(qint64(14200000))}};
        for (const QVariant& bad : QList<QVariant>{QVariant(true), QVariant("14000000"), QVariant(14000000.0),
                QVariant::fromValue(qint64(-1)), QVariant::fromValue(qint64(100000000)), QVariant::fromValue(qint64(14200000))}) {
            QVariantMap a = base; a["lowHz"] = bad; f.wires.clear();
            QVERIFY(f.errors.contains(f.call("scope.ranges.set", a))); QVERIFY(f.wires.isEmpty());
        }
        QVariantMap incomplete = base; incomplete.remove("highHz"); f.wires.clear();
        QVERIFY(f.errors.contains(f.call("scope.ranges.set", incomplete))); QVERIFY(f.wires.isEmpty());
    }

    void rfFramesRespectTransportAndGeometry() {
        Fixture f; f.ready(); f.load(); int count = 0; QByteArray latest;
        QObject::connect(f.backend.get(), &IRadioBackend::spectrumFrameReady, f.backend.get(),
            [&](int, const QByteArray& bytes, const SpectrumCoverage&) { ++count; latest = bytes; });
        const QByteArray valid = "##DD2" + QByteArray("00").repeated(639) + "8C";
        f.observe(valid); QCOMPARE(count, 1); QCOMPARE(latest.size(), int(640 * sizeof(float)));
        float first = 1, last = 1;
        std::memcpy(&first, latest.constData(), sizeof(float));
        std::memcpy(&last, latest.constData() + 639 * sizeof(float), sizeof(float));
        QCOMPARE(first, 0.0F); QCOMPARE(last, -100.0F);
        for (const QByteArray& bad : {valid.chopped(1), valid + "00", QByteArray("##DD2") + QByteArray("8D").repeated(640),
                QByteArray("##DD2") + QByteArray("zz").repeated(640)}) { f.observe(bad); }
        QCOMPARE(count, 1);
        f.observe("DD00"); f.observe(valid); QCOMPARE(count, 1);
        f.observe("DD04"); f.observe(valid); QCOMPARE(count, 1);
        f.observe("DD01"); f.observe("BSO1"); f.observe(valid); QCOMPARE(count, 1);
        f.observe("BSO0"); f.observe("BSK0"); f.observe(valid); QCOMPARE(count, 1);
        f.observe("BSK4"); f.observe("AI0"); f.observe(valid); QCOMPARE(count, 1);
    }

    void sentUnverifiedPairCanRetryAndFreshCallbackWins() {
        Fixture f; f.ready(); f.load(); f.observe("BS31"); f.observe("BSM01400000014200000");
        const auto arguments = [&f](qint64 low, qint64 high) {
            return QVariantMap{{"context", f.state.value("context")}, {"slot", 1}, {"revision", f.range(1).value("revision")},
                {"lowHz", QVariant::fromValue(low)}, {"highHz", QVariant::fromValue(high)}};
        };
        f.wires.clear(); f.call("scope.ranges.set", arguments(14010000, 14190000));
        QVERIFY(!f.range(1).value("pending").toBool()); QVERIFY(!f.range(1).contains("lowHz"));
        f.observe("BSM01401000014190000");
        f.call("scope.ranges.set", arguments(14020000, 14180000));
        QVERIFY(f.wires.contains("BSM11401000014190000;")); QVERIFY(f.wires.contains("BSM11402000014180000;"));
        QVERIFY(!f.range(1).value("pending").toBool()); QVERIFY(!f.range(1).contains("lowHz"));
        f.observe("BSM01402000014180000"); bool injected = false;
        f.onWrite = [&](const QByteArray& wire) {
            if (!injected && wire == "BSM11403000014170000;") {
                injected = true; f.observe("BSM01403000014170000"); f.observe("BSM11403000014170000");
            }
        };
        f.call("scope.ranges.set", arguments(14030000, 14170000)); QVERIFY(injected); f.onWrite = {};
        QCOMPARE(f.range(1).value("lowHz").toLongLong(), qint64(14030000));
        QCOMPARE(f.range(1).value("highHz").toLongLong(), qint64(14170000));
        QVERIFY(!f.range(1).value("pending").toBool());
        // A new explicit request owns a new revision; old terminal cleanup
        // must neither revive old values nor retain old producer ownership.
        f.call("scope.ranges.set", arguments(14040000, 14160000));
        QVERIFY(f.wires.contains("BSM11404000014160000;"));
        QVERIFY(!f.range(1).value("pending").toBool()); QVERIFY(!f.range(1).contains("lowHz"));
    }

    void nestedPairRequestOutlivesOuterTerminalCleanup() {
        Fixture f; f.ready(); f.load(); f.observe("BS31"); f.observe("BSM01400000014200000");
        const auto arguments = [&f](qint64 low, qint64 high) {
            return QVariantMap{{"context", f.state.value("context")}, {"slot", 1}, {"revision", f.range(1).value("revision")},
                {"lowHz", QVariant::fromValue(low)}, {"highHz", QVariant::fromValue(high)}};
        };
        bool replaced = false;
        f.onWrite = [&](const QByteArray& wire) {
            if (!replaced && wire == "BSM11401000014190000;") {
                replaced = true; f.observe("BSM01401000014190000"); f.observe("BSM11401000014190000");
                f.call("scope.ranges.set", arguments(14020000, 14180000));
            }
        };
        f.wires.clear(); f.call("scope.ranges.set", arguments(14010000, 14190000)); f.onWrite = {};
        QVERIFY(replaced); QCOMPARE(f.wires.count("BSM11401000014190000;"), 1); QCOMPARE(f.wires.count("BSM11402000014180000;"), 1);
        QVERIFY(!f.range(1).contains("lowHz")); QVERIFY(!f.range(1).value("pending").toBool());
        kenwood::KenwoodBackendTestAccess::sameBatch(*f.backend, "BSM11401000014190000");
        QVERIFY(!f.range(1).contains("lowHz"));
        f.observe("BSM11402000014180000"); QCOMPARE(f.range(1).value("lowHz").toLongLong(), qint64(14020000));
    }

    void nativeGeometryPendingCannotUseOldSharedShadowTuple() {
        Fixture f; f.ready(); f.load(); int frames = 0;
        QObject::connect(f.backend.get(), &IRadioBackend::spectrumFrameReady, f.backend.get(),
            [&](int, const QByteArray&, const SpectrumCoverage&) { ++frames; });
        const QByteArray bytes = "##DD2" + QByteArray("46").repeated(640);
        f.observe(bytes); QCOMPARE(frames, 1); bool nested = false;
        f.onStatus = [&](const QVariantMap& state) {
            if (!nested && findRow(state, "span").value("pending").toBool()) {
                nested = true; f.observe("BS30"); f.observe(bytes);
            }
        };
        f.wires.clear(); f.set("span", 1); QVERIFY(nested); QVERIFY(f.wires.contains("BS41;"));
        QCOMPARE(frames, 1);
        // Fresh native mode alone must not reuse the old shared span/shift/
        // expansion shadow while the actual span producer still owns them.
        kenwood::KenwoodBackendTestAccess::sameBatch(*f.backend, "BS40"); f.observe(bytes); QCOMPARE(frames, 1);
        f.observe("BS30"); f.observe(bytes); QCOMPARE(frames, 1);
        f.onStatus = {}; f.observe("BS41"); f.observe("BSK4"); f.observe("BSO0"); f.observe("BS30");
        f.observe(bytes); QCOMPARE(frames, 2);
    }

    void resumedOldBatchCannotReplaceReleasedFreshProjectionWidth() {
        Fixture f; f.ready(); f.load(); double sharedWidthMhz = 0; QList<qint64> frameWidths;
        QObject::connect(f.backend.get(), &IRadioBackend::panCenterBandwidthChanged, f.backend.get(),
            [&](const QString&, double, double width) { sharedWidthMhz = width; });
        QObject::connect(f.backend.get(), &IRadioBackend::spectrumFrameReady, f.backend.get(),
            [&](int, const QByteArray&, const SpectrumCoverage& coverage) {
                // Existing Kenwood frames omit optional wider coverage; their
                // effective grid then uses the actual public pan geometry.
                const double width = coverage.valid() ? coverage.highMhz - coverage.lowMhz : sharedWidthMhz;
                frameWidths.append(qRound64(width * 1e6));
            });
        f.observe("BS41"); f.qualifyGeometry(0, 0);
        const QByteArray frame = "##DD2" + QByteArray("46").repeated(640);
        f.observe(frame); QVERIFY(!frameWidths.isEmpty()); QCOMPARE(frameWidths.last(), qint64(5000));
        bool nested = false;
        f.onStatus = [&](const QVariantMap& state) {
            if (!nested && findRow(state, "reference").value("value").toInt() == 1) {
                nested = true; f.set("span", 1); f.qualifyGeometry(0, 1); f.observe(frame);
            }
        };
        // The second old-batch message resumes only after a nested write and
        // complete newer readback have released the new10kHz projection.
        kenwood::KenwoodBackendTestAccess::batch(*f.backend, {"BSC001", "BS40"});
        QVERIFY(nested); f.onStatus = {};
        QVERIFY(frameWidths.size() >= 2); QCOMPARE(frameWidths.last(), qint64(10000));
        const qsizetype beforeFreshFrame = frameWidths.size();
        f.observe(frame); QCOMPARE(frameWidths.size(), beforeFreshFrame + 1);
        QCOMPARE(frameWidths.last(), qint64(10000));
        QCOMPARE(f.row("span").value("value").toInt(), 1);
    }

    void sameTransitionFrameAndDeletionDoNotEscapeOwnership() {
        Fixture f; f.ready(); f.load(); int frames = 0;
        QObject::connect(f.backend.get(), &IRadioBackend::spectrumFrameReady, f.backend.get(),
            [&](int, const QByteArray&, const SpectrumCoverage&) { ++frames; });
        const QByteArray frame = "##DD2" + QByteArray("46").repeated(640);
        f.observe(frame); QCOMPARE(frames, 1);
        kenwood::KenwoodBackendTestAccess::batch(*f.backend, {"FA00014075000", frame});
        QCOMPARE(frames, 1);
        f.load(); f.observe(frame); QCOMPARE(frames, 2);
        Fixture destroyed; destroyed.ready(); destroyed.load();
        const QPointer<Backend> alive(destroyed.backend.get()); bool deleted = false;
        destroyed.onStatus = [&](const QVariantMap& state) {
            if (!deleted && findRow(state, "reference").value("pending").toBool()) { deleted = true; destroyed.backend.reset(); }
        };
        destroyed.wires.clear(); destroyed.set("reference", 1); QVERIFY(deleted); QVERIFY(alive.isNull());
        QVERIFY(!destroyed.wires.contains("BSC001;"));
    }

    void externalModeOrRangeChangeRetiresCurrentBounds() {
        for (bool changeMode : {false, true}) {
            Fixture f; f.ready(); f.load(); f.qualifyGeometry(1, 0, 1);
            QVERIFY(f.state.value("currentBounds").toMap().contains("lowHz"));
            QVERIFY(f.state.value("transport").toMap().value("rfProjectionAvailable").toBool());
            f.observe(changeMode ? "BS32" : "BS52");
            QVERIFY2(!f.state.value("currentBounds").toMap().contains("lowHz"),
                changeMode ? "External Fixed to Auto-scroll must retire old current bounds." : "External fixed-slot selection must retire old current bounds.");
            QVERIFY(!f.state.value("currentBounds").toMap().contains("highHz"));
            QVERIFY(!f.state.value("transport").toMap().value("rfProjectionAvailable").toBool());
            f.observe("BSM01401000014300000");
            QCOMPARE(f.state.value("currentBounds").toMap().value("lowHz").toLongLong(), qint64(14010000));
            QCOMPARE(f.state.value("currentBounds").toMap().value("highHz").toLongLong(), qint64(14300000));
            QVERIFY(f.state.value("transport").toMap().value("rfProjectionAvailable").toBool());
            // An unchanged mode/slot poll must retain this qualified pair.
            f.observe(changeMode ? "BS32" : "BS52");
            QCOMPARE(f.state.value("currentBounds").toMap().value("lowHz").toLongLong(), qint64(14010000));
        }
    }
    void coalescedExternalRangeCannotReprojectOldBounds() {
        Fixture f; f.ready(); f.load(); f.qualifyGeometry(1, 0, 1);
        int frames = 0; double sharedWidthMhz = 0; qint64 widthHz = 0;
        QObject::connect(f.backend.get(), &IRadioBackend::panCenterBandwidthChanged, f.backend.get(),
            [&](const QString&, double, double width) { sharedWidthMhz = width; });
        QObject::connect(f.backend.get(), &IRadioBackend::spectrumFrameReady, f.backend.get(),
            [&](int, const QByteArray&, const SpectrumCoverage& coverage) {
                ++frames; widthHz = qRound64((coverage.valid() ? coverage.highMhz - coverage.lowMhz : sharedWidthMhz) * 1e6);
            });
        f.observe("BSM01400000014200000");
        const QByteArray frame = "##DD2" + QByteArray("46").repeated(640);
        f.observe(frame); QCOMPARE(frames, 1); QCOMPARE(widthHz, qint64(200000));
        kenwood::KenwoodBackendTestAccess::batch(*f.backend, {"BS52", "BSM01400000014200000"});
        QVERIFY(!f.state.value("currentBounds").toMap().contains("lowHz"));
        f.observe(frame); QCOMPARE(frames, 1);
        f.observe("BSM01401000014300000");
        const int beforeFresh = frames; f.observe(frame); QCOMPARE(frames, beforeFresh + 1); QCOMPARE(widthHz, qint64(290000));
    }

    void afFrameFreshnessRetiresOnceWithoutControlSnapshotSpam() {
        Fixture f; f.ready(); f.load(); const QByteArray bytes = "##DD3" + QByteArray("00").repeated(213);
        f.observe(bytes); QVERIFY(f.frame.value("known").toBool());
        const QString controlRevision = f.state.value("statusRevision").toString();
        const quint64 first = f.frame.value("sequence").toString().toULongLong();
        kenwood::KenwoodBackendTestAccess::pollFrameAge(*f.backend, 1499); QVERIFY(f.frame.value("known").toBool());
        f.wires.clear(); kenwood::KenwoodBackendTestAccess::pollFrameAge(*f.backend, 1501);
        QVERIFY(!f.frame.value("known").toBool()); QVERIFY(f.frame.value("values").toList().isEmpty());
        QVERIFY(!f.frame.value("reason").toString().isEmpty()); QVERIFY(f.frame.value("sequence").toString().toULongLong() > first);
        const QString retired = f.frame.value("sequence").toString();
        kenwood::KenwoodBackendTestAccess::pollFrameAge(*f.backend, 2000); QCOMPARE(f.frame.value("sequence").toString(), retired);
        QCOMPARE(f.state.value("statusRevision").toString(), controlRevision); QVERIFY(f.wires.isEmpty());
        f.observe(bytes); QVERIFY(f.frame.value("known").toBool());
    }

    void strictAfFramesAndPrerequisites() {
        Fixture f; f.ready(); f.load();
        QByteArray valid("##DD3"); valid += QByteArray("00").repeated(212); valid += "32";
        f.observe(valid); QVERIFY(f.frame.value("known").toBool());
        const QVariantList values = f.frame.value("values").toList(); QCOMPARE(values.size(), 213); QCOMPARE(values.first().toInt(), 0); QCOMPARE(values.last().toInt(), 50);
        const QString sequence = f.frame.value("sequence").toString();
        for (const QByteArray& bad : {valid.chopped(1), valid + "00", QByteArray("##DD3") + QByteArray("33").repeated(213),
                QByteArray("##DD3") + QByteArray("xz").repeated(213)}) { f.observe(bad); }
        QCOMPARE(f.frame.value("sequence").toString(), sequence);
        f.observe("DD10"); f.observe(valid); QVERIFY(!f.frame.value("known").toBool());
        f.observe("DD11"); f.observe("BS13"); f.observe(valid); QVERIFY(!f.frame.value("known").toBool());
        f.observe("BS10"); f.observe("AI0"); f.observe(valid); QVERIFY(!f.frame.value("known").toBool());
    }
};
int main(int argc, char** argv) {
    TestSettingsProfile profile("kenwood-scope-controls"); if (!profile.isValid()) { return 1; }
    QCoreApplication app(argc, argv); KenwoodScopeControlsTest test; return QTest::qExec(&test, argc, argv);
}
#include "kenwood_scope_controls_test.moc"
