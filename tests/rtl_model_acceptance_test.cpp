// Actual RadioModel/SliceModel/backend/worker with injected USB, no sockets or RF.
// Optimistic setter publication, slice-zero routing, and accepting stale model
// intents must each break a behavioral assertion below.
#include "TestSettingsProfile.h"
#include "RtlInjectedDevice.h"
#include "core/AppSettings.h"
#include "core/backends/rtl/RtlSdrBackend.h"
#include "models/RadioModel.h"
#include "models/SliceModel.h"
#include "models/PanadapterModel.h"
#include <QCoreApplication>
#include <QElapsedTimer>
#include <QPointer>
#include <cstdio>
#include <algorithm>
#include <cmath>
#include <limits>
#include <memory>
#include <thread>
#include <utility>

using namespace AetherSDR;
using T = rtl::RtlCaptureTransaction;
namespace AetherSDR {
class RadioModelSliceLifecycleTestAccess {
public:
    static void restore(RadioModel& model, const QString& serial)
    {
        model.m_lastInfo.serial = serial;
        model.m_lastInfo.serialIdentity = {serial, false};
        model.handRestoredStateToBackend();
    }
    static void flush(RadioModel& model)
    { model.m_operatingStateSaveTimer.start(); model.flushPendingOperatingState(); }
    static void stage(RadioModel& model) { model.stageSessionModelsForReconnect(); }
};
}
namespace AetherSDR::rtl {
struct RtlCaptureBackendTestAccess {
    static void start(RtlSdrBackend& backend, std::unique_ptr<RtlSdrWorker::Device> device, int capacity = 1)
    {
        backend.m_receiverCapacity = capacity;
        backend.m_capture = T({8, static_cast<std::size_t>(capacity)});
        backend.m_requested = {};
        backend.m_requested.hardware = {static_cast<std::uint32_t>(backend.m_panCenterHz),
            backend.m_sampleRateHz, 0, 0, backend.m_ppmCorrection, 240};
        backend.m_requested.dcSuppression = backend.m_dcSuppression;
        // Keep the production bootstrap selected by handRestoredStateToBackend;
        // replace only the USB entry point. In particular, do not inject a
        // different WFM graph or discard the provisional monitor mute here.
        backend.m_requested.receivers = {backend.initialReceiver()};
        if (capacity > 1) {
            // The separate membership fixture deliberately admits several FM
            // receivers; it has no saved receiver state or production capacity.
            backend.m_requested.hardware = {100'000'000, 2'400'000, 0, 0, 0, 240};
            backend.m_requested.receivers = {{{0, 100'000'000, -8000, 8000, 0, 3000, 3000}, T::Mode::Fm}};
        }
        backend.startCapture(std::make_unique<RtlSdrWorker>(std::move(device), nullptr, capacity));
    }
    static bool idle(const RtlSdrBackend& backend) { return !backend.m_capture.busy() && !backend.m_pendingDrag; }
    static T::State state(const RtlSdrBackend& backend) { return *backend.m_capture.confirmed(); }
    static void pilot(RtlSdrBackend& backend, int id, T::Token token, bool stereo,
                      std::uint32_t sequence = 2, double magnitude = 0.1)
    {
        RtlReceivePipeline::Packet packet;
        const auto& receivers = backend.m_lastPublished->receivers;
        const auto receiver = std::ranges::find_if(receivers,
            [id](const auto& value) { return value.passband.stableId == id; });
        packet.slot = id; packet.token = token;
        packet.wfmStereoDetected = stereo && !receiver->wfmForceMono;
        packet.wfmReception = WfmReceptionDiagnostics{true, magnitude, stereo,
            stereo ? 5000U : 0U, 0, 0, 6000, 5000, 0.06, 0.03, 100, 0, 93, 187, sequence};
        backend.observeWfm(packet);
    }
    static void expirePilot(RtlSdrBackend& backend, int id)
    {
        backend.m_wfmObservationAge[id] = QElapsedTimer();
        backend.m_wfmObservationAge[id].start();
        QThread::msleep(510);
        backend.expireWfmObservations();
    }
    static void spectrum(RtlSdrBackend& backend, const QByteArray& frame, T::Token token)
    { emit backend.m_worker->spectrumFrameReady(token.session, token.revision, 0, frame); }
};
}
static int failures = 0;
static void check(bool ok, const char* message)
{
    if (!ok) { ++failures; std::fprintf(stderr, "FAIL: %s\n", message); }
}
template<class F> static bool waitFor(F predicate)
{
    QElapsedTimer timer; timer.start();
    while (!predicate() && timer.elapsed() < 5000) {
        QCoreApplication::processEvents(); QThread::msleep(2);
    }
    return predicate();
}
// This fixture's capture remains 2.4 MS/s with 8192 complex frames per
// callback. Queue at most one callback at a time and no faster than real time.
// Accumulating callbacks during off-thread planning would replay a burst into
// nonblocking WDSP and test artificial output underruns instead of adoption.
class CaptureClock {
public:
    explicit CaptureClock(std::shared_ptr<test::DeviceState> device)
        : m_device(std::move(device))
    { m_clock.start(); }
    void advance()
    {
        const qint64 now = m_clock.elapsed();
        if (m_device->starts.load() == 0 || m_device->callbacks.load() < m_queued
            || now < m_nextBlockMs) { return; }
        m_device->block();
        ++m_queued;
        m_nextBlockMs = now + 4;
    }
private:
    std::shared_ptr<test::DeviceState> m_device;
    QElapsedTimer m_clock;
    int m_queued = 0;
    qint64 m_nextBlockMs = 0;
};

static std::uint64_t monotonicMs()
{
    return std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count();
}

