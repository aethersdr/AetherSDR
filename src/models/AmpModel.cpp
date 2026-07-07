#include "AmpModel.h"

namespace AetherSDR {

void AmpModel::applyStatus(const QString& handle, const QString& model,
                           const QMap<QString, QString>& kvs)
{
    // Presence: any non-empty, non-TGXL amp model marks a power amp present.
    // (The radio reports both TGXL and PGXL via the "amplifier" API; the caller
    // routes TunerGeniusXL to TunerModel and everything else here.)
    if (!model.isEmpty() && model != QLatin1String("TunerGeniusXL")) {
        m_handle = handle;
        if (!m_present) {
            m_present = true;
            m_ip = kvs.value(QStringLiteral("ip"));
            m_model = model;
            emit presenceChanged(true);
        }
    }

    if (!m_handle.isEmpty() && handle == m_handle) {
        // PGXL uses "state=OPERATE|IDLE|STANDBY|TRANSMIT…" (not operate=/bypass=
        // like TGXL). IDLE = on/ready, STANDBY = off, TRANSMIT* = keyed.
        const QString state = kvs.value(QStringLiteral("state")).toUpper();
        if (!state.isEmpty()) {
            const bool op = (state == QLatin1String("IDLE")
                             || state == QLatin1String("OPERATE")
                             || state.startsWith(QLatin1String("TRANSMIT")));
            if (m_operate != op) {
                m_operate = op;
                emit stateChanged();
            }
        }
        // Forward the full KVS so the GUI can update telemetry (drain current,
        // mains voltage, meffa, temp) without a direct PGXL TCP connection.
        emit telemetryUpdated(kvs);
    }
}

void AmpModel::handleRemoval(const QString& handle)
{
    if (handle == m_handle) {
        m_handle.clear();
        m_present = false;
        m_model.clear();
        emit presenceChanged(false);
    }
}

void AmpModel::reset()
{
    m_present = false;
    m_handle.clear();
    m_operate = false;
}

void AmpModel::setOperate(bool on)
{
    if (m_handle.isEmpty()) return;
    // FlexLib API: "amplifier set <handle> operate=0|1". The radio relays it to
    // the PGXL (the only path that works remote/SmartLink).
    emit commandReady(QString("amplifier set %1 operate=%2").arg(m_handle).arg(on ? 1 : 0));
}

}  // namespace AetherSDR
