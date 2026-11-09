// Actual Qt controls -> RadioModel -> KenwoodBackend -> injected transport.
// No reference peer, socket or hardware; observations are explicit fixtures.
#include "TestSettingsProfile.h"
#include "core/AppSettings.h"
#include "core/backends/kenwood/KenwoodBackend.h"
#include "gui/NativeScopeSettingsWidget.h"
#include "models/RadioModel.h"
#include <QAccessible>
#include <QApplication>
#include <QByteArray>
#include <QList>
#include <QMetaType>
#include <QString>
#include <QStringList>
#include <QVariantList>
#include <QVariantMap>
#include <QComboBox>
#include <QGraphicsView>
#include <QGraphicsScene>
#include <QGraphicsPathItem>
#include <QPainterPath>
#include <QStandardItem>
#include <QLabel>
#include <QPointer>
#include <QPushButton>
#include <QScopeGuard>
#include <QSpinBox>
#include <QStandardItemModel>
#include <QtTest>
#include <functional>
#include <memory>

namespace AetherSDR::kenwood {
struct KenwoodBackendTestAccess {
    static void authenticate(KenwoodBackend& b) {
        b.m_clock.start(); b.m_protocol.start("scope-user", "scope-password", false, 0);
        b.m_protocol.receive("##CN1;", 1); b.m_protocol.receive("##ID1;", 2); b.ready();
    }
    static void observe(KenwoodBackend& b, const QByteArray& bytes) { b.adopt(bytes); }
};
}
namespace {
using namespace AetherSDR;
using Backend = kenwood::KenwoodBackend;
struct Row { const char* id; const char* command; int minimum; int maximum; int digits; };
const QList<Row> kRows{
    {"display", "BS0", 0, 1, 1}, {"display_type", "BS1", 0, 4, 1}, {"mode", "BS3", 0, 2, 1},
    {"span", "BS4", 0, 7, 1}, {"fixed_range", "BS5", 0, 3, 1}, {"pause", "BS6", 0, 1, 1},
    {"marker", "BS7", 0, 1, 1}, {"attenuation", "BS8", 0, 3, 1}, {"max_hold", "BS9", 0, 1, 1},
    {"average", "BSA", 0, 3, 1}, {"waterfall_speed", "BSB", 1, 4, 1}, {"reference", "BSC", 0, 60, 3},
    {"audio_attenuation", "BSG", 0, 3, 1}, {"audio_span", "BSH", 0, 1, 1},
    {"oscilloscope_level", "BSI", 0, 3, 1}, {"oscilloscope_sweep", "BSJ", 0, 5, 1},
    {"shift", "BSK", 0, 8, 1}, {"audio_pause", "BSN", 0, 1, 1}, {"expand", "BSO", 0, 1, 1},
    {"bandscope_output", "DD0", 0, 3, 1}, {"filter_output", "DD1", 0, 1, 1}};
QVariantMap rowFor(const QVariantMap& state, const QString& id) {
    for (const QVariant& item : state.value("controls").toList()) { if (item.toMap().value("id").toString() == id) { return item.toMap(); } }
    return {};
}
std::function<void(QAccessibleEvent*)> accessibilityCallback;
void accessibleUpdate(QAccessibleEvent* event) { if (accessibilityCallback) { accessibilityCallback(event); } }
struct Fixture {
    QList<QByteArray> wires;
    QVariantMap state, frame;
    std::function<void(const QVariantMap&)> onStatus;
    std::function<void(quint64, const QVariant&)> onResult;
    RadioModel model;
    Backend* backend = nullptr;
    std::unique_ptr<NativeScopeSettingsWidget> widget;
    explicit Fixture(char mode = '2') { install(mode); makeWidget(); }
    void install(char mode = '2') {
        auto owned = std::make_unique<Backend>(Backend::Transport{[this](const QByteArray& bytes) { wires.append(bytes); return true; }, {}});
        backend = owned.get(); model.setBackendForTest(std::move(owned), "kenwood");
        QObject::connect(backend, &IRadioBackend::extensionStatus, &model,
            [this](const QString&, const QString& kind, const QVariantMap& value) {
                if (kind == "scopeControls") { state = value; if (onStatus) { onStatus(value); } }
                if (kind == "scopeFilterFrame") { frame = value; }
            });
        QObject::connect(backend, &IRadioBackend::extensionResult, &model,
            [this](quint64 id, const QVariant& value) { if (onResult) { onResult(id, value); } });
        kenwood::KenwoodBackendTestAccess::authenticate(*backend);
        for (const QByteArray& bytes : {QByteArray("ID024"), QByteArray("FR0"), QByteArray("FT0"), QByteArray("FA00014074000"),
                QByteArray("OM0") + mode, QByteArray("SF000014074000") + mode, QByteArray("TS0"), QByteArray("RX")}) { observe(bytes); }
        // setBackendForTest omits production setupBackend's lifecycle wiring;
        // supply its real public notification, without a copied status relay.
        emit model.connectionStateChanged(true);
    }
    void makeWidget() { widget = std::make_unique<NativeScopeSettingsWidget>(model); widget->resize(950, 950); widget->show(); QCoreApplication::processEvents(); }
    void observe(const QByteArray& bytes) { kenwood::KenwoodBackendTestAccess::observe(*backend, bytes); }
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
    template<class T> T child(const QString& name) { return widget->findChild<T>(name); }
    QPushButton* apply(const QString& id) { return child<QPushButton*>("nativeScope_" + id + "_Apply"); }
    QLabel* observed(const QString& id) { return child<QLabel*>("nativeScope_" + id + "_Observed"); }
    void stage(const QString& id, int value) {
        if (id == "reference") {
            QSpinBox* spin = child<QSpinBox*>("nativeScope_reference_Draft");
            spin->setFocus(); spin->selectAll(); QTest::keyClicks(spin, QString::number(value)); spin->interpretText();
        } else {
            QComboBox* combo = child<QComboBox*>("nativeScope_" + id + "_Choice");
            const int target = combo->findData(value);
            if (target < 0) { qFatal("Missing native raw choice %s=%d", qPrintable(id), value); }
            combo->setFocus(); QTest::keyClick(combo, Qt::Key_Home);
            for (int index = 0; index < target; ++index) { QTest::keyClick(combo, Qt::Key_Down); }
            QTest::keyClick(combo, Qt::Key_Return);
        }
    }
    void clickRead() { QTest::mouseClick(child<QPushButton*>("nativeScopeRead"), Qt::LeftButton); }
    void publish(const QVariantMap& value) { emit backend->extensionStatus("kenwood", "scopeControls", value); }
};
}
class NativeScopeSettingsWidgetTest final : public QObject {
    Q_OBJECT
private slots:
    void unknownRowsAreVisibleDimmedAndReadable() {
        Fixture f;
        QVERIFY(f.child<QPushButton*>("nativeScopeRead")->isEnabled());
        for (const Row& row : kRows) {
            QVERIFY(f.observed(row.id)); QVERIFY(!f.observed(row.id)->isHidden());
            QVERIFY(f.apply(row.id)); QVERIFY(!f.apply(row.id)->isEnabled());
            QVERIFY(!f.apply(row.id)->accessibleDescription().isEmpty());
        }
        QVERIFY(f.observed("overflow")); QVERIFY(!f.apply("overflow")->isEnabled());
        for (int slot = 1; slot <= 3; ++slot) {
            QVERIFY(f.child<QLabel*>(QString("nativeScopeRange%1_Observed").arg(slot)));
            QVERIFY(!f.child<QPushButton*>(QString("nativeScopeRange%1_Apply").arg(slot))->isEnabled());
        }
        f.wires.clear(); f.clickRead();
        for (const QByteArray& wire : f.wires) { QVERIFY(!wire.startsWith("PS") && !wire.startsWith("TX")); }
    }
    void everyFiniteChoiceUsesActualUiAndFreshReadback() {
        Fixture f;
        for (const Row& row : kRows) {
            f.load(); if (QByteArray(row.command) == "BS5") { f.qualifyGeometry(1); }
            for (int value = row.minimum; value <= row.maximum; ++value) {
                QVERIFY2(f.apply(row.id)->isEnabled(), row.id);
                const int old = rowFor(f.state, row.id).value("value").toInt();
                f.wires.clear(); f.stage(row.id, value); QVERIFY(f.wires.isEmpty());
                QTest::mouseClick(f.apply(row.id), Qt::LeftButton);
                const QByteArray expected = QByteArray(row.command) + QByteArray::number(value).rightJustified(row.digits, '0') + ';';
                QVERIFY2(f.wires.contains(expected), expected.constData()); QCOMPARE(f.wires.count(expected), 1);
                QCOMPARE(rowFor(f.state, row.id).value("value").toInt(), old);
                QVERIFY(!f.apply(row.id)->isEnabled()); QVERIFY(!f.apply(row.id)->accessibleDescription().isEmpty());
                f.observe(expected.chopped(1)); QCOMPARE(rowFor(f.state, row.id).value("value").toInt(), value);
                if (QStringList{"mode", "span", "fixed_range", "shift", "expand"}.contains(row.id)) {
                    f.qualifyGeometry(QString(row.id) == "mode" ? value : QString(row.id) == "fixed_range" ? 1 : 0,
                        QString(row.id) == "span" ? value : 0, QString(row.id) == "fixed_range" ? value : 0,
                        QString(row.id) == "shift" ? value : 4, QString(row.id) == "expand" ? value : 0);
                }
                QVERIFY(f.apply(row.id)->isEnabled()); QVERIFY(!f.observed(row.id)->text().isEmpty());
            }
        }
    }
    void everyModeRetainsRealRoutesAndSerialChoicesAreDimmed() {
        for (char mode : QByteArray("12345679ABCDEF")) {
            Fixture f(mode); f.load();
            for (const Row& row : kRows) {
                f.load(); if (QByteArray(row.command) == "BS5") { f.qualifyGeometry(1); }
                f.wires.clear(); f.stage(row.id, row.maximum); QTest::mouseClick(f.apply(row.id), Qt::LeftButton);
                const QByteArray expected = QByteArray(row.command) + QByteArray::number(row.maximum).rightJustified(row.digits, '0') + ';';
                QVERIFY2(f.wires.contains(expected), row.id); f.observe(expected.chopped(1));
            }
        }
        Fixture f; f.load();
        for (const QString& id : {QString("bandscope_output"), QString("filter_output")}) {
            auto* combo = f.child<QComboBox*>("nativeScope_" + id + "_Choice");
            auto* model = qobject_cast<QStandardItemModel*>(combo->model()); QVERIFY(model);
            for (int raw = id == "bandscope_output" ? 4 : 2; raw <= (id == "bandscope_output" ? 5 : 2); ++raw) {
                const int index = combo->findData(raw); QVERIFY(index >= 0); QVERIFY(!model->item(index)->isEnabled());
                QVERIFY(!combo->itemData(index, Qt::ToolTipRole).toString().isEmpty());
            }
        }
        f.observe("DD04"); QVERIFY(f.observed("bandscope_output")->text().contains("COM", Qt::CaseInsensitive));
    }
    void actionsResetsAndFullRangePairsAreProductionUi() {
        Fixture f; f.load(); f.wires.clear();
        QTest::mouseClick(f.child<QPushButton*>("nativeScope_span_Reset"), Qt::LeftButton); QVERIFY(f.wires.contains("BS49;")); f.observe("BS42");
        f.wires.clear(); QTest::mouseClick(f.child<QPushButton*>("nativeScopeAction_clear"), Qt::LeftButton); QVERIFY(f.wires.contains("BSD;"));
        f.qualifyGeometry(1, 2);
        f.wires.clear(); QTest::mouseClick(f.child<QPushButton*>("nativeScopeAction_marker_center"), Qt::LeftButton); QVERIFY(f.wires.contains("BSE;"));
        f.qualifyGeometry(1, 2);
        for (int slot = 1; slot <= 3; ++slot) {
            const QString prefix = QString("nativeScopeRange%1_").arg(slot);
            for (const QString& field : {QString("Low"), QString("High")}) {
                auto* spin = f.child<QSpinBox*>(prefix + field); QVERIFY(spin);
                spin->setFocus(); spin->selectAll(); QTest::keyClicks(spin, field == "Low" ? "14010000" : "14190000"); spin->interpretText();
            }
            QCOMPARE(f.child<QSpinBox*>(prefix + "Low")->value(), 14010000);
            QCOMPARE(f.child<QSpinBox*>(prefix + "High")->value(), 14190000);
            QVERIFY2(f.child<QPushButton*>(prefix + "Apply")->isEnabled(), "Complete fresh Fixed geometry and both explicit typed drafts are required before pair Apply.");
            f.wires.clear(); QTest::mouseClick(f.child<QPushButton*>(prefix + "Apply"), Qt::LeftButton);
            QVERIFY(f.wires.contains("BSM" + QByteArray::number(slot) + "1401000014190000;"));
            f.observe("BSM" + QByteArray::number(slot) + "1401000014190000");
            f.qualifyGeometry(1, 2);
            QVERIFY(f.child<QPushButton*>(prefix + "Reset")->isEnabled());
            f.wires.clear(); QTest::mouseClick(f.child<QPushButton*>(prefix + "Reset"), Qt::LeftButton);
            QVERIFY(f.wires.contains("BSM" + QByteArray::number(slot) + "9999999999999999;"));
            f.observe("BSM" + QByteArray::number(slot) + "1400000014200000");
            f.qualifyGeometry(1, 2);
        }
    }
    void olderPushAndGetCannotRegressNewerObservation() {
        Fixture f; f.load(); bool nested = false; QString nestedLabel; const QVariantMap old = f.state;
        f.onStatus = [&](const QVariantMap& state) {
            if (!nested && rowFor(state, "reference").value("value").toInt() == 41) {
                nested = true; f.observe("BSC042"); nestedLabel = f.observed("reference")->text();
            }
        };
        f.observe("BSC041"); QVERIFY(nested); f.onStatus = {};
        QCOMPARE(f.observed("reference")->text(), nestedLabel);
        const QString latest = f.observed("reference")->text();
        f.publish(old); QCOMPARE(f.observed("reference")->text(), latest);
        bool getNested = false; QString getLabel;
        f.onResult = [&](quint64, const QVariant& value) {
            if (!getNested && value.toMap().contains("controls")) { getNested = true; f.observe("BSC043"); getLabel = f.observed("reference")->text(); }
        };
        f.clickRead(); QVERIFY(getNested); f.onResult = {};
        QCOMPARE(f.observed("reference")->text(), getLabel);
        QCOMPARE(rowFor(f.state, "reference").value("value").toInt(), 43);
        QVERIFY(f.observed("reference")->text() != latest);
    }
    void malformedHighBodyCannotPoisonReadRecoveryOrDraft() {
        Fixture f; f.load(); f.stage("reference", 17);
        for (const QString& revision : {QString("1000000"), QString("18446744073709551615")}) {
            QVariantMap bad = f.state; bad["statusRevision"] = revision; QVariantList rows = bad.value("controls").toList();
            for (QVariant& item : rows) { QVariantMap row = item.toMap(); if (row.value("id") == "reference") { row["value"] = true; item = row; } }
            bad["controls"] = rows; f.publish(bad); QVERIFY(!f.apply("reference")->isEnabled());
            f.clickRead(); QVERIFY(f.apply("reference")->isEnabled());
            QCOMPARE(f.child<QSpinBox*>("nativeScope_reference_Draft")->value(), 17);
        }
        for (int index = 0; index < 30; ++index) { f.observe("BSC001"); }
        f.install(); f.load();
        QCOMPARE(f.child<QSpinBox*>("nativeScope_reference_Draft")->value(), 0);
        QVERIFY(f.apply("reference")->isEnabled());
    }
    void malformedDescriptorTokensAndRowsRecoverThroughActualRead() {
        Fixture f; f.load(); const QVariantMap valid = f.state;
        QList<QVariantMap> malformed;
        for (const QVariant& token : QList<QVariant>{QVariant(1), QVariant(true), QVariant("01"), QVariant("-1"), QVariant("18446744073709551616")}) {
            QVariantMap bad = valid; bad["statusRevision"] = token; malformed.append(bad);
        }
        QVariantMap bad = valid; bad["context"] = 1; malformed.append(bad);
        bad = valid; bad["editable"] = "true"; malformed.append(bad);
        bad = valid; bad["controls"] = QVariantList{}; malformed.append(bad);
        bad = valid; QVariantList rows = bad.value("controls").toList(); rows[1] = rows[0]; bad["controls"] = rows; malformed.append(bad);
        for (const QVariantMap& value : malformed) {
            f.publish(value); QVERIFY(!f.apply("reference")->isEnabled());
            f.clickRead(); QVERIFY(f.apply("reference")->isEnabled());
        }
    }

