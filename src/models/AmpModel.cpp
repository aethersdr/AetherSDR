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
    if (d.detectedModel && !d.handle.isEmpty()) {
        m_handle = d.handle;
    }

    const bool appliesToAmp = d.detectedModel.has_value()
        || (!m_handle.isEmpty() && d.handle == m_handle);
    bool stateDidChange = false;
    if (appliesToAmp) {
        // Apply state before publishing first presence. The presence signal
        // makes the applet visible and reads operate() immediately; publishing
        // first used to paint a real operating PGXL as STANDBY during startup.
        if (d.operate && m_operate != *d.operate) {
            m_operate = *d.operate;
            stateDidChange = true;
        }
    }

    if (d.detectedModel) {
        if (!m_present) {
            m_present = true;
            // Strict parity with the prior applyStatus (m_ip = kvs.value("ip"),
            // which blanked to "" when absent) — keeps this a behavior-neutral move.
            m_ip = d.ip.value_or(QString());
            m_model = *d.detectedModel;
            emit presenceChanged(true);
        }
    }

    if (stateDidChange) {
        emit stateChanged();
    }

    if (appliesToAmp) {
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
    if (m_handle.isEmpty()) return;   // no amp present → nothing to command
    // Neutral intent. FlexBackend translates it to "amplifier set <handle>
    // operate=0|1" (radio-relayed — the only path that works remote/SmartLink);
    // the handle is supplied by RadioModel via invokeExtension's vendor arg.
    emit operateRequested(on);
}

}  // namespace AetherSDR
