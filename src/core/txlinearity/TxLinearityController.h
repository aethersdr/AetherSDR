#pragma once

#include "ITxCaptureSource.h"
#include "ImdAnalyzer.h"
#include "TxLinearitySafety.h"

#include <QObject>
#include <QTimer>

#include <complex>
#include <functional>
#include <memory>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace AetherSDR::txlin {

// An adapter to an explicitly authorized, original-session TX operation.
// start must arm an independent, non-renewable cutoff BEFORE dispatching via
// the existing TX gate. Destruction must release that operation. The development
// AetherPDRunLease composes this path; its hardware cutoff remains unqualified.
class ITxLinearityRunLease {
public:
    enum class State { AwaitingKey, Keyed, Released, Failed };
    virtual ~ITxLinearityRunLease() = default;
    virtual bool start(std::int64_t absoluteDeadlineMs) = 0;
    virtual void release() = 0;
    // Observe this operation; expiry may withdraw authority but never key. Released
    // requires confirmed release; enqueueing an unkey request is insufficient.
    [[nodiscard]] virtual State state() const = 0;
};

struct LinearityRunConfig {
    CaptureConfig capture;
    ImdConfig imd;
    WindowKind window = WindowKind::BlackmanHarris7;
    int targetAverages = 8;
    double maxForwardWatts = 0.0; // Required operator-selected limit.
    std::optional<bool> expectedCorrection;
};

struct LinearityCaptureStamp {
    std::string id;
    std::uint64_t timestampNs = 0;
    // Original bounded feedback, retained only after confirmed TX release.
    std::vector<std::complex<float>> feedback;
    std::optional<CaptureStreamIdentity> sourceStream;
};

struct LinearityRunResult {
    // No calibrated full-scale evidence is supplied by the raw capture path.
    // Absolute spectral levels use raw IQ units; dBc ratios remain meaningful.
    bool fullScaleKnown = false;
    std::optional<LinearityResult> measurement;
    std::vector<LinearityCaptureStamp> captures;
    std::optional<bool> correctionActive;
    std::string error;
};

// Owner-thread controller. Readiness never keys: startRun is a separate,
// explicit operator action. Capture memory is bounded, and spectral analysis
// runs only after confirmed release so FFT work cannot extend the TX interval.
class TxLinearityController final : public QObject {
public:
    static constexpr int kMaxTransmitMs = 10000;
    enum class State { Idle, CheckingReceive, Ready, AwaitingKey, Capturing, Releasing, Analyzing };
    struct Environment {
        std::function<std::int64_t()> nowMs;
        std::function<bool()> sessionCurrent;
        std::function<TxTelemetry(std::int64_t)> telemetry;
    };
    using ReadyCallback = std::function<void()>;
    using FinishedCallback = std::function<void(const LinearityRunResult&)>;

    TxLinearityController(std::unique_ptr<ITxCaptureSource> source, Environment environment,
                          QObject* parent = nullptr);
    ~TxLinearityController() override;
    bool setCallbacks(ReadyCallback ready, FinishedCallback finished);
    bool setSafetyLimits(const SafetyLimits& limits, bool allowOverride = false);
    void confirmDummyLoad();
    bool prepare(const LinearityRunConfig& config, const RunRequest& request);
    bool startRun(std::shared_ptr<ITxLinearityRunLease> lease);
    void stop();
    void poll(); // Also called by the owner-thread timer and on telemetry updates.
    [[nodiscard]] State state() const { return m_state; }
    [[nodiscard]] const std::string& status() const { return m_status; }
    [[nodiscard]] const PreflightResult& preflightResult() const { return m_preflight; }

private:
    bool current() const;
    bool validConfig(const LinearityRunConfig& config, const RunRequest& request) const;
    void receiveReady(std::uint64_t generation);
    void receiveFrame(const CaptureFrame& frame, std::uint64_t generation);
    void finishTelemetryCapture(const std::string& error, std::int64_t now, const TxTelemetry& telemetry);
    void finishCapture(const std::string& error);
    void complete();

    std::unique_ptr<ITxCaptureSource> m_source;
    Environment m_environment;
    QTimer m_timer;
    TxLinearitySafety m_safety;
    LinearityRunConfig m_config;
    RunRequest m_request;
    PreflightResult m_preflight;
    ReadyCallback m_readyCallback;
    FinishedCallback m_finishedCallback;
    std::shared_ptr<ITxLinearityRunLease> m_lease;
    std::vector<CaptureFrame> m_frames;
    State m_state = State::Idle;
    std::string m_status;
    std::uint64_t m_generation = 0;
    std::int64_t m_prepareMs = 0;
    std::int64_t m_readyMs = 0;
    std::int64_t m_deadlineMs = 0;
    std::optional<std::int64_t> m_keyObservedMs;
    bool m_gateOpen = false;
    bool m_shuttingDown = false;
    bool m_finishingCapture = false;
};
} // namespace AetherSDR::txlin