    void rangeResetCannotBypassGlobalReadOnlyDescriptor() {
        Fixture f; f.load(); f.observe("BS31"); f.observe("BSM01400000014200000");
        QVariantMap readOnly = f.state; readOnly["editable"] = false;
        QVariantList rows = readOnly.value("controls").toList();
        for (QVariant& item : rows) { QVariantMap row = item.toMap(); row["editable"] = false; item = row; }
        readOnly["controls"] = rows;
        QVariantList ranges = readOnly.value("ranges").toList();
        for (QVariant& item : ranges) { QVariantMap row = item.toMap(); row["editable"] = false; row["resettable"] = true; item = row; }
        readOnly["ranges"] = ranges;
        QVariantMap actions = readOnly.value("actions").toMap();
        for (auto it = actions.begin(); it != actions.end(); ++it) { QVariantMap action = it.value().toMap(); action["editable"] = false; it.value() = action; }
        readOnly["actions"] = actions; f.publish(readOnly);
        for (int slot = 1; slot <= 3; ++slot) {
            auto* reset = f.child<QPushButton*>(QString("nativeScopeRange%1_Reset").arg(slot));
            QVERIFY2(!reset->isEnabled(), "Typed range.resettable cannot override global editable=false.");
            f.wires.clear(); QTest::mouseClick(reset, Qt::LeftButton); QVERIFY(f.wires.isEmpty());
        }
    }

