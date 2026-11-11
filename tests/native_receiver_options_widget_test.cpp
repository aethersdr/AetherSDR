#include "TestSettingsProfile.h"
#include "core/AppSettings.h"
#include "core/backends/kenwood/KenwoodBackend.h"
#include "gui/NativeReceiverOptionsWidget.h"
#include "models/RadioModel.h"
#include <QApplication>
#include <QComboBox>
#include <QDir>
#include <QLabel>
#include <QPushButton>
#include <QtTest>
#include <memory>
#include <utility>

namespace AetherSDR::kenwood {
struct KenwoodBackendTestAccess {
    static void authenticate(KenwoodBackend& b) {
        b.m_clock.start(); b.m_protocol.start("fixture", "fixture", false, 0);
        b.m_protocol.receive("##CN1;", 1); b.m_protocol.receive("##ID1;", 2); b.ready();
    }
    static void feed(KenwoodBackend& b, const QByteArray& frame) { b.adopt(frame); }
    static void poll(KenwoodBackend& b, qint64 advance = 0) { b.pollReceiverOptions(b.m_clock.elapsed() + advance); }
};
}
using namespace AetherSDR;
namespace {
struct Fixture {
    QList<QByteArray> writes;
    QVariantMap state;
    RadioModel model;
    kenwood::KenwoodBackend* backend = nullptr;
    std::unique_ptr<NativeReceiverOptionsWidget> widget;
    Fixture(bool receive = true) {
        auto owned = std::make_unique<kenwood::KenwoodBackend>(kenwood::KenwoodBackend::Transport{
            [this](const QByteArray& bytes) { writes.append(bytes); return true; }, {}});
        backend = owned.get(); model.setBackendForTest(std::move(owned), "kenwood");
        QObject::connect(backend, &IRadioBackend::extensionStatus, &model,
            [this](const QString&, const QString& kind, const QVariantMap& value) { if (kind == "receiverOptions") { state = value; } });
        widget = std::make_unique<NativeReceiverOptionsWidget>(model); widget->resize(900, 650); widget->show();
        kenwood::KenwoodBackendTestAccess::authenticate(*backend); feed("ID024");
        if (receive) { feed("RX"); }
        emit model.connectionStateChanged(true); writes.clear();
    }
    void feed(const QByteArray& frame) { kenwood::KenwoodBackendTestAccess::feed(*backend, frame); }
    void poll(qint64 advance = 0) { kenwood::KenwoodBackendTestAccess::poll(*backend, advance); }
    QPushButton* button(const char* name) { return widget->findChild<QPushButton*>(name); }
    QComboBox* draft(const QString& id) { return widget->findChild<QComboBox*>("receiverOptions_" + id + "Draft"); }
    QLabel* observed(const QString& id) { return widget->findChild<QLabel*>("receiverOptions_" + id + "Observed"); }
    QPushButton* apply(const QString& id) { return widget->findChild<QPushButton*>("receiverOptions_" + id + "Apply"); }
    void read() {
        QTest::mouseClick(button("receiverOptionsRead"), Qt::LeftButton); poll();
        feed("QS0"); poll(); feed("BK0"); poll(); feed("BY1"); poll(); feed("RE0000"); poll();
    }
};
bool choose(QComboBox* combo, bool value) {
    if (!combo) { return false; }
    const int index = combo->findData(value); if (index < 0) { return false; }
    combo->setCurrentIndex(index);
    return QMetaObject::invokeMethod(combo, "activated", Qt::DirectConnection, Q_ARG(int, index));
}
}
class NativeReceiverOptionsWidgetTest final : public QObject {
    Q_OBJECT
private slots:
    void recordingUsesActualReadRouteAndNeverExposesActions() {
        Fixture f(false);
        QLabel* recording = f.widget->findChild<QLabel*>("receiverOptionsRecording");
        QVERIFY(recording); QVERIFY(recording->text().contains("Unknown")); QVERIFY(f.writes.isEmpty());
        f.read(); QCOMPARE(f.writes, (QList<QByteArray>{"QS;", "BK;", "BY;", "RE;"}));
        QVERIFY(recording->text().contains("Stopped"));
        f.feed("RE3050"); QVERIFY(recording->text().contains("Quick playback")); QVERIFY(recording->text().contains("50%"));
        f.feed("RE4000"); QVERIFY(recording->text().contains("pause/resume"));
        f.feed("RE6000"); QVERIFY(recording->text().contains("Recording failure"));
        f.feed("RE2000"); QVERIFY(recording->text().contains("Unknown")); QVERIFY(recording->text().contains("Recording failure"));
        const QVariantMap good = f.state;
        QVariantMap invalid = good; invalid["statusRevision"] = "18446744073709551615";
        QVariantMap badRecording = invalid.value("recording").toMap(); badRecording["state"] = true; invalid["recording"] = badRecording;
        emit f.backend->extensionStatus("kenwood", "receiverOptions", invalid);
        QVERIFY(!f.apply("speakerMute")->isEnabled());
        f.feed("RE7100"); QVERIFY(recording->text().contains("Playback failure")); QVERIFY(recording->text().contains("100%"));
        QVERIFY(!recording->text().contains("Unknown"));
        f.writes.clear(); f.widget.reset(); QVERIFY(f.writes.isEmpty());
    }
    void openingIsCachedAndExplicitReadNeedsNoVfoOrReceiveMode() {
        Fixture f(false);
        QVERIFY(f.writes.isEmpty());
        QVERIFY(f.button("receiverOptionsRead") && f.button("receiverOptionsRead")->isEnabled());
        QVERIFY(f.observed("speakerMute")->text().contains("Unknown", Qt::CaseInsensitive));
        f.read();
        QCOMPARE(f.writes, (QList<QByteArray>{"QS;", "BK;", "BY;", "RE;"}));
        QVERIFY(!f.observed("speakerMute")->text().contains("Unknown", Qt::CaseInsensitive));
        QVERIFY(!f.apply("speakerMute")->isEnabled());
        QVERIFY(!f.widget->findChild<QPushButton*>("receiverOptions_busyApply"));
    }
    void bothFieldsAndChoicesUseProductionModelAndActualButtons() {
        for (const auto& [id, command] : {std::pair{"speakerMute", "QS"}, std::pair{"signalBlanking", "BK"}}) {
            Fixture f; f.read();
            for (bool value : {false, true, false}) {
                f.writes.clear();
                QVERIFY(choose(f.draft(id), value)); QVERIFY(f.writes.isEmpty());
                QVERIFY(f.apply(id)->isEnabled());
                QTest::mouseClick(f.apply(id), Qt::LeftButton); f.poll();
                QVERIFY(f.writes.contains(QByteArray(command) + (value ? "1;" : "0;")));
                QVERIFY(f.writes.contains(QByteArray(command) + ";"));
                QVERIFY(!f.apply(id)->isEnabled());
                f.feed(QByteArray(command) + (value ? "1" : "0")); f.poll();
                QVERIFY(f.draft(id)->isEnabled());
            }
            const QString qa = qEnvironmentVariable("AETHER_RECEIVER_OPTIONS_QA_DIR");
            if (!qa.isEmpty()) { QVERIFY(QDir().mkpath(qa)); QApplication::processEvents(); QVERIFY(f.widget->grab().save(qa + '/' + id + ".png")); }
            f.writes.clear(); f.widget.reset(); QVERIFY(f.writes.isEmpty());
        }
    }
    void busyExpiresWithoutTurningIntoAnOffObservation() {
        Fixture f; f.read(); f.writes.clear(); f.poll(1601);
        QVERIFY(f.observed("busy")->text().contains("Unknown", Qt::CaseInsensitive));
        QVERIFY(f.state.value("rows").toList().last().toMap().value("value").toBool());
        QVERIFY(f.writes.isEmpty());
    }
    void invalidHighRevisionCannotLendAuthorityOrPoisonRecovery() {
        Fixture f; f.read(); const QVariantMap good = f.state;
        QVariantMap bad = good; bad["statusRevision"] = "999999"; bad["connected"] = false;
        emit f.backend->extensionStatus("kenwood", "receiverOptions", bad);
        QVERIFY(!f.apply("speakerMute")->isEnabled());
        QVERIFY(f.observed("speakerMute")->text().contains("Unknown", Qt::CaseInsensitive));
        emit f.backend->extensionStatus("kenwood", "receiverOptions", good);
        QVERIFY(f.draft("speakerMute")->isEnabled());
        QVERIFY(!f.observed("speakerMute")->text().contains("Unknown", Qt::CaseInsensitive));
    }
    void safetyContextChangeRetiresStagedDraft() {
        Fixture f; f.read(); QVERIFY(choose(f.draft("speakerMute"), true));
        f.feed("TX0");
        QVERIFY(!f.apply("speakerMute")->isEnabled());
        f.feed("RX"); f.writes.clear();
        QTest::mouseClick(f.apply("speakerMute"), Qt::LeftButton); f.poll();
        QVERIFY(!f.writes.contains("QS1;"));
    }
};
int main(int argc, char** argv) {
    TestSettingsProfile profile("native-receiver-options-widget"); if (!profile.isValid()) { return 1; }
    QApplication app(argc, argv); AppSettings::instance().load();
    NativeReceiverOptionsWidgetTest test; return QTest::qExec(&test, argc, argv);
}
#include "native_receiver_options_widget_test.moc"