static void analogBankRestoreAndSharedCapture()
{
    using Access = rtl::RtlCaptureBackendTestAccess;
    const RadioSettingsScope scope("rtl", "analog-bank-restore");
    QVector<RtlSliceSettings::Slice> saved;
    for (int id = 0; id < 8; ++id) {
        RtlSliceSettings::Slice receiver;
        receiver.id = id;
        receiver.frequencyHz = 99'400'000 + id * 200'000;
        receiver.mode = id < 2 ? QStringLiteral("WFM") : QStringLiteral("FMN");
        receiver.filterLowHz = id < 2 ? -90000 : -8000;
        receiver.filterHighHz = -receiver.filterLowHz;
        receiver.wfmDeemphasisUs = id == 0 ? 50 : 75;
        receiver.wfmForceMono = id == 0;
        receiver.audioGain = 40 + id * 5;
        receiver.audioPan = id * 12;
        receiver.audioMute = id == 3;
        receiver.squelchEnabled = id >= 2;
        receiver.automaticSquelch = id >= 2;
        receiver.automaticSquelchMarginDb = 5 + id;
        saved.append(receiver);
    }
    check(RtlSliceSettings(scope).patch(100'000'000, 2'400'000, saved), "eight analog saved recipes seeded");
    RadioModel model;
    check(model.rebuildBackendForTest("rtl"), "restore bank model initialized");
    RadioModelSliceLifecycleTestAccess::restore(model, "analog-bank-restore");
    auto& backend = *static_cast<rtl::RtlSdrBackend*>(model.backend());
    auto device = std::make_shared<test::DeviceState>();
    CaptureClock clock(device);
    Access::start(backend, std::make_unique<test::InjectedDevice>(device), 8);
    device->releaseReadback();
    const auto settled = [&] { clock.advance(); return Access::idle(backend); };
    check(waitFor([&] { return settled() && model.slice(7); }), "eight analog saved receivers adopted");
    if (!model.slice(7)) { backend.disconnectRadio(); return; }
    for (int id = 0; id < 8; ++id) {
        const auto* slice = model.slice(id);
        check(slice && slice->mode() == saved[id].mode && slice->frequency() * 1e6 == saved[id].frequencyHz
            && slice->audioGain() == saved[id].audioGain && slice->audioPan() == saved[id].audioPan
            && slice->audioMute() == saved[id].audioMute && slice->filterLow() == saved[id].filterLowHz
            && slice->automaticSquelch() == saved[id].automaticSquelch
            && slice->automaticSquelchMarginDb() == saved[id].automaticSquelchMarginDb,
            "restore retains each stable receiver recipe and monitor settings");
    }
    check(model.slice(0)->wfmForceMono() && model.slice(0)->wfmDeemphasisUs() == 50
        && !model.slice(1)->wfmForceMono() && model.slice(1)->wfmDeemphasisUs() == 75,
        "restore retains distinct analog stereo and deemphasis policies");
    model.slice(2)->setActive(true);
    const auto before = Access::state(backend);
    backend.invokeExtension("rtl", "ppm.set", 800, -16);
    backend.invokeExtension("rtl", "ppm.set", 801, -17);
    check(waitFor([&] { return settled() && Access::state(backend).hardware.ppm == -17; }),
        "coalesced PPM edit reaches the entire eight-receiver capture");
    check(Access::state(backend).receivers == before.receivers && model.slice(2)->isActive(),
        "shared PPM changes preserve every recipe and selection");
    const auto valid = Access::state(backend);
    backend.invokeExtension("rtl", "sample_rate.set", 802, 0);
    check(Access::idle(backend) && Access::state(backend).token == valid.token,
        "invalid capture rate refuses without altering eight receivers");
    device->failWriteAt = device->writes + 1;
    backend.invokeExtension("rtl", "ppm.set", 803, -18);
    check(waitFor([&] { return settled(); }) && backend.isConnected()
        && Access::state(backend).hardware.ppm == -17
        && Access::state(backend).receivers == before.receivers && model.slice(2)->isActive(),
        "failed PPM application compensates all eight recipes without moving focus");
    backend.disconnectRadio();
}

