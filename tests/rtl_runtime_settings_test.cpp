#include "TestSettingsProfile.h"
#include "RtlInjectedDevice.h"
#include <QElapsedTimer>
#include "core/AppSettings.h"
#include "core/RadioStateMemory.h"
#include "core/RtlDeviceSettings.h"
#include "models/RadioModel.h"
#include "core/backends/rtl/RtlSdrBackend.h"
#include <QCoreApplication>
#include <QJsonObject>
#include <QStringList>
#include <array>
#include <cmath>
#include <cstdio>
using namespace AetherSDR;
namespace AetherSDR {
class RadioModelSliceLifecycleTestAccess {
public:
    static void forcePendingFlush(RadioModel& model)
    {
        // Normal never-published sessions have no pending timer. Explicitly
        // inject a pending flush to exercise the defensive owner refusal too.
        model.m_operatingStateSaveTimer.start();
        model.flushPendingOperatingState();
    }
    static void queueDisconnectFlush(RadioModel& model)
    { model.m_operatingStateSaveTimer.start(10000); }
    static void restore(RadioModel& model, const QString& serial)
    {
        model.m_lastInfo.serial = serial;
        model.m_lastInfo.serialIdentity = {serial, false};
        model.handRestoredStateToBackend();
    }
};
}
namespace AetherSDR::rtl {
struct RtlCaptureBackendTestAccess {
    static void accepted(RtlSdrBackend& backend, int id, double hz)
    {
        RtlCaptureTransaction::State state;
        state.token = {1, 1}; state.capture = {1, 1, 100000000, 2400000, 1080000, 1080000};
        state.hardware.centerHz = 100000000;
        state.receivers = {{{id, hz, -8000, 8000, 0, 3000, 3000}, RtlCaptureTransaction::Mode::Fm}};
        state.receivingIds = {id};
        backend.m_lastPublished = state;
        backend.m_requested.receivers = state.receivers;
        backend.m_requested.receivers[0].passband.carrierHz = hz + 100000; // unaccepted intent
    }
    static bool activate(RtlSdrBackend& backend) { return backend.activateSettings(); }
    static void start(RtlSdrBackend& backend, std::unique_ptr<RtlSdrWorker::Device> device)
    {
        backend.m_receiverCapacity = 4;
        backend.m_capture = RtlCaptureTransaction({8, 4});
        backend.m_requested.hardware = {100000000, 2400000, 0, 0, 0, 240};
        const double wfmGuard = RtlReceivePipeline::kQualifiedWfmEnabled ? 3000 : 0;
        backend.m_requested.receivers = {{{0, 100000000, -100000, 100000, 0, wfmGuard, wfmGuard}, RtlCaptureTransaction::Mode::Wfm}};
        backend.startCapture(std::make_unique<RtlSdrWorker>(std::move(device), nullptr, 4));
    }
    static void startRestored(RtlSdrBackend& backend, std::unique_ptr<RtlSdrWorker::Device> device)
    {
        // Keep applyRestoredState's real bootstrap choice. Replace only the USB
        // entry, then use the production transaction, worker and restore path.
        backend.m_requested = {};
        backend.m_requested.hardware = {static_cast<std::uint32_t>(backend.m_panCenterHz),
            backend.m_sampleRateHz, 0, 0, backend.m_ppmCorrection, 240};
        backend.m_requested.dcSuppression = backend.m_dcSuppression;
        backend.m_requested.receivers = {backend.initialReceiver()};
        backend.startCapture(std::make_unique<RtlSdrWorker>(std::move(device)));
    }
    static bool bootstrapMuted(const RtlSdrBackend& backend)
    { return backend.m_requested.receivers.front().audioMute; }
    static bool restored(const RtlSdrBackend& backend)
    {
        return backend.m_settingsActive && backend.m_restoreAttempted && !backend.m_restoreToken.revision
            && backend.m_lastPublished && backend.m_lastPublished->receivers.front().passband.stableId == 1;
    }
    static bool restoreFinished(const RtlSdrBackend& backend)
    {
        return backend.m_settingsActive && backend.m_restoreAttempted && !backend.m_restoreToken.revision
            && backend.m_lastPublished && !backend.m_capture.busy();
    }
    static std::optional<RtlCaptureTransaction::State> state(const RtlSdrBackend& backend)
    { return backend.m_lastPublished; }
    static bool busy(const RtlSdrBackend& backend) { return backend.m_capture.busy(); }
    static int restoredPpm(const RtlSdrBackend& backend) { return backend.m_ppmCorrection; }
    static void remove(RtlSdrBackend& backend, int id) { backend.m_removedSettings.append(id); }
};
}
static int failures = 0;
static void check(bool value, const char* message)
{ if (!value) { ++failures; std::fprintf(stderr, "FAIL: %s\n", message); } }
static QJsonObject legacy()
{
    return {{"rfFrequencyHz", 100000000.0}, {"mode", "FM"}, {"filterLowHz", -8000},
        {"filterHighHz", 8000}, {"sampleRateHz", 2400000},
        {"ext", QJsonObject{{"rfGain", QJsonObject{{"gainDb", 24}}}}}, {"future", "preserve"}};
}
static void reconnectAtCaptureLimits()
{
    struct SavedCapture {
        std::uint32_t centerHz;
        std::uint32_t rateHz;
        const char* mode;
        int filterHz;
    };
    // The first two need a smaller provisional receiver at the lowest legal
    // rate. The others straddle the WFM RF+transition guard's lower RF edge.
    constexpr std::array<SavedCapture, 5> cases{{
        {100'000'000, 225'001, "WFM", 90'000},
        {100'000'000, 225'001, "FM", 7'000},
        {100'000, 2'400'000, "WFM", 90'000},
        {102'999, 2'400'000, "FM", 7'000},
        {103'000, 2'400'000, "WFM", 90'000}
    }};
    for (const SavedCapture& capture : cases) {
        const QString serial = QStringLiteral("bootstrap-%1-%2-%3")
            .arg(capture.centerHz).arg(capture.rateHz).arg(QLatin1String(capture.mode));
        const RadioSettingsScope scope("rtl", serial);
        RtlSliceSettings::Slice saved;
        saved.id = 1; saved.frequencyHz = capture.centerHz;
        saved.mode = QLatin1String(capture.mode);
        saved.filterLowHz = -capture.filterHz; saved.filterHighHz = capture.filterHz;
        saved.wfmDeemphasisUs = 50;
        saved.wfmForceMono = true;
        check(RtlSliceSettings(scope).patch(capture.centerHz, capture.rateHz, {saved}),
              "capture-limit reconnect fixture preserves a valid saved receiver");
        const QJsonObject original = scope.featureExact("RtlSlices");
        rtl::RtlSdrBackend receiver;
        for (int attempt = 0; attempt < 2; ++attempt) {
            std::fprintf(stderr, "RTL_RECONNECT center=%u rate=%u mode=%s attempt=%d\n",
                capture.centerHz, capture.rateHz, capture.mode, attempt);
            receiver.configureSettingsScope(scope, {serial, false});
            receiver.applyRestoredState({});
            check(receiver.currentOperatingState().sampleRateHz == int(capture.rateHz),
                  "real bootstrap retains the saved capture rate before USB startup");
            auto device = std::make_shared<test::DeviceState>();
            rtl::RtlCaptureBackendTestAccess::startRestored(receiver,
                std::make_unique<test::InjectedDevice>(device));
            check(rtl::RtlCaptureBackendTestAccess::bootstrapMuted(receiver),
                  "saved restoration keeps the provisional speaker muted until adoption");
            device->releaseReadback();
            QElapsedTimer deadline; deadline.start();
            int queued = 0;
            qint64 nextBlockMs = 0;
            const int callbackMs = static_cast<int>(std::ceil(8192.0 * 1000 / capture.rateHz));
            while (deadline.elapsed() < 8000 && !device->destroyed
                && !(rtl::RtlCaptureBackendTestAccess::restored(receiver)
                    && !rtl::RtlCaptureBackendTestAccess::busy(receiver))) {
                // Never burst queued callbacks while WDSP is still preparing.
                if (device->starts > 0 && device->callbacks >= queued
                    && deadline.elapsed() >= nextBlockMs) {
                    device->block(); ++queued;
                    nextBlockMs = deadline.elapsed() + callbackMs;
                }
                QCoreApplication::processEvents(); QThread::msleep(1);
            }
            const auto accepted = rtl::RtlCaptureBackendTestAccess::state(receiver);
            const bool restored = receiver.isConnected()
                && rtl::RtlCaptureBackendTestAccess::restored(receiver)
                && !rtl::RtlCaptureBackendTestAccess::busy(receiver);
            check(restored && accepted && accepted->hardware.centerHz == capture.centerHz
                && accepted->hardware.sampleRateHz == capture.rateHz
                && accepted->receivers.size() == 1 && accepted->receivingIds == std::vector<int>{1},
                "low-rate and RF-boundary reconnect preserves capture and restores the receiving sparse slice");
            if (restored && accepted) {
                const auto& actual = accepted->receivers.front();
                const auto expectedMode = saved.mode == QLatin1String("WFM")
                    ? rtl::RtlCaptureTransaction::Mode::Wfm : rtl::RtlCaptureTransaction::Mode::Fm;
                check(actual.mode == expectedMode && actual.passband.carrierHz == saved.frequencyHz
                    && actual.passband.filterLowHz == saved.filterLowHz
                    && actual.passband.filterHighHz == saved.filterHighHz
                    && actual.wfmDeemphasisUs == 50 && actual.wfmForceMono
                    && actual.audioMute == saved.audioMute,
                    "provisional bootstrap does not resize or replace saved receiver choices");
                check(device->starts == 1 && device->writes == 6 && device->cancels == 0,
                    "accepted saved receivers restore without a second hardware capture transaction");
                check(receiver.storeOperatingState(scope, receiver.currentOperatingState()).value_or(false),
                    "accepted restored receiver remains owned by the RTL settings writer");
            }
            check(scope.featureExact("RtlSlices") == original,
                  "reconnect and accepted persistence preserve the complete saved slice document");
            receiver.disconnectRadio();
        }
    }
}
static void unsupportedStoredWfmRemainsIntact()
{
    if (!rtl::RtlReceivePipeline::kQualifiedWfmEnabled) { return; }
    const QString serial = QStringLiteral("legacy-wfm-narrow-filter");
    const RadioSettingsScope scope("rtl", serial);
    RtlSliceSettings::Slice saved;
    // Old WFM ignored these stored RF edges. They must not acquire new RF
    // meaning or be silently migrated when the native decoder is enabled.
    saved.id = 0; saved.frequencyHz = 100'100'000; saved.mode = QStringLiteral("WFM");
    saved.filterLowHz = -4000; saved.filterHighHz = 4000;
    saved.audioMute = false; saved.audioGain = 23; saved.audioPan = 71;
    saved.agcMode = QStringLiteral("slow"); saved.agcThreshold = 17;
    saved.squelchEnabled = true; saved.squelchLevel = 42;
    check(RtlSliceSettings(scope).patch(100'000'000, 2'400'000, {saved}),
          "seed schema-valid legacy WFM with unsupported native RF edges");
    QJsonObject original = scope.featureExact("RtlSlices");
    original.insert(QStringLiteral("futureDocument"), QJsonObject{{"keep", "document extension"}});
    QJsonObject slices = original.value(QStringLiteral("slices")).toObject();
    QJsonObject entry = slices.value(QStringLiteral("0")).toObject();
    entry.remove(QStringLiteral("wfmDeemphasisUs")); // Genuine old schema-one entry.
    entry.insert(QStringLiteral("futureSlice"), QJsonObject{{"keep", "slice extension"}});
    QJsonObject squelch = entry.value(QStringLiteral("squelch")).toObject();
    squelch.insert(QStringLiteral("futureSquelch"), 91);
    entry.insert(QStringLiteral("squelch"), squelch);
    slices.insert(QStringLiteral("0"), entry);
    original.insert(QStringLiteral("slices"), slices);
    check(scope.setFeature("RtlSlices", RtlSliceSettings::kSchemaVersion, original),
          "retain old optional-field absence and unknown members in the legacy document");

    RadioModel model;
    check(model.rebuildBackendForTest("rtl"), "legacy WFM restore uses the real model settings owner");
    auto& receiver = *static_cast<rtl::RtlSdrBackend*>(model.backend());
    QStringList warnings;
    QObject::connect(&receiver, &IRadioBackend::configurationWarning, &model,
        [&](const QString& warning) { warnings.append(warning); });
    RadioModelSliceLifecycleTestAccess::restore(model, serial);
    check(receiver.currentOperatingState().mode == QLatin1String("AM"),
          "real restored-state application chooses the provisional AM bootstrap");
    auto device = std::make_shared<test::DeviceState>();
    rtl::RtlCaptureBackendTestAccess::startRestored(receiver,
        std::make_unique<test::InjectedDevice>(device));
    check(rtl::RtlCaptureBackendTestAccess::bootstrapMuted(receiver),
          "unsupported saved WFM never starts audible provisional AM");
    device->releaseReadback();
    QElapsedTimer deadline; deadline.start();
    int queued = 0;
    qint64 nextBlockMs = 0;
    while (deadline.elapsed() < 8000 && !device->destroyed
        && !(device->starts > 0 && rtl::RtlCaptureBackendTestAccess::restoreFinished(receiver))) {
        if (device->starts > 0 && device->callbacks >= queued && deadline.elapsed() >= nextBlockMs) {
            device->block(); ++queued;
            nextBlockMs = deadline.elapsed() + 4; // 8192 / 2.4 MS/s, rounded up.
        }
        QCoreApplication::processEvents(); QThread::msleep(1);
    }
    const auto accepted = rtl::RtlCaptureBackendTestAccess::state(receiver);
    check(receiver.isConnected() && rtl::RtlCaptureBackendTestAccess::restoreFinished(receiver)
              && accepted && accepted->hardware.centerHz == 100'000'000
              && accepted->hardware.sampleRateHz == 2'400'000
              && accepted->receivers.size() == 1
              && accepted->receivers.front().passband.stableId == 0
              && accepted->receivers.front().mode == rtl::RtlCaptureTransaction::Mode::Am
              && accepted->receivers.front().audioMute,
          "unsupported WFM remains unadopted while the confirmed provisional AM receiver stays muted");
    bool explained = false;
    for (const QString& warning : warnings) {
        const QString text = warning.toLower();
        explained |= text.contains(QStringLiteral("wfm")) && text.contains(QStringLiteral("filter"))
            && (text.contains(QStringLiteral("saved")) || text.contains(QStringLiteral("stored")))
            && (text.contains(QStringLiteral("unsupported")) || text.contains(QStringLiteral("not supported")));
    }
    check(explained, "typed configuration warning explains the unsupported stored WFM filter");
    check(device->starts == 1 && device->writes == 6 && device->cancels == 0,
          "unsupported stored WFM does not trigger a replacement hardware capture");
    check(scope.featureExact("RtlSlices") == original,
          "bootstrap and refused WFM restore preserve the complete saved document");
    check(receiver.storeOperatingState(scope, receiver.currentOperatingState()).value_or(false),
          "RTL settings owner handles the accepted provisional-state store");
    RadioModelSliceLifecycleTestAccess::forcePendingFlush(model);
    check(scope.featureExact("RtlSlices") == original,
          "explicit store and model flush cannot overwrite or resize the omitted legacy WFM entry");
    RadioModelSliceLifecycleTestAccess::queueDisconnectFlush(model);
    receiver.disconnectRadio();
    check(!receiver.isConnected() && scope.featureExact("RtlSlices") == original,
          "disconnect flush preserves all legacy WFM choices and unknown document members");
}
int main(int argc, char** argv)
{
    TestSettingsProfile profile(QStringLiteral("rtl-runtime-settings"));
    if (!profile.isValid()) { return 1; }
    QCoreApplication app(argc, argv); AppSettings::instance().load();
    {
        const RadioSettingsScope calibrated("rtl", "calibrated-receiver");
        QString reason;
        check(RtlDeviceSettings(calibrated).saveAccepted({17, false}, reason), "seed accepted device calibration");
        rtl::RtlSdrBackend restored;
        restored.configureSettingsScope(calibrated, {"calibrated-receiver", false});
        restored.applyRestoredState({});
        check(rtl::RtlCaptureBackendTestAccess::restoredPpm(restored) == 17,
            "restore sends stored device PPM through the existing initial hardware transaction");
        check(!restored.currentOperatingState().extension.contains("ppm"), "generic OperatingState is not a second calibration writer");
        restored.configureSettingsScope(RadioSettingsScope("rtl", "different-receiver"), {"different-receiver", false});
        restored.applyRestoredState({});
        check(rtl::RtlCaptureBackendTestAccess::restoredPpm(restored) == 0,
            "reusing a backend for another device clears previous calibration");
    }
    const RadioSettingsScope scope("rtl", "0");
    check(scope.setFeature("OperatingState", 2, legacy()), "legacy numeric-serial snapshot seeded");
    rtl::RtlSdrBackend backend;
    backend.configureSettingsScope(scope, {"0", false});
    check(!rtl::RtlCaptureBackendTestAccess::activate(backend), "ownership cannot cut over before accepted hardware state");
    rtl::RtlCaptureBackendTestAccess::accepted(backend, 2, 100200000);
    check(rtl::RtlCaptureBackendTestAccess::activate(backend), "accepted runtime claims new settings owner");
    const auto domains = backend.capabilities().clientSettingsDomains;
    using D = RadioCapabilities::ClientSettingsDomain;
    check(domains.testFlag(D::RtlSlices) && !domains.testFlag(D::Tuning)
        && !domains.testFlag(D::Passband) && !domains.testFlag(D::SpanRate), "overlapping OperatingState domains removed together");
    RestoredRadioState gain; gain.extensionSchemaVersion = 1; gain.extension = {{"rfGain", QJsonObject{{"gainDb", 30}}}};
    const auto result = backend.storeOperatingState(scope, gain);
    check(result.has_value() && *result, "backend owns accepted state persistence and RF gain patch");
    const auto saved = RtlSliceSettings(scope).load();
    check(saved.status == RtlSliceSettings::ReadStatus::Ready && saved.document.slices.contains(0)
        && saved.document.slices[2].frequencyHz == 100200000, "accepted sparse slice persisted, pending intent excluded, omitted migration entry retained");
    const auto old = scope.featureExact("OperatingState");
    check(old.value("future") == "preserve" && old.value("rfFrequencyHz").toDouble() == 100000000
        && old.value("ext").toObject().value("rfGain").toObject().value("gainDb").toInt() == 30,
        "RF gain changes preserve complete downgrade snapshot");
    rtl::RtlCaptureBackendTestAccess::remove(backend, 0);
    check(backend.storeOperatingState(scope, gain).value_or(false)
        && !RtlSliceSettings(scope).load().document.slices.contains(0), "only accepted explicit removal erases saved stable ID");
    const QJsonObject newer{{"opaque", "future owner"}};
    check(scope.setFeature("RtlSlices", 99, newer), "newer schema installed");
    const auto refused = backend.storeOperatingState(scope, gain);
    check(refused.has_value() && !*refused && scope.featureExact("RtlSlices") == newer,
        "refused atomic feature write stays handled, cannot fall through to generic writer");
    const auto snapshot = saved.document.slices[2];
    check(scope.removeFeature("RtlSlices") && RtlSliceSettings(scope).patch(100000000, 2400000, {snapshot}), "supported document restored for retry");
    check(backend.storeOperatingState(scope, gain).value_or(false), "previous refused write can retry without losing ownership");
    rtl::RtlSdrBackend anonymous;
    const auto family = RadioSettingsScope::anonymousRadio("rtl");
    anonymous.configureSettingsScope(family, {{}, true});
    rtl::RtlCaptureBackendTestAccess::accepted(anonymous, 0, 100000000);
    check(rtl::RtlCaptureBackendTestAccess::activate(anonymous), "synthetic index uses model-supplied anonymous scope");
    check(RtlSliceSettings(family).load().status == RtlSliceSettings::ReadStatus::Ready
        && RadioSettingsScope("rtl", "rtl:7").featureExact("RtlSlices").isEmpty(), "USB index never becomes a persistent identity");
    check(!backend.storeOperatingState(RadioSettingsScope("rtl", "another"), gain).value_or(true), "scope mismatch refuses writes");
    {
        const RadioSettingsScope futureRate("rtl", "future-rate");
        check(RtlSliceSettings(futureRate).patch(100000000, 1e12, {snapshot}), "schema-valid future capture rate seeded");
        rtl::RtlSdrBackend bounded;
        bounded.configureSettingsScope(futureRate, {"future-rate", false}); bounded.applyRestoredState({});
        check(bounded.currentOperatingState().sampleRateHz == 2400000
            && RtlSliceSettings(futureRate).load().document.sampleRateHz == 1e12,
            "unsupported saved hardware rate preserves valid initial capture without integer overflow or document rewrite");
    }
    {
        // Exercise setupBackend's actual disconnect-flush ownership route.
        const RadioSettingsScope unconfirmedScope("rtl", "never-published");
        check(unconfirmedScope.setFeature("OperatingState", 2, legacy()), "pre-connect snapshot seeded");
        RadioModel model;
        check(model.rebuildBackendForTest("rtl"), "real model builds RTL without opening USB");
        RadioModelSliceLifecycleTestAccess::restore(model, "never-published");
        auto* unconfirmed = static_cast<rtl::RtlSdrBackend*>(model.backend());
        auto device = std::make_shared<test::DeviceState>();
        rtl::RtlCaptureBackendTestAccess::start(*unconfirmed, std::make_unique<test::InjectedDevice>(device));
        check(model.settingsScope().radioId() == unconfirmedScope.radioId(), "model owns the expected unconfirmed radio scope");
        RadioModelSliceLifecycleTestAccess::forcePendingFlush(model);
        check(unconfirmedScope.featureExact("OperatingState") == legacy()
            && unconfirmedScope.featureExact("RtlSlices").isEmpty(),
            "forced flush during unconfirmed connect preserves saved state without claiming ownership");
        bool disconnected = false;
        QObject::connect(unconfirmed, &IRadioBackend::disconnected, [&] { disconnected = true; });
        RadioModelSliceLifecycleTestAccess::queueDisconnectFlush(model);
        device->badReadback = true; device->releaseReadback();
        QElapsedTimer deadline; deadline.start();
        while (!disconnected && deadline.elapsed() < 3000) {
            QCoreApplication::processEvents(); QThread::msleep(1);
        }
        check(disconnected && !unconfirmed->isConnected(), "invalid initial capture reaches production disconnect");
        check(unconfirmedScope.featureExact("OperatingState") == legacy()
            && unconfirmedScope.featureExact("RtlSlices").isEmpty(),
            "disconnect flush refuses speculative fallback before first accepted capture");
    }
    {
        const RadioSettingsScope liveScope("rtl", "restore-live");
        RtlSliceSettings::Slice first;
        first.id = 1; first.frequencyHz = 99700000; first.mode = "FM"; first.filterLowHz = -8000; first.filterHighHz = 8000;
        first.audioMute = true; first.audioGain = 22; first.audioPan = 11;
        auto second = first; second.id = 3; second.frequencyHz = 100600000;
        auto outside = first; outside.id = 5; outside.frequencyHz = 120000000;
        check(RtlSliceSettings(liveScope).patch(100000000, 2400000, {outside, second, first}), "unordered saved slices seeded");
        RadioModel model;
        check(model.rebuildBackendForTest("rtl"), "live owner test uses production capability relay");
        RadioModelSliceLifecycleTestAccess::restore(model, "restore-live");
        auto& live = *static_cast<rtl::RtlSdrBackend*>(model.backend());
        bool ownershipRepublished = false;
        QHash<int, bool> publishedCaptureMembership;
        QObject::connect(&live, &IRadioBackend::sliceChanged,
            [&](int id, const SliceDelta& delta) {
                if (delta.inCapture) { publishedCaptureMembership[id] = *delta.inCapture; }
            });
        QObject::connect(&model, &RadioModel::capabilitiesChanged,
            [&](bool, const RadioCapabilities& caps) {
                if (caps.clientSettingsDomains.testFlag(D::RtlSlices)) {
                    ownershipRepublished = !caps.clientSettingsDomains.testFlag(D::Tuning)
                        && !caps.clientSettingsDomains.testFlag(D::Passband)
                        && !caps.clientSettingsDomains.testFlag(D::SpanRate);
                }
            });
        auto device = std::make_shared<test::DeviceState>();
        rtl::RtlCaptureBackendTestAccess::start(live, std::make_unique<test::InjectedDevice>(device));
        device->releaseReadback();
        QElapsedTimer deadline; deadline.start();
        while (deadline.elapsed() < 3000 && !rtl::RtlCaptureBackendTestAccess::restored(live)) {
            device->block(); QCoreApplication::processEvents(); QThread::msleep(5);
        }
        check(rtl::RtlCaptureBackendTestAccess::restored(live), "actual backend restores sparse IDs after readback and adoption");
        const auto restored = rtl::RtlCaptureBackendTestAccess::state(live);
        check(restored && restored->receivers.size() == 3
            && restored->receivers[0].passband.stableId == 1
            && restored->receivers[1].passband.stableId == 3
            && restored->receivers[2].passband.stableId == 5
            && restored->receivingIds == (std::vector<int>{1, 3})
            && restored->hardware.centerHz == 100'000'000
            && restored->hardware.sampleRateHz == 2'400'000
            && publishedCaptureMembership.value(1) && publishedCaptureMembership.value(3)
            && publishedCaptureMembership.contains(5) && !publishedCaptureMembership.value(5),
            "saved slices stay configured while the out-of-capture slice is parked");
        check(ownershipRepublished && model.backendCapabilities().clientSettingsDomains.testFlag(D::RtlSlices),
            "live settings ownership reaches model consumers through capabilitiesChanged");
        check(device->starts == 1 && device->writes == 6 && device->cancels == 0,
            "restore never retunes or restarts the accepted capture");
        model.flushPendingOperatingState();
        const auto document = RtlSliceSettings(liveScope).load().document;
        check(document.slices.size() == 3 && document.slices[1].audioMute && document.slices[1].audioGain == 22
            && document.slices[1].audioPan == 11 && document.slices[5].frequencyHz == 120000000,
            "restored monitor controls and parked slice remain saved");
        live.setPanCenter(QStringLiteral("0xe1000000"), 120'000'000,
            IRadioBackend::PanCenterIntent::Drag);
        deadline.restart();
        while (deadline.elapsed() < 3000) {
            device->block(); QCoreApplication::processEvents(); QThread::msleep(5);
            const auto current = rtl::RtlCaptureBackendTestAccess::state(live);
            if (!rtl::RtlCaptureBackendTestAccess::busy(live) && current
                && current->hardware.centerHz == 120'000'000
                && current->receivingIds == (std::vector<int>{5})) { break; }
        }
        const auto resumed = rtl::RtlCaptureBackendTestAccess::state(live);
        check(restored && resumed && resumed->hardware.centerHz == 120'000'000
            && resumed->receivingIds == (std::vector<int>{5})
            && resumed->receivers == restored->receivers
            && resumed->hardware.sampleRateHz == restored->hardware.sampleRateHz
            && resumed->hardware.directSampling == restored->hardware.directSampling
            && resumed->hardware.offsetTuning == restored->hardware.offsetTuning
            && resumed->hardware.gainTenths == restored->hardware.gainTenths
            && resumed->hardware.ppm == restored->hardware.ppm
            && !publishedCaptureMembership.value(1) && !publishedCaptureMembership.value(3)
            && publishedCaptureMembership.value(5),
            "drag resumes the saved RF slice and parks earlier slices without changing their settings");
        model.flushPendingOperatingState();
        const auto movedDocument = RtlSliceSettings(liveScope).load().document;
        check(movedDocument.captureCenterHz == 120'000'000
            && movedDocument.sampleRateHz == document.sampleRateHz
            && movedDocument.slices.size() == 3
            && movedDocument.slices[1].frequencyHz == first.frequencyHz
            && movedDocument.slices[3].frequencyHz == second.frequencyHz
            && movedDocument.slices[5].frequencyHz == outside.frequencyHz
            && movedDocument.slices[5].audioMute == outside.audioMute
            && movedDocument.slices[5].audioGain == outside.audioGain
            && movedDocument.slices[5].audioPan == outside.audioPan,
            "drag persists the new capture center without changing saved slice RF or monitor settings");
        const auto beforeMeters = rtl::RtlCaptureBackendTestAccess::state(live);
        int meterResults = 0;
        int meterErrors = 0;
        QHash<int, QString> reasons;
        QObject::connect(&live, &IRadioBackend::extensionResult, [&](quint64 id, const QVariant&) {
            if (id >= 9100 && id <= 9102) { ++meterResults; }
        });
        QObject::connect(&live, &IRadioBackend::extensionError, [&](quint64 id, const QString&) {
            if (id >= 9100 && id <= 9102) { ++meterErrors; }
        });
        QObject::connect(&live, &IRadioBackend::meterDefined, [&](const MeterDef& def) {
            reasons[def.sourceIndex] = def.unavailableReason;
        });
        live.invokeExtension("rtl", "receive_meters.set", 9100, false);
        check(meterResults == 1 && meterErrors == 0 && reasons.size() == 3
            && !reasons[1].isEmpty() && !reasons[3].isEmpty() && !reasons[5].isEmpty()
            && !RtlDeviceSettings(liveScope).load().values.receiveMetersEnabled,
            "disabled meters publish reasons for sparse IDs and persist through the sole device owner");
        const auto afterMeters = rtl::RtlCaptureBackendTestAccess::state(live);
        check(beforeMeters && afterMeters && beforeMeters->token == afterMeters->token
            && beforeMeters->hardware == afterMeters->hardware
            && beforeMeters->receivers == afterMeters->receivers
            && beforeMeters->receivingIds == afterMeters->receivingIds
            && beforeMeters->dcSuppression == afterMeters->dcSuppression
            && !rtl::RtlCaptureBackendTestAccess::busy(live),
            "meter toggle does not restart capture or change receivers, squelch or audio settings");
        live.invokeExtension("rtl", "receive_meters.set", 9101, QStringLiteral("true"));
        check(meterErrors == 1 && meterResults == 1, "meter control rejects a string masquerading as boolean");
        live.invokeExtension("rtl", "receive_meters.set", 9102, true);
        check(meterResults == 2 && reasons[1].isEmpty() && reasons[3].isEmpty() && reasons[5].isEmpty()
            && RtlDeviceSettings(liveScope).load().values.receiveMetersEnabled,
            "re-enabled sparse meters clear unavailable reasons and save accepted state");
        live.disconnectRadio();
    }
    reconnectAtCaptureLimits();
    unsupportedStoredWfmRemainsIntact();
    std::fprintf(stderr, "rtl_runtime_settings_test: %d failures\n", failures);
    return failures ? 1 : 0;
}
