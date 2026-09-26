#include "TestSettingsProfile.h"
#include "RtlInjectedDevice.h"
#include "core/RtlDeviceSettings.h"
#include "core/backends/rtl/RtlSdrBackend.h"

#include <QCoreApplication>
#include <QElapsedTimer>
#include <cstdio>
#include <cstring>

using namespace AetherSDR;
using T = rtl::RtlCaptureTransaction;
namespace AetherSDR::rtl {
struct RtlCaptureBackendTestAccess {
    static void start(RtlSdrBackend& backend, const std::shared_ptr<test::DeviceState>& device)
    {
        backend.m_serial = backend.m_settingsScope.radioId();
        backend.verifyDeviceSettingsIdentity(backend.m_serial);
        backend.m_requested = {};
        backend.m_requested.hardware = {100000000, 2400000, 0, 0, backend.m_ppmCorrection, 240};
        backend.m_requested.dcSuppression = backend.m_dcSuppression;
        backend.m_requested.receivers = {{{0, 100200000, -8000, 8000, 0, 3000, 3000}, T::Mode::Fm}};
        backend.startCapture(std::make_unique<RtlSdrWorker>(std::make_unique<test::InjectedDevice>(device)));
    }
    static bool busy(const RtlSdrBackend& backend) { return backend.m_capture.busy(); }
    static QVariantMap status(const RtlSdrBackend& backend) { return backend.deviceSettingsStatus(); }
    static void mismatch(RtlSdrBackend& backend) { backend.verifyDeviceSettingsIdentity(QStringLiteral("different-usb-device")); }
};
}
namespace {
int failures = 0;
void check(bool value, const char* message)
{
    if (!value) { ++failures; std::fprintf(stderr, "FAIL: %s\n", message); }
}
template<typename Predicate>
bool waitFor(const std::shared_ptr<test::DeviceState>& device, Predicate ready)
{
    QElapsedTimer timer; timer.start();
    while (timer.elapsed() < 10000) {
        QCoreApplication::processEvents();
        if (ready()) { return true; }
        device->block();
        QThread::msleep(1);
    }
    return false;
}
}
int main(int argc, char** argv)
{
    TestSettingsProfile profile(QStringLiteral("rtl-device-controls"));
    if (!profile.isValid()) { return 1; }
    QCoreApplication app(argc, argv);
    AppSettings::instance().load();
    using Access = rtl::RtlCaptureBackendTestAccess;
    const RadioSettingsScope scope("rtl", "device-controls-test");
    RtlDeviceSettings settings(scope);
    QString reason;
    check(settings.saveAccepted({17, true}, reason), "seed previous accepted device settings");
    rtl::RtlSdrBackend backend;
    backend.configureSettingsScope(scope, {scope.radioId(), false});
    backend.applyRestoredState({});
    auto device = std::make_shared<test::DeviceState>();
    device->releaseReadback();
    float displayPeak = 0;
    int displayFrames = 0;
    QObject::connect(&backend, &IRadioBackend::spectrumFrameReady, &app,
        [&](int, const QByteArray& frame) {
            float peak = -200;
            for (qsizetype offset = 0; offset + qsizetype(sizeof(float)) <= frame.size(); offset += sizeof(float)) {
                float value = 0; std::memcpy(&value, frame.constData() + offset, sizeof(value));
                peak = std::max(peak, value);
            }
            displayPeak = peak; ++displayFrames;
        });
    QHash<quint64, QVariant> results;
    QHash<quint64, QString> errors;
    QObject::connect(&backend, &IRadioBackend::extensionResult, &app,
        [&](quint64 id, const QVariant& value) { results[id] = value; });
    QObject::connect(&backend, &IRadioBackend::extensionError, &app,
        [&](quint64 id, const QString& value) { errors[id] = value; });
    Access::start(backend, device);
    check(waitFor(device, [&] { return backend.isConnected() && !Access::busy(backend); }), "initial settings adopted by actual production worker");
    backend.invokeExtension("rtl", "settings.get", 1);
    check(results[1].toMap().value("ppm").toInt() == 17
        && results[1].toMap().value("dcSuppression").toBool()
        && results[1].toMap().value("saved").toBool(), "confirmed snapshot restores both values and save status");
    check(device->hardware.ppm == 17, "saved PPM actually reached device operations");
    const int correctedStart = displayFrames;
    check(waitFor(device, [&] { return displayFrames >= correctedStart + 12; }) && displayPeak < -110,
        "production worker sends genuinely DC-corrected IQ to the display after settling");
    const int beforeInvalid = device->writes;
    quint64 invalid = 10;
    for (const QVariant& value : {QVariant(17.38), QVariant(true), QVariant("18"), QVariant(1001), QVariant(-1001)}) {
        backend.invokeExtension("rtl", "ppm.set", invalid, value);
        check(errors.contains(invalid++), "unsupported PPM input refused without integer rounding");
    }
    backend.invokeExtension("rtl", "dc_suppression.set", 19, 1);
    check(errors.contains(19) && device->writes == beforeInvalid, "invalid controls never dispatch USB writes");

    backend.invokeExtension("rtl", "ppm.set", 20, 18);
    check(settings.load().values.ppm == 17 && Access::status(backend).value("ppm").toInt() == 17,
        "pending PPM is neither displayed as applied nor persisted");
    check(waitFor(device, [&] { return results.contains(20); }) && results[20].toInt() == 18
        && settings.load().values.ppm == 18, "hardware and DSP confirmation precede persistence and extension completion");
    device->failWriteAt = device->writes + 3;
    backend.invokeExtension("rtl", "ppm.set", 21, 19);
    check(waitFor(device, [&] { return errors.contains(21); }) && backend.isConnected()
        && settings.load().values.ppm == 18 && device->hardware.ppm == 18,
        "USB refusal rolls back complete capture and cannot save refused calibration");
    device->failWriteAt = 0;

    {
        std::lock_guard lock(device->mutex); device->holdReadback = true;
    }
    backend.invokeExtension("rtl", "ppm.set", 30, 19);
    check(waitFor(device, [&] { std::lock_guard lock(device->mutex); return device->inReadback; }), "hold first readback for deterministic coalescing");
    backend.invokeExtension("rtl", "ppm.set", 31, 20);
    check(errors.contains(30) && settings.load().values.ppm == 18, "superseded promise refused before any speculative save");
    device->releaseReadback();
    check(waitFor(device, [&] { return results.contains(31); }) && results[31].toInt() == 20
        && settings.load().values.ppm == 20 && device->hardware.ppm == 20,
        "coalesced latest correction survives compensation and is sole accepted save");

    const int beforeDc = device->writes;
    backend.invokeExtension("rtl", "dc_suppression.set", 40, false);
    check(settings.load().values.dcSuppression, "pending DC change is not persisted");
    check(waitFor(device, [&] { return results.contains(40); }) && !results[40].toBool()
        && !settings.load().values.dcSuppression && device->writes == beforeDc,
        "software DC adoption changes actual worker state without USB hardware writes");

    const int rawStart = displayFrames;
    check(waitFor(device, [&] { return displayFrames >= rawStart + 2; }) && displayPeak > -50,
        "disabling DC suppression restores the raw injected constant IQ spectrum");

    const QJsonObject future{{"new-schema", 99}};
    check(scope.setFeature(RtlDeviceSettings::featureName(), 99, future), "seed newer document while connected");
    backend.invokeExtension("rtl", "ppm.set", 50, -7);
    check(waitFor(device, [&] { return results.contains(50); }) && results[50].toInt() == -7
        && !Access::status(backend).value("saved").toBool()
        && !Access::status(backend).value("saveReason").toString().isEmpty()
        && scope.featureExact(RtlDeviceSettings::featureName()) == future,
        "applied versus saved remain distinct when document cannot be overwritten");
    backend.disconnectRadio();
    backend.invokeExtension("rtl", "ppm.set", 60, 22);
    check(errors.contains(60) && scope.featureExact(RtlDeviceSettings::featureName()) == future,
        "disconnected setter cannot overwrite preserved state");

    check(scope.setFeature(RtlDeviceSettings::featureName(), 1, {{"ppm", 20}, {"dcSuppression", false}}), "restore valid saved document");
    backend.configureSettingsScope(scope, {scope.radioId(), false});
    backend.applyRestoredState({});
    device = std::make_shared<test::DeviceState>(); device->releaseReadback();
    Access::start(backend, device);
    check(waitFor(device, [&] { return backend.isConnected() && !Access::busy(backend); })
        && device->hardware.ppm == 20 && !Access::status(backend).value("dcSuppression").toBool(),
        "reconnect re-applies saved settings through a fresh worker session");
    backend.disconnectRadio();
    backend.configureSettingsScope(scope, {scope.radioId(), false}); backend.applyRestoredState({});
    Access::mismatch(backend);
    check(Access::status(backend).value("ppm").toInt() == 0
        && !Access::status(backend).value("saved").toBool(), "changed USB identity cannot inherit another device's calibration");

    T transaction({8, 1}); transaction.beginSession();
    T::Desired desired;
    desired.receivers = {{{0, 95200000, -8000, 8000, 0, 3000, 3000}, T::Mode::Fm}};
    check(bool(transaction.submit(desired)), "initial transaction valid");
    auto work = transaction.takeWork();
    check(work && transaction.complete({work->token, T::ResultCode::Applied, work->target, work->operation}) == T::Completion::Published, "initial completion accepted");
    desired.hardware = transaction.confirmed()->hardware;
    desired.dcSuppression = true;
    check(bool(transaction.submit(desired)), "software DC revision accepted");
    work = transaction.takeWork();
    check(work && !work->hardwareChanged && work->target.dcSuppression, "DC correction is explicit software state rather than hardware offset tuning");
    if (work) {
        auto forged = work->target; forged.dcSuppression = false;
        check(transaction.complete({work->token, T::ResultCode::Applied, forged, work->operation}) == T::Completion::Invalidated,
            "incorrect DSP adoption cannot publish requested DC state");
    }
    return failures ? 1 : 0;
}
