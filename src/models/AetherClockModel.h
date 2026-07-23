#pragma once

// First-class AetherClock model: full Q_PROPERTY coverage so the state is
// protocol-serializable (bridge `get clock`) and QML-ready. Thin — mirrors
// engine state, owns no DSP. All engine→model wiring is QUEUED
// (attachEngine) so the engine may live on a worker thread.

#include "core/ClockDiagnostics.h" // WS-7 acquisition telemetry
#include "core/TimeFrameVoter.h"   // ClockLockState / ClockStation

#include <QDateTime>
#include <QObject>
#include <QPointer>
#include <QString>

namespace AetherSDR {

class AetherClockEngine;

class AetherClockModel : public QObject {
    Q_OBJECT
    Q_PROPERTY(int state READ state NOTIFY stateChanged)
    Q_PROPERTY(QString stateName READ stateName NOTIFY stateChanged)
    Q_PROPERTY(int station READ station NOTIFY stationChanged)
    Q_PROPERTY(QString stationName READ stationName NOTIFY stationChanged)
    Q_PROPERTY(QDateTime decodedUtc READ decodedUtc NOTIFY decodedUtcChanged)
    Q_PROPERTY(double offsetMs READ offsetMs NOTIFY offsetMsChanged)
    Q_PROPERTY(int lockQuality READ lockQuality NOTIFY lockQualityChanged)
    Q_PROPERTY(int sliceId READ sliceId WRITE setSliceId NOTIFY sliceIdChanged)
    Q_PROPERTY(bool gpsTimeAvailable READ gpsTimeAvailable
               WRITE setGpsTimeAvailable NOTIFY gpsTimeAvailableChanged)
    // WS-7 acquisition telemetry (PRD-C): the funnel-relevant diagnostics
    // subset, all riding ONE notify — the snapshot arrives atomically at
    // ~1 Hz, so per-field signals would only fragment a single update.
    Q_PROPERTY(double toneSnrDb READ toneSnrDb NOTIFY diagnosticsChanged)
    Q_PROPERTY(double pwmContrast READ pwmContrast NOTIFY diagnosticsChanged)
    Q_PROPERTY(bool toneDetected READ toneDetected NOTIFY diagnosticsChanged)
    Q_PROPERTY(bool phaseLocked READ phaseLocked NOTIFY diagnosticsChanged)
    Q_PROPERTY(double delayEstMs READ delayEstMs NOTIFY diagnosticsChanged)
    Q_PROPERTY(bool anchored READ anchored NOTIFY diagnosticsChanged)
    Q_PROPERTY(int badFrameStreak READ badFrameStreak NOTIFY diagnosticsChanged)
    Q_PROPERTY(int classifiedPct READ classifiedPct NOTIFY diagnosticsChanged)
    Q_PROPERTY(int framesInWindow READ framesInWindow NOTIFY diagnosticsChanged)
    Q_PROPERTY(int windowSize READ windowSize NOTIFY diagnosticsChanged)
    Q_PROPERTY(double voteQuality READ voteQuality NOTIFY diagnosticsChanged)
    Q_PROPERTY(int refusalReason READ refusalReason NOTIFY diagnosticsChanged)
    Q_PROPERTY(QString refusalName READ refusalName NOTIFY diagnosticsChanged)
public:
    explicit AetherClockModel(QObject* parent = nullptr);

    // Connects the engine's signals to this model with Qt::QueuedConnection
    // ONLY (cross-thread-safe). Detaches any previously attached engine.
    void attachEngine(AetherClockEngine* engine);

    int state() const { return int(m_state); }
    ClockLockState lockState() const { return m_state; }
    QString stateName() const;   // "NoSignal" | "Acquiring" | "Locked"
    int station() const { return int(m_station); }
    QString stationName() const; // "Unknown" | "WWV" | "WWVH" | "WWVB"
    QDateTime decodedUtc() const { return m_decodedUtc; }
    double offsetMs() const { return m_offsetMs; }
    int lockQuality() const { return m_lockQuality; }
    int sliceId() const { return m_sliceId; }
    bool gpsTimeAvailable() const { return m_gpsTimeAvailable; }

    // WS-7 diagnostics mirror (last snapshot received from the engine).
    double toneSnrDb() const { return m_diag.toneSnrDb; }
    double pwmContrast() const { return m_diag.pwmContrast; }
    bool toneDetected() const { return m_diag.toneDetected; }
    bool phaseLocked() const { return m_diag.phaseLocked; }
    double delayEstMs() const { return m_diag.delayEstMs; }
    bool anchored() const { return m_diag.anchored; }
    int badFrameStreak() const { return m_diag.badFrameStreak; }
    int classifiedPct() const { return m_diag.classifiedPct; }
    int framesInWindow() const { return m_diag.framesInWindow; }
    int windowSize() const { return m_diag.windowSize; }
    double voteQuality() const { return m_diag.voteQuality; }
    int refusalReason() const { return int(m_diag.refusalReason); }
    QString refusalName() const;  // "None" | "QualityFloor" | ...
    const ClockDiagnostics& diagnostics() const { return m_diag; }

public slots:
    void setSliceId(int id);
    void setGpsTimeAvailable(bool available);

signals:
    void stateChanged(int state);
    void stationChanged(int station);
    void decodedUtcChanged(const QDateTime& utc);
    void offsetMsChanged(double offsetMs);
    void lockQualityChanged(int quality);
    void sliceIdChanged(int id);
    void gpsTimeAvailableChanged(bool available);
    // WS-7: one notify per diagnostics snapshot; frameDecoded forwards the
    // engine's raw per-frame decode to the debug pane.
    void diagnosticsChanged();
    void frameDecoded(const AetherSDR::ClockFrameInfo& frame);

private slots:
    void onLockStateChanged(AetherSDR::ClockLockState state);
    void onStationDetected(AetherSDR::ClockStation station);
    void onTimeDecoded(const QDateTime& utc, double offsetMs, int quality);
    void onDiagnostics(const AetherSDR::ClockDiagnostics& diag);

private:
    QPointer<AetherClockEngine> m_engine;
    ClockLockState m_state{ClockLockState::NoSignal};
    ClockStation m_station{ClockStation::Unknown};
    QDateTime m_decodedUtc;
    double m_offsetMs{0.0};
    int m_lockQuality{0};
    int m_sliceId{-1};
    bool m_gpsTimeAvailable{false};
    ClockDiagnostics m_diag;
};

} // namespace AetherSDR
