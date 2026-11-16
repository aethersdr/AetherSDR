#pragma once

#include <QByteArray>
#include <QString>
#include <QtGlobal>

#include <optional>

namespace AetherSDR::aetherpd {

// These are the shim's bounded control verbs. None can key the transmitter.
enum class Operation { Status, ArmMeasurement, ArmCalibration, CancelCalibration, Stop, RestoreReceive, Start, EnableCorrection, BypassCorrection };
enum class State {
    Unavailable, Starting, Disabled, BlockedByRadioApd, MeasurementArmed, CalibrationArmed,
    RecalibrationRequired, Idle, Correcting, Calibrating, Stopped, CleanupUnconfirmed,
    StoppedReceiveUnconfirmed, FeedbackBindingPending, FeedbackBindingFailed
};
enum class SourcePurpose { Calibration, Measurement };
enum class CalibrationState { Unavailable, Idle, Completed, Canceled, Expired, Armed, Running };
enum class Acknowledgment { MeasurementArmAccepted, ArmAccepted, CancelAccepted, StopRequested, ReceiveRestored, StartRequested, CorrectionEnabled, CorrectionBypassed };

struct IQSelection {
    quint32 channel{0};
    quint32 pan{0};
    bool valid() const { return channel >= 1 && channel <= 4 && pan != 0; }
    bool operator==(const IQSelection&) const = default;
};

struct StartIdentity {
    QString guiClientId;
    quint32 guiHandle{0};
    std::optional<IQSelection> iq{};
};

struct Faults {
    quint64 dispatchDeadlines{0};
    quint64 sendErrors{0};
    quint64 rejectedInput{0};
    quint64 staleIq{0};
    bool any() const;
};

struct MeasurementSource {
    quint64 sourceId{0};
    bool tones{false};
};

inline constexpr quint32 kMinToneSpacingHz = 200;
inline constexpr quint32 kMaxToneSpacingHz = 10000;
inline constexpr quint32 kDefaultToneSpacingHz = 1190;
inline constexpr double kToneCenterHz = 1305;
struct MeasurementToneConfig {
    quint64 sourceId{0};
    quint32 spacingHz{kDefaultToneSpacingHz};
    bool valid() const;
    double lowerOffsetHz() const { return kToneCenterHz - double(spacingHz) / 2; }
    double upperOffsetHz() const { return kToneCenterHz + double(spacingHz) / 2; }
    bool operator==(const MeasurementToneConfig&) const = default;
};

struct FeedbackBinding {
    quint32 channel{0};
    quint32 pan{0};
    quint32 guiHandle{0};
    bool valid{false};
    bool failed{false};
    bool operator==(const FeedbackBinding&) const = default;
};

struct Status {
    std::optional<FeedbackBinding> feedbackBinding;
    std::optional<MeasurementSource> measurement;
    std::optional<MeasurementToneConfig> measurementToneConfig;
    bool measurementToneControl{false};
    bool available{false};
    State state{State::Unavailable};
    bool registered{false};
    bool keyed{false};
    bool correctionEnabled{false};
    bool correctionControl{false};
    bool correcting{false};
    bool calibrationRequired{false};
    bool calibrationControl{false};
    bool measurementControl{false};
    SourcePurpose sourcePurpose{SourcePurpose::Calibration};
    CalibrationState calibrationState{CalibrationState::Unavailable};
    quint32 calibrationSamplesLeft{0};
    quint64 inputLimitedSamples{0};
    Faults faults;
};

struct Request {
    QByteArray method;
    QString path;
    int timeoutMs{0};
};

inline constexpr int kCaptureSamples = 16384;
inline constexpr int kCaptureChunkSamples = 512;
struct CaptureSourceIdentity {
    quint32 streamId{0};
    quint64 generation{0};
    bool operator==(const CaptureSourceIdentity&) const = default;
};
struct CaptureManifest {
    std::optional<FeedbackBinding> feedbackBinding;
    std::optional<CaptureSourceIdentity> source;
    std::optional<MeasurementSource> measurement;
    std::optional<MeasurementToneConfig> measurementToneConfig;
    QString captureId;
    quint32 sampleRateHz{0};
    quint64 centerHz{0};
    quint64 firstReceiveNs{0};
    quint64 lastReceiveNs{0};
    bool keyed{false};
    bool correcting{false};
};

Request captureBeginRequest(bool withSourceIdentity = false);
Request captureBeginWithTonesRequest();
std::optional<Request> captureChunkRequest(const CaptureManifest& manifest, int chunk);
std::optional<CaptureManifest> parseCaptureManifest(const QByteArray& json);
// Returns exactly 512 unchanged little-endian float32 complex samples.
std::optional<QByteArray> parseCaptureChunk(const QByteArray& json,
                                          const CaptureManifest& manifest, int chunk);

enum class TestOperation { Begin, Status, Stop };
enum class TestState { Observing, Retiring, Ended };
struct TestObservation {
    QString testId;
    TestState state{TestState::Ended};
    quint64 cleanupDueMs{0}; // Radio CLOCK_MONOTONIC; never a desktop timestamp.
    int remainingMs{0};
};
std::optional<Request> testRequest(TestOperation operation, const QString& testId = {});
std::optional<QByteArray> testBeginBody(int cleanupAfterMs);
std::optional<TestObservation> parseTestObservation(const QByteArray& json);

// Derived from exact operator-approved upgrade or first-install bytes. Parsing grants
// no installation authority and does not authenticate a release or consent.
struct InstallationPlan {
    bool firstInstall{false};
    QString planDigest;
    QString previousDigest;
    QString candidateDigest;
    QString previousKey;
    QString candidateKey;
    quint32 profile{0};
    QString firmware;
    QString serial;
};
struct InstallationObservation {
    bool firstInstall{false};
    bool committed{false};
    QString planDigest;
    QString selectedDigest;
    QString selectedKey;
    quint32 profile{0};
    QString firmware;
    QString serial;
};
enum class InstallationMatch { Committed, RolledBack, OtherPlan, Mismatch };
std::optional<InstallationPlan> parseInstallationPlan(const QByteArray& approved);
// PDFST001 carries no publisher key. The caller supplies the raw 32-byte key
// from authenticated package metadata, never from the observed radio reply.
std::optional<InstallationPlan> parseFirstInstallationPlan(const QByteArray& approved,
                                                          const QByteArray& publisherKey);
// Create before operator approval; retain these exact bytes for every retry of
// delivery/observation. A new installation attempt needs a fresh random nonce.
std::optional<QByteArray> installationAttempt(const QByteArray& plan, const QByteArray& nonce);
std::optional<InstallationObservation> parseInstallationObservation(const QByteArray& json);
InstallationMatch matchInstallation(const InstallationPlan& plan, const InstallationObservation& observed);

// Start carries the current GUI identity and optional exact IQ selection. Configured measurement carries
// only a bounded spacing; neither request grants transmitter authority.
// Acknowledgments never establish observed registration or correction state.
std::optional<Request> request(Operation operation);
std::optional<QByteArray> startBody(const StartIdentity& identity);
Request selectedStartRequest();
std::optional<QByteArray> measurementToneBody(quint32 spacingHz);
Request measurementToneRequest();
std::optional<Status> parseStatus(const QByteArray& json);
std::optional<Acknowledgment> parseAcknowledgment(Operation operation, int httpStatus,
                                                 const QByteArray& json);

} // namespace AetherSDR::aetherpd