    void actualAfFrameUsesRelativePlotAndSuppressionReason() {
        Fixture f; f.load(); const QByteArray frame = "##DD3" + QByteArray("00").repeated(212) + "32";
        f.observe(frame); auto* plot = f.child<QGraphicsView*>("nativeScopeFilterPlot"); QVERIFY(plot); QVERIFY(plot->scene());
        bool spectrumPath = false;
        for (QGraphicsItem* item : plot->scene()->items()) {
            const auto* path = qgraphicsitem_cast<QGraphicsPathItem*>(item);
            if (path && path->path().elementCount() == 213) { spectrumPath = true; }
        }
        QVERIFY2(spectrumPath, "Actual213-bin backend frame must become the production plot path, not just an axis item.");
        f.observe("BS13"); f.observe(frame);
        auto* reason = f.child<QLabel*>("nativeScopeFilterStatus"); QVERIFY(reason); QVERIFY(!reason->text().isEmpty());
    }
    void malformedControlsRecoveryCannotResetSameContextAfFrameFloor() {
        Fixture f; f.load();
        const auto hasFrame = [&f] {
            auto* plot = f.child<QGraphicsView*>("nativeScopeFilterPlot");
            if (!plot || !plot->scene()) { return false; }
            for (QGraphicsItem* item : plot->scene()->items()) {
                const auto* path = qgraphicsitem_cast<QGraphicsPathItem*>(item);
                if (path && path->path().elementCount() == 213) { return true; }
            }
            return false;
        };
        const QByteArray oldBytes = "##DD3" + QByteArray("00").repeated(213);
        const QByteArray newerBytes = "##DD3" + QByteArray("10").repeated(213);
        f.observe(oldBytes); QVERIFY(hasFrame()); const QVariantMap oldFrame = f.frame;
        f.observe(newerBytes); QVERIFY(hasFrame());
        QVERIFY(f.frame.value("sequence").toString().toULongLong() > oldFrame.value("sequence").toString().toULongLong());
        QVariantMap malformed = f.state; malformed["statusRevision"] = "18446744073709551615"; malformed["editable"] = "true";
        f.publish(malformed); QVERIFY(!hasFrame());
        f.clickRead(); QVERIFY(!hasFrame());
        emit f.backend->extensionStatus("kenwood", "scopeFilterFrame", oldFrame);
        QVERIFY2(!hasFrame(), "Same-context control recovery must not revive a captured older AF frame.");
        f.observe(newerBytes); QVERIFY(hasFrame());
    }

