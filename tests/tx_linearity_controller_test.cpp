#include "core/txlinearity/TxLinearityController.h"
#include "TxLinearityTestSignals.h"

#include <QCoreApplication>
#include <QJsonDocument>
#include <QJsonObject>
#include <QString>

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
std::vector<QJsonObject> abortEvidence;
std::function<bool()> releaseObserved;
bool evidenceAfterRelease = true;
QtMessageHandler previousHandler = nullptr;
void captureDiagnostic(QtMsgType type, const QMessageLogContext& context, const QString& message)
{
    const QString prefix = QStringLiteral("TX linearity safety abort ");
    if (message.startsWith(prefix)) {
        evidenceAfterRelease &= !releaseObserved || releaseObserved();
        abortEvidence.push_back(QJsonDocument::fromJson(message.sliced(prefix.size()).toUtf8()).object());
    } else if (previousHandler) {
        previousHandler(type, context, message);
    }
}
void check(bool value, const char* message)
{
    if (!value) { std::cerr << "FAIL: " << message << '\n'; ++failures; }
}
struct SourceState {
    ITxCaptureSource::FrameCallback frame;
    ITxCaptureSource::ErrorCallback error;
    ITxCaptureSource::ReadyCallback ready;
    ITxCaptureSource::AcquisitionCallback acquired;
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
    void setAcquisitionCallback(AcquisitionCallback callback) override { m_data->acquired = std::move(callback); }
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
    std::optional<TxTelemetry> observedTelemetry;
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
            if (observedTelemetry) { return *observedTelemetry; }
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
    previousHandler = qInstallMessageHandler(captureDiagnostic);
    for (int fault = 0; fault < 6; ++fault) {
        Fixture f;
        f.start();
        f.source->acquired(fault == 1 ? 1 : 2);
        check(f.lease->releases == 1 && f.finished == 0, "batch receipt did not request release before download");
        if (fault == 1) {
            f.lease->observed = Lease::State::Released; f.controller->poll();
            check(f.result && !f.result->error.empty() && !f.result->measurement, "wrong sealed count accepted");
            continue;
        }
        check(f.source->running, "sealed batch download stopped before delivery");
        f.lease->observed = Lease::State::Released;
        f.controller->poll();
        check(f.finished == 0, "confirmed unkey published a partial batch");
        if (fault == 2) { f.controller->stop(); }
        if (fault == 3) { f.current = false; f.controller->poll(); }
        if (fault == 4) { f.now += 6000; f.controller->poll(); }
        if (fault == 5) { const auto error = f.source->error; error("download failed"); }
        f.send(frame(0)); f.send(frame(1));
        check(f.lease->starts == 1 && f.lease->releases == 1 && f.finished == 1 && !f.source->running,
            "batch completion duplicated key/release or kept source alive");
        check(f.result && (f.result->measurement.has_value() == (fault == 0)), "sealed batch cancellation or completion was misreported");
    }
    {
        Fixture f; f.start(); f.source->acquired(2);
        f.send(frame(0)); f.send(frame(1));
        check(f.finished == 0 && !f.source->running && f.lease->releases == 1,
            "complete download analyzed before confirmed release");
        f.lease->observed = Lease::State::Released; f.controller->poll();
        check(f.result && f.result->measurement && f.finished == 1, "released batch failed analysis");
    }
    {
        Fixture f;
        SafetyLimits limits;
        limits.maxTransmitMs = 10001;
        check(!f.controller->setSafetyLimits(limits, true),
              "even telemetry override cannot extend analyzer transmit ceiling");
        limits.maxTransmitMs = 8000;
        check(f.controller->setSafetyLimits(limits), "shorter analyzer deadline refused");
        f.start();
        check(f.lease->deadline == 9000, "shorter deadline did not reach lease");
        f.now = 9000;
        f.controller->poll();
        check(f.lease->releases == 1 && !f.source->gate && f.finished == 0,
              "deadline did not withdraw capture and request release before analysis");
        f.lease->observed = Lease::State::Released; f.controller->poll();
        check(f.finished == 1 && f.result && !f.result->measurement,
              "deadline expiry fabricated a measurement or skipped qualified release");
    }
    for (int timeout : {3000, 8000, 10000}) {
        Fixture f; f.config.timeoutMs = timeout; f.start();
        check(f.lease->deadline == 1000 + timeout, "selected timeout was not snapped into lease deadline");
        f.config.timeoutMs = 10000;
        f.now = 1000 + timeout;
        f.controller->poll();
        check(f.lease->releases == 1 && f.finished == 0, "selected expiry did not request release independently of caller changes");
        f.lease->observed = Lease::State::Released; f.controller->poll();
        check(f.result && f.result->error == "Transmit timeout reached", "selected expiry lost timeout reason");
    }
    for (int timeout : {0, -1, 10001, 14000}) {
        Fixture f; f.config.timeoutMs = timeout;
        check(!f.controller->prepare(f.config, f.request) && !f.source->running && f.lease->starts == 0,
              "invalid requested timeout passed preflight");
    }
    for (const int variant : {0, 1, 2, 3, 4, 5, 6}) {
        Fixture f; f.start();
        CaptureFrame first = frame(1), second = frame(2);
        if (variant != 5) {
            first.sourceStream = second.sourceStream = CaptureStreamIdentity{1073741825, 7};
        }
        if (variant == 1) { second.sourceStream->streamId += 1; }
        if (variant == 2) { second.sourceStream->generation += 1; }
        if (variant == 3) { second.sourceStream.reset(); }
        if (variant == 4) { second.sourceStream->generation = 0; }
        if (variant == 6) { first.sourceStream.reset(); }
        f.send(first); f.send(second);
        check(f.lease->releases == 1 && f.finished == 0, "identity check bypassed release confirmation");
        f.lease->observed = Lease::State::Released; f.controller->poll();
        if (variant == 0 || variant == 5) {
            check(f.result && f.result->measurement && f.result->captures.size() == 2
                && f.result->captures.front().sourceStream == first.sourceStream,
                "completed measurement lost source identity or legacy uncertainty");
        } else {
            check(f.result && !f.result->error.empty() && !f.result->measurement,
                "mixed or invalid acquisition identity survived averaging");
        }
    }
    {
        Fixture f; f.start();
        abortEvidence.clear(); evidenceAfterRelease = true;
        const std::int64_t sampledAt = f.now - 17;
        f.observedTelemetry = TxTelemetry{{2.58, sampledAt}, {40.0, f.now - 31},
                                         {13.8, f.now - 42}, {5.0, f.now - 9}};
        releaseObserved = [&] { return f.lease->releases == 1 && !f.source->running; };
        f.lease->onRelease = [&] {
            f.observedTelemetry->swr = {1.08, f.now};
            f.now += 500;
        };
        f.controller->poll();
        check(f.lease->releases == 1 && !f.source->running && f.finished == 0,
              "SWR spike did not release before diagnostic completion");
        check(evidenceAfterRelease && abortEvidence.size() == 1,
              "SWR abort did not emit exactly one diagnostic after requesting release and stopping capture");
        if (abortEvidence.size() == 1) {
            const QJsonObject record = abortEvidence.front();
            const QJsonObject swr = record.value("swr").toObject();
            check(record.value("reason").toString() == QStringLiteral("SWR above the configured limit")
                && record.value("phase").toString() == QStringLiteral("keyed")
                && record.value("now_ms").toInteger() == sampledAt + 17
                && swr.value("value").toDouble() == 2.58
                && swr.value("observed_at_ms").toInteger() == sampledAt
                && swr.value("age_ms").toInteger() == 17
                && record.value("limits").toObject().value("max_swr").toDouble() == 2.0
                && record.value("forward_watts").toObject().value("value").toDouble() == 5.0,
                "abort evidence resampled the spike or lost its threshold and original timestamp");
        }
        f.controller->poll();
        check(abortEvidence.size() == 1, "pending release repeated abort evidence");
        f.lease->observed = Lease::State::Released; f.controller->poll();
        check(f.finished == 1 && f.result && !f.result->measurement,
              "SWR diagnostic altered refusal or release semantics");
        releaseObserved = {};
        f.observedTelemetry.reset(); f.now += 30000; f.controller->confirmDummyLoad();
        check(f.controller->prepare(f.config, f.request), "diagnostic prevented subsequent receive preparation");
        check(abortEvidence.size() == 1, "new preparation reused old abort evidence");
    }
    for (const int fault : {0, 1, 2, 3}) {
        Fixture f; f.start(); abortEvidence.clear();
        f.observedTelemetry = TxTelemetry{{1.08, f.now}, {40.0, f.now}, {13.8, f.now}, {5.0, f.now}};
        if (fault == 0) { f.observedTelemetry->swr = {}; }
        if (fault == 1) { f.observedTelemetry->swr.value = std::numeric_limits<double>::quiet_NaN(); }
        if (fault == 2) { f.observedTelemetry->swr.observedAtMs = f.now + 1; }
        if (fault == 3) { f.observedTelemetry->forwardWatts.value = 10.01; }
        f.controller->poll();
        check(f.lease->releases == 1 && abortEvidence.size() == 1, "invalid telemetry lost abort evidence");
        if (abortEvidence.size() == 1) {
            const QJsonObject swr = abortEvidence.front().value("swr").toObject();
            if (fault == 0) {
                check(!swr.value("present").toBool() && swr.value("value").isNull()
                    && swr.value("observed_at_ms").isNull() && swr.value("age_ms").isNull(),
                    "missing telemetry became a zero or fresh reading in the diagnostic");
            }
            if (fault == 1) {
                check(swr.value("present").toBool() && !swr.value("finite").toBool()
                    && swr.value("value").isNull(), "nonfinite telemetry was serialized as valid");
            }
            if (fault == 2) {
                check(swr.value("observed_at_ms").toInteger() == f.now + 1 && swr.value("age_ms").isNull(),
                    "future telemetry timestamp was silently clamped to fresh");
            }
            if (fault == 3) {
                check(abortEvidence.front().value("reason").toString() == QStringLiteral("Forward power exceeded the test limit")
                    && abortEvidence.front().value("limits").toObject().value("max_forward_watts").toDouble() == 10.0,
                    "selected power-limit abort lost its exact limit");
            }
        }
    }
    {
        Fixture f; f.prepare(); abortEvidence.clear();
        f.observedTelemetry = TxTelemetry{{1.08, f.now}, {40.0, f.now}, {10.8, f.now - 29}, {0.0, f.now}};
        check(!f.controller->startRun(f.lease) && f.lease->starts == 0 && f.finished == 1,
              "pre-key diagnostic changed voltage refusal");
        check(abortEvidence.size() == 1 && abortEvidence.front().value("phase").toString() == QStringLiteral("pre_key")
            && abortEvidence.front().value("supply_volts").toObject().value("age_ms").toInteger() == 29,
            "pre-key refusal lost the sampled voltage age");
    }
    {
        Fixture f; abortEvidence.clear();
        f.controller->setCallbacks([&] { ++f.ready; }, [&](const LinearityRunResult&) {
            ++f.finished; f.controller.reset();
        });
        f.start(); f.lease->releaseImmediately = true; f.watts = 10.01;
        f.controller->poll();
        check(!f.controller && f.finished == 1 && abortEvidence.size() == 1,
              "controller destruction during release lost abort evidence");
    }
    {
        Fixture f; f.start(); abortEvidence.clear();
        check(f.lease->deadline == 11000 && f.lease->starts == 1, "lease deadline did not cover original start");
        const CaptureFrame first = frame(1);
        CaptureFrame second = frame(2);
        for (std::complex<float>& sample : second.feedback) { sample *= 0.9f; }
        f.send(first); f.send(second);
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
        check(abortEvidence.empty(), "successful measurement reported a safety abort");
        check(f.result && f.result->captures.size() == 2
            && f.result->captures[0].feedback == first.feedback
            && f.result->captures[1].feedback == second.feedback,
            "completed result discarded or changed original IQ samples");
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
        Fixture f; f.request.tone1Hz += 100; f.start();
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
    for (const int order : {std::numeric_limits<int>::min(), -1, 0, 8, std::numeric_limits<int>::max()}) {
        Fixture f; f.config.imd.maxOrder = f.request.maxOrder = order;
        check(!TxLinearityController::spectralConfigurationError(f.config, f.request).empty()
            && !f.controller->prepare(f.config, f.request) && f.lease->starts == 0,
            "invalid IMD order reached product-span arithmetic");
    }
    for (const int order : {3, 5, 7}) {
        Fixture f;
        f.config.capture.sampleRateHz = 48000;
        f.config.imd.maxOrder = f.request.maxOrder = order;
        f.request.tone1Hz = 7150000 + 1305 - 5000;
        f.request.tone2Hz = 7150000 + 1305 + 5000;
        const bool accepted = f.controller->prepare(f.config, f.request);
        check(accepted == (order == 3) && f.lease->starts == 0,
            "requested IMD products outside IQ bandwidth admitted or valid order refused");
    }
    {
        Fixture f; f.config.capture.sampleRateHz = 48000;
        f.request.tone1Hz = 7150000; f.request.tone2Hz = 7152610;
        check(!f.controller->prepare(f.config, f.request) && !f.source->running && f.lease->starts == 0,
            "DC-overlapping fundamental entered receive/TX preparation");
    }
    for (const int order : {3, 5, 7}) {
        Fixture f; f.config.capture.sampleRateHz = 48000;
        f.config.imd.maxOrder = f.request.maxOrder = order;
        const double spacing = 2610.0 / order;
        f.request.tone1Hz = 7151305 - spacing / 2;
        f.request.tone2Hz = 7151305 + spacing / 2;
        check(TxLinearityController::spectralConfigurationError(f.config, f.request).find("DC leakage") != std::string::npos
            && !f.controller->prepare(f.config, f.request) && !f.source->running && f.lease->starts == 0,
            "DC leakage could be reported as a requested IMD product");
    }
    for (const int fault : {0, 1, 2, 3}) {
        Fixture f; f.start();
        CaptureFrame first = frame(1);
        first.sourceTones = CaptureToneProfile{31, 750, 1875};
        if (fault == 0) { first.sourceTones->upperOffsetHz = 1900; }
        f.send(first);
        if (fault != 0) {
            CaptureFrame second = frame(2); second.sourceTones = first.sourceTones;
            if (fault == 1) { second.sourceTones.reset(); }
            if (fault == 2) { second.sourceTones->sourceId = 32; }
            f.send(second);
        }
        check(f.lease->releases == 1 && !f.source->gate, "profile validation failed to close the original TX lease");
        f.lease->observed = Lease::State::Released; f.controller->poll();
        if (fault == 3) {
            check(f.result && f.result->measurement && f.result->captures.size() == 2
                && f.result->captures.front().sourceTones == first.sourceTones,
                "confirmed profile missing from retained captures");
        } else {
            check(f.result && !f.result->measurement && !f.result->error.empty(), "invalid profile reached analysis");
        }
    }
    for (int fault = 0; fault < 6; ++fault) {
        Fixture f; f.start();
        CaptureFrame first = frame(1), second = frame(2);
        first.sourceStream = second.sourceStream = CaptureStreamIdentity{1073741825, 7};
        first.sourceBinding = second.sourceBinding = CaptureBindingIdentity{3, 1073741826, 7};
        if (fault == 1) { second.sourceBinding->channel = 2; }
        if (fault == 2) { second.sourceBinding->pan += 1; }
        if (fault == 3) { second.sourceBinding->guiHandle += 1; }
        if (fault == 4) { second.sourceBinding.reset(); }
        if (fault == 5) { first.sourceBinding->channel = 5; }
        f.send(first); if (fault != 5) { f.send(second); }
        check(f.lease->releases == 1 && !f.source->gate, "binding completion did not release original lease");
        f.lease->observed = Lease::State::Released; f.controller->poll();
        if (fault == 0) {
            check(f.result && f.result->measurement && f.result->captures.front().sourceBinding == first.sourceBinding,
                "released result lost snapshot binding");
        } else {
            check(f.result && !f.result->measurement && !f.result->error.empty(), "mixed binding reached averaged result");
        }
    }
    qInstallMessageHandler(previousHandler);
    return failures == 0 ? 0 : 1;
}
