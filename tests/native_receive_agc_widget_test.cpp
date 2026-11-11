// Actual Qt events -> production model -> injected native CAT; no radio/socket.
#include "TestSettingsProfile.h"
#include "core/AppSettings.h"
#include "core/backends/kenwood/KenwoodBackend.h"
#include "gui/NativeReceiveAgcWidget.h"
#include "models/RadioModel.h"

#include <QApplication>
#include <QAccessible>
#include <QScopeGuard>
#include <QByteArray>
#include <QCheckBox>
#include <QCoreApplication>
#include <QLabel>
#include <QList>
#include <QPointer>
#include <QPushButton>
#include <QSpinBox>
#include <QStringList>
#include <QVariantMap>
#include <QtTest>
#include <functional>
#include <memory>
#include <utility>

namespace AetherSDR::kenwood {
struct KenwoodBackendTestAccess {
    static void authenticate(KenwoodBackend& backend)
    {
        backend.m_clock.start(); backend.m_protocol.start("fixture-user", "fixture-password", false, 0);
        backend.m_protocol.receive("##CN1;", 1); backend.m_protocol.receive("##ID1;", 2); backend.ready();
    }
    static void observe(KenwoodBackend& backend, const QByteArray& message) { backend.adopt(message); }
};
}
namespace {
using namespace AetherSDR;
using Backend = kenwood::KenwoodBackend;
std::function<void(QAccessibleEvent*)> accessibilityCallback;
void accessibleUpdate(QAccessibleEvent* event) { if (accessibilityCallback) { accessibilityCallback(event); } }
template<class T> T* child(QWidget& widget, const QString& name)
{
    T* result = widget.findChild<T*>(QStringLiteral("nativeReceiveAgc") + name);
    if (!result) { qFatal("Missing AGC control: %s", qPrintable(name)); }
    return result;
}
void edit(QSpinBox* spin, int value)
{
    spin->setFocus(); spin->selectAll(); QTest::keyClicks(spin, QString::number(value)); QTest::keyClick(spin, Qt::Key_Return);
}
struct Fixture {
    QList<QByteArray> writes;
    QVariantMap status;
    std::function<void(const QByteArray&)> onWrite;
    std::function<void(const QVariantMap&)> onStatus;
    std::function<void(quint64, const QVariant&)> onResult;
    RadioModel model;
    Backend* backend = nullptr;
    std::unique_ptr<NativeReceiveAgcWidget> widget;
    explicit Fixture(char mode = '2') { install(mode); makeWidget(); }
    void install(char mode = '2')
    {
        auto owned = std::make_unique<Backend>(Backend::Transport{
            [this](const QByteArray& wire) { writes.append(wire); if (onWrite) { onWrite(wire); } return true; }, {}});
        backend = owned.get(); model.setBackendForTest(std::move(owned), "kenwood");
        QObject::connect(backend, &IRadioBackend::extensionStatus, &model,
            [this](const QString&, const QString& kind, const QVariantMap& value) {
                if (kind == "receiveAgc") { status = value; if (onStatus) { onStatus(value); } }
            });
        QObject::connect(backend, &IRadioBackend::extensionResult, &model,
            [this](quint64 id, const QVariant& result) { if (onResult) { onResult(id, result); } });
        kenwood::KenwoodBackendTestAccess::authenticate(*backend);
        for (const QByteArray& message : {QByteArray("ID024"), QByteArray("FR0"), QByteArray("FT0"), QByteArray("TS0"),
                QByteArray("SF000014074000") + mode, QByteArray("RX")}) { observe(message); }
        // setBackendForTest omits setupBackend wiring. Use the real public
        // model publication/binding edge, not a copied extension relay.
        emit model.connectionStateChanged(true);
    }
    void makeWidget()
    {
        widget = std::make_unique<NativeReceiveAgcWidget>(model); widget->resize(1000, 650); widget->show();
        QCoreApplication::processEvents();
    }
    void observe(const QByteArray& wire) { kenwood::KenwoodBackendTestAccess::observe(*backend, wire); }
    void load(const QByteArray& vector = "GT141209", int enabled = 1, int threshold = 5, int selected = 1)
    {
        observe(vector); observe("AQ0" + QByteArray::number(enabled));
        observe("AQ1" + QByteArray::number(threshold).rightJustified(2, '0')); observe("GC" + QByteArray::number(selected));
    }
    QPushButton* button(const QString& name) { return child<QPushButton>(*widget, name); }
    QSpinBox* spin(const QString& name) { return child<QSpinBox>(*widget, name); }
    QLabel* label(const QString& name) { return child<QLabel>(*widget, name); }
    QCheckBox* quick() { return child<QCheckBox>(*widget, "QuickEnabled"); }
    void click(const QString& name) { QTest::mouseClick(button(name), Qt::LeftButton); }
    void publish(const QVariantMap& state) { emit backend->extensionStatus("kenwood", "receiveAgc", state); }
};
}
class NativeReceiveAgcWidgetTest : public QObject {
    Q_OBJECT
private slots:
    void unknownIsUnknown()
    {
        Fixture f;
        QCOMPARE(f.quick()->checkState(), Qt::PartiallyChecked);
        QCOMPARE(f.label("Observedslow")->text(), QString("Unknown"));
        QVERIFY(!f.button("Applyslow")->isEnabled()); QVERIFY(!f.button("Restore")->isEnabled());
        QVERIFY(!f.quick()->accessibleDescription().isEmpty());
    }
    void everyPresetSetAndResetUsesCompositeWire()
    {
        const QStringList banks{"slow", "mid", "fast"};
        for (int bank = 0; bank < 3; ++bank) {
            for (int target : {1, 11, 20}) {
                Fixture f; f.load();
                QVERIFY2(f.spin("Draft" + banks[bank])->isEnabled(), qPrintable(banks[bank]));
                edit(f.spin("Draft" + banks[bank]), target); QVERIFY(f.button("Apply" + banks[bank])->isEnabled());
                const QString old = f.label("Observed" + banks[bank])->text(); f.writes.clear();
                f.click("Apply" + banks[bank]);
                QByteArray expected("GT141209"); expected.replace(2 + bank * 2, 2, QByteArray::number(target).rightJustified(2, '0'));
                QCOMPARE(f.writes, QList<QByteArray>({expected + ';', "GT;"}));
                QCOMPARE(f.label("Observed" + banks[bank])->text(), old);
                QVERIFY(!f.button("Reset" + banks[bank])->isEnabled());
                f.observe(expected); QCOMPARE(f.label("Observed" + banks[bank])->text(), QString("Confirmed index: %1").arg(target));
                f.writes.clear(); f.click("Reset" + banks[bank]);
                expected.replace(2 + bank * 2, 2, "99");
                QCOMPARE(f.writes, QList<QByteArray>({expected + ';', "GT;"}));
                QVERIFY(!f.label("Observed" + banks[bank])->text().contains("99"));
                f.observe("GT141209"); QVERIFY(f.button("Reset" + banks[bank])->isEnabled());
                QCOMPARE(f.label("Observed" + banks[bank])->text(), QString("Confirmed index: %1").arg(QList<int>{14, 12, 9}[bank]));
            }
        }
    }
    void quickAndThresholdActualControlsAwaitReadback()
    {
        Fixture f; f.load(); f.writes.clear();
        f.quick()->setFocus(); QTest::keyClick(f.quick(), Qt::Key_Space);
        QCOMPARE(f.writes, QList<QByteArray>({"AQ02;", "AQ0;"})); QCOMPARE(f.quick()->checkState(), Qt::Unchecked);
        f.observe("AQ02"); QCOMPARE(f.quick()->checkState(), Qt::Checked);
        QVERIFY(!f.quick()->accessibleDescription().contains("not currently active"));
        f.writes.clear(); f.quick()->setFocus(); QTest::keyClick(f.quick(), Qt::Key_Space);
        QCOMPARE(f.writes, QList<QByteArray>({"AQ01;", "AQ0;"})); f.observe("AQ01");
        for (int target : {1, 7, 10}) {
            edit(f.spin("ThresholdDraft"), target); f.writes.clear(); f.click("ThresholdApply");
            const QByteArray command = "AQ1" + QByteArray::number(target).rightJustified(2, '0');
            QCOMPARE(f.writes, QList<QByteArray>({command + ';', "AQ1;"}));
            f.observe(command); QCOMPARE(f.label("ThresholdObserved")->text(), QString("Confirmed index: %1").arg(target));
        }
        f.writes.clear(); f.click("ThresholdReset"); QCOMPARE(f.writes, QList<QByteArray>({"AQ199;", "AQ1;"}));
        QVERIFY(!f.label("ThresholdObserved")->text().contains("99")); f.observe("AQ105"); QCOMPARE(f.label("ThresholdObserved")->text(), QString("Confirmed index: 5"));
    }
    void deliberateReadQueriesEveryFieldAndRestoreIsObserved()
    {
        Fixture f; f.load(); f.writes.clear(); f.click("Read");
        QCOMPARE(f.writes, QList<QByteArray>({"GT;", "AQ0;", "AQ1;", "GC;"}));
        f.load("GT141209", 1, 5, 0); QVERIFY(f.button("Restore")->isEnabled());
        f.writes.clear(); f.click("Restore"); QCOMPARE(f.writes, QList<QByteArray>({"GC4;", "GC;"}));
        QVERIFY(f.label("Context")->text().contains("Off")); QVERIFY(!f.button("Restore")->isEnabled());
        f.observe("GC3"); QVERIFY(f.label("Context")->text().contains("Fast")); QVERIFY(!f.button("Restore")->isEnabled());
    }
    void everyModeAndAgcOffAdmission()
    {
        for (char mode : QByteArray("12345679ABCDEF")) {
            Fixture f(mode); f.load("GT141209", 1, 5, 0);
            const bool legal = mode != '4' && mode != 'E';
            QCOMPARE(f.spin("Draftslow")->isEnabled(), legal); QCOMPARE(f.quick()->isEnabled(), legal);
            QCOMPARE(f.button("ThresholdReset")->isEnabled(), legal); QCOMPARE(f.button("Restore")->isEnabled(), legal);
            if (!legal) { QVERIFY(!f.quick()->accessibleDescription().isEmpty()); }
        }
    }
    void contextAndRevisionRetireDrafts()
    {
        Fixture f; f.load(); edit(f.spin("Draftslow"), 20); QVERIFY(f.button("Applyslow")->isEnabled());
        f.observe("GT151209"); QCOMPARE(f.spin("Draftslow")->value(), 15); QVERIFY(!f.button("Applyslow")->isEnabled());
        edit(f.spin("Draftslow"), 20); f.observe("SF0000140740003");
        QVERIFY(!f.button("Applyslow")->isEnabled()); f.load(); QVERIFY(!f.button("Applyslow")->isEnabled());
    }
    void malformedDescriptionsFailClosed()
    {
        Fixture f; f.load(); const QVariantMap good = f.status;
        const auto check = [&](const QVariantMap& malformed) {
            f.publish(malformed); QVERIFY(!f.quick()->isEnabled()); QVERIFY(!f.button("Resetslow")->isEnabled());
            QCOMPARE(f.quick()->checkState(), Qt::PartiallyChecked); f.publish(good); QVERIFY(f.quick()->isEnabled());
        };
        QVariantMap bad = good; bad["quickEnabled"] = true; check(bad);
        bad = good; bad["quickThreshold"] = 11; check(bad);
        bad = good; QVariantMap presets = bad["presets"].toMap(); presets.remove("mid"); bad["presets"] = presets; check(bad);
        bad = good; presets = bad["presets"].toMap(); presets["fast"] = 99; bad["presets"] = presets; check(bad);
        bad = good; QVariantMap operations = bad["operations"].toMap(); QVariantMap restore = operations["restore"].toMap();
        restore["editable"] = true; operations["restore"] = restore; bad["operations"] = operations; check(bad);
        bad = good; bad["context"] = 7; check(bad);
        f.publish(bad); QVERIFY(f.button("Read")->isEnabled()); f.writes.clear(); f.click("Read");
        QVERIFY(f.writes.contains("GT;")); f.load(); QVERIFY(f.quick()->isEnabled());
    }
    void synchronousContextWithdrawalPreventsWrite()
    {
        Fixture f; f.load(); edit(f.spin("Draftslow"), 20); bool retired = false;
        f.onStatus = [&](const QVariantMap& state) {
            if (!retired && state.value("presets").toMap().value("pending").toBool()) {
                retired = true; f.observe("SF0000140740004");
            }
        };
        f.writes.clear(); f.click("Applyslow"); QVERIFY(retired); QVERIFY(!f.writes.contains("GT201209;"));
        for (const QString& bank : QStringList{"slow", "mid", "fast"}) {
            QCOMPARE(f.label("Observed" + bank)->text(), QString("Unknown"));
        }
        QCOMPARE(f.quick()->checkState(), Qt::PartiallyChecked);
        QCOMPARE(f.label("ThresholdObserved")->text(), QString("Unknown"));
        QVERIFY(f.label("Context")->text().contains("AGC selection: Unknown"));
    }
    void olderPushCannotRestoreWithdrawnObservations()
    {
        Fixture f; f.load(); const QVariantMap old = f.status;
        f.observe("SF0000140740004");
        QVERIFY(f.status.value("statusRevision").toString().toULongLong() > old.value("statusRevision").toString().toULongLong());
        f.publish(old);
        for (const QString& bank : QStringList{"slow", "mid", "fast"}) {
            QCOMPARE(f.label("Observed" + bank)->text(), QString("Unknown"));
        }
        QCOMPARE(f.quick()->checkState(), Qt::PartiallyChecked); QCOMPARE(f.label("ThresholdObserved")->text(), QString("Unknown"));
        QVERIFY(f.label("Context")->text().contains("AGC selection: Unknown"));
        f.observe("SF0000140740003"); f.load("GT151310", 2, 7, 3); f.publish(old);
        QCOMPARE(f.label("Observedslow")->text(), QString("Confirmed index: 15")); QCOMPARE(f.quick()->checkState(), Qt::Checked);
        f.observe("FR1");
        for (const QString& bank : QStringList{"slow", "mid", "fast"}) { QCOMPARE(f.label("Observed" + bank)->text(), QString("Unknown")); }
        QCOMPARE(f.quick()->checkState(), Qt::PartiallyChecked); QCOMPARE(f.label("ThresholdObserved")->text(), QString("Unknown"));
    }
    void replacementAcceptsLowerNewProducerRevision()
    {
        Fixture f;
        for (int read = 0; read < 20; ++read) { f.load(); }
        const quint64 retiredRevision = f.status.value("statusRevision").toString().toULongLong();
        const QPointer<Backend> retired(f.backend); f.install(); QVERIFY(retired.isNull());
        f.load("GT171411", 2, 8, 3);
        QVERIFY(f.status.value("statusRevision").toString().toULongLong() < retiredRevision);
        QCOMPARE(f.label("Observedslow")->text(), QString("Confirmed index: 17"));
        QCOMPARE(f.quick()->checkState(), Qt::Checked);
        QCOMPARE(f.label("ThresholdObserved")->text(), QString("Confirmed index: 8"));
    }
    void malformedRevisionCannotOrderOrPoisonReadRecovery()
    {
        Fixture f; f.load();
        const QList<QVariant> invalid{QVariant(), QVariant(1), QVariant(true), QVariant(1.0), QVariant(QString()),
            QVariant(QString("-1")), QVariant(QString("+1")), QVariant(QString("01")), QVariant(QString("1 ")),
            QVariant(QString("18446744073709551616")), QVariant(QString(200, QChar('9')))};
        for (const QVariant& revision : invalid) {
            QVariantMap bad = f.status; bad["statusRevision"] = revision; f.publish(bad);
            QCOMPARE(f.quick()->checkState(), Qt::PartiallyChecked); QVERIFY(f.button("Read")->isEnabled());
            f.writes.clear(); f.click("Read"); QCOMPARE(f.writes, QList<QByteArray>({"GT;", "AQ0;", "AQ1;", "GC;"}));
            f.load(); QCOMPARE(f.label("Observedslow")->text(), QString("Confirmed index: 14"));
        }
        for (const QString& high : QStringList{"1000000", "18446744073709551615"}) {
            QVariantMap bad = f.status; bad["statusRevision"] = high; bad["quickEnabled"] = true; f.publish(bad);
            QCOMPARE(f.quick()->checkState(), Qt::PartiallyChecked); f.writes.clear(); f.click("Read");
            QCOMPARE(f.writes, QList<QByteArray>({"GT;", "AQ0;", "AQ1;", "GC;"})); f.load("GT161310", 2, 7, 3);
            QVERIFY(f.status.value("statusRevision").toString().toULongLong() < high.toULongLong());
            QCOMPARE(f.label("Observedslow")->text(), QString("Confirmed index: 16")); QCOMPARE(f.quick()->checkState(), Qt::Checked);
        }
    }
    void independentThresholdDraftSurvivesPresetReadback()
    {
        Fixture f; f.load(); edit(f.spin("ThresholdDraft"), 8); QVERIFY(f.button("ThresholdApply")->isEnabled());
        f.observe("GT151209"); QCOMPARE(f.spin("ThresholdDraft")->value(), 8); QVERIFY(f.button("ThresholdApply")->isEnabled());
        f.observe("AQ106"); QCOMPARE(f.spin("ThresholdDraft")->value(), 6); QVERIFY(!f.button("ThresholdApply")->isEnabled());
    }
    void newerPushedStatusWinsOverOuterGetResult()
    {
        Fixture f; f.load(); bool nested = false;
        f.onResult = [&](quint64, const QVariant& result) {
            if (!nested && result.toMap().contains("presets")) { nested = true; f.observe("GT151209"); }
        };
        f.click("Read"); QVERIFY(nested);
        QVERIFY(f.label("Observedslow")->text().contains("15")); QCOMPARE(f.spin("Draftslow")->value(), 15);
    }
    void accessibilityReentryCannotResumeStaleRender()
    {
        Fixture f; f.load();
        const bool previouslyActive = QAccessible::isActive();
        const QAccessible::UpdateHandler previous = QAccessible::installUpdateHandler(accessibleUpdate);
        const auto cleanup = qScopeGuard([previous, previouslyActive] {
            accessibilityCallback = {}; QAccessible::installUpdateHandler(previous); QAccessible::setActive(previouslyActive);
        });
        QAccessible::setActive(true);
        if (!QAccessible::isActive()) { QSKIP("Platform accessibility backend is inactive; explicit a11y callback reentry is not exercised."); }
        bool nested = false;
        accessibilityCallback = [&](QAccessibleEvent* event) {
            if (!nested && event->object() == f.label("Observedslow")) {
                nested = true; f.observe("GT171410");
            }
        };
        f.observe("GT161309"); QVERIFY(nested);
        QCOMPARE(f.label("Observedslow")->text(), QString("Confirmed index: 17"));
        QCOMPARE(f.label("Observedmid")->text(), QString("Confirmed index: 14"));
        QCOMPARE(f.label("Observedfast")->text(), QString("Confirmed index: 10"));
        nested = false;
        accessibilityCallback = [&](QAccessibleEvent* event) {
            if (!nested && event->object() == f.label("Observedslow")) {
                nested = true; f.backend->disconnectRadio(); emit f.model.connectionStateChanged(false);
            }
        };
        f.observe("GT181511"); QVERIFY(nested);
        for (const QString& bank : QStringList{"slow", "mid", "fast"}) {
            QCOMPARE(f.label("Observed" + bank)->text(), QString("Unknown"));
        }
        QVERIFY(!f.quick()->isEnabled());
    }
    void retiredBackendGetResultCannotReplaceNewSession()
    {
        Fixture f; f.load(); quint64 oldRequest = 0; QVariant oldResult; bool replaced = false;
        f.onResult = [&](quint64 id, const QVariant& result) {
            if (!replaced && result.toMap().contains("presets")) {
                replaced = true; oldRequest = id; oldResult = result;
                f.install(); f.load("GT161310", 2, 7, 3);
            }
        };
        f.click("Read"); QVERIFY(replaced); QVERIFY(oldRequest != 0);
        QVERIFY(f.label("Observedslow")->text().contains("16"));
        emit f.backend->extensionResult(oldRequest, oldResult);
        QVERIFY(f.label("Observedslow")->text().contains("16")); QCOMPARE(f.quick()->checkState(), Qt::Checked);
    }
    void callbackDeletesWidgetAndReplacementRetiresDraft()
    {
        Fixture f; f.load(); edit(f.spin("Draftslow"), 20);
        const QPointer<Backend> old(f.backend); f.install(); QVERIFY(old.isNull());
        QVERIFY(!f.button("Applyslow")->isEnabled()); f.load(); QCOMPARE(f.spin("Draftslow")->value(), 14);
        f.onWrite = [&](const QByteArray& wire) { if (wire.startsWith("AQ02")) { f.widget.reset(); } };
        f.quick()->setFocus(); QTest::keyClick(f.quick(), Qt::Key_Space); QVERIFY(!f.widget);
    }
};
int main(int argc, char** argv)
{
    TestSettingsProfile profile(QStringLiteral("native-receive-agc-widget"));
    if (!profile.isValid()) { return 1; }
    QApplication application(argc, argv);
    AetherSDR::AppSettings::instance().load();
    NativeReceiveAgcWidgetTest test; return QTest::qExec(&test, argc, argv);
}
#include "native_receive_agc_widget_test.moc"
