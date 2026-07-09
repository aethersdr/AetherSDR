#include "AmpModel.h"

namespace AetherSDR {

void AmpModel::applyChanges(const AmpDelta& d)
{
    if (d.removed) {
        // Clear only if it's our amp (matches the original removal semantics —
        // leaves m_ip/m_operate untouched; consumers gate on present()).
        if (d.handle == m_handle) {
            m_handle.clear();
            m_present = false;
            m_model.clear();
            emit presenceChanged(false);
        }
        return;
    }

    // Presence latch: a detected (non-TGXL) power-amp model marks us present.
    if (d.detectedModel) {
        m_handle = d.handle;
        if (!m_present) {
            m_present = true;
            // Strict parity with the prior applyStatus (m_ip = kvs.value("ip"),
            // which blanked to "" when absent) — keeps this a behavior-neutral move.
            m_ip = d.ip.value_or(QString());
            m_model = *d.detectedModel;
            emit presenceChanged(true);
        }
    }

    if (!m_handle.isEmpty() && d.handle == m_handle) {
        // Operate is change-gated; a status without a "state" leaves it as-is.
        if (d.operate && m_operate != *d.operate) {
            m_operate = *d.operate;
            emit stateChanged();
        }
        // Forward telemetry (drain current, mains voltage, meffa, temp, …) so
        // the GUI updates without a direct PGXL TCP connection.
        emit telemetryUpdated(d.telemetry);
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