    void transportPresentationStatesAreExplicit() {
        RadioModel disconnected;
        NativeScopeSettingsWidget unavailable(disconnected);
        auto* unknown = unavailable.findChild<QLabel*>("nativeScopeTransport"); QVERIFY(unknown);
        QCOMPARE(unknown->text(), QString("RF projection: Unknown\nAF spectrum: Unknown"));
        Fixture f; f.load(); auto* status = f.child<QLabel*>("nativeScopeTransport"); QVERIFY(status);
        QVERIFY(f.state.value("transport").toMap().value("rfProjectionAvailable").toBool());
        QVERIFY(f.state.value("transport").toMap().value("filterFrameAvailable").toBool());
        QVERIFY(status->text().contains("RF projection: Available"));
        QVERIFY(status->text().contains("AF spectrum: Prerequisites available; received-frame status is shown below."));
        f.observe("DD00"); f.observe("DD10");
        QVERIFY(status->text().contains("RF projection: Unavailable: "));
        QVERIFY(status->text().contains("AF spectrum: Unavailable: "));
        QVERIFY(status->text().contains(f.state.value("transport").toMap().value("rfProjectionReason").toString()));
        QVERIFY(status->text().contains(f.state.value("transport").toMap().value("filterFrameReason").toString()));
    }
    void knownOverflowControlsExplainReadOnlyAfterRefreshAndDraft() {
        Fixture f; f.load(); f.observe("BSL0");
        auto* choice = f.child<QComboBox*>("nativeScope_overflow_Choice");
        auto* apply = f.apply("overflow"); QVERIFY(choice && apply);
        const auto check = [&] {
            return !choice->isEnabled() && !apply->isEnabled()
                && choice->accessibleDescription().contains("read-only", Qt::CaseInsensitive)
                && apply->accessibleDescription().contains("read-only", Qt::CaseInsensitive)
                && choice->toolTip().contains("read-only", Qt::CaseInsensitive)
                && apply->toolTip().contains("read-only", Qt::CaseInsensitive);
        };
        QVERIFY2(check(), "Confirmed overflow is read-only, not missing an editable receive context.");
        f.clickRead(); QVERIFY(check());
        f.wires.clear(); f.stage("reference", 17); QVERIFY(f.wires.isEmpty()); QVERIFY(check());
    }

