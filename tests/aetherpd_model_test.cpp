// Our own transport is injected; these tests open no sockets and key no RF.
#include "AetherPDTestNetwork.h"
#include "models/AetherPDModel.h"
#include "core/txlinearity/AetherPDCaptureSource.h"
#include "core/txlinearity/AetherPDTestClient.h"

#include <QThread>
#include <QJsonDocument>
#include <QJsonObject>

int main(int argc, char** argv)
{
    QCoreApplication app(argc, argv);
    using AetherSDR::AetherPDModel;
    using AetherSDR::AetherPDClient;
    using namespace AetherSDR::aetherpd;
    const QHostAddress address(QStringLiteral("192.168.50.100"));
    const QString token(43, u'a');
    QFile fixture(QStringLiteral(AETHERPD_TESTDATA_DIR "/idle.json"));
    if (!fixture.open(QIODevice::ReadOnly)) {
        return 1;
    }
    const QByteArray idle = fixture.readAll();
    fixture.close();
    fixture.setFileName(QStringLiteral(AETHERPD_TESTDATA_DIR "/armed.json"));
    if (!fixture.open(QIODevice::ReadOnly)) {
        return 1;
    }
    const QByteArray armed = fixture.readAll();
    {
        using AetherSDR::txlin::AetherPDCaptureSource;
        using AetherSDR::txlin::CaptureConfig;
        using AetherSDR::txlin::CaptureFrame;
        bool bindingOk = true;
        for (const int change : {0, 1, 2, 3, 4, 5, 6}) {
            Network controlNetwork, captureNetwork;
            std::unique_ptr<AetherPDModel> owner = std::make_unique<AetherPDModel>(controlNetwork);
            AetherPDCaptureSource source(captureNetwork);
            int frames = 0, errors = 0;
            source.setFrameCallback([&](const CaptureFrame&) { ++frames; });
            source.setErrorCallback([&](const std::string&) { ++errors; });
            bool current = true;
            const StartIdentity identity{QStringLiteral("0D3EA0E8-B052-4AC6-9D32-D96918634469"), 7};
            owner->setSession(address, token, [&] { return current; }, identity);
            bindingOk &= check(!owner->bindCaptureSource(source), "capture bound before observed availability");
            controlNetwork.last->complete(200, idle);
            const std::function<bool()> fence = owner->bindCaptureSource(source);
            bindingOk &= check(fence && fence() && captureNetwork.sent.empty(), "binding missing fence or started capture");
            CaptureConfig config; config.centerFreqHz = 7150000;
            bindingOk &= check(source.start(config), "bound capture refused start");
            source.setGateOpen(true);
            bindingOk &= check(captureNetwork.sent.size() == 1
                && captureNetwork.sent[0].request.rawHeader("Authorization") == "Bearer " + token.toLatin1(),
                "capture did not use session credential");
            if (change == 0) { owner->clearSession(); }
            if (change == 1) { owner->setSession(address, QString(43, u'b'), [&] { return current; }, identity); controlNetwork.last->complete(200, idle); }
            if (change == 2) { owner->setSession(address, token, [&] { return current; }, identity); controlNetwork.last->complete(200, idle); }
            if (change == 3) { current = false; }
            if (change == 4) { owner.reset(); }
            if (change == 5) { owner->refresh(); controlNetwork.last->complete(401, "{}"); }
            if (change == 6) {
                owner->request(Operation::ArmCalibration);
                controlNetwork.last->complete(200, R"({"state":"calibration_arm_accepted"})");
                controlNetwork.last->complete(200, idle);
            }
            bindingOk &= check(!fence(), "retired measurement session remained current");
            captureNetwork.last->complete(200, R"({"capture_id":"0123456789abcdef0123456789abcdef","format":"f32le-iq-v1","sample_rate_hz":192000,"center_hz":7150000,"sample_count":16384,"chunk_samples":512,"keyed":true,"correcting":true,"first_receive_ns":"1000000000","last_receive_ns":"1085000000"})");
            bindingOk &= check(frames == 0 && errors == 1 && captureNetwork.sent.size() == 1,
                               "old capture advanced after session retirement");
            if (change == 3) {
                current = true;
                bindingOk &= check(!fence(), "retired shared fence revived when owner predicate returned true");
            }
            if (change == 1 || change == 2) {
                bindingOk &= check(!fence(), "fresh same-address status revived old measurement binding");
                const std::function<bool()> replacement = owner->bindCaptureSource(source);
                bindingOk &= check(replacement && replacement() && source.start(config), "explicit new binding refused");
                source.setGateOpen(true);
                const QString expected = change == 1 ? QString(43, u'b') : token;
                bindingOk &= check(captureNetwork.sent.back().request.rawHeader("Authorization") == "Bearer " + expected.toLatin1(),
                                   "explicit replacement retained old credential");
            }
        }
        if (!bindingOk) { return 1; }
    }
    {
        using AetherSDR::txlin::AetherPDTestClient;
        bool bindingOk = true;
        const QByteArray observing = R"({"test_id":"0123456789abcdef0123456789abcdef","state":"observing","cleanup_due_ms":"20000","remaining_ms":15000})";
        for (const int change : {0, 1, 2, 3, 4, 5, 6}) {
            Network controlNetwork, testNetwork;
            std::unique_ptr<AetherPDModel> owner = std::make_unique<AetherPDModel>(controlNetwork);
            AetherPDTestClient client(testNetwork, [] { return 1000; });
            bool current = true;
            const StartIdentity identity{QStringLiteral("0D3EA0E8-B052-4AC6-9D32-D96918634469"), 7};
            client.setSession(address, token, [] { return true; });
            bindingOk &= check(!owner->bindTestClient(client) && !client.begin(15000)
                && testNetwork.sent.empty(), "invalid model binding retained helper credentials");
            AetherPDTestClient run(testNetwork, [] { return 1000; });
            owner->setSession(address, token, [&] { return current; }, identity);
            bindingOk &= check(!owner->bindTestClient(run), "helper bound before observed availability");
            controlNetwork.last->complete(200, idle);
            const std::function<bool()> fence = owner->bindTestClient(run);
            bindingOk &= check(fence && fence() && testNetwork.sent.empty(), "binding started helper or lost fence");
            bindingOk &= check(run.begin(15000), "bound helper refused explicit begin");
            bindingOk &= check(testNetwork.sent.back().request.rawHeader("Authorization") == "Bearer " + token.toLatin1(),
                "helper did not use model credential");
            testNetwork.last->complete(200, observing);
            testNetwork.last->complete(200, observing);
            bindingOk &= check(run.remainingMs() == 15000 && !owner->bindTestClient(run)
                && run.remainingMs() == 15000 && testNetwork.sent.size() == 2,
                "rebinding a used helper disturbed its existing run");
            if (change == 0) { owner->clearSession(); }
            if (change == 1) { owner->setSession(address, QString(43, u'b'), [&] { return current; }, identity); controlNetwork.last->complete(200, idle); }
            if (change == 2) { owner->setSession(address, token, [&] { return current; }, identity); controlNetwork.last->complete(200, idle); }
            if (change == 3) { current = false; }
            if (change == 4) { owner.reset(); }
            if (change == 5) { owner->refresh(); controlNetwork.last->complete(401, "{}"); }
            if (change == 6) { owner->request(Operation::ArmCalibration); }
            bindingOk &= check(!fence() && run.remainingMs() == 0, "retired helper kept measurement readiness");
            if (change == 3) {
                current = true;
                bindingOk &= check(!fence() && run.remainingMs() == 0, "retired helper fence revived");
            }
            bindingOk &= check(run.requestStop()
                && testNetwork.sent.back().request.rawHeader("Authorization") == "Bearer " + token.toLatin1()
                && testNetwork.sent.back().request.url().path().endsWith("/0123456789abcdef0123456789abcdef/stop"),
                "retired helper lost original cleanup identity");
        }
        if (!bindingOk) { return 1; }
    }
    {
        Network measurementNetwork;
        AetherPDModel measurement(measurementNetwork);
        measurement.setSession(address, token);
        measurementNetwork.last->complete(200, idle);
        const size_t requests = measurementNetwork.sent.size();
        if (!check(!measurement.request(Operation::ArmMeasurement)
            && measurementNetwork.sent.size() == requests, "legacy model admitted measurement source")) { return 1; }
        QJsonObject supported = QJsonDocument::fromJson(idle).object();
        supported[QStringLiteral("measurement_control")] = true;
        supported[QStringLiteral("source_purpose")] = QStringLiteral("calibration");
        supported[QStringLiteral("correction_enabled")] = false;
        supported[QStringLiteral("state")] = QStringLiteral("disabled");
        measurement.refresh();
        measurementNetwork.last->complete(200, QJsonDocument(supported).toJson());
        if (!check(measurement.request(Operation::ArmMeasurement)
            && measurementNetwork.sent.back().request.url().path() == QStringLiteral("/v1/aetherpd/measurement/arm"),
            "uncorrected measurement source was not submitted")) { return 1; }
        Reply* pending = measurementNetwork.last;
        if (!check(measurement.request(Operation::CancelCalibration) && pending->aborted,
            "cancel failed to preempt pending measurement source")) { return 1; }
        pending->complete(200, R"({"state":"measurement_arm_accepted"})");
        if (!check(!measurement.lastAcknowledgment() && measurement.pendingOperation() == Operation::CancelCalibration,
            "late measurement receipt overrode cancellation")) { return 1; }
    }
    {
        Network controls, captures;
        AetherPDModel owner(controls);
        owner.setSession(address, token, [] { return true; },
            StartIdentity{QStringLiteral("0D3EA0E8-B052-4AC6-9D32-D96918634469"), 0x321b4de});
        controls.last->complete(200, idle);
        const size_t legacyRequests = controls.sent.size();
        if (!check(!owner.request(Operation::BypassCorrection) && !owner.request(Operation::EnableCorrection)
            && controls.sent.size() == legacyRequests, "legacy service accepted correction selection")) { return 1; }
        QJsonObject supported = QJsonDocument::fromJson(idle).object();
        supported["correction_control"] = true;
        owner.refresh(); controls.last->complete(200, QJsonDocument(supported).toJson());
        AetherSDR::txlin::AetherPDCaptureSource source(captures);
        const std::function<bool()> fence = owner.bindCaptureSource(source);
        if (!check(fence && fence(), "supported service could not bind capture")) { return 1; }
        if (!check(owner.request(Operation::BypassCorrection) && !owner.status() && !fence()
            && controls.sent.back().request.url().path() == QStringLiteral("/v1/aetherpd/correction/bypass"),
            "explicit bypass failed to invalidate old capture preparation")) { return 1; }
        controls.last->complete(200, R"({"state":"correction_bypass_accepted"})");
        if (!check(!owner.status() && owner.pendingOperation() == Operation::Status,
            "correction receipt was treated as observed state")) { return 1; }
        supported["correction_enabled"] = false; supported["state"] = "disabled";
        controls.last->complete(200, QJsonDocument(supported).toJson());
        if (!check(owner.status() && !owner.status()->correctionEnabled && !fence(),
            "fresh bypass status revived old preparation or lost state")) { return 1; }
        if (!check(owner.request(Operation::EnableCorrection), "explicit enable refused")) { return 1; }
        Reply* enable = controls.last;
        if (!check(owner.request(Operation::Stop) && enable->aborted,
            "Stop could not preempt correction selection")) { return 1; }
        enable->complete(200, R"({"state":"correction_enable_accepted"})");
        if (!check(owner.pendingOperation() == Operation::Stop && !owner.lastAcknowledgment(),
            "late correction receipt replaced Stop")) { return 1; }
        controls.last->complete(202, R"({"state":"stop_requested"})");
        controls.last->complete(200, R"({"available":false,"state":"stopped"})");
        for (bool keyed : {false, true}) {
            QJsonObject busy = QJsonDocument::fromJson(idle).object(); busy["correction_control"] = true;
            if (keyed) { busy["keyed"] = true; busy["state"] = "calibrating"; }
            else { busy["calibration_state"] = "armed"; busy["calibration_samples_left"] = 192000; busy["state"] = "calibration_armed"; }
            owner.refresh(); controls.last->complete(200, QJsonDocument(busy).toJson());
            const size_t before = controls.sent.size();
            if (!check(owner.status() && !owner.request(Operation::BypassCorrection)
                && controls.sent.size() == before, "armed/keyed selection reached HTTP")) { return 1; }
        }
    }
    Network network;
    AetherPDModel model(network);
    bool ok = true;
    ok &= check(!model.refresh() && !model.request(Operation::ArmCalibration)
        && network.sent.empty(), "no connection means no network action");
    model.setSession(address, token);
    ok &= check(model.active() && !model.status() && network.sent.size() == 1
        && network.sent.back().method == QNetworkAccessManager::GetOperation,
        "attach requests status only; unknown until reply");
    network.last->complete(200, idle);
    ok &= check(model.status() && model.status()->state == State::Idle, "observed idle published");
    ok &= check(model.request(Operation::ArmCalibration) && !model.status(), "explicit arm invalidates old state");
    Reply* oldArm = network.last;
    const size_t beforeBusy = network.sent.size();
    ok &= check(!model.request(Operation::ArmCalibration) && network.sent.size() == beforeBusy
        && model.pendingOperation() == Operation::ArmCalibration, "duplicate cannot lose pending arm");
    oldArm->complete(200, R"({"state":"calibration_arm_accepted"})");
    ok &= check(model.lastAcknowledgment() == Acknowledgment::ArmAccepted && !model.status()
        && model.pendingOperation() == Operation::Status,
        "arm ACK is a receipt, followed only by status reconciliation");
    network.last->complete(200, armed);
    ok &= check(model.status() && model.status()->state == State::CalibrationArmed,
        "armed state needs observed status");
    const size_t alreadyArmed = network.sent.size();
    ok &= check(!model.request(Operation::ArmCalibration) && network.sent.size() == alreadyArmed,
        "fresh armed state cannot admit duplicate arm");

    model.refresh();
    Reply* oldStatus = network.last;
    model.setSession(address, token);
    oldStatus->complete(200, armed);
    ok &= check(oldStatus->aborted && !model.status() && !model.lastAcknowledgment()
        && model.pendingOperation() == Operation::Status,
        "same-address reconnect fences old observations and receipts");
    network.last->complete(200, idle);
    model.request(Operation::ArmCalibration);
    oldArm = network.last;
    ok &= check(model.request(Operation::CancelCalibration) && oldArm->aborted,
        "cancel supersedes in-flight arm");
    oldArm->complete(200, R"({"state":"calibration_arm_accepted"})");
    ok &= check(!model.status() && !model.lastAcknowledgment()
        && model.pendingOperation() == Operation::CancelCalibration, "late ACK cannot undo cancel");
    network.last->complete(200, R"({"state":"calibration_cancel_accepted"})");
    network.last->complete(200, idle);
    model.request(Operation::Stop);
    network.last->complete(202, R"({"state":"stop_requested"})");
    ok &= check(!model.status() && model.lastAcknowledgment() == Acknowledgment::StopRequested,
        "stop receipt is not a stopped or radio-idle claim");
    network.last->complete(503, R"({"available":false,"state":"unavailable"})");
    ok &= check(!model.status() && model.lastFailure() == AetherPDClient::Failure::Refused,
        "unavailable cannot establish completed cleanup");

    model.refresh();
    network.last->complete(200, idle);
    model.request(Operation::ArmCalibration);
    const size_t beforeFailure = network.sent.size();
    network.last->complete(0, {}, QNetworkReply::RemoteHostClosedError);
    ok &= check(!model.status() && !model.pendingOperation()
        && network.sent.size() == beforeFailure, "lost arm reply does not replay arm or claim cancellation");
    model.refresh();
    network.last->complete(401, "{}");
    const size_t beforeRevoked = network.sent.size();
    ok &= check(!model.active() && !model.refresh() && !model.request(Operation::Stop)
        && network.sent.size() == beforeRevoked, "authorization loss retires credential and monitoring");

    model.setSession(address, token, [] { return true; },
        StartIdentity{QStringLiteral("0D3EA0E8-B052-4AC6-9D32-D96918634469"), 7});
    network.last->complete(200, idle);
    Network staleCaptureNetwork;
    AetherSDR::txlin::AetherPDCaptureSource staleCapture(staleCaptureNetwork);
    const std::function<bool()> staleFence = model.bindCaptureSource(staleCapture);
    Network staleTestNetwork;
    AetherSDR::txlin::AetherPDTestClient staleTest(staleTestNetwork, [] { return 1000; });
    const std::function<bool()> staleTestFence = model.bindTestClient(staleTest);
    ok &= check(staleTestFence && staleTestFence(), "fresh status could not bind helper");
    ok &= check(staleFence && staleFence(), "fresh status could not bind capture before expiry");
    // Do not dispatch timers: a stalled event loop cannot extend freshness.
    QThread::msleep(3'050);
    const size_t beforeStale = network.sent.size();
    ok &= check(!model.status() && !model.request(Operation::ArmCalibration)
        && network.sent.size() == beforeStale, "monotonic expiration gates arm even before timer delivery");
    model.refresh(); network.last->complete(200, idle);
    ok &= check(!staleFence(), "new status after an unobserved freshness gap revived old capture");
    ok &= check(!staleTestFence() && !staleTest.begin(15000) && staleTestNetwork.sent.empty(),
                "new status after freshness gap revived helper binding");
    QCoreApplication::processEvents();
    for (size_t i = beforeStale; i < network.sent.size(); ++i) {
        ok &= check(network.sent[i].method == QNetworkAccessManager::GetOperation,
            "automatic work only reads status");
    }
    model.clearSession();
    const size_t beforeDisconnected = network.sent.size();
    QCoreApplication::processEvents();
    ok &= check(!model.active() && !model.status() && !model.pendingOperation()
        && network.sent.size() == beforeDisconnected, "disconnect neither retries control nor restarts monitoring");

    model.setSession(address, token);
    network.last->complete(200, idle);
    model.request(Operation::ArmCalibration);
    const QMetaObject::Connection resetOnAck = QObject::connect(&model, &AetherPDModel::changed, &app, [&] {
        if (model.lastAcknowledgment()) {
            model.clearSession();
        }
    });
    const size_t beforeReentry = network.sent.size();
    network.last->complete(200, R"({"state":"calibration_arm_accepted"})");
    ok &= check(!model.active() && network.sent.size() == beforeReentry,
        "synchronous session retirement prevents ACK follow-up request");
    QObject::disconnect(resetOnAck);
    model.setSession(QHostAddress(QStringLiteral("8.8.8.8")), token);
    ok &= check(!model.active() && network.sent.size() == beforeReentry,
        "invalid endpoint retires monitoring without sending credentials");
    bool current = true;
    model.setSession(address, token, [&] { return current; });
    network.last->complete(200, idle);
    model.request(Operation::ArmCalibration);
    current = false;
    const size_t beforeOwnerLoss = network.sent.size();
    bool publishedRetiredAck = false;
    const QMetaObject::Connection retiredAck = QObject::connect(&model, &AetherPDModel::changed, &app, [&] {
        publishedRetiredAck |= model.lastAcknowledgment().has_value();
    });
    network.last->complete(200, R"({"state":"calibration_arm_accepted"})");
    QObject::disconnect(retiredAck);
    ok &= check(!model.active() && !model.lastAcknowledgment() && !model.pendingOperation()
        && !publishedRetiredAck && network.sent.size() == beforeOwnerLoss,
        "owner fence rejects late ACK before publishing or following up");
    model.setSession(address, token, [&] { return current; });
    ok &= check(!model.active() && network.sent.size() == beforeOwnerLoss,
        "retired owner cannot attach even with valid address and credential");
    {
        std::unique_ptr<AetherPDModel> retired = std::make_unique<AetherPDModel>(network);
        QObject::connect(retired.get(), &AetherPDModel::changed, &app, [&] { retired.reset(); });
        const size_t beforeRetired = network.sent.size();
        retired->setSession(address, token);
        ok &= check(!retired && network.sent.size() == beforeRetired,
            "owner deletion during attach notification cannot start a request");
    }
    {
        std::unique_ptr<AetherPDModel> retired = std::make_unique<AetherPDModel>(network);
        retired->setSession(address, token);
        QObject::connect(retired.get(), &AetherPDModel::changed, &app, [&] { retired.reset(); });
        network.last->complete(200, idle);
        ok &= check(!retired, "owner may delete model while publishing status");
    }
    {
        Network startNetwork;
        AetherPDModel starter(startNetwork);
        const StartIdentity first{QStringLiteral("0D3EA0E8-B052-4AC6-9D32-D96918634469"), 7};
        const StartIdentity second{first.guiClientId, 8};
        starter.setSession(address, token);
        const size_t missingIdentity = startNetwork.sent.size();
        ok &= check(!starter.request(Operation::Start) && startNetwork.sent.size() == missingIdentity,
                    "monitor-only session cannot manufacture a start identity");
        bool ownerCurrent = true;
        starter.setSession(address, token, [&] { return ownerCurrent; }, first);
        ok &= check(starter.pendingOperation() == Operation::Status,
                    "attaching an identity still requests only status");
        ok &= check(starter.request(Operation::Start) && !starter.status(), "explicit start admitted");
        Reply* oldStart = startNetwork.last;
        starter.setSession(address, token, [&] { return ownerCurrent; }, second);
        oldStart->complete(202, R"({"state":"start_requested"})");
        ok &= check(oldStart->aborted && !starter.lastAcknowledgment()
            && starter.pendingOperation() == Operation::Status,
            "reboot identity replacement rejects the old start receipt");
        ok &= check(starter.request(Operation::Start) && startNetwork.sent.back().body == *startBody(second),
                    "new startup carries the new session handle at the same address");
        startNetwork.last->complete(202, R"({"state":"start_requested"})");
        ok &= check(starter.lastAcknowledgment() == Acknowledgment::StartRequested && !starter.status()
            && starter.pendingOperation() == Operation::Status,
            "start ACK means requested only and follows with observation");
        startNetwork.last->complete(200, idle);
        const size_t registered = startNetwork.sent.size();
        ok &= check(!starter.request(Operation::Start) && startNetwork.sent.size() == registered,
                    "observed active service rejects duplicate startup locally");
        ownerCurrent = false;
        ok &= check(!starter.request(Operation::Start) && !starter.active()
            && startNetwork.sent.size() == registered, "retired owner cannot start on a replacement radio");
    }
    std::cout << (ok ? "PASS" : "FAIL") << ": PD model session fencing, observed state, cancellation, freshness and no automatic control\n";
    return ok ? 0 : 1;
}
