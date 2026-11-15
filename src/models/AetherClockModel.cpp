#include "AetherClockModel.h"

#include "core/AetherClockEngine.h"

namespace AetherSDR {

AetherClockModel::AetherClockModel(QObject* parent)
    : QObject(parent)
{}

// All engine->model wiring uses Qt::QueuedConnection ONLY: per the model
// header's threading contract the engine may be moved to a worker thread.
void AetherClockModel::attachEngine(AetherClockEngine* engine)
{
    // Detach any previously attached engine first: drop every connection it
    // made to this model so a re-attach can never double-deliver.
    if (m_engine) {
        disconnect(m_engine, nullptr, this, nullptr);
    }

    m_engine = engine;
    if (!engine) {
        return;
    }

    connect(engine, &AetherClockEngine::lockStateChanged,
            this, &AetherClockModel::onLockStateChanged, Qt::QueuedConnection);
    connect(engine, &AetherClockEngine::stationDetected,
            this, &AetherClockModel::onStationDetected, Qt::QueuedConnection);
    connect(engine, &AetherClockEngine::timeDecoded,
            this, &AetherClockModel::onTimeDecoded, Qt::QueuedConnection);
    connect(engine, &AetherClockEngine::diagnosticsUpdated,
            this, &AetherClockModel::onDiagnostics, Qt::QueuedConnection);
    // Signal-to-signal forward: the model adds no state for per-frame decodes,
    // it just carries them across the thread boundary for the debug pane.
    connect(engine, &AetherClockEngine::frameDecoded,
            this, &AetherClockModel::frameDecoded, Qt::QueuedConnection);
}

QString AetherClockModel::stateName() const
{
    switch (m_state) {
    case ClockLockState::NoSignal:  return QStringLiteral("NoSignal");
    case ClockLockState::Acquiring: return QStringLiteral("Acquiring");
    case ClockLockState::Locked:    return QStringLiteral("Locked");
    }
    return QStringLiteral("NoSignal");
}

QString AetherClockModel::refusalName() const
{
    switch (ClockLockRefusal(m_diag.refusalReason)) {
    case ClockLockRefusal::None:         return QStringLiteral("None");
    case ClockLockRefusal::QualityFloor: return QStringLiteral("QualityFloor");
    case ClockLockRefusal::Plausibility: return QStringLiteral("Plausibility");
    case ClockLockRefusal::Staleness:    return QStringLiteral("Staleness");
    case ClockLockRefusal::Contested:    return QStringLiteral("Contested");
    }
    return QStringLiteral("None");
}

QString AetherClockModel::stationName() const
{
    switch (m_station) {
    case ClockStation::Unknown: return QStringLiteral("Unknown");
    case ClockStation::Wwv:     return QStringLiteral("WWV");
    case ClockStation::Wwvh:    return QStringLiteral("WWVH");
    case ClockStation::Wwvb:    return QStringLiteral("WWVB");
    }
    return QStringLiteral("Unknown");
}

void AetherClockModel::setSliceId(int id)
{
    if (m_sliceId == id) return;
    m_sliceId = id;
    emit sliceIdChanged(id);
}

void AetherClockModel::setGpsTimeAvailable(bool available)
{
    if (m_gpsTimeAvailable == available) return;
    m_gpsTimeAvailable = available;
    emit gpsTimeAvailableChanged(available);
}

void AetherClockModel::onLockStateChanged(AetherSDR::ClockLockState state)
{
    if (m_state == state) return;
    m_state = state;
    emit stateChanged(int(state));
}

void AetherClockModel::onStationDetected(AetherSDR::ClockStation station)
{
    if (m_station == station) return;
    m_station = station;
    emit stationChanged(int(station));
}

void AetherClockModel::onDiagnostics(const AetherSDR::ClockDiagnostics& diag)
{
    m_diag = diag;
    emit diagnosticsChanged();  // one snapshot, one notify (arrives at ~1 Hz)
}

void AetherClockModel::onTimeDecoded(const QDateTime& utc, double offsetMs, int quality)
{
    if (m_decodedUtc != utc) {
        m_decodedUtc = utc;
        emit decodedUtcChanged(utc);
    }
    if (m_offsetMs != offsetMs) {
        m_offsetMs = offsetMs;
        emit offsetMsChanged(offsetMs);
    }
    if (m_lockQuality != quality) {
        m_lockQuality = quality;
        emit lockQualityChanged(quality);
    }
}

} // namespace AetherSDR
