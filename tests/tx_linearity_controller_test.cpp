#include "core/txlinearity/TxLinearityController.h"
#include "TxLinearityTestSignals.h"

#include <QCoreApplication>

#include <cmath>
#include <complex>
#include <cstdint>
#include <functional>
#include <iostream>
#include <limits>
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>

using namespace AetherSDR::txlin;
namespace {
int failures = 0;
void check(bool value, const char* message)
{
    if (!value) { std::cerr << "FAIL: " << message << '\n'; ++failures; }
}
struct SourceState {
    ITxCaptureSource::FrameCallback frame;
    ITxCaptureSource::ErrorCallback error;
    ITxCaptureSource::ReadyCallback ready;
    bool gate = false;
    bool running = false;
    int stops = 0;
};
class Source final : public ITxCaptureSource {
public:
    explicit Source(std::shared_ptr<SourceState> data) : m_data(std::move(data)) {}
    bool start(const CaptureConfig&) override { m_data->running = true; return true; }
    void stop() override { m_data->running = m_data->gate = false; ++m_data->stops; }
    bool checkReceiveReady(ReadyCallback ready) override { m_data->ready = std::move(ready); return true; }
    bool hasReferenceChannel() const override { return false; }
    std::string describe() const override { return "injected source"; }
    void setFrameCallback(FrameCallback callback) override { m_data->frame = std::move(callback); }
    void setErrorCallback(ErrorCallback callback) override { m_data->error = std::move(callback); }
    void setGateOpen(bool open) override { m_data->gate = open; }
private:
    std::shared_ptr<SourceState> m_data;
};
class Lease final : public ITxLinearityRunLease {
public:
    bool start(std::int64_t limit) override
    {
        ++starts; deadline = limit;
        if (onStart) { onStart(); }
        return accept;
    }
    void release() override
    {
        ++releases;
        if (releaseImmediately) { observed = State::Released; }
        if (onRelease) { onRelease(); }
    }
    State state() const override { return observed; }
    State observed = State::AwaitingKey;
    bool accept = true;
    bool releaseImmediately = false;
    int starts = 0;
    int releases = 0;
    std::int64_t deadline = 0;
    std::function<void()> onStart;
    std::function<void()> onRelease;
};
CaptureFrame frame(int index)
{
    const std::vector<std::complex<double>> raw = txlin_test::twoTone(16384, 192000, 750, 1875, .2);
    CaptureFrame result;
    result.feedback = txlin_test::toFloat(raw, 0, raw.size());
    result.sampleRateHz = 192000;
    result.centerFreqHz = 7150000;
    result.timestampNs = 1000000000ULL + std::uint64_t(index) * 100000000;
    result.sourceCaptureId = std::to_string(index);
    result.correctionActive = true;
    result.clippingKnown = false;
    return result;
}
struct Fixture {
    std::int64_t now = 1000;
    bool current = true;
    bool stale = false;
    double watts = 0;
    int ready = 0;
    int finished = 0;
    std::optional<LinearityRunResult> result;
    std::shared_ptr<SourceState> source = std::make_shared<SourceState>();
    std::shared_ptr<Lease> lease = std::make_shared<Lease>();
    std::unique_ptr<TxLinearityController> controller;
    LinearityRunConfig config;
    RunRequest request;
    Fixture()
    {
        TxLinearityController::Environment environment;
        environment.nowMs = [&] { return now; };
        environment.sessionCurrent = [&] { return current; };
        environment.telemetry = [&](std::int64_t at) {
            const std::int64_t stamp = stale ? 0 : at;
            TxTelemetry telemetry;
            telemetry.paTempC = {40.0, stamp}; telemetry.supplyVolts = {13.8, stamp};
            telemetry.forwardWatts = {watts, stamp}; telemetry.swr = {1.08, stamp};
            return telemetry;
        };
        controller = std::make_unique<TxLinearityController>(std::make_unique<Source>(source), environment);
        controller->setCallbacks([&] { ++ready; }, [&](const LinearityRunResult& value) { ++finished; result = value; });
        controller->confirmDummyLoad();
        config.capture.centerFreqHz = 7150000;
        config.targetAverages = 2;
        config.maxForwardWatts = 10;
        config.expectedCorrection = true;
        request.tone1Hz = 7150750; request.tone2Hz = 7151875;
        request.allocations = {{7000000, 7300000}};
    }
    void prepare()
    {
        check(controller->prepare(config, request), "prepare refused");
        check(lease->starts == 0 && ready == 0, "prepare keyed or reported premature readiness");
        const auto callback = source->ready; callback();
        check(ready == 1 && lease->starts == 0 && !source->gate, "receive readiness keyed or opened measurement gate");
    }
    void start()
    {
        prepare();
        check(controller->startRun(lease), "explicit start refused");
        check(!source->gate, "request acceptance opened capture before key confirmation");
        now += 20; watts = 5; lease->observed = Lease::State::Keyed; controller->poll();
        check(!source->gate, "capture opened without settling from observed key");
        now += 50; controller->poll();
        check(source->gate, "settled keyed capture gate did not open");
    }
    void send(CaptureFrame value)
    {
        const auto callback = source->frame; callback(value);
    }
};
}

