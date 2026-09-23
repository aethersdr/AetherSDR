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
#include <limits>
#include <thread>

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
        backend.m_requested.hardware = {100'000'000, 2'400'000, 0, 0, 0, 240};
        backend.m_requested.receivers = {{{0, 100'000'000, -100'000, 100'000, 0, 0, 0}, T::Mode::Wfm}};
        if (capacity > 1) {
            backend.m_requested.receivers = {{{0, 100'000'000, -8000, 8000, 0, 3000, 3000}, T::Mode::Fm}};
        }
        backend.startCapture(std::make_unique<RtlSdrWorker>(std::move(device), nullptr, capacity));
    }
    static bool idle(const RtlSdrBackend& backend) { return !backend.m_capture.busy(); }
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
int main(int argc, char** argv)
{
    TestSettingsProfile profile(QStringLiteral("rtl-model-acceptance"));
    if (!profile.isValid()) { return 1; }
    QCoreApplication app(argc, argv); AppSettings::instance().load();
    const RadioSettingsScope scope("rtl", "model-accepted");
    RtlSliceSettings::Slice saved;
    saved.id = 3; saved.frequencyHz = 99'900'000; saved.mode = "FM";
    saved.filterLowHz = -8000; saved.filterHighHz = 8000;
    saved.audioGain = 22; saved.audioPan = 11; saved.audioMute = false;
    check(RtlSliceSettings(scope).patch(100'000'000, 2'400'000, {saved}), "sparse saved receiver seeded");
    RadioModel model;
    check(model.rebuildBackendForTest("rtl"), "real model backend initialized");
    RadioModelSliceLifecycleTestAccess::restore(model, "model-accepted");
    auto& backend = *static_cast<rtl::RtlSdrBackend*>(model.backend());
    auto device = std::make_shared<test::DeviceState>();
    rtl::RtlCaptureBackendTestAccess::start(backend, std::make_unique<test::InjectedDevice>(device));
    device->releaseReadback();
    check(waitFor([&] {
        device->block();
        return model.slice(3) && !model.slice(0) && rtl::RtlCaptureBackendTestAccess::idle(backend);
    }), "capacity-one restore materializes stable ID 3 instead of zero");
    QPointer<SliceModel> slice = model.slice(3);
    if (!slice) { backend.disconnectRadio(); return 1; }
    int observed = 0, commands = 0;
    QVector<double> frequencies;
    QObject::connect(slice, &SliceModel::frequencyChanged, &model, [&](double mhz) { ++observed; frequencies.append(mhz); });
    QObject::connect(slice, &SliceModel::modeChanged, &model, [&] { ++observed; });
    QObject::connect(slice, &SliceModel::filterChanged, &model, [&] { ++observed; });
    QObject::connect(slice, &SliceModel::audioGainChanged, &model, [&] { ++observed; });
    QObject::connect(slice, &SliceModel::audioMuteChanged, &model, [&] { ++observed; });
    QObject::connect(slice, &SliceModel::audioPanChanged, &model, [&] { ++observed; });
    QObject::connect(slice, &SliceModel::commandReady, &model, [&] { ++commands; });
    RadioModelSliceLifecycleTestAccess::flush(model);
    const auto initial = scope.featureExact("RtlSlices");
    const int before = observed;
    std::thread foreign([&] {
        slice->setFrequency(110.0); slice->setMode("AM");
        slice->setFilterWidth(-6000, 6000); slice->setAudioGain(88);
        slice->setAudioPan(88); slice->setAudioMute(true);
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
    check(slice->frequency() == 99.9 && slice->reportedFrequency() == 99.9
        && slice->filterLow() == -8000 && slice->filterHigh() == 8000
        && slice->audioGain() == 22 && slice->audioPan() == 11 && !slice->audioMute(),
        "pending sparse receiver edits do not change observed getters");
    check(observed == before && commands == 0, "pending edits emit neither observations nor Flex wire text");
    RadioModelSliceLifecycleTestAccess::flush(model);
    check(scope.featureExact("RtlSlices") == initial, "forced pending flush preserves accepted document");
    check(waitFor([&] {
        device->block();
        return rtl::RtlCaptureBackendTestAccess::idle(backend) && slice->frequency() == 100.1
            && slice->filterLow() == -7000 && slice->audioGain() == 37 && slice->audioMute()
            && slice->audioPan() == 73;
    }), "callback adoption publishes all sparse-ID tuning/filter/audio edits");
    RadioModelSliceLifecycleTestAccess::flush(model);
    check(RtlSliceSettings(scope).load().document.slices[3].audioPan == 73,
          "accepted sparse receiver audio reaches persistence");
    const auto accepted = scope.featureExact("RtlSlices");
    const int refusedBefore = observed;
    slice->setFilterWidth(9000, -9000); slice->setMode("not-a-mode");
    check(observed == refusedBefore && slice->mode() == "FM" && slice->filterLow() == -7000,
          "synchronous refusal never changes observed state");
    RadioModelSliceLifecycleTestAccess::flush(model);
    check(scope.featureExact("RtlSlices") == accepted, "refused edits never persist");
    // A return to the already observed value must still supersede pending intent.
    slice->setFrequency(100.2); slice->setFrequency(100.1);
    check(waitFor([&] { device->block(); return rtl::RtlCaptureBackendTestAccess::idle(backend); }),
          "observed-value request cancels a pending different value");
    check(slice->frequency() == 100.1 && !frequencies.contains(100.2),
          "superseded receiver-only value never becomes observable");
    const int modeBefore = observed;
    slice->setMode("FMN");
    check(slice->mode() == "FM" && observed == modeBefore, "mode waits for adopted DSP");
    check(waitFor([&] { device->block(); return slice->mode() == "FMN" && rtl::RtlCaptureBackendTestAccess::idle(backend); }),
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
    check(waitFor([&] { device->block(); return rtl::RtlCaptureBackendTestAccess::idle(backend) && slice->frequency() == 107.0; }),
          "latest hardware state publishes after compensation");
    check(!frequencies.contains(106.0), "superseded hardware state never publishes");
    // Pan requests share the capture transaction and must not move the view
    // or tell a caller to move it before the new capture is adopted.
    PanadapterModel* pan = model.panadapter(slice->panId());
    check(pan != nullptr, "accepted receiver resolves its model pan");
    if (pan) {
        const double oldCenter = pan->centerMhz();
        {
            std::lock_guard lock(device->mutex); device->holdReadback = true;
        }
        check(!model.requestPanCenter(pan->panId(), 108.0), "pending pan request does not report convergence");
        check(pan->centerMhz() == oldCenter && slice->frequency() == 107.0,
              "pending pan capture leaves both models at accepted geometry");
        check(waitFor([&] { std::lock_guard lock(device->mutex); return device->inReadback; }),
              "pan request reaches held hardware readback");
        // Return to the observed geometry; the abandoned center must not leak.
        model.requestPanCenter(pan->panId(), oldCenter);
        device->releaseReadback();
        check(waitFor([&] { device->block(); return rtl::RtlCaptureBackendTestAccess::idle(backend); }),
              "corrected pan request settles after compensation");
        check(pan->centerMhz() == oldCenter && slice->frequency() == oldCenter
            && !frequencies.contains(108.0), "superseded pan geometry never publishes");
        // The existing single-receiver pan operation tunes to that center.
        // Restore the prior offset tune before testing model staging below.
        slice->setFrequency(107.0);
        check(waitFor([&] { device->block(); return slice->frequency() == 107.0
            && rtl::RtlCaptureBackendTestAccess::idle(backend); }), "offset tune restored after pan correction");
    }
    // A staged object cannot control an otherwise-connected backend.
    RadioModelSliceLifecycleTestAccess::stage(model);
    const int stagedWrites = device->writes;
    slice->setFrequency(108.0); slice->setAudioGain(90);
    check(rtl::RtlCaptureBackendTestAccess::idle(backend) && device->writes == stagedWrites
        && slice->frequency() == 107.0 && slice->audioGain() == 37,
        "staged slice cannot mutate accepted state or dispatch controls");
    // A fresh accepted report reclaims the existing object and its one binding.
    int reentrantEdits = 0;
    const auto reentrant = QObject::connect(slice, &SliceModel::frequencyChanged, &model, [&](double value) {
        if (value == 107.1) { ++reentrantEdits; slice->setAudioGain(41); }
    });
    backend.setSliceFrequency(3, 107'100'000);
    check(waitFor([&] { device->block(); return model.slice(3) == slice && slice->frequency() == 107.1
        && slice->audioGain() == 41 && rtl::RtlCaptureBackendTestAccess::idle(backend); }),
          "accepted state reclaims the staged object and adopts a reentrant edit");
    check(reentrantEdits == 1, "reclaimed binding publishes the accepted tune once");
    QObject::disconnect(reentrant);
    RadioModelSliceLifecycleTestAccess::flush(model);
    const auto beforeDisconnect = scope.featureExact("RtlSlices");
    slice->setFrequency(107.2); slice->setAudioGain(80);
    backend.disconnectRadio();
    check(!frequencies.contains(107.2) && scope.featureExact("RtlSlices") == beforeDisconnect,
          "disconnect discards pending receiver edits and flushes only accepted state");
    const int disconnectedBefore = observed;
    if (slice) {
        slice->setFrequency(109.0); slice->setMode("AM"); slice->setAudioMute(false);
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
        check(waitFor([&] { usb->block(); return multi.slice(1) && rtl::RtlCaptureBackendTestAccess::idle(radio); }),
              "pending receiver adopted");
        check(multi.slice(1) && multi.slice(1)->frequency() == 100.3 && multi.slice(1)->filterLow() == -8000
            && multi.slice(1)->audioGain() == 100 && multi.slice(1)->audioPan() == 50 && !multi.slice(1)->audioMute(),
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
        if (retired) {
            retired->setFrequency(100.7); retired->setFilterWidth(-4000, 4000); retired->setAudioGain(9);
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
        // Retire the other receiver so the out-of-window tune is admissible.
        check(radio.removeSlice(0), "remove sibling before hardware failure test");
        check(waitFor([&] { usb->block(); return !multi.slice(0) && rtl::RtlCaptureBackendTestAccess::idle(radio); }),
              "one receiver remains for hardware failure");
        usb->badReadback = true;
        survivor->setFrequency(105.0);
        check(waitFor([&] { return !radio.isConnected(); }), "invalid readback and rollback disconnect the model session");
        check(wrong == 0 && survivor && survivor->frequency() == 100.3,
              "failed apply and failed rollback never publish requested frequency");
    }
    std::fprintf(stderr, "rtl_model_acceptance_test: %d failures\n", failures);
    return failures ? 1 : 0;
}
