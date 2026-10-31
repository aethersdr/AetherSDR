// #6506: production receive routing and firmware status, injected without sockets.
#include "TestSettingsProfile.h"
#include "core/AgcTCalibrator.h"
#include "core/backends/flex/FlexBackend.h"
#include "models/RadioModel.h"
#include "models/SliceModel.h"

#include <QCoreApplication>
#include <QMap>
#include <QSignalSpy>
#include <QtTest/QTest>
#include <cstdio>
#include <memory>
#include <thread>

using namespace AetherSDR;
namespace {
int failures = 0;
void check(bool ok, const char* claim)
{
    std::printf("[%s] %s\n", ok ? "PASS" : "FAIL", claim);
    failures += !ok;
}

class Backend final : public IRadioBackend {
public:
    bool connected{true};
    bool supported{true};
    QList<AgcThresholdMode> requests;
    QList<SliceAgcRequest> agcs;
    RadioCapabilities capabilities() const override
    {
        RadioCapabilities caps;
        if (supported) {
            caps.autoAgcThresholdControl = RadioCapabilities::AutoAgcThresholdControl{
                QStringLiteral("auto_agc_t")};
        }
        return caps;
    }
    void connectRadio(const RadioConnectRequest&) override { connected = true; }
    void disconnectRadio() override { connected = false; }
    bool isConnected() const override { return connected; }
    ReceiveDispatch requestSliceAgcThresholdMode(int, AgcThresholdMode mode) override
    { requests.append(mode); return ReceiveDispatch::Dispatched; }
    void requestSliceAgc(int, const SliceAgcRequest& request) override { agcs.append(request); }
    void setSliceFrequency(int, double) override {}
    void setSliceMode(int, const QString&) override {}
    void setSliceFilter(int, int, int) override {}
    void setSliceAgc(int, const QString&, int) override {}
    void setPanCenter(const QString&, double, PanCenterIntent) override {}
    void setKeying(bool, const TxCoordinator::Operation&, const TxCoordinator::Completion&) override {}
    void invokeExtension(const QString&, const QString&, quint64, const QVariant&) override {}
};

void license(RadioModel& radio, const QString& enabled)
{
    using Kvs = QMap<QString, QString>;
    const Kvs kvs{{QStringLiteral("name"), QStringLiteral("auto_agc_t")},
                  {QStringLiteral("enabled"), enabled},
                  {QStringLiteral("reason"), QStringLiteral("PLUS")}};
    QMetaObject::invokeMethod(&radio, "onStatusReceived", Qt::DirectConnection,
                              Q_ARG(QString, QStringLiteral("license feature")), Q_ARG(Kvs, kvs));
}

SliceDelta report(std::optional<AgcThresholdMode> mode = AgcThresholdMode::Manual)
{
    SliceDelta delta;
    delta.panId = QStringLiteral("receiver");
    delta.frequency = 14.2;
    delta.mode = QStringLiteral("USB");
    delta.agcMode = QStringLiteral("med");
    delta.agcThreshold = 65;
    delta.agcThresholdMode = mode;
    delta.inUse = true;
    return delta;
}

struct Fixture {
    RadioModel radio;
    Backend* backend;
    Fixture()
    {
        auto owned = std::make_unique<Backend>();
        backend = owned.get();
        radio.setBackendForTest(std::move(owned), QStringLiteral("auto-agct-test"));
        emit backend->sliceChanged(0, report());
    }
    SliceModel* slice() { return radio.slice(0); }
};

void wireAndParser()
{
    FlexBackend flex;
    QString firmware;
    bool licensed = false;
    QStringList commands;
    flex.setFirmwareVersionProvider([&] { return firmware; });
    flex.setAutoAgcThresholdLicenseProvider([&] { return licensed; });
    flex.setSliceCommandSink([&](const QString& command) { commands.append(command); });
    for (const QString& version : {QString(), QStringLiteral("4.2.20"), QStringLiteral("4.3.15"),
                                  QStringLiteral("4.3.16beta"), QStringLiteral("4.3.16.42034junk")}) {
        firmware = version;
        check(!flex.capabilities().autoAgcThresholdControl,
              "absent, old and malformed firmware do not advertise Auto AGC-T");
        check(flex.requestSliceAgcThresholdMode(0, AgcThresholdMode::Automatic)
                  == ReceiveDispatch::Unsupported, "unsupported firmware sends no Auto mode command");
    }
    firmware = QStringLiteral("4.3.16.42034");
    check(flex.capabilities().autoAgcThresholdControl.has_value(), "4.3.16 firmware declares the implemented verb");
    check(flex.requestSliceAgcThresholdMode(0, AgcThresholdMode::Automatic)
              == ReceiveDispatch::Unsupported && commands.isEmpty(), "unconfirmed license fails closed at backend");
    licensed = true;
    check(flex.requestSliceAgcThresholdMode(-1, AgcThresholdMode::Automatic) == ReceiveDispatch::Unsupported
              && flex.requestSliceAgcThresholdMode(0, static_cast<AgcThresholdMode>(99)) == ReceiveDispatch::Unsupported,
          "negative slice and invalid enum send nothing");
    flex.requestSliceAgcThresholdMode(2, AgcThresholdMode::Automatic);
    flex.requestSliceAgcThresholdMode(2, AgcThresholdMode::Manual);
    check(commands == QStringList{QStringLiteral("slice set 2 agc_threshold_mode=auto"),
                                   QStringLiteral("slice set 2 agc_threshold_mode=manual")},
          "exact FlexLib mode commands carry no threshold, speed or off-level field");
    SliceModel slice(2);
    QSignalSpy requests(&slice, &SliceModel::receiveAgcThresholdModeRequested);
    QObject::connect(&flex, &IRadioBackend::sliceChanged, &slice,
                     [&](int, const SliceDelta& delta) { slice.applyChanges(delta); });
    for (const QString& invalid : {QStringLiteral("AUTO"), QStringLiteral("Manual"), QStringLiteral("1"),
                                  QStringLiteral("auto "), QString()}) {
        flex.decodeSliceStatus(2, {{QStringLiteral("agc_threshold_mode"), invalid}});
        check(!slice.agcThresholdMode(), "invalid mode status leaves observation unknown");
    }
    flex.decodeSliceStatus(2, {{QStringLiteral("agc_threshold_mode"), QStringLiteral("auto")}});
    check(slice.agcThresholdMode() == AgcThresholdMode::Automatic, "radio auto status is authoritative");
    flex.decodeSliceStatus(2, {{QStringLiteral("agc_threshold_mode"), QStringLiteral("manual")}});
    check(slice.agcThresholdMode() == AgcThresholdMode::Manual && requests.isEmpty() && commands.size() == 2,
          "radio manual status changes observation without command feedback");
}

void gatesAndAuthority()
{
    Fixture f;
    SliceModel* s = f.slice();
    check(s && !f.radio.autoAgcThresholdUnavailableReason(s).isEmpty(), "unknown license blocks mode request");
    s->requestAgcThresholdMode(AgcThresholdMode::Automatic);
    license(f.radio, QStringLiteral("0"));
    s->requestAgcThresholdMode(AgcThresholdMode::Automatic);
    check(f.backend->requests.isEmpty(), "unknown and refused license send nothing");
    license(f.radio, QStringLiteral("1"));
    check(f.radio.autoAgcThresholdUnavailableReason(s).isEmpty(), "confirmed feature license permits known current slice");
    s->requestAgcThresholdMode(AgcThresholdMode::Automatic);
    check(f.backend->requests.size() == 1 && s->agcThresholdMode() == AgcThresholdMode::Manual
              && s->automaticAgcThresholdActiveOrPending(), "Auto request suppresses writers before echo without optimistic observed mode");
    emit f.backend->sliceChanged(0, report(AgcThresholdMode::Manual));
    check(s->automaticAgcThresholdActiveOrPending(), "delayed pre-command manual status cannot release pending Auto");
    s->requestAgcThresholdMode(AgcThresholdMode::Manual);
    check(!s->automaticAgcThresholdActiveOrPending(), "explicit Manual request cancels pending Auto");
    emit f.backend->sliceChanged(0, report(AgcThresholdMode::Automatic));
    const qsizetype before = f.backend->agcs.size();
    s->recallAgcThreshold(30);
    s->setAgcThresholdFromCalibration(31);
    check(f.backend->agcs.size() == before, "host recall and calibration cannot overwrite firmware Auto");
    s->setAgcThreshold(s->agcThreshold());
    check(f.backend->agcs.size() == before + 1
              && f.backend->agcs.last().field == SliceAgcRequest::Field::Threshold,
          "same-value operator threshold gesture sends the existing field-specific manual override");
    const qsizetype modeBefore = f.backend->requests.size();
    f.backend->connected = false;
    s->requestAgcThresholdMode(AgcThresholdMode::Automatic);
    f.backend->connected = true;
    std::thread worker([&] { s->requestAgcThresholdMode(AgcThresholdMode::Automatic); });
    worker.join();
    check(f.backend->requests.size() == modeBefore, "disconnected and off-thread mode intents are dropped");
    s->setExternalReceiveAudioReplacementMute(true, false);
    s->requestAgcThresholdMode(AgcThresholdMode::Automatic);
    check(f.backend->requests.size() == modeBefore, "external receive replacement does not command radio Auto");
}

void reentrantPendingNotifications()
{
    {
        Fixture f;
        license(f.radio, QStringLiteral("1"));
        SliceModel* s = f.slice();
        QObject::connect(s, &SliceModel::agcThresholdModeChanged, &f.radio, [&] {
            if (s->automaticAgcThresholdActiveOrPending()) {
                s->setAgcThreshold(44);
            }
        });
        s->requestAgcThresholdMode(AgcThresholdMode::Automatic);
        check(f.backend->requests.isEmpty() && f.backend->agcs.size() == 1,
              "reentrant operator threshold supersedes Auto before its command is dispatched");
    }
    {
        Fixture f;
        license(f.radio, QStringLiteral("1"));
        SliceModel* s = f.slice();
        QObject::connect(s, &SliceModel::agcThresholdModeChanged, &f.radio, [&] {
            if (s->automaticAgcThresholdActiveOrPending()) {
                f.radio.stageSessionModelsForReconnectForTest();
            }
        });
        s->requestAgcThresholdMode(AgcThresholdMode::Automatic);
        check(f.backend->requests.isEmpty() && !s->agcThresholdMode(),
              "session invalidation from pending notification drops the prior-session Auto command");
    }
}

void staleManualCurve()
{
    Fixture f;
    license(f.radio, QStringLiteral("1"));
    SliceModel* s = f.slice();
    AgcTCalibrator calibrator;
    calibrator.setSlice(s);
    calibrator.setSettleMs(1);
    QObject::connect(s, &SliceModel::agcThresholdChanged, &calibrator, &AgcTCalibrator::onValueChanged);
    for (const int value : {20, 50, 80}) {
        calibrator.onAudioLevel(static_cast<float>(value) / 100.0f);
        calibrator.onValueChanged(value);
        QTest::qWait(140);
    }
    check(calibrator.hasRecommendation() && calibrator.curve().size() == 3,
          "manual calibration establishes a real curve and recommendation");
    QSignalSpy finished(&calibrator, &AgcTCalibrator::finished);
    s->requestAgcThresholdMode(AgcThresholdMode::Automatic);
    check(!calibrator.hasRecommendation() && calibrator.curve().isEmpty(),
          "Auto takeover discards the complete old calibration curve");
    check(finished.size() == 1 && !finished.constFirst().constFirst().toBool(),
          "discarding an idle manual curve notifies its presentation");
    SliceDelta automatic = report(AgcThresholdMode::Automatic);
    automatic.agcThreshold = 35;
    emit f.backend->sliceChanged(0, automatic);
    QTest::qWait(140);
    emit f.backend->sliceChanged(0, report(AgcThresholdMode::Manual));
    const qsizetype writes = f.backend->agcs.size();
    calibrator.applyRecommendation();
    check(!calibrator.hasRecommendation() && f.backend->agcs.size() == writes,
          "automatic threshold reports cannot resurrect an old recommendation for later Manual apply");
}

void calibrationAuthorityTransitions()
{
    Fixture f;
    license(f.radio, QStringLiteral("1"));
    SliceModel* s = f.slice();
    AgcTCalibrator calibrator;
    calibrator.setSlice(s);
    calibrator.setSettleMs(10);
    calibrator.startAutoSweep();
    check(calibrator.isRunning(), "external RX regression starts a sweep");
    const qsizetype writes = f.backend->agcs.size();
    s->setExternalReceiveAudioReplacementMute(true, false);
    check(!calibrator.isRunning(), "external RX takeover cancels immediately before the next timer tick");
    s->setExternalReceiveAudioReplacementMute(false, false);
    QTest::qWait(30);
    calibrator.stop();
    check(f.backend->agcs.size() == writes,
          "brief external RX takeover cannot resume or restore an old sweep");

    f.radio.stageSessionModelsForReconnectForTest();
    emit f.backend->sliceChanged(0, report());
    check(f.slice() == s, "reconnect reclaims the existing slice object");
    calibrator.setSlice(s);
    calibrator.onAudioLevel(0.2f);
    calibrator.onValueChanged(40);
    QTest::qWait(140);
    check(calibrator.curve().size() == 1,
          "explicit rebind of a reclaimed slice permits fresh manual recording");
}

void calibrationAndSession()
{
    Fixture f;
    license(f.radio, QStringLiteral("1"));
    SliceModel* s = f.slice();
    AgcTCalibrator calibrator;
    calibrator.setSlice(s);
    calibrator.setSettleMs(10);
    calibrator.startAutoSweep();
    check(calibrator.isRunning(), "manual current slice starts host calibration");
    s->requestAgcThresholdMode(AgcThresholdMode::Automatic);
    const qsizetype writes = f.backend->agcs.size();
    check(!calibrator.isRunning(), "pending Auto immediately cancels host sweep");
    emit f.backend->sliceChanged(0, report(AgcThresholdMode::Manual));
    calibrator.stop();
    calibrator.applyRecommendation();
    calibrator.startAutoSweep();
    QTest::qWait(30);
    check(!calibrator.isRunning() && f.backend->agcs.size() == writes,
          "delayed manual echo, stop, apply and timer cannot restore or resume over pending Auto");
    s->requestAgcThresholdMode(AgcThresholdMode::Manual);
    calibrator.startAutoSweep();
    check(calibrator.isRunning(), "explicit Manual releases pending suppression");
    f.radio.stageSessionModelsForReconnectForTest();
    check(!s->agcThresholdMode() && !calibrator.isRunning(), "session staging invalidates mode and cancels calibration");
    emit f.backend->sliceChanged(0, report());
    const qsizetype afterReclaim = f.backend->agcs.size();
    calibrator.stop();
    calibrator.applyRecommendation();
    check(f.backend->agcs.size() == afterReclaim, "old calibration cannot restore onto a reclaimed session");
    QPointer<SliceModel> retired(s);
    emit f.backend->sliceRemoved(0);
    emit f.backend->sliceChanged(0, report());
    check(f.slice() != retired.data(), "replacement can reuse the same numeric slice ID");
    const qsizetype beforeRetired = f.backend->requests.size();
    retired->requestAgcThresholdMode(AgcThresholdMode::Automatic);
    calibrator.startAutoSweep();
    check(!calibrator.isRunning() && f.backend->requests.size() == beforeRetired,
          "retired object cannot request Auto or restart host calibration against replacement");
    auto replacement = std::make_unique<Backend>();
    f.radio.setBackendForTest(std::move(replacement), QStringLiteral("another-session"));
    check(!f.radio.licenseFeatureSeen(QStringLiteral("auto_agc_t")), "backend replacement clears prior entitlement");
}
} // namespace

int main(int argc, char** argv)
{
    TestSettingsProfile profile(QStringLiteral("auto-agc-threshold"));
    QCoreApplication app(argc, argv);
    check(profile.isValid(), "settings profile is isolated");
    wireAndParser();
    gatesAndAuthority();
    reentrantPendingNotifications();
    staleManualCurve();
    calibrationAuthorityTransitions();
    calibrationAndSession();
    return failures == 0 ? 0 : 1;
}
