#pragma once

#include <QByteArray>
#include <QString>
#include <QtGlobal>

#include <optional>

namespace AetherSDR::aetherpd {

// These are the shim's bounded control verbs. None can key the transmitter.
enum class Operation { Status, ArmCalibration, CancelCalibration, Stop, RestoreReceive, Start };
enum class State {
    Unavailable, Starting, Disabled, BlockedByRadioApd, CalibrationArmed,
    RecalibrationRequired, Idle, Correcting, Calibrating
};
enum class CalibrationState { Unavailable, Idle, Completed, Canceled, Expired, Armed, Running };
enum class Acknowledgment { ArmAccepted, CancelAccepted, StopRequested, ReceiveRestored, StartRequested };

struct StartIdentity {
    QString guiClientId;
    quint32 guiHandle{0};
};

struct Faults {
    quint64 dispatchDeadlines{0};
    quint64 sendErrors{0};
    quint64 rejectedInput{0};
    quint64 staleIq{0};
    bool any() const;
};

struct Status {
    bool available{false};
    State state{State::Unavailable};
    bool registered{false};
    bool keyed{false};
    bool correctionEnabled{false};
    bool correcting{false};
    bool calibrationRequired{false};
    bool calibrationControl{false};
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
struct CaptureManifest {
    QString captureId;
    quint32 sampleRateHz{0};
    quint64 centerHz{0};
    quint64 firstReceiveNs{0};
    quint64 lastReceiveNs{0};
    bool keyed{false};
    bool correcting{false};
};

Request captureBeginRequest();
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

// Derived from the exact operator-approved PDUPGR01/PDUPGR02 bytes. This parser grants
// no installation authority and does not authenticate a release or consent.
struct InstallationPlan {
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
// Create before operator approval; retain these exact bytes for every retry of
// delivery/observation. A new installation attempt needs a fresh random nonce.
std::optional<QByteArray> installationAttempt(const QByteArray& plan, const QByteArray& nonce);
std::optional<InstallationObservation> parseInstallationObservation(const QByteArray& json);
InstallationMatch matchInstallation(const InstallationPlan& plan, const InstallationObservation& observed);

// Start carries only the current GUI identity; all other requests are empty.
// Acknowledgments never establish observed registration or correction state.
std::optional<Request> request(Operation operation);
std::optional<QByteArray> startBody(const StartIdentity& identity);
std::optional<Status> parseStatus(const QByteArray& json);
std::optional<Acknowledgment> parseAcknowledgment(Operation operation, int httpStatus,
                                                 const QByteArray& json);

} // namespace AetherSDR::aetherpd