int main(int argc, char** argv)
{
    QCoreApplication app(argc, argv);
    {
        Fixture f; f.start();
        check(f.lease->deadline == 11000 && f.lease->starts == 1, "lease deadline did not cover original start");
        f.send(frame(1)); f.send(frame(2));
        check(f.lease->releases == 1 && !f.source->running && f.finished == 0
            && f.controller->state() == TxLinearityController::State::Releasing,
            "result published before confirmed release or capture left running");
        f.lease->observed = Lease::State::Released; f.controller->poll();
        check(f.finished == 1 && f.result && f.result->error.empty() && f.result->measurement
            && f.result->measurement->valid && !f.result->measurement->clippingKnown
            && !f.result->fullScaleKnown
            && f.result->measurement->averages == 2 && f.result->captures.size() == 2
            && f.result->correctionActive == true && f.result->measurement->sampledPepDb,
            "completed result lost averages, provenance, overload uncertainty or PEP");
        f.controller->confirmDummyLoad();
        check(!f.controller->prepare(f.config, f.request), "cooldown bypassed on repeat run");
    }
    for (const int fault : {0, 1, 2, 3}) {
        Fixture f; f.prepare();
        if (fault == 0) { f.now += 2001; }
        if (fault == 1) { f.stale = true; f.now = 2500; }
        if (fault == 2) { f.watts = 5; }
        if (fault == 3) { f.current = false; }
        check(!f.controller->startRun(f.lease) && f.lease->starts == 0, "invalid readiness/telemetry/session keyed");
        f.controller->poll();
        check(f.finished == 1 && f.result && !f.result->measurement, "pre-key refusal did not terminate preparation");
    }
    for (const int fault : {0, 1, 2, 3, 4, 5, 6, 7}) {
        Fixture f; f.start();
        if (fault == 0) { f.now = f.lease->deadline; }
        if (fault == 1) { f.now = 4000; f.stale = true; }
        if (fault == 2) { f.current = false; }
        if (fault == 3) { f.watts = 10.01; }
        if (fault == 4) { f.lease->observed = Lease::State::Failed; }
        if (fault == 5) { f.lease->observed = Lease::State::AwaitingKey; }
        if (fault == 6) { f.now = 999; }
        if (fault == 7) { f.now = f.lease->deadline - f.config.capture.settleMsBeforeUnkey; }
        f.controller->poll();
        check(f.lease->releases == 1 && !f.source->gate && f.finished == 0, "runtime fault failed to request release");
        f.send(frame(1));
        f.lease->observed = Lease::State::Released; f.controller->poll();
        check(f.finished == 1 && f.result && !f.result->measurement && !f.result->error.empty(), "runtime fault produced measurement");
    }
    for (const int fault : {0, 1, 2, 3, 4, 5, 6}) {
        Fixture f; f.start(); f.send(frame(1));
        CaptureFrame bad = frame(2);
        if (fault == 0) { bad.correctionActive = false; }
        if (fault == 1) { bad.sampleRateHz = 96000; }
        if (fault == 2) { bad.sourceCaptureId = "1"; }
        if (fault == 3) { bad.timestampNs = frame(1).timestampNs; }
        if (fault == 4) { bad.centerFreqHz = std::numeric_limits<double>::quiet_NaN(); }
        if (fault == 5) { bad.feedback[0] = {std::numeric_limits<float>::infinity(), 0}; }
        if (fault == 6) { bad.feedback.pop_back(); }
        f.send(std::move(bad));
        check(f.lease->releases == 1 && f.finished == 0, "invalid frame failed to end capture");
        f.lease->observed = Lease::State::Released; f.controller->poll();
        check(f.finished == 1 && f.result && !f.result->measurement && !f.result->error.empty(), "mixed or invalid frame reached result");
    }
    {
        Fixture f; f.prepare();
        f.lease->accept = false; f.lease->releaseImmediately = true;
        check(!f.controller->startRun(f.lease), "refused lease start accepted");
        check(f.lease->releases == 1 && f.finished == 1 && !f.result->measurement, "refused start did not release partial operation");
    }
    {
        Fixture f; f.prepare();
        f.lease->onStart = [&] { f.controller->stop(); };
        check(!f.controller->startRun(f.lease) && f.lease->releases == 1 && !f.source->gate,
              "reentrant stop during keying revived operation");
    }
    {
        Fixture f; f.start();
        f.lease->releaseImmediately = true;
        f.lease->onRelease = [&] { f.controller->poll(); };
        f.send(frame(1)); f.send(frame(2));
        check(f.finished == 1 && !f.source->running, "reentrant release published before capture stopped or duplicated completion");
    }
    {
        Fixture f; f.start();
        const auto lateFrame = f.source->frame;
        const auto lateError = f.source->error;
        f.controller.reset();
        check(f.lease->releases == 1 && !f.source->running, "destruction left transmission/capture active");
        lateFrame(frame(1)); lateError("late failure");
        check(f.finished == 0, "destroyed controller delivered a result");
    }
    {
        Fixture f; f.start();
        const auto oldFrame = f.source->frame;
        const auto oldReady = f.source->ready;
        f.lease->releaseImmediately = true;
        f.controller->stop();
        f.now += 30000; f.controller->confirmDummyLoad();
        check(f.controller->prepare(f.config, f.request), "fresh prepare after cooldown refused");
        oldReady(); oldFrame(frame(1));
        check(f.ready == 1 && f.finished == 1 && f.controller->state() == TxLinearityController::State::CheckingReceive,
              "old generation affected new preparation");
        f.controller->stop(); f.controller->confirmDummyLoad();
        check(f.controller->prepare(f.config, f.request), "receive-only cancellation incorrectly renewed TX cooldown");
    }
    {
        Fixture f;
        f.controller->setCallbacks([&] { ++f.ready; f.controller.reset(); }, [&](const LinearityRunResult&) { ++f.finished; });
        check(f.controller->prepare(f.config, f.request), "readiness destruction prepare failed");
        const auto callback = f.source->ready; callback();
        check(!f.controller && f.ready == 1 && !f.source->running, "readiness observer destruction failed");
    }
    {
        Fixture f;
        f.controller->setCallbacks([&] { ++f.ready; }, [&](const LinearityRunResult&) { ++f.finished; f.controller.reset(); });
        f.start(); f.lease->releaseImmediately = true;
        f.send(frame(1)); f.send(frame(2));
        check(!f.controller && f.finished == 1 && !f.source->running, "result observer destruction failed");
    }
    {
        Fixture f; f.start();
        const auto error = f.source->error; error("");
        check(f.lease->releases == 1 && f.finished == 0, "empty source failure did not release");
        f.lease->observed = Lease::State::Released; f.controller->poll();
        check(f.result && !f.result->error.empty() && !f.result->measurement, "empty source failure became a success");
    }
    {
        Fixture f; f.request.tone1Hz += 200; f.start();
        f.lease->releaseImmediately = true;
        f.send(frame(1)); f.send(frame(2));
        check(f.result && f.result->measurement && !f.result->measurement->valid,
              "unrelated tones reported as the authorized measurement");
    }
    {
        Fixture f; f.start();
        f.lease->releaseImmediately = true;
        CaptureFrame first = frame(1); first.clipped = true; first.clippingKnown = true;
        f.send(std::move(first)); f.send(frame(2));
        check(f.result && f.result->measurement && f.result->measurement->clipped
              && !f.result->measurement->clippingKnown, "averaging lost overload or overstated overload knowledge");
    }
    for (const int fault : {0, 1, 2, 3, 4, 5}) {
        Fixture f;
        if (fault == 0) { f.config.targetAverages = 33; }
        if (fault == 1) { f.config.capture.frameSamples = std::numeric_limits<int>::max(); }
        if (fault == 2) { f.config.capture.settleMsAfterKey = -1; }
        if (fault == 3) { f.config.maxForwardWatts = std::numeric_limits<double>::quiet_NaN(); }
        if (fault == 4) { f.config.window = static_cast<WindowKind>(99); }
        if (fault == 5) { f.config.imd.dcNotchBins = std::numeric_limits<int>::max(); }
        check(!f.controller->prepare(f.config, f.request) && !f.source->running && f.lease->starts == 0,
              "unbounded or invalid configuration accepted");
    }
    {
        Fixture f;
        check(f.controller->prepare(f.config, f.request), "timeout preparation refused");
        f.now += 6000; f.controller->poll();
        const auto callback = f.source->ready; callback();
        check(f.ready == 0 && f.finished == 1 && f.lease->starts == 0, "late receive readiness escaped timeout");
    }
    return failures == 0 ? 0 : 1;
}
