#include "core/aprs/AprsBeacon.h"

#include "core/aprs/AprsPacket.h"

namespace AetherSDR {

using ax25::Address;
using ax25::Frame;

namespace {
const QString kTocall = QStringLiteral("APZATH"); // experimental tocall
} // namespace

AprsBeacon::AprsBeacon(QObject* parent)
    : QObject(parent)
{
    connect(&m_timer, &QTimer::timeout, this, [this] { sendNow(); });
}

void AprsBeacon::setEnabled(bool on)
{
    // No-op when unchanged: applyAprsConfigFromUi() calls this on every dialog
    // interaction (editingFinished / symbol change / message send / Beacon Now),
    // and an unconditional timer restart would let an operator defer a timed
    // beacon indefinitely by fiddling with the UI.
    if (m_enabled == on)
        return;
    m_enabled = on;
    // Timer only — no immediate transmission. Enabling the beacon (or the
    // app restoring a persisted enable on startup) must not key the radio by
    // itself; "Beacon Now" is the explicit immediate send. PmsMailbox's
    // beacon follows the same contract.
    if (on)
        m_timer.start(m_intervalMin * 60 * 1000);
    else
        m_timer.stop();
}

void AprsBeacon::setIntervalMinutes(int minutes)
{
    const int clamped = qBound(1, minutes, 24 * 60);
    if (clamped == m_intervalMin)
        return;  // unchanged — don't restart the timer (would defer the beacon)
    m_intervalMin = clamped;
    if (m_enabled)
        m_timer.start(m_intervalMin * 60 * 1000);
}

void AprsBeacon::setGpsPosition(double lat, double lon, bool valid)
{
    if (!valid)
        return; // keep the last good fix / manual fallback
    m_gpsValid = true;
    m_gpsLat = lat;
    m_gpsLon = lon;
}

void AprsBeacon::setManualPosition(double lat, double lon, bool valid)
{
    m_manualValid = valid;
    m_manualLat = lat;
    m_manualLon = lon;
}

bool AprsBeacon::currentPosition(double& lat, double& lon) const
{
    if (m_gpsValid) {
        lat = m_gpsLat;
        lon = m_gpsLon;
        return true;
    }
    if (m_manualValid) {
        lat = m_manualLat;
        lon = m_manualLon;
        return true;
    }
    return false;
}

bool AprsBeacon::sendNow()
{
    if (!m_myAddress.isValid()) {
        emit activity(QStringLiteral("APRS beacon skipped: no callsign configured."));
        return false;
    }
    double lat = 0.0, lon = 0.0;
    if (!currentPosition(lat, lon)) {
        emit activity(QStringLiteral(
            "APRS beacon skipped: no GPS fix and no manual position."));
        return false;
    }

    const QString info = aprs::encodeUncompressedPosition(
        lat, lon, m_symbolTable, m_symbolCode, m_statusText, true);
    Address dest;
    dest.call = kTocall;
    const Frame frame = Frame::makeUI(dest, m_myAddress, m_path, info.toLatin1());
    emit transmitFrame(frame.encode());
    emit activity(QStringLiteral("APRS beacon sent (%1 via %2): %3")
                      .arg(m_gpsValid ? QStringLiteral("GPS")
                                      : QStringLiteral("manual position"),
                           m_myAddress.toString(), info));

    if (m_enabled)
        m_timer.start(m_intervalMin * 60 * 1000); // re-arm from "now"
    return true;
}

} // namespace AetherSDR
