// Socket-free production model and widget test: credentials and HTTP are injected.
#include "AetherPDTestRadio.h"
#include "TestSettingsProfile.h"
#include "core/AppSettings.h"
#include "gui/AetherPDDialog.h"

#include <QApplication>
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
    Network network;
    RadioModel radio;
    Access::install(radio, network);
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