static void analogBankSelection(int capacity = 4)
{
    using Access = rtl::RtlCaptureBackendTestAccess;
    RadioModel model;
    check(model.rebuildBackendForTest("rtl"), "analog bank model initialized");
    auto& backend = *static_cast<rtl::RtlSdrBackend*>(model.backend());
    auto device = std::make_shared<test::DeviceState>();
    CaptureClock clock(device);
    Access::start(backend, std::make_unique<test::InjectedDevice>(device), capacity);
    device->releaseReadback();
    const auto settled = [&] { clock.advance(); return Access::idle(backend); };
    check(waitFor([&] { return settled() && model.slice(0); }), "analog bank initial receiver adopted");
    if (!model.slice(0)) { backend.disconnectRadio(); return; }
    const auto selected = [&](int id) {
        int count = 0;
        for (int slot = 0; slot < 8; ++slot) {
            if (const auto* slice = model.slice(slot); slice && slice->isActive()) {
                ++count;
                if (slot != id) { return false; }
            }
        }
        return count == 1;
    };
    check(selected(0), "bootstrap selects exactly one stable receiver");
    for (int id = 1; id < capacity; ++id) {
        check(backend.createSlice({}, 99'500'000 + id * 200'000), "analog sibling creation admitted");
        check(waitFor([&] { return settled() && model.slice(id); }), "analog sibling creation adopted");
        if (!model.slice(id)) { backend.disconnectRadio(); return; }
        model.slice(id)->setActive(true);
        check(selected(id), "selection clears every previous active model");
    }
    check(!backend.createSlice({}, 100'650'000), "receiver above configured capacity refused at evaluation capacity");
    check(waitFor([&] {
        clock.advance();
        for (int id = 0; id < capacity; ++id) {
            if (!model.meterModel().relativeLevelForSlice(id)) { return false; }
        }
        return true;
    }), "real injected IQ reaches relative RF meters for every stable receiver ID");
    for (int id = 0; id < capacity; ++id) {
        check(!model.meterModel().sLevelForSlice(id), "RTL never claims a calibrated S reading");
    }

    for (int id = 0; id < capacity; ++id) {
        model.slice(id)->setAutomaticSquelch(true, 5 + id);
        model.slice(id)->setAudioMute(id % 2);
    }
    check(waitFor([&] { return settled() && model.slice(capacity - 1)->automaticSquelch(); }),
        "all receiver Auto requests adopt");
    for (int id = 0; id < capacity; ++id) {
        check(model.slice(id)->automaticSquelchMarginDb() == 5 + id
            && model.slice(id)->audioMute() == bool(id % 2), "SQL and mute stay per receiver");
    }
    const auto beforeFocus = Access::state(backend);
    const int beforeFocusWrites = device->writes;
    for (int iteration = 0; iteration < 32; ++iteration) {
        const int id = iteration % capacity;
        model.slice(id)->setActive(true);
        check(selected(id), "rapid focus retains one exact model identity");
    }
    check(Access::state(backend).token == beforeFocus.token && device->writes == beforeFocusWrites
        && Access::state(backend).receivers == beforeFocus.receivers,
        "focus changes no capture revision, hardware, monitor or receiver recipe");
    model.slice(3)->setActive(true); // start the reentrant scenario on its observed source
    bool redirected = false;
    const auto redirect = QObject::connect(model.slice(3), &SliceModel::activeChanged, &model,
        [&](bool active) {
            if (!active && !redirected) { redirected = true; model.slice(2)->setActive(true); }
        });
    model.slice(1)->setActive(true);
    check(redirected && selected(2), "reentrant newer selection supersedes the old publication tail");
    QObject::disconnect(redirect);
    model.slice(3)->setActive(true);
    model.slice(1)->setMode("WFM");
    check(waitFor([&] { return settled() && model.slice(1)->mode() == "WFM"; }),
        "WFM admitted beside three native analog receivers");
    model.slice(2)->setMode("WFM");
    check(waitFor([&] { return settled() && model.slice(2)->mode() == "WFM"; }),
        "second WFM receiver independently adopted");
    model.slice(1)->setWfmForceMono(true);
    model.slice(1)->setWfmDeemphasis(50);
    model.slice(1)->setFilterWidth(-90000, 90000);
    model.slice(2)->setWfmForceMono(false);
    model.slice(2)->setWfmDeemphasis(75);
    model.slice(2)->setFilterWidth(-80000, 80000);
    check(waitFor([&] { return settled() && model.slice(1)->filterLow() == -90000
        && model.slice(2)->filterLow() == -80000; }), "two independent WFM recipes adopt");
    check(model.slice(1)->wfmForceMono()
        && model.slice(1)->wfmDeemphasisUs() == 50
        && !model.slice(2)->wfmForceMono()
        && model.slice(2)->wfmDeemphasisUs() == 75 && selected(3),
        "WFM recipes and focus survive sibling publications");
    const auto beforeRefusal = Access::state(backend);
    int warnings = 0;
    QObject::connect(&backend, &IRadioBackend::configurationWarning, &model, [&](const QString&) { ++warnings; });
    model.slice(0)->setMode("AM");
    check(Access::idle(backend) && Access::state(backend).token == beforeRefusal.token
        && model.slice(0)->mode() == "FM" && model.slice(1)->wfmForceMono()
        && warnings >= 1, "unsupported legacy combinations refuse without altering recipes");
    model.slice(2)->setActive(true);
    auto* pan = model.panadapter(model.slice(2)->panId());
    check(pan != nullptr, "multi receiver pan materialized");
    if (pan) {
        model.requestPanCenter(pan->panId(), 105.0, -1.0, IRadioBackend::PanCenterIntent::Drag);
        check(waitFor([&] { return settled() && Access::state(backend).receivingIds.empty(); }),
            "all configured analog receivers can park together");
        for (int id = 0; id < capacity; ++id) {
            check(!model.meterModel().relativeLevelForSlice(id), "parking withdraws live RF meter samples");
        }

        check(selected(2) && model.slice(1)->wfmDeemphasisUs() == 50
            && !model.slice(1)->wfmReceptionDiagnostics().valid,
            "parked selection and recipe survive while reception clears");
        model.requestPanCenter(pan->panId(), 100.0, -1.0, IRadioBackend::PanCenterIntent::Drag);
        check(waitFor([&] { return settled() && Access::state(backend).receivingIds.size() == static_cast<std::size_t>(capacity); }),
            "all configured analog receivers resume at their configured RF");
    }
    QPointer<SliceModel> retired = model.slice(2);
    QObject::connect(&backend, &IRadioBackend::sliceRemoved, &model, [&](int id) {
        if (id == 2 && retired) { QCoreApplication::removePostedEvents(retired, QEvent::DeferredDelete); }
    });
    check(backend.removeSlice(2), "selected middle slot removal admitted");
    check(waitFor([&] { return settled() && !model.slice(2); }), "selected middle slot removal adopted");
    check(selected(0), "removed selection resolves deterministically to a surviving identity");
    check(model.meterModel().findMeter("SLC", "LEVEL", 2) == -1,
        "removed receiver withdraws its meter definition");

    check(backend.createSlice({}, 100'200'000), "middle stable slot reuse admitted");
    check(waitFor([&] { return settled() && model.slice(2); }), "middle stable slot replacement adopted");
    check(retired && model.slice(2) != retired && selected(0), "replacement never inherits retired selection");
    if (retired) {
        int staleFocusEdges = 0;
        QObject::connect(retired, &SliceModel::activeChanged, &model,
            [&](bool) { ++staleFocusEdges; });
        retired->setActive(true);
        check(selected(0) && staleFocusEdges == 0,
            "retired selection cannot emit a UI focus edge or target a reused numeric slot");
        retired->deleteLater();
    }
    const auto disconnectDuringSelect = QObject::connect(model.slice(0), &SliceModel::activeChanged,
        &model, [&](bool active) { if (!active) { backend.disconnectRadio(); } });
    model.slice(1)->setActive(true);
    check(!backend.isConnected() && !model.slice(1)->isActive(),
        "reentrant disconnect cancels the unaccepted selection tail");
    check(!model.meterModel().relativeLevelForSlice(0), "disconnect clears relative meter samples");
    QObject::disconnect(disconnectDuringSelect);
    backend.disconnectRadio();
}

int main(int argc, char** argv)
{
    TestSettingsProfile profile(QStringLiteral("rtl-model-acceptance"));
    if (!profile.isValid()) { return 1; }
    QCoreApplication app(argc, argv); AppSettings::instance().load();
    analogBankSelection();
    analogBankSelection(8);
    analogBankRestoreAndSharedCapture();
    const RadioSettingsScope scope("rtl", "model-accepted");
    RtlSliceSettings::Slice saved;
    saved.id = 3; saved.frequencyHz = 99'900'000; saved.mode = "FM";
    saved.filterLowHz = -8000; saved.filterHighHz = 8000;
    saved.audioGain = 22; saved.audioPan = 11; saved.audioMute = false;
    saved.squelchEnabled = true; saved.squelchLevel = 33;
    check(RtlSliceSettings(scope).patch(100'000'000, 2'400'000, {saved}), "sparse saved receiver seeded");
    RadioModel model;
    check(QMetaType::fromName("AetherSDR::SpectrumCoverage").isValid(),
          "coverage payload is registered for name-based seam consumers");
    check(model.rebuildBackendForTest("rtl"), "real model backend initialized");
    RadioModelSliceLifecycleTestAccess::restore(model, "model-accepted");
    auto& backend = *static_cast<rtl::RtlSdrBackend*>(model.backend());
    auto device = std::make_shared<test::DeviceState>();
    CaptureClock captureClock(device);
    rtl::RtlCaptureBackendTestAccess::start(backend, std::make_unique<test::InjectedDevice>(device));
    device->releaseReadback();
    check(waitFor([&] {
        captureClock.advance();
        return model.slice(3) && !model.slice(0) && rtl::RtlCaptureBackendTestAccess::idle(backend);
    }), "capacity-one restore materializes stable ID 3 instead of zero");
    QPointer<SliceModel> slice = model.slice(3);
    if (!slice) { backend.disconnectRadio(); return 1; }
    check(slice->squelchOn() && slice->squelchLevel() == 33 && slice->squelchStateKnown(),
          "accepted sparse receiver restores squelch enabled and threshold");
    int observed = 0, commands = 0;
    QVector<double> frequencies;
    QObject::connect(slice, &SliceModel::frequencyChanged, &model, [&](double mhz) { ++observed; frequencies.append(mhz); });
    QObject::connect(slice, &SliceModel::modeChanged, &model, [&] { ++observed; });
    QObject::connect(slice, &SliceModel::filterChanged, &model, [&] { ++observed; });
    QObject::connect(slice, &SliceModel::audioGainChanged, &model, [&] { ++observed; });
    QObject::connect(slice, &SliceModel::audioMuteChanged, &model, [&] { ++observed; });
    QObject::connect(slice, &SliceModel::audioPanChanged, &model, [&] { ++observed; });
    QObject::connect(slice, &SliceModel::squelchChanged, &model, [&] { ++observed; });
    QObject::connect(slice, &SliceModel::commandReady, &model, [&] { ++commands; });
    RadioModelSliceLifecycleTestAccess::flush(model);
    const auto initial = scope.featureExact("RtlSlices");
    const int before = observed;
    std::thread foreign([&] {
        slice->setFrequency(110.0); slice->setMode("AM");
        slice->setFilterWidth(-6000, 6000); slice->setAudioGain(88);
        slice->setAudioPan(88); slice->setAudioMute(true);
        slice->setSquelch(false, 88);
        emit slice->frequencyCommandIssued(111.0);
    });
    foreign.join();
    slice->setAudioGain(std::numeric_limits<float>::quiet_NaN());
    QCoreApplication::processEvents();
    check(rtl::RtlCaptureBackendTestAccess::idle(backend) && observed == before,
          "foreign-thread and nonfinite intents neither dispatch nor publish");
    slice->setFrequency(100.1);
    slice->setFilterWidth(-7000, 7000);
    slice->setAudioGain(37); slice->setAudioPan(73); slice->setAudioMute(true);
    slice->setSquelch(true, 61);
    check(slice->frequency() == 99.9 && slice->reportedFrequency() == 99.9
        && slice->filterLow() == -8000 && slice->filterHigh() == 8000
        && slice->audioGain() == 22 && slice->audioPan() == 11 && !slice->audioMute()
        && slice->squelchOn() && slice->squelchLevel() == 33,
        "pending sparse receiver edits do not change observed getters");
    check(observed == before && commands == 0, "pending edits emit neither observations nor Flex wire text");
    RadioModelSliceLifecycleTestAccess::flush(model);
    check(scope.featureExact("RtlSlices") == initial, "forced pending flush preserves accepted document");
    check(waitFor([&] {
        captureClock.advance();
        return rtl::RtlCaptureBackendTestAccess::idle(backend) && slice->frequency() == 100.1
            && slice->filterLow() == -7000 && slice->audioGain() == 37 && slice->audioMute()
            && slice->audioPan() == 73 && slice->squelchLevel() == 61;
    }), "callback adoption publishes all sparse-ID tuning/filter/audio edits");
    RadioModelSliceLifecycleTestAccess::flush(model);
    check(RtlSliceSettings(scope).load().document.slices[3].audioPan == 73,
          "accepted sparse receiver audio reaches persistence");
    check(RtlSliceSettings(scope).load().document.slices[3].squelchEnabled
        && RtlSliceSettings(scope).load().document.slices[3].squelchLevel == 61,
          "only accepted squelch threshold reaches persistence");
    slice->setAutomaticSquelch(true, 12);
    check(!slice->automaticSquelch(), "pending Auto intent is not published optimistically");
    check(waitFor([&] { captureClock.advance(); return rtl::RtlCaptureBackendTestAccess::idle(backend)
        && slice->automaticSquelch(); }), "Auto command adopts on sparse stable receiver");
    check(slice->automaticSquelchMarginDb() == 12 && slice->squelchLevel() == 61,
        "Auto margin is distinct from preserved manual threshold");
    RadioModelSliceLifecycleTestAccess::flush(model);
    const auto sqlSaved = RtlSliceSettings(scope).load().document.slices[3];
    check(sqlSaved.automaticSquelch && sqlSaved.automaticSquelchMarginDb == 12,
        "accepted per-receiver Auto persists");
    slice->setAutomaticSquelch(true, 100);
    check(rtl::RtlCaptureBackendTestAccess::idle(backend) && slice->automaticSquelchMarginDb() == 12,
        "invalid Auto margin is refused");
    slice->setSquelch(true, 61);
    check(waitFor([&] { captureClock.advance(); return rtl::RtlCaptureBackendTestAccess::idle(backend)
        && !slice->automaticSquelch(); }), "same manual threshold still exits Auto");
    RadioModelSliceLifecycleTestAccess::flush(model);
    const auto accepted = scope.featureExact("RtlSlices");
    const int refusedBefore = observed;
    slice->setFilterWidth(9000, -9000); slice->setMode("not-a-mode");
    check(observed == refusedBefore && slice->mode() == "FM" && slice->filterLow() == -7000,
          "synchronous refusal never changes observed state");
    RadioModelSliceLifecycleTestAccess::flush(model);
    check(scope.featureExact("RtlSlices") == accepted, "refused edits never persist");
    // A return to the already observed value must still supersede pending intent.
    slice->setFrequency(100.2); slice->setFrequency(100.1);
    check(waitFor([&] { captureClock.advance(); return rtl::RtlCaptureBackendTestAccess::idle(backend); }),
          "observed-value request cancels a pending different value");
    check(slice->frequency() == 100.1 && !frequencies.contains(100.2),
          "superseded receiver-only value never becomes observable");
    const int modeBefore = observed;
    slice->setMode("FMN");
    check(slice->mode() == "FM" && observed == modeBefore, "mode waits for adopted DSP");
    check(waitFor([&] { captureClock.advance(); return slice->mode() == "FMN" && rtl::RtlCaptureBackendTestAccess::idle(backend); }),
          "accepted mode reaches model through backend status");
    // Fail the center write, then hold the verified compensation readback.
    {
        std::lock_guard lock(device->mutex); device->holdReadback = true;
    }
    device->failWriteAt = device->writes + 5;
    const int rollbackBefore = observed;
    slice->tuneAndRecenter(105.0);
    check(waitFor([&] { std::lock_guard lock(device->mutex); return device->inReadback; }),
          "failed hardware edit reaches held rollback readback");
    check(slice->frequency() == 100.1 && observed == rollbackBefore, "rollback pending remains accepted-only");
    device->releaseReadback();
    check(waitFor([&] { return rtl::RtlCaptureBackendTestAccess::idle(backend); }), "verified rollback settles");
    check(backend.isConnected() && slice->frequency() == 100.1 && !frequencies.contains(105.0),
          "successful rollback retains connection and accepted model");
    // Supersede an actual hardware readback, requiring compensation before latest.
    {
        std::lock_guard lock(device->mutex); device->holdReadback = true;
    }
    slice->setFrequency(106.0);
    check(waitFor([&] { std::lock_guard lock(device->mutex); return device->inReadback; }), "hardware readback held");
    slice->setFrequency(107.0);
    check(slice->frequency() == 100.1, "both hardware requests remain invisible while pending");
    device->releaseReadback();
    check(waitFor([&] { captureClock.advance(); return rtl::RtlCaptureBackendTestAccess::idle(backend) && slice->frequency() == 107.0; }),
          "latest hardware state publishes after compensation");
    check(!frequencies.contains(106.0), "superseded hardware state never publishes");
    // Display geometry must never become a hardware/sample-rate/slice command.
    PanadapterModel* pan = model.panadapter(slice->panId());
    check(pan != nullptr, "accepted receiver resolves its model pan");
    if (pan) {
        const int beforeWrites = device->writes;
        RadioModelSliceLifecycleTestAccess::flush(model);
        const double captureCenter = RtlSliceSettings(scope).load().document.captureCenterHz;
        model.requestPanCenter(pan->panId(), 107.0, 0.2);
        check(pan->bandwidthMhz() < 0.201 && pan->bandwidthMhz() > 0.198,
              "zoom publishes a genuine narrow display window");
        check(rtl::RtlCaptureBackendTestAccess::idle(backend) && device->writes == beforeWrites,
              "display zoom performs no capture transaction or USB writes");
        // Quarter-rate FM placement leaves 106.7 MHz inside usable capture,
        // while the 107 MHz slice is outside this 200 kHz display window.
        model.requestPanCenter(pan->panId(), 106.7, -1.0, IRadioBackend::PanCenterIntent::Drag);
        check(pan->centerMhz() < 106.9 && slice->frequency() == 107.0,
              "panning can put a receiving slice offscreen without retuning it");
        check(device->writes == beforeWrites && rtl::RtlCaptureBackendTestAccess::idle(backend),
              "display drag does not stop acquisition");
        RadioModelSliceLifecycleTestAccess::flush(model);
        check(RtlSliceSettings(scope).load().document.captureCenterHz == captureCenter,
              "display geometry cannot overwrite persisted capture context");
        // The same two intents used by a typed frequency entry: receiver first,
        // then commanded display centering. Wait for receiver adoption.
        slice->setFrequency(107.05);
        model.requestPanCenter(pan->panId(), 107.05);
        check(slice->frequency() == 107.0, "typed frequency waits for receiver adoption");
        check(waitFor([&] { captureClock.advance(); return slice->frequency() == 107.05
            && rtl::RtlCaptureBackendTestAccess::idle(backend); }), "typed in-window tune adopted");
        check(std::abs(pan->centerMhz() - 107.05) < 0.002 && device->writes == beforeWrites,
              "typed centering moves only the view when capture already fits");
        check(model.requestConfirmedReceiveTune(3, 107.08, IRadioBackend::ReceiveTuneView::Center),
              "confirmed GUI tune admits receiver and view as one intent");
        check(slice->frequency() == 107.05, "joint typed intent remains unobserved until adoption");
        check(waitFor([&] { captureClock.advance(); return slice->frequency() == 107.08
            && rtl::RtlCaptureBackendTestAccess::idle(backend); }), "joint typed intent adopted");
        check(std::abs(pan->centerMhz() - 107.08) < 0.002 && device->writes == beforeWrites,
              "joint in-window typed intent centers display without USB writes");
        const double acceptedView = pan->centerMhz();
        check(model.requestConfirmedReceiveTune(3, 107.2, IRadioBackend::ReceiveTuneView::Center)
            && model.requestConfirmedReceiveTune(3, 107.08, IRadioBackend::ReceiveTuneView::Preserve),
              "newer preserve-view tune supersedes an unadopted centering intent");
        check(waitFor([&] { captureClock.advance(); return rtl::RtlCaptureBackendTestAccess::idle(backend); })
            && slice->frequency() == 107.08 && pan->centerMhz() == acceptedView,
              "superseded frequency and centering never become observations");
        check(!model.requestConfirmedReceiveTune(3, 3000.0, IRadioBackend::ReceiveTuneView::Center)
            && pan->centerMhz() == acceptedView,
              "refused typed frequency cannot move the accepted display");
        {
            std::lock_guard lock(device->mutex); device->holdReadback = true;
        }
        device->failWriteAt = device->writes + 5;
        check(model.requestConfirmedReceiveTune(3, 115.0, IRadioBackend::ReceiveTuneView::Center),
              "out-of-capture typed intent admitted provisionally");
        check(waitFor([&] { std::lock_guard lock(device->mutex); return device->inReadback; }),
              "typed retune failure reaches held compensation");
        check(slice->frequency() == 107.08 && pan->centerMhz() == acceptedView,
              "pending typed hardware change moves neither slice nor view");
        device->releaseReadback();
        check(waitFor([&] { return rtl::RtlCaptureBackendTestAccess::idle(backend); }),
              "typed retune rollback completed");
        check(slice->frequency() == 107.08 && pan->centerMhz() == acceptedView,
              "typed retune rollback preserves both accepted observations");
        const auto captured = rtl::RtlCaptureBackendTestAccess::state(backend);
        check(model.requestConfirmedReceiveTune(3, captured.capture.centerHz / 1e6,
            IRadioBackend::ReceiveTuneView::Center), "operator can tune onto DC without an implicit capture retune");
        check(waitFor([&] { captureClock.advance(); return rtl::RtlCaptureBackendTestAccess::idle(backend); }),
              "on-DC receiver tune adopted");
        check(!backend.healthSnapshot().values.value("rtlCaptureDcClear").toBool(),
              "DC overlap is operator-visible rather than hidden by a removed bin");
        check(model.requestReceiveCaptureRecenter(pan->panId()), "explicit DC placement admitted");
        check(waitFor([&] { captureClock.advance(); return rtl::RtlCaptureBackendTestAccess::idle(backend); }),
              "explicit DC placement adopted");
        const auto displaced = rtl::RtlCaptureBackendTestAccess::state(backend);
        check(displaced.hardware.centerHz != captured.hardware.centerHz
            && slice->frequency() == captured.capture.centerHz / 1e6
            && backend.healthSnapshot().values.value("rtlCaptureDcClear").toBool(),
              "explicit placement moves capture while preserving receiver RF and reporting DC clearance");
        QVector<float> observedBins;
        QVector<float> coverageBins;
        double coverageLowMhz = 0, coverageHighMhz = 0;
        int waterfallRows = 0;
        model.requestPanDisplayRates(pan->panId(), 25, 100);
        const auto waterfallConnection = QObject::connect(&model, &RadioModel::panFeedWaterfallRowReady,
            &model, [&](quint32, const QVector<float>& bins, double low, double high, quint32, qint64) {
                coverageBins = bins; coverageLowMhz = low; coverageHighMhz = high; ++waterfallRows;
            });
        int frames = 0;
        const auto frameConnection = QObject::connect(&model, &RadioModel::panFeedSpectrumReady,
            &model, [&](quint32, const QVector<float>& bins, qint64) { observedBins = bins; ++frames; });
        QVector<float> input(rtl::RtlViewport::kRtlSpectrumBins);
        for (int i = 0; i < input.size(); ++i) { input[i] = -120.0f + i * 0.02f; }
        input[32768] = -12.0f;
        const QByteArray raw(reinterpret_cast<const char*>(input.constData()), input.size() * sizeof(float));
        model.requestPanCenter(pan->panId(), displaced.capture.centerHz / 1e6, 0.01875);
        rtl::RtlCaptureBackendTestAccess::spectrum(backend, raw, displaced.token);
        check(observedBins.size() == 512 && observedBins[256] == -12.0f
            && observedBins.front() == input[32512] && observedBins.back() == input[33023],
              "display crop preserves the genuine DC bin and unchanged neighboring amplitudes");
        const auto usable = rtl::RtlViewport::fit(displaced.capture, input.size(),
            displaced.capture.centerHz, displaced.capture.achievedSampleRateHz);
        check(usable && coverageBins == input.sliced(usable->firstBin, usable->binCount)
            && std::abs(coverageLowMhz - (usable->centerHz - usable->spanHz / 2) / 1e6) < 1e-10
            && std::abs(coverageHighMhz - (usable->centerHz + usable->spanHz / 2) / 1e6) < 1e-10
            && waterfallRows == 1,
            "one history row preserves full usable real capture bins and RF bounds behind a narrow view");
        const QVector<float> fullCoverage = coverageBins;
        model.requestPanCenter(pan->panId(), displaced.capture.centerHz / 1e6,
            16 * displaced.capture.achievedSampleRateHz / input.size() / 1e6);
        rtl::RtlCaptureBackendTestAccess::spectrum(backend, raw, displaced.token);
        check(observedBins.size() == 16 && coverageBins == fullCoverage && waterfallRows == 2,
              "maximum zoom keeps real capture coverage while retaining all sixteen genuine close-view bins");
        if (usable) {
            const double rightCenter = (usable->centerHz + usable->spanHz / 2) / 1e6 - .009375;
            model.requestPanCenter(pan->panId(), rightCenter, .01875);
            rtl::RtlCaptureBackendTestAccess::spectrum(backend, raw, displaced.token);
            check(observedBins == input.sliced(usable->firstBin + usable->binCount - 512, 512)
                && coverageBins == fullCoverage && waterfallRows == 3,
                "near-edge asymmetric view carries no invented coverage past the actual usable capture");
        }
        const int acceptedFrames = frames;
        const int acceptedRows = waterfallRows;
        rtl::RtlCaptureBackendTestAccess::spectrum(backend, raw, {displaced.token.session, displaced.token.revision + 1});
        rtl::RtlCaptureBackendTestAccess::spectrum(backend, raw.left(17), displaced.token);
        for (const SpectrumCoverage& invalid : {
                 SpectrumCoverage{raw.left(17), 99, 101},
                 SpectrumCoverage{raw, std::numeric_limits<double>::quiet_NaN(), 101},
                 SpectrumCoverage{raw, 102, 101}, SpectrumCoverage{raw, -1, 101},
                 SpectrumCoverage{raw, 99, 1e300}}) {
            emit backend.spectrumFrameReady(0, raw, invalid);
        }
        check(frames == acceptedFrames && waterfallRows == acceptedRows,
              "stale or malformed view/coverage payloads populate neither spectrum nor history");
        QObject::disconnect(frameConnection);
        QObject::disconnect(waterfallConnection);
        slice->setFrequency(107.0);
        check(waitFor([&] { captureClock.advance(); return slice->frequency() == 107.0
            && rtl::RtlCaptureBackendTestAccess::idle(backend); }), "test receiver returned after viewport checks");
    }
    // A staged object cannot control an otherwise-connected backend.
    RadioModelSliceLifecycleTestAccess::stage(model);
    const int stagedWrites = device->writes;
    slice->setFrequency(108.0); slice->setAudioGain(90);
    slice->setSquelch(false, 90);
    check(rtl::RtlCaptureBackendTestAccess::idle(backend) && device->writes == stagedWrites
        && slice->frequency() == 107.0 && slice->audioGain() == 37,
        "staged slice cannot mutate accepted state or dispatch controls");
    // A fresh accepted report reclaims the existing object and its one binding.
    int reentrantEdits = 0;
    const auto reentrant = QObject::connect(slice, &SliceModel::frequencyChanged, &model, [&](double value) {
        if (value == 107.1) { ++reentrantEdits; slice->setAudioGain(41); }
    });
    backend.setSliceFrequency(3, 107'100'000);
    check(waitFor([&] { captureClock.advance(); return model.slice(3) == slice && slice->frequency() == 107.1
        && slice->audioGain() == 41 && rtl::RtlCaptureBackendTestAccess::idle(backend); }),
          "accepted state reclaims the staged object and adopts a reentrant edit");
    check(reentrantEdits == 1, "reclaimed binding publishes the accepted tune once");
    QObject::disconnect(reentrant);
    slice->setMode("WFM");
    check(waitFor([&] { captureClock.advance(); return slice->mode() == "WFM"
        && rtl::RtlCaptureBackendTestAccess::idle(backend); }),
          "WFM mode adopted before control checks");
    if constexpr (rtl::RtlReceivePipeline::kQualifiedWfmEnabled) {
        check(slice->filterLow() == -100000 && slice->filterHigh() == 100000,
              "entering native WFM selects the established broadcast RF width");
        RadioModelSliceLifecycleTestAccess::flush(model);
        const auto acceptedWfmSettings = scope.featureExact("RtlSlices");
        slice->setFilterWidth(-90000, 90000);
        slice->setWfmDeemphasis(50);
        slice->setWfmForceMono(true);
        check(!slice->wfmForceMono(), "forced Mono request is not optimistic accepted state");
        check(slice->filterLow() == -100000 && slice->wfmDeemphasisUs() == 75
            && scope.featureExact("RtlSlices") == acceptedWfmSettings,
            "WFM filter and deemphasis requests remain intent until adopted");
        check(waitFor([&] { captureClock.advance(); return rtl::RtlCaptureBackendTestAccess::idle(backend)
            && slice->filterLow() == -90000 && slice->filterHigh() == 90000
            && slice->wfmDeemphasisUs() == 50 && slice->wfmForceMono(); }),
            "WFM filter, deemphasis and forced Mono adopt together");
        slice->setSquelch(true, 75);
        slice->setWfmDeemphasis(60);
        check(rtl::RtlCaptureBackendTestAccess::idle(backend) && !slice->squelchOn()
            && slice->wfmDeemphasisUs() == 50,
            "manual WFM squelch and unsupported deemphasis remain refused");
        RadioModelSliceLifecycleTestAccess::flush(model);
        check(RtlSliceSettings(scope).load().document.slices.value(3).wfmDeemphasisUs == 50,
              "only accepted WFM deemphasis reaches persisted receiver state");
        check(RtlSliceSettings(scope).load().document.slices.value(3).wfmForceMono,
              "only adopted forced Mono reaches persisted settings");
        const auto token = rtl::RtlCaptureBackendTestAccess::state(backend).token;
        rtl::RtlCaptureBackendTestAccess::pilot(backend, 3, token, true);
        check(slice->wfmStereoStatus() == WfmStereoStatus::Mono,
              "current forced Mono output is reported independently of acquired pilot");
        check(slice->wfmForceMono() && slice->wfmReceptionDiagnostics().valid
            && slice->wfmReceptionDiagnostics().pilotLocked,
            "actual pilot observation is distinct from selected forced Mono");
        rtl::RtlCaptureBackendTestAccess::pilot(backend, 3, {token.session, token.revision - 1}, false);
        rtl::RtlCaptureBackendTestAccess::pilot(backend, 3, {token.session + 1, token.revision}, false);
        check(slice->wfmStereoStatus() == WfmStereoStatus::Mono
            && slice->wfmReceptionDiagnostics().pilotLocked,
              "old revision and foreign session cannot change WFM observation");
        rtl::RtlCaptureBackendTestAccess::expirePilot(backend, 3);
        check(slice->wfmStereoStatus() == WfmStereoStatus::Acquiring
            && !slice->wfmReceptionDiagnostics().valid,
              "stopped decoder observations expire rather than retaining stereo");
        rtl::RtlCaptureBackendTestAccess::pilot(backend, 3, token, true);
        check(!slice->wfmReceptionDiagnostics().valid,
              "cached decoder sequence cannot refresh expired reception");
        rtl::RtlCaptureBackendTestAccess::pilot(backend, 3, token, true, 4,
            std::numeric_limits<double>::quiet_NaN());
        check(!slice->wfmReceptionDiagnostics().valid, "invalid magnitude cannot refresh reception");
        rtl::RtlCaptureBackendTestAccess::pilot(backend, 3, token, false, 4);
        check(slice->wfmStereoStatus() == WfmStereoStatus::Mono,
              "current no-pilot observation reports mono");
        slice->setWfmForceMono(false);
        check(slice->wfmForceMono(), "Auto Stereo remains intent until decoder adoption");
        check(waitFor([&] { captureClock.advance(); return rtl::RtlCaptureBackendTestAccess::idle(backend)
            && !slice->wfmForceMono(); }), "Auto Stereo adoption confirms the selection");
        check(!slice->wfmReceptionDiagnostics().valid
            || slice->wfmReceptionDiagnostics().observationDurationMs < 6000,
            "new decoder revision cannot retain the old reception history");
        RadioModelSliceLifecycleTestAccess::flush(model);
        check(!RtlSliceSettings(scope).load().document.slices.value(3).wfmForceMono,
              "accepted Auto Stereo replaces only the chosen persistent policy");
    } else {
        const int wfmLow = slice->filterLow();
        const int wfmHigh = slice->filterHigh();
        const int wfmObserved = observed;
        RadioModelSliceLifecycleTestAccess::flush(model);
        const auto wfmSettings = scope.featureExact("RtlSlices");
        slice->setFilterWidth(-90'000, 90'000);
        slice->setSquelch(true, 75);
        check(rtl::RtlCaptureBackendTestAccess::idle(backend)
            && slice->filterLow() == wfmLow && slice->filterHigh() == wfmHigh
            && observed == wfmObserved && !slice->squelchOn(),
              "unsupported WFM filter neither queues work nor changes observed state");
        RadioModelSliceLifecycleTestAccess::flush(model);
        check(scope.featureExact("RtlSlices") == wfmSettings,
              "unsupported WFM filter does not persist a cosmetic passband");
    }
    slice->setMode("FMN");
    check(waitFor([&] { captureClock.advance(); return slice->mode() == "FMN"
        && rtl::RtlCaptureBackendTestAccess::idle(backend); }),
          "FMN restored after WFM control checks");
    check(slice->wfmStereoStatus() == WfmStereoStatus::Unavailable,
          "leaving WFM invalidates the pilot observation");
    if constexpr (rtl::RtlReceivePipeline::kQualifiedWfmEnabled) {
        check(slice->wfmDeemphasisUs() == 50,
              "mode changes retain the selected broadcast deemphasis");
    }
    RadioModelSliceLifecycleTestAccess::flush(model);
    const auto beforeDisconnect = scope.featureExact("RtlSlices");
    slice->setFrequency(107.2); slice->setAudioGain(80);
    slice->setSquelch(true, 80);
    backend.disconnectRadio();
    check(!frequencies.contains(107.2) && scope.featureExact("RtlSlices") == beforeDisconnect,
          "disconnect discards pending receiver edits and flushes only accepted state");
    const int disconnectedBefore = observed;
    if (slice) {
        slice->setFrequency(109.0); slice->setMode("AM"); slice->setAudioMute(false);
        slice->setSquelch(true, 90);
        check(observed == disconnectedBefore && slice->frequency() == 107.1 && slice->mode() == "FMN",
              "disconnected retained model never publishes unaccepted edits");
    }
    // Test-only admission of two receivers: pending membership is not accepted
    // membership, and a removed object must never control a reused numeric ID.
    {
        RadioModel multi;
        check(multi.rebuildBackendForTest("rtl"), "membership model initialized");
        auto& radio = *static_cast<rtl::RtlSdrBackend*>(multi.backend());
        auto usb = std::make_shared<test::DeviceState>();
        rtl::RtlCaptureBackendTestAccess::start(radio, std::make_unique<test::InjectedDevice>(usb), 4);
        usb->releaseReadback();
        check(waitFor([&] { usb->block(); return multi.slice(0) && rtl::RtlCaptureBackendTestAccess::idle(radio); }),
              "test-only FM receiver initialized");
        check(radio.createSlice({}, 100'300'000), "second receiver creation admitted");
        radio.setSliceFrequency(1, 100'400'000); radio.setSliceFilter(1, -6000, 6000);
        radio.setSliceAudioGain(1, 12); radio.setSliceAudioPan(1, 25); radio.setSliceAudioMute(1, true);
        radio.setSliceSquelch(1, true, 75);
        check(waitFor([&] { usb->block(); return multi.slice(1) && rtl::RtlCaptureBackendTestAccess::idle(radio); }),
              "pending receiver adopted");
        check(multi.slice(1) && multi.slice(1)->frequency() == 100.3 && multi.slice(1)->filterLow() == -8000
            && multi.slice(1)->audioGain() == 100 && multi.slice(1)->audioPan() == 50 && !multi.slice(1)->audioMute()
            && !multi.slice(1)->squelchOn(),
            "unaccepted membership cannot receive tuning/filter/audio commands");
        QPointer<SliceModel> retired = multi.slice(0);
        const auto retainRetired = QObject::connect(&radio, &IRadioBackend::sliceRemoved, &multi, [&](int id) {
            // Retain this test-owned retired object across numeric ID reuse.
            // The model's earlier connection has already queued its deletion.
            if (id == 0 && retired) { QCoreApplication::removePostedEvents(retired, QEvent::DeferredDelete); }
        });
        check(radio.removeSlice(0), "accepted receiver removal admitted");
        check(waitFor([&] { usb->block(); return !multi.slice(0) && rtl::RtlCaptureBackendTestAccess::idle(radio); }),
              "receiver removal confirmed");
        check(radio.createSlice({}, 100'500'000), "same numeric slot reused");
        check(waitFor([&] { usb->block(); return multi.slice(0) && rtl::RtlCaptureBackendTestAccess::idle(radio); }),
              "replacement receiver adopted");
        check(retired && multi.slice(0) != retired, "retired object retained until deferred deletion, with a distinct replacement");
        auto* multiPan = multi.panadapter(multi.slice(1)->panId());
        const double multiView = multiPan->centerMhz();
        const int multiWrites = usb->writes;
        int captureTransitions = 0;
        QObject::connect(multi.slice(0), &SliceModel::inCaptureChanged, &multi,
            [&](bool) { ++captureTransitions; });
        check(multi.slice(0)->inCapture() && multi.slice(1)->inCapture(),
              "both receivers initially report live capture");
        check(multi.requestConfirmedReceiveTune(1, 105.0, IRadioBackend::ReceiveTuneView::Center),
              "distant typed tune admits its selected receiver while sibling may park");
        check(multi.slice(1)->frequency() == 100.3 && multi.slice(0)->inCapture()
            && multiPan->centerMhz() == multiView,
              "pending distant tune changes no accepted model observation");
        check(waitFor([&] { usb->block(); return rtl::RtlCaptureBackendTestAccess::idle(radio)
            && multi.slice(1)->frequency() == 105.0 && !multi.slice(0)->inCapture(); }),
              "confirmed distant tune parks the sibling only after readback");
        const double halfFftBinMhz = 2'400'000.0 / (2.0 * rtl::RtlViewport::kRtlSpectrumBins * 1e6);
        check(multi.slice(0)->frequency() == 100.5 && multi.slice(1)->inCapture()
            && multiPan->bandwidthMhz() > 2.0
            && std::abs(multiPan->centerMhz() - 105.0) <= halfFftBinMhz
            && usb->writes > multiWrites,
              "full-width typed Center tune centers the selected RF within half an FFT bin");
        multi.requestPanCenter(multiPan->panId(), 100.4, -1.0,
            IRadioBackend::PanCenterIntent::Drag);
        const bool returned = waitFor([&] { usb->block(); return rtl::RtlCaptureBackendTestAccess::idle(radio)
            && multi.slice(0)->inCapture() && !multi.slice(1)->inCapture(); });
        check(returned,
              "return drag automatically resumes one fixed-RF slice and parks the distant one");
        check(multi.slice(0)->frequency() == 100.5 && multi.slice(1)->frequency() == 105.0
            && captureTransitions == 2,
              "park and resume each publish one capture-availability transition without tuning either RF");
        const int parkedWrites = usb->writes;
        multi.slice(1)->setFilterWidth(-8000, 50'000);
        check(rtl::RtlCaptureBackendTestAccess::idle(radio) && usb->writes == parkedWrites
            && multi.slice(1)->filterLow() == -8000 && multi.slice(1)->filterHigh() == 8000,
              "parked FM rejects an out-of-DSP filter without publishing or touching USB");
        multi.slice(1)->setFilterWidth(-6000, 6000);
        check(waitFor([&] { usb->block(); return rtl::RtlCaptureBackendTestAccess::idle(radio)
            && multi.slice(1)->filterLow() == -6000 && multi.slice(1)->filterHigh() == 6000
            && !multi.slice(1)->inCapture() && usb->writes == parkedWrites; }),
              "valid parked FM filter is accepted without claiming a live receiver");
        check(multi.requestConfirmedReceiveTune(1, 100.3, IRadioBackend::ReceiveTuneView::Preserve),
              "distant slice can be brought back into the shared capture");
        check(waitFor([&] { usb->block(); return rtl::RtlCaptureBackendTestAccess::idle(radio)
            && multi.slice(1)->frequency() == 100.3 && multi.slice(0)->inCapture()
            && multi.slice(1)->inCapture() && multi.slice(1)->filterLow() == -6000
            && multi.slice(1)->filterHigh() == 6000; }),
              "both configured receivers resume with the valid parked filter after accepted retune");
        if (retired) {
            retired->setFrequency(100.7); retired->setFilterWidth(-4000, 4000); retired->setAudioGain(9);
            retired->setSquelch(true, 90);
        }
        check(rtl::RtlCaptureBackendTestAccess::idle(radio) && multi.slice(0)->frequency() == 100.5,
              "retired object cannot dispatch into replacement with same ID");
        QObject::disconnect(retainRetired);
        if (retired) { retired->deleteLater(); }
        // Invalid apply and invalid rollback retire the session without exposing
        // either requested hardware state to the actual shared model.
        QPointer<SliceModel> survivor = multi.slice(1);
        int wrong = 0;
        QObject::connect(survivor, &SliceModel::frequencyChanged, &multi, [&](double value) {
            if (value == 105.0) { ++wrong; }
        });
        // Remove the sibling to isolate rollback from membership changes.
        check(radio.removeSlice(0), "remove sibling before hardware failure test");
        check(waitFor([&] { usb->block(); return !multi.slice(0) && rtl::RtlCaptureBackendTestAccess::idle(radio); }),
              "one receiver remains for hardware failure");
        usb->badReadback = true;
        survivor->setFrequency(105.0);
        check(waitFor([&] { return !radio.isConnected(); }), "invalid readback and rollback disconnect the model session");
        check(wrong == 0 && survivor && survivor->frequency() == 100.3,
              "failed apply and failed rollback never publish requested frequency");
    }
    // A later zoom while a typed Center tune is awaiting readback must become
    // part of the pending capture placement before either view is published.
    {
        RadioModel zoom;
        check(zoom.rebuildBackendForTest("rtl"), "pending-zoom model initialized");
        auto& radio = *static_cast<rtl::RtlSdrBackend*>(zoom.backend());
        auto usb = std::make_shared<test::DeviceState>();
        rtl::RtlCaptureBackendTestAccess::start(radio, std::make_unique<test::InjectedDevice>(usb), 4);
        usb->releaseReadback();
        check(waitFor([&] { usb->block(); return zoom.slice(0)
            && rtl::RtlCaptureBackendTestAccess::idle(radio); }),
              "pending-zoom FM receiver initialized");
        auto* pan = zoom.panadapter(zoom.slice(0)->panId());
        check(pan != nullptr, "pending-zoom pan resolved");
        if (pan) {
            zoom.requestPanCenter(pan->panId(), 100.0, 0.2);
            check(pan->bandwidthMhz() < 0.201 && pan->bandwidthMhz() > 0.198,
                  "pending-zoom test starts with a narrow display");
            {
                std::lock_guard lock(usb->mutex); usb->holdReadback = true;
            }
            check(zoom.requestConfirmedReceiveTune(0, 105.0, IRadioBackend::ReceiveTuneView::Center),
                  "pending narrow typed Center admitted");
            check(waitFor([&] { std::lock_guard lock(usb->mutex); return usb->inReadback; }),
                  "pending typed Center readback held");
            zoom.requestPanBandwidth(pan->panId(), 2.4);
            check(zoom.slice(0)->frequency() == 100.0 && pan->bandwidthMhz() < 0.201,
                  "concurrent full-width zoom publishes neither pending receiver nor view");
            usb->releaseReadback();
            check(waitFor([&] { usb->block(); return rtl::RtlCaptureBackendTestAccess::idle(radio)
                && zoom.slice(0)->frequency() == 105.0; }),
                  "latest typed tune and zoom settle after verified readback");
            const double halfFftBinMhz = 2'400'000.0 / (2.0 * rtl::RtlViewport::kRtlSpectrumBins * 1e6);
            check(pan->bandwidthMhz() > 2.0
                && std::abs(pan->centerMhz() - 105.0) <= halfFftBinMhz,
                  "full-width zoom during pending typed Center keeps the RF centered within half an FFT bin");
        }
        int rowsAfterDisconnect = 0;
        QObject::connect(&zoom, &RadioModel::panFeedWaterfallRowReady, &zoom,
            [&](quint32, const QVector<float>&, double, double, quint32, qint64) { ++rowsAfterDisconnect; });
        QObject::connect(&zoom, &RadioModel::panFeedSpectrumReady, &zoom,
            [&](quint32, const QVector<float>&, qint64) { radio.disconnectRadio(); });
        const auto state = rtl::RtlCaptureBackendTestAccess::state(radio);
        const QVector<float> lastFrame(rtl::RtlViewport::kRtlSpectrumBins, -80);
        rtl::RtlCaptureBackendTestAccess::spectrum(radio,
            QByteArray(reinterpret_cast<const char*>(lastFrame.constData()), lastFrame.size() * sizeof(float)),
            state.token);
        check(!radio.isConnected() && rowsAfterDisconnect == 0,
              "reentrant disconnect during spectrum publication cannot leak its paired history row");
        radio.disconnectRadio();
    }
    std::fprintf(stderr, "rtl_model_acceptance_test: %d failures\n", failures);
    return failures ? 1 : 0;
}