    void accessibilityNewerObservationAndDeletionFence() {
        Fixture f; f.load();
        const auto previous = QAccessible::installUpdateHandler(accessibleUpdate);
        const auto restore = qScopeGuard([&] { accessibilityCallback = {}; QAccessible::installUpdateHandler(previous); });
        QAccessible::setActive(true);
        if (!QAccessible::isActive()) { QSKIP("Platform accessibility backend inactive; no callback proof."); }
        bool nested = false; QString nestedLabel;
        accessibilityCallback = [&](QAccessibleEvent* event) {
            if (!nested && event->object() == f.observed("reference")) { nested = true; f.observe("BSC042"); nestedLabel = f.observed("reference")->text(); }
        };
        f.observe("BSC041"); QVERIFY2(nested, "Actual observed-label accessibility publication must invoke callback.");
        QCOMPARE(f.observed("reference")->text(), nestedLabel);
        const QString latest = f.observed("reference")->text(); f.observe("BSC042"); QCOMPARE(f.observed("reference")->text(), latest);
        bool deleted = false; QPointer<NativeScopeSettingsWidget> alive(f.widget.get());
        accessibilityCallback = [&](QAccessibleEvent* event) {
            if (!deleted && event->object() == f.observed("reference")) { deleted = true; f.widget.reset(); }
        };
        f.observe("BSC043"); QVERIFY(deleted); QVERIFY(alive.isNull());
    }
};
int main(int argc, char** argv) {
    TestSettingsProfile profile("native-scope-settings-widget"); if (!profile.isValid()) { return 1; }
    QApplication app(argc, argv); AppSettings::instance().load(); NativeScopeSettingsWidgetTest test; return QTest::qExec(&test, argc, argv);
}
#include "native_scope_settings_widget_test.moc"
