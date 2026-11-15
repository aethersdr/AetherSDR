// Socket-free production model and widget test: credentials and HTTP are injected.
#include "AetherPDTestRadio.h"
#include "TestSettingsProfile.h"
#include "core/AppSettings.h"
#include "gui/AetherPDDialog.h"
#include "gui/AetherPDMeasurementDialog.h"
#include "models/SliceModel.h"
#include "core/TxKeyingMarker.h"

#include <QApplication>
#include <QCheckBox>
#include <QDoubleSpinBox>
#include <QPlainTextEdit>
#include <QLabel>
#include <QJsonDocument>
#include <QJsonObject>
#include <QPushButton>
#include <functional>
#include <memory>

int main(int argc, char** argv)
{
    TestSettingsProfile profile(QStringLiteral("aetherpd_dialog_test"));
    if (!profile.isValid()) { return 1; }
    QApplication app(argc, argv);
    using namespace AetherSDR;
    using namespace AetherSDR::aetherpd;
    using Access = SessionControlTestAccess;
    AppSettings::instance().load();
    Network network, captureNetwork;
    RadioModel radio;
    Access::install(radio, network);
    check(radio.automationApplySliceFixture(0, QStringLiteral("A")), "injected receive slice created");
    SliceDelta station;
    station.txSlice = true; station.mode = QStringLiteral("USB");
    station.frequency = 7.15; station.txAntenna = QStringLiteral("ANT1");
    radio.slice(0)->applyChanges(station);
    TransmitDelta power; power.rfPower = 5;
    radio.applyBackendTransmitDeltaForTest(power);
    Access::measurement(radio, captureNetwork);
    Access::begin(radio);
    const QString token(43, u'a');
    std::function<void(const QString&)> loaded;
    int loads = 0;
    bool ok = true;
    const AetherPDDialog::TokenLoader loader = [&](const QString& serial, QObject*, auto callback) {
        ok &= check(serial == QStringLiteral("PD-TEST"), "credential request uses original radio serial");
        ++loads;
        loaded = std::move(callback);
    };
    const auto button = [](AetherPDDialog& dialog, const char* name) {
        return dialog.findChild<QPushButton*>(QString::fromLatin1(name));
    };
    auto dialog = std::make_unique<AetherPDDialog>(&radio, loader);
    QCoreApplication::processEvents();
    ok &= check(loads == 1 && loaded && network.sent.empty()
        && !button(*dialog, "pdStart")->isEnabled(), "opening waits for credentials and never starts PD");
    loaded(token);
    ok &= check(network.sent.size() == 1 && network.sent.back().method == QNetworkAccessManager::GetOperation,
                "credentials attach with status only");
    loaded(token);
    ok &= check(network.sent.size() == 1, "duplicate credential callback cannot reattach");
    network.last->complete(503, "{}");
    ok &= check(button(*dialog, "pdStart")->isEnabled()
        && !button(*dialog, "pdArm")->isEnabled(), "explicit Start available while status is unknown; arm refused");
    button(*dialog, "pdStart")->click();
    Reply* start = network.last;
    ok &= check(network.sent.back().request.url().path() == QStringLiteral("/v1/aetherpd/start")
        && !button(*dialog, "pdStart")->isEnabled() && button(*dialog, "pdStop")->isEnabled(),
        "Start is explicit and pending startup remains stoppable");
    button(*dialog, "pdStop")->click();
    ok &= check(start->aborted && network.sent.back().request.url().path() == QStringLiteral("/v1/aetherpd/stop"),
                "Stop interrupts startup through the production model");
    start->complete(202, R"({"state":"start_requested"})");
    network.last->complete(202, R"({"state":"stop_requested"})");
    const auto label = [&](const char* name) {
        return dialog->findChild<QLabel*>(QString::fromLatin1(name))->text();
    };
    ok &= check(label("pdReceipt").contains(QStringLiteral("not yet confirmed")),
                "Stop receipt cannot imply cleanup completion");
    network.last->complete(200, R"({"available":false,"state":"stopped"})");
    ok &= check(label("pdState").contains(QStringLiteral("cleanup confirmed"))
        && !label("pdReceipt").contains(QStringLiteral("not yet confirmed"))
        && !label("pdDetail").contains(QStringLiteral("Dispatch faults"))
        && button(*dialog, "pdStart")->isEnabled() && !button(*dialog, "pdArm")->isEnabled(),
        "supervisor completion visible without live DSP counters or calibration");
    button(*dialog, "pdRefresh")->click();
    network.last->complete(200, R"({"available":false,"state":"cleanup_unconfirmed"})");
    ok &= check(label("pdState").contains(QStringLiteral("cleanup unconfirmed"))
        && label("pdReceipt").contains(QStringLiteral("not yet confirmed"))
        && !label("pdDetail").contains(QStringLiteral("Dispatch faults"))
        && !button(*dialog, "pdArm")->isEnabled(), "cleanup failure remains explicitly unconfirmed");
    button(*dialog, "pdRefresh")->click();
    const size_t beforeRecoveryStatus = network.sent.size();
    network.last->complete(200, R"({"available":false,"state":"stopped_receive_unconfirmed"})");
    ok &= check(label("pdState") == QStringLiteral("Stopped — receive recovery unconfirmed")
        && label("pdReceipt").contains(QStringLiteral("DSP and worker retirement"))
        && label("pdDetail").contains(QStringLiteral("not confirmed restored"))
        && !label("pdDetail").contains(QStringLiteral("Dispatch faults"))
        && !button(*dialog, "pdArm")->isEnabled() && network.sent.size() == beforeRecoveryStatus,
        "receive recovery error is distinct and triggers no automatic restoration or rearm");
    button(*dialog, "pdRefresh")->click();
    network.last->complete(503, "{}");
    ok &= check(label("pdState") == QStringLiteral("Status unknown"),
                "lost status cannot retain completion as current observation");
    button(*dialog, "pdRefresh")->click();
    QFile fixture(QStringLiteral(AETHERPD_TESTDATA_DIR "/idle.json"));
    if (!fixture.open(QIODevice::ReadOnly)) { return 1; }
    const QByteArray idleBody = fixture.readAll();
    network.last->complete(200, idleBody);
    ok &= check(!button(*dialog, "pdStart")->isEnabled() && button(*dialog, "pdArm")->isEnabled(),
                "fresh registered state gates calibration and duplicate startup");
    QJsonObject measurementStatus = QJsonDocument::fromJson(idleBody).object();
    measurementStatus[QStringLiteral("measurement_control")] = true;
    measurementStatus[QStringLiteral("source_purpose")] = QStringLiteral("measurement");
    measurementStatus[QStringLiteral("calibration_state")] = QStringLiteral("armed");
    measurementStatus[QStringLiteral("calibration_samples_left")] = 192000;
    measurementStatus[QStringLiteral("state")] = QStringLiteral("measurement_armed");
    button(*dialog, "pdRefresh")->click();
    const size_t beforeMeasurement = network.sent.size();
    network.last->complete(200, QJsonDocument(measurementStatus).toJson());
    ok &= check(label("pdState") == QStringLiteral("Measurement tones armed — waiting for your transmission")
        && button(*dialog, "pdCancel")->isEnabled() && !button(*dialog, "pdArm")->isEnabled()
        && network.sent.size() == beforeMeasurement, "measurement status neither keys nor rearms and keeps source cancel available");
    button(*dialog, "pdMeasurement")->click();
    AetherPDMeasurementDialog* measurement = dialog->findChild<AetherPDMeasurementDialog*>();
    ok &= check(measurement && captureNetwork.sent.empty(), "opening measurement sends no helper or capture request");
    const auto mb = [measurement](const char* name) { return measurement->findChild<QPushButton*>(QString::fromLatin1(name)); };
    ok &= check(mb("pdMeasureTransmit")->property(kTxKeyingProperty).toBool()
        && !mb("pdMeasureTransmit")->isEnabled() && !mb("pdMeasurePrepare")->isEnabled(),
        "measurement key control is marked and initially unavailable");
    measurement->findChild<QCheckBox*>(QStringLiteral("pdMeasureDummyLoad"))->setChecked(true);
    measurement->findChild<QDoubleSpinBox*>(QStringLiteral("pdMeasurePowerLimit"))->setValue(10);
    ok &= check(!mb("pdMeasurePrepare")->isEnabled(), "source capability without identity cannot prepare from UI");
    QJsonObject unarmedStatus = QJsonDocument::fromJson(idleBody).object();
    unarmedStatus["measurement_control"] = true;
    unarmedStatus["source_purpose"] = QStringLiteral("calibration");
    button(*dialog, "pdRefresh")->click();
    network.last->complete(200, QJsonDocument(unarmedStatus).toJson());
    const auto measurementText = [measurement] {
        return measurement->findChild<QLabel*>(QStringLiteral("pdMeasureState"))->text();
    };
    mb("pdMeasureArm")->click();
    ok &= check(network.sent.back().request.url().path() == QStringLiteral("/v1/aetherpd/measurement/arm"),
        "explicit measurement arm uses the bounded source endpoint");
    network.last->complete(409, "{\"state\":\"calibration_blocked\"}");
    ok &= check(measurementText().contains("request refused"), "refused arm is visible in measurement window");
    const size_t afterRefusal = network.sent.size();
    button(*dialog, "pdRefresh")->click();
    network.last->complete(200, QJsonDocument(unarmedStatus).toJson());
    ok &= check(measurementText().contains("request refused") && network.sent.size() == afterRefusal + 1
        && network.sent.back().method == QNetworkAccessManager::GetOperation,
        "successful status poll preserves arm refusal without retrying the source");
    mb("pdMeasureArm")->click();
    ok &= check(!measurementText().contains("request refused") && network.sent.size() == afterRefusal + 2
        && network.sent.back().request.url().path() == QStringLiteral("/v1/aetherpd/measurement/arm"),
        "only a new explicit arm clears the refusal and submits another source request");
    network.last->complete(200, "{\"state\":\"measurement_arm_accepted\"}");
    measurementStatus["measurement"] = QJsonObject{{"source_id", "0000000000000001"}, {"tones", false}};
    network.last->complete(200, QJsonDocument(measurementStatus).toJson());
    ok &= check(mb("pdMeasurePrepare")->isEnabled(), "observed original source enables explicit receive preparation");
    mb("pdMeasurePrepare")->click();
    ok &= check(radio.aetherPDMeasurementModel().state() == AetherPDMeasurementModel::State::CheckingReceive
        && captureNetwork.sent.size() == 1 && !mb("pdMeasureTransmit")->isEnabled(),
        "UI preparation uses real station configuration and receive-only capture");
    if (!captureNetwork.last) { return 1; }
    const QString id = QStringLiteral("0123456789abcdef0123456789abcdef");
    const auto encode = [](const QJsonObject& o) { return QJsonDocument(o).toJson(QJsonDocument::Compact); };
    captureNetwork.last->complete(200, encode({{"capture_id", id}, {"format", "f32le-iq-v1"},
        {"sample_rate_hz", 48000}, {"center_hz", 7150000}, {"sample_count", 16384}, {"chunk_samples", 512},
        {"keyed", false}, {"correcting", false}, {"first_receive_ns", "1000000000"}, {"last_receive_ns", "1341000000"},
        {"measurement", QJsonObject{{"source_id", "0000000000000001"}, {"tones", false}}}}));
    for (int i = 0; i < 32; ++i) {
        captureNetwork.last->complete(200, encode({{"capture_id", id}, {"offset", i * 512},
            {"iq_f32le", QString::fromLatin1(QByteArray(4096, '\0').toBase64())}}));
    }
    ok &= check(mb("pdMeasureTransmit")->isEnabled()
        && radio.aetherPDMeasurementModel().state() == AetherPDMeasurementModel::State::Ready,
        "complete receive frame enables explicit transmit only");
    const QString readyText = measurement->findChild<QLabel*>(QStringLiteral("pdMeasureState"))->text();
    ok &= check(readyText.contains("7.150000 MHz") && readyText.contains("ANT1")
        && readyText.contains("nominal setting 5 W") && readyText.contains("abort limit 10.0 W"),
        "ready display identifies the original station and separates drive from abort limit");
    const auto bridge = std::make_shared<TxController>(&radio);
    TxKeyingAction::Prepared queued = prepareTxKeyingAction(mb("pdMeasureTransmit"), bridge, "click", {});
    ok &= check(bool(queued), "measurement supplies a scoped automation action");
    const size_t beforeQueued = captureNetwork.sent.size();
    bridge->invalidate();
    if (queued) { queued(); }
    ok &= check(captureNetwork.sent.size() == beforeQueued
        && radio.aetherPDMeasurementModel().state() == AetherPDMeasurementModel::State::Ready,
        "revoked automation input cannot launch a helper or key");
    const auto delayedBridge = std::make_shared<TxController>(&radio);
    TxKeyingAction::Prepared oldPreparation = prepareTxKeyingAction(mb("pdMeasureTransmit"), delayedBridge, "click", {});
    const QString screenshotPath = qEnvironmentVariable("AETHER_PD_MEASUREMENT_SCREENSHOT");
    if (!screenshotPath.isEmpty()) {
        measurement->resize(680, 700);
        QCoreApplication::processEvents();
        ok &= check(measurement->grab().save(screenshotPath), "measurement preview saved");
    }
    mb("pdMeasureStop")->click();
    mb("pdMeasurePrepare")->click();
    ok &= check(radio.aetherPDMeasurementModel().state() == AetherPDMeasurementModel::State::CheckingReceive,
        "a new preparation can follow canceled readiness");
    captureNetwork.last->complete(200, encode({{"capture_id", id}, {"format", "f32le-iq-v1"},
        {"sample_rate_hz", 48000}, {"center_hz", 7150000}, {"sample_count", 16384}, {"chunk_samples", 512},
        {"keyed", false}, {"correcting", false}, {"first_receive_ns", "2000000000"}, {"last_receive_ns", "2341000000"},
        {"measurement", QJsonObject{{"source_id", "0000000000000001"}, {"tones", false}}}}));
    for (int i = 0; i < 32; ++i) {
        captureNetwork.last->complete(200, encode({{"capture_id", id}, {"offset", i * 512},
            {"iq_f32le", QString::fromLatin1(QByteArray(4096, '\0').toBase64())}}));
    }
    const size_t beforeOldPreparation = captureNetwork.sent.size();
    ok &= check(bool(oldPreparation) && mb("pdMeasureTransmit")->isEnabled(), "replacement preparation is independently ready");
    if (oldPreparation) { oldPreparation(); }
    ok &= check(captureNetwork.sent.size() == beforeOldPreparation
        && radio.aetherPDMeasurementModel().state() == AetherPDMeasurementModel::State::Ready,
        "queued authorization for an older preparation cannot start its replacement");
    measurement->reject();
    ok &= check(radio.aetherPDMeasurementModel().state() == AetherPDMeasurementModel::State::Idle,
        "closing measurement cancels preparation without stopping correction");
    const size_t beforeClose = network.sent.size();
    dialog->reject();
    loaded(token);
    ok &= check(radio.aetherPDModel().active() && network.sent.size() == beforeClose,
                "Escape close neither stops correction nor replays credentials");
    dialog.reset();
    radio.detachAetherPD(radio.controlSession());

    dialog = std::make_unique<AetherPDDialog>(&radio, loader);
    QCoreApplication::processEvents();
    const size_t beforeLate = network.sent.size();
    dialog->reject();
    loaded(token);
    ok &= check(!radio.aetherPDModel().active() && network.sent.size() == beforeLate,
                "credential arriving after reject cannot attach a hidden panel");
    dialog.reset();

    dialog = std::make_unique<AetherPDDialog>(&radio, loader);
    QCoreApplication::processEvents();
    Access::retire(radio);
    Access::begin(radio, 8);
    loaded(token);
    ok &= check(network.sent.size() == beforeLate && !radio.aetherPDModel().active(),
                "old window credentials cannot attach after same-address reconnect");
    dialog.reset();

    dialog = std::make_unique<AetherPDDialog>(&radio, loader);
    QCoreApplication::processEvents();
    QMetaObject::Connection destroyOnAttach = QObject::connect(&radio.aetherPDModel(), &AetherPDModel::changed,
        &app, [&] { dialog.reset(); });
    loaded(token);
    QObject::disconnect(destroyOnAttach);
    ok &= check(!dialog && radio.aetherPDModel().active(), "synchronous model observer may destroy panel during attachment");
    network.last->complete(503, "{}");
    dialog = std::make_unique<AetherPDDialog>(&radio, loader);
    QCoreApplication::processEvents();
    network.last->complete(503, "{}");
    QMetaObject::Connection destroyOnStart = QObject::connect(&radio.aetherPDModel(), &AetherPDModel::changed,
        &app, [&] { dialog.reset(); });
    button(*dialog, "pdStart")->click();
    QObject::disconnect(destroyOnStart);
    ok &= check(!dialog && network.sent.back().request.url().path() == QStringLiteral("/v1/aetherpd/start"),
                "request handler survives synchronous panel deletion");
    Access::retire(radio);
    std::cout << (ok ? "PASS" : "FAIL") << ": AetherPD explicit controls and credential/window lifetime\n";
    return ok ? 0 : 1;
}
