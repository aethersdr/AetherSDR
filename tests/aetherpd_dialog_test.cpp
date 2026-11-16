// Socket-free production model and widget test: credentials and HTTP are injected.
#include "AetherPDTestRadio.h"
#include "TestSettingsProfile.h"
#include "core/AppSettings.h"
#include "core/ThemeManager.h"
#include "gui/AetherPDDialog.h"
#include "gui/AetherPDMeasurementDialog.h"
#include "gui/AetherPDMeasurementSpectrum.h"
#include "gui/AetherPDMeasurementExportDialog.h"
#include <QBuffer>
#include <QComboBox>
#include <QFile>
#include <QTemporaryDir>
#include <QTest>
#include <QAccessible>
#include <QImage>
#include <limits>
#include <cstdint>
#include "models/SliceModel.h"
#include "core/TxKeyingMarker.h"

#include <QApplication>
#include <QCheckBox>
#include <QDoubleSpinBox>
#include <QSpinBox>
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
    {
        AetherPDMeasurementSpectrum spectrum;
        spectrum.resize(900, 360);
        if (!check(spectrum.exportPng().isEmpty(), "no spectrum PNG exists before a valid released result")) { return 1; }
        AetherPDMeasurementModel::Result capture;
        capture.sourceId = 42;
        capture.powerSetting = 5;
        capture.completedAtUtc = QDateTime::fromString(QStringLiteral("2026-11-16T10:00:00Z"), Qt::ISODate);
        capture.config.capture.centerFreqHz = 7150000;
        capture.measurement.correctionActive = false;
        txlin::LinearityResult value;
        value.valid = true; value.clippingKnown = false;
        value.sampleRateHz = 48000; value.fftSize = 16384; value.binHz = 48000.0 / 16384;
        value.tone1 = {true, -1000, -10}; value.tone2 = {true, 1000, -10};
        value.toneSpacingHz = 2000; value.noiseFloorDb = -90;
        value.spectrumDb.assign(16384, -90);
        value.spectrumDb[7851] = -10; value.spectrumDb[8533] = -10;
        value.spectrumDb[7168] = -50;
        value.products.push_back({3, false, -3000, -50, -40, -46.0206, {}, false});
        value.products.push_back({3, true, 3000, -90, -80, -86.0206, {}, true});
        value.acprLow = txlin::BandPower{-6000, -4000, -70, -60, -66.0206, {}};
        value.acprHigh = txlin::BandPower{4000, 6000, -70, -60, -66.0206, {}};
        capture.measurement.measurement = value;
        capture.measurement.captures.push_back({"fixture", 1, {}, {}});
        if (!check(capture.sourceSummary().contains("identity not reported")
            && capture.toCsv().contains("\"Capture IQ stream ID\",\"\",\"\",\"fixture\",\"unknown\""),
            "legacy export must preserve missing source identity")) { return 1; }
        capture.measurement.captures.front().sourceStream = txlin::CaptureStreamIdentity{
            1073741825, std::numeric_limits<std::uint64_t>::max()};
        if (!check(capture.toCsv().contains("18446744073709551615"),
            "CSV must preserve the full unsigned generation")) { return 1; }
        spectrum.setResult(&capture);
        QAccessibleInterface* accessible = QAccessible::queryAccessibleInterface(&spectrum);
        const QImage image = QImage::fromData(spectrum.exportPng(), "PNG");
        if (!check(!image.isNull() && image.size() == spectrum.size()
            && image.text(QStringLiteral("Description")).contains("-40.0 dBc per tone")
            && image.text(QStringLiteral("Description")).contains("noise limited")
            && image.text(QStringLiteral("Description")).contains("5 W setting")
            && image.text(QStringLiteral("Description")).contains("DAX IQ stream 0x40000001")
            && !image.text(QStringLiteral("Description")).contains("dBFS")
            && accessible && accessible->role() == QAccessible::Graphic
            && accessible->text(QAccessible::Value).contains("ADC headroom unknown"),
            "PNG and accessible graphic preserve per-tone references, power-setting and calibration limits")) { return 1; }
        int peakPixels = 0;
        const QColor cyan = ThemeManager::instance().color(&spectrum, QStringLiteral("color.canon.cyan"));
        for (int y = image.height() / 3; y < image.height() * 2 / 3; ++y) {
            for (int x = image.width() / 5; x < image.width() / 3; ++x) {
                const QColor pixel = image.pixelColor(x, y);
                if (std::abs(pixel.red() - cyan.red()) < 15 && std::abs(pixel.green() - cyan.green()) < 15
                    && std::abs(pixel.blue() - cyan.blue()) < 15) { ++peakPixels; }
            }
        }
        if (!check(peakPixels > 15 && accessible->text(QAccessible::Value).contains("-6000.0 to -4000.0 Hz"),
            "single-bin IMD peak survives display reduction and integration-band bounds remain accessible")) { return 1; }
        const QString path = qEnvironmentVariable("AETHERPD_SPECTRUM_SCREENSHOT");
        if (!path.isEmpty() && !check(image.save(path), "spectrum preview saved")) { return 1; }
        capture.measurement.measurement->valid = false;
        capture.measurement.measurement->invalidReason = "Capture no longer valid";
        spectrum.setResult(&capture);
        if (!check(spectrum.exportPng().isEmpty()
            && accessible->text(QAccessible::Value).contains("Capture no longer valid"),
            "invalid capture clears cached trace and PNG instead of exporting a stale measurement")) { return 1; }
        capture.measurement.measurement->valid = true;
        capture.measurement.measurement->spectrumDb[5] = std::numeric_limits<float>::quiet_NaN();
        spectrum.setResult(&capture);
        if (!check(spectrum.exportPng().isEmpty(), "nonfinite spectrum cannot enter raster output")) { return 1; }
        spectrum.setResult(nullptr);
        if (!check(!spectrum.hasResult() && spectrum.exportPng().isEmpty(), "connection clear withdraws spectrum export")) { return 1; }
    }
    {
        QTemporaryDir destination;
        if (!check(destination.isValid(), "export destination is isolated")) { return 1; }
        QByteArray json("{\"capture\":\"original\"}\n");
        const QByteArray csv("Measurement,Value\r\nIMD3,-40\r\n");
        QByteArray png;
        QBuffer encoded(&png);
        QImage fixture(2, 2, QImage::Format_ARGB32);
        fixture.fill(Qt::transparent);
        if (!encoded.open(QIODevice::WriteOnly) || !fixture.save(&encoded, "PNG")) { return 1; }
        const QByteArray expected[] = {json, csv, png};
        const QString suffixes[] = {QStringLiteral("json"), QStringLiteral("csv"), QStringLiteral("png")};
        for (int format = 0; format < 3; ++format) {
            AetherPDMeasurementExportDialog exporter(json, csv, png);
            exporter.setOption(QFileDialog::DontUseNativeDialog);
            exporter.setDirectory(destination.path());
            exporter.open();
            QCoreApplication::processEvents();
            auto* formats = exporter.findChild<QComboBox*>(QStringLiteral("fileTypeCombo"));
            if (!check(formats != nullptr, "save chooser exposes format selection")) { return 1; }
            formats->setFocus();
            QTest::keyClick(formats, Qt::Key_Home);
            for (int i = 0; i < format; ++i) { QTest::keyClick(formats, Qt::Key_Down); }
            if (!check(exporter.defaultSuffix() == suffixes[format], "operator format selection updates automatic extension")) { return 1; }
            const QString base = destination.filePath(QStringLiteral("capture%1").arg(format));
            exporter.selectFile(base);
            int receipts = 0;
            bool saved = false;
            QObject::connect(&exporter, &AetherPDMeasurementExportDialog::exportCompleted,
                &exporter, [&](bool success) { ++receipts; saved = success; });
            const QByteArray original = json;
            json = "replacement capture";
            if (!QMetaObject::invokeMethod(&exporter, "accept", Qt::DirectConnection)) { return 1; }
            json = original;
            QFile output(base + QLatin1Char('.') + suffixes[format]);
            if (!check(receipts == 1 && saved && output.open(QIODevice::ReadOnly) && output.readAll() == expected[format],
                "chooser writes exactly the selected snapshot with its matching extension")) { return 1; }
        }
        const QString existingPath = destination.filePath(QStringLiteral("existing.json"));
        QFile existing(existingPath);
        if (!existing.open(QIODevice::WriteOnly) || existing.write("keep original") != 13) { return 1; }
        existing.close();
        {
            AetherPDMeasurementExportDialog exporter(json, csv, {});
            exporter.setOption(QFileDialog::DontUseNativeDialog);
            exporter.setDirectory(destination.path());
            exporter.selectFile(existingPath);
            int receipts = 0;
            QObject::connect(&exporter, &AetherPDMeasurementExportDialog::exportCompleted, &exporter, [&](bool) { ++receipts; });
            exporter.open();
            exporter.reject();
            if (!check(receipts == 0 && exporter.nameFilters().size() == 2
                && existing.open(QIODevice::ReadOnly) && existing.readAll() == "keep original",
                "cancel preserves destination and invalid spectrum offers no PNG export")) { return 1; }
            existing.close();
        }
        {
            AetherPDMeasurementExportDialog exporter({}, csv, {});
            exporter.setOption(QFileDialog::DontUseNativeDialog);
            exporter.setOption(QFileDialog::DontConfirmOverwrite);
            exporter.setDirectory(destination.path());
            exporter.selectFile(existingPath);
            int receipts = 0;
            bool saved = true;
            QObject::connect(&exporter, &AetherPDMeasurementExportDialog::exportCompleted,
                &exporter, [&](bool success) { ++receipts; saved = success; });
            exporter.open();
            if (!QMetaObject::invokeMethod(&exporter, "accept", Qt::DirectConnection)) { return 1; }
            if (!check(receipts == 1 && !saved && existing.open(QIODevice::ReadOnly) && existing.readAll() == "keep original",
                "missing snapshot reports failure without replacing an existing destination")) { return 1; }
        }
    }
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
    ok &= check(!button(*dialog, "pdEnableCorrection")->isEnabled()
        && !button(*dialog, "pdBypassCorrection")->isEnabled()
        && !button(*dialog, "pdBypassCorrection")->accessibleDescription().isEmpty(),
        "legacy correction controls were active or lacked an unavailable reason");
    QJsonObject selectable = QJsonDocument::fromJson(idleBody).object();
    selectable["correction_control"] = true;
    button(*dialog, "pdRefresh")->click();
    network.last->complete(200, QJsonDocument(selectable).toJson());
    ok &= check(button(*dialog, "pdBypassCorrection")->isEnabled()
        && !button(*dialog, "pdEnableCorrection")->isEnabled(), "correction choice did not follow observed state");
    button(*dialog, "pdBypassCorrection")->click();
    ok &= check(network.sent.back().request.url().path() == QStringLiteral("/v1/aetherpd/correction/bypass")
        && !button(*dialog, "pdBypassCorrection")->isEnabled(), "bypass click did not use bounded model request");
    network.last->complete(200, R"({"state":"correction_bypass_accepted"})");
    ok &= check(label("pdState") == QStringLiteral("Status unknown"), "bypass ACK became observed state");
    selectable["correction_enabled"] = false; selectable["state"] = "disabled";
    network.last->complete(200, QJsonDocument(selectable).toJson());
    ok &= check(button(*dialog, "pdEnableCorrection")->isEnabled()
        && !button(*dialog, "pdBypassCorrection")->isEnabled(), "bypass status lost inverse control");
    button(*dialog, "pdEnableCorrection")->click();
    network.last->complete(200, R"({"state":"correction_enable_accepted"})");
    network.last->complete(200, idleBody);
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
    auto* windowChoice = measurement->findChild<QComboBox*>(QStringLiteral("pdMeasureWindow"));
    auto* averageChoice = measurement->findChild<QSpinBox*>(QStringLiteral("pdMeasureAverages"));
    auto* orderChoice = measurement->findChild<QComboBox*>(QStringLiteral("pdMeasureOrder"));
    const size_t beforeOptions = network.sent.size();
    windowChoice->setCurrentIndex(windowChoice->findData(static_cast<int>(txlin::WindowKind::Hann)));
    averageChoice->setValue(2);
    orderChoice->setCurrentIndex(orderChoice->findData(5));
    const auto selectedOptions = radio.aetherPDMeasurementModel().analysisOptions();
    ok &= check(selectedOptions.usable && selectedOptions.values == AetherPDMeasurementModel::AnalysisOptions{txlin::WindowKind::Hann, 2, 5}
        && network.sent.size() == beforeOptions && captureNetwork.sent.empty(),
        "analysis controls persist locally for this radio without capture or RF commands");
    ok &= check(!radio.aetherPDMeasurementModel().setAnalysisOptions({}, {}),
        "foreign session cannot replace analysis preferences");
    measurement->findChild<QCheckBox*>(QStringLiteral("pdMeasureDummyLoad"))->setChecked(true);
    measurement->findChild<QDoubleSpinBox*>(QStringLiteral("pdMeasurePowerLimit"))->setValue(10);
    ok &= check(!mb("pdMeasurePrepare")->isEnabled(), "source capability without identity cannot prepare from UI");
    measurementStatus["measurement"] = QJsonObject{{"source_id", "0000000000000001"}, {"tones", false}};
    button(*dialog, "pdRefresh")->click();
    network.last->complete(200, QJsonDocument(measurementStatus).toJson());
    ok &= check(mb("pdMeasurePrepare")->isEnabled(), "fresh armed identity permits receive preparation");
    button(*dialog, "pdRefresh")->click();
    ok &= check(mb("pdMeasurePrepare")->isEnabled() && !mb("pdMeasureTransmit")->isEnabled()
        && captureNetwork.sent.empty(), "routine status poll preserves receive preparation without RF or capture");
    network.last->complete(503, "{}");
    ok &= check(!mb("pdMeasurePrepare")->isEnabled(), "failed status poll withdraws receive preparation");
    button(*dialog, "pdRefresh")->click();
    network.last->complete(200, QJsonDocument(measurementStatus).toJson());
    mb("pdMeasureCancelSource")->click();
    ok &= check(!mb("pdMeasurePrepare")->isEnabled(), "pending source mutation blocks receive preparation");
    network.last->complete(503, "{}");
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
    ok &= check(!windowChoice->isEnabled() && !averageChoice->isEnabled() && !orderChoice->isEnabled()
        && !radio.aetherPDMeasurementModel().setAnalysisOptions(radio.controlSession(), {})
        && radio.aetherPDMeasurementModel().analysisOptions().values == selectedOptions.values,
        "analysis options freeze in both GUI and model throughout measurement preparation");
    if (!captureNetwork.last) { return 1; }
    const QString id = QStringLiteral("0123456789abcdef0123456789abcdef");
    const auto encode = [](const QJsonObject& o) { return QJsonDocument(o).toJson(QJsonDocument::Compact); };
    captureNetwork.last->complete(200, encode({{"capture_id", id}, {"format", "f32le-iq-v1"}, {"source", QJsonObject{{"kind", "local_dax_iq"}, {"stream_id", 1073741825}, {"generation", "7"}}},
        {"sample_rate_hz", 48000}, {"center_hz", 7150000}, {"sample_count", 16384}, {"chunk_samples", 512},
        {"keyed", false}, {"correcting", false}, {"first_receive_ns", "1000000000"}, {"last_receive_ns", "1341000000"},
        {"measurement", QJsonObject{{"source_id", "0000000000000001"}, {"tones", false}}}}));
    for (int i = 0; i < 32; ++i) {
        captureNetwork.captureChunk(i)->complete(200, encode({{"capture_id", id}, {"offset", i * 512},
            {"iq_f32le", QString::fromLatin1(QByteArray(4096, '\0').toBase64())}}));
    }
    ok &= check(mb("pdMeasureTransmit")->isEnabled()
        && radio.aetherPDMeasurementModel().state() == AetherPDMeasurementModel::State::Ready,
        "complete receive frame enables explicit transmit only");
    const QString readyText = measurement->findChild<QLabel*>(QStringLiteral("pdMeasureState"))->text();
    ok &= check(readyText.contains("7.150000 MHz") && readyText.contains("ANT1")
        && readyText.contains("nominal setting 5 W") && readyText.contains("abort limit 10.0 W")
        && readyText.contains("DAX IQ stream 0x40000001") && readyText.contains("Hann") && readyText.contains("2 averages") && readyText.contains("IMD5"),
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
    captureNetwork.last->complete(200, encode({{"capture_id", id}, {"format", "f32le-iq-v1"}, {"source", QJsonObject{{"kind", "local_dax_iq"}, {"stream_id", 1073741825}, {"generation", "7"}}},
        {"sample_rate_hz", 48000}, {"center_hz", 7150000}, {"sample_count", 16384}, {"chunk_samples", 512},
        {"keyed", false}, {"correcting", false}, {"first_receive_ns", "2000000000"}, {"last_receive_ns", "2341000000"},
        {"measurement", QJsonObject{{"source_id", "0000000000000001"}, {"tones", false}}}}));
    for (int i = 0; i < 32; ++i) {
        captureNetwork.captureChunk(i)->complete(200, encode({{"capture_id", id}, {"offset", i * 512},
            {"iq_f32le", QString::fromLatin1(QByteArray(4096, '\0').toBase64())}}));
    }
    const size_t beforeOldPreparation = captureNetwork.sent.size();
    ok &= check(bool(oldPreparation) && mb("pdMeasureTransmit")->isEnabled(), "replacement preparation is independently ready");
    if (oldPreparation) { oldPreparation(); }
    ok &= check(captureNetwork.sent.size() == beforeOldPreparation
        && radio.aetherPDMeasurementModel().state() == AetherPDMeasurementModel::State::Ready,
        "queued authorization for an older preparation cannot start its replacement");
    mb("pdMeasureTransmit")->click();
    ok &= check(measurementText().contains("Radio telemetry stopped updating")
        && captureNetwork.sent.size() == beforeOldPreparation,
        "transmit refusal exposes the missing telemetry reason and sends no helper request");
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
