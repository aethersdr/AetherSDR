#include "AmpModel.h"
#include "core/PgxlConnection.h"

#include <cmath>

namespace AetherSDR {

namespace {

// "RADIO_AAB" / "RADIO_AB" — the prefix says where the bias choice comes from
// and is the same on every port, so the panel shows only the profile.
QString biasProfile(const QString& raw)
{
    static const QString kPrefix = QStringLiteral("RADIO_");
    return raw.startsWith(kPrefix) ? raw.mid(kPrefix.size()) : raw;
}

// The amplifier reports bandX=0 for a port it is not driving.
QString bandName(const QString& raw)
{
    return (raw.isEmpty() || raw == QLatin1String("0")) ? QString() : raw;
}

}  // namespace

bool AmpModel::hasDirectConnection() const
{
    return m_directConn && m_directConn->isConnected();
}

void AmpModel::setDirectConnection(PgxlConnection* conn)
{
    if (m_directConn == conn) return;
    if (m_directConn) {
        disconnect(m_directConn, nullptr, this, nullptr);
    }
    m_directConn = conn;
    if (!m_directConn) return;

    connect(m_directConn, &PgxlConnection::statusUpdated, this,
            [this](const QMap<QString, QString>& kvs) { applyDirectStatus(kvs); });

    connect(m_directConn, &PgxlConnection::alertChanged, this,
            [this](const QString& text) {
        if (m_alert == text) return;
        m_alert = text;
        emit alertChanged(m_alert);
    });

    connect(m_directConn, &PgxlConnection::disconnected, this, [this]() {
        // Everything below came from a device we can no longer see. A frozen
        // band or bias claims the amplifier is set up a way we have stopped
        // being told about, which is worse than showing nothing.
        //
        // The meters included: a gauge left standing at the last reading taken
        // before the link dropped reports power out of an amplifier we are no
        // longer talking to. Zero watts is what we now know.
        m_directFwdWatts = 0.0f;
        m_directSwr = 1.0f;
        emit directMetersChanged(m_directFwdWatts, m_directSwr);
        if (!m_alert.isEmpty()) {
            m_alert.clear();
            emit alertChanged(m_alert);
        }
        if (m_havePortInfo) {
            m_havePortInfo = false;
            m_portA = {};
            m_portB = {};
            emit portsChanged();
        }
    });
}

void AmpModel::applyStateWord(const QString& state)
{
    if (m_state == state) return;
    m_state = state;
    emit ampStateChanged(m_state);

    // Which port is keyed is carried by the state word rather than by a
    // per-port PTT field, so the lamps move when it does. Nothing else on a
    // port changes here, so this is gated on the lamps alone.
    if (!m_havePortInfo) return;
    const bool a = (m_state == QLatin1String("TRANSMIT_A"));
    const bool b = (m_state == QLatin1String("TRANSMIT_B"));
    if (m_portA.ptt == a && m_portB.ptt == b) return;
    m_portA.ptt = a;
    m_portB.ptt = b;
    emit portsChanged();
}

QString AmpModel::outputForAntenna(const QString& antenna) const
{
    return m_antennaOutputs.value(antenna.trimmed());
}

void AmpModel::applyDirectStatus(const QMap<QString, QString>& kvs)
{
    if (kvs.contains(QStringLiteral("state"))) {
        applyStateWord(kvs.value(QStringLiteral("state")));
    }

    // Metering first, and independently of the per-port block below: the two
    // are not carried by the same frames in every firmware, and a status that
    // omits bandA/bandB must not cost us the power reading with it.
    //
    // `fwd` is dBm on the wire -- the amplifier's own FWD meter declares
    // 30.0..63.0 dBm (1 W..2 kW) and the direct status floors at exactly
    // 30.0 when nothing is being transmitted. `swr` is return loss in dB,
    // reported NEGATIVE here (-60.0 at rest), unlike the relayed RL meter
    // which reports the same quantity positive.
    bool meters = false;
    if (kvs.contains(QStringLiteral("fwd"))) {
        const float dBm = kvs.value(QStringLiteral("fwd")).toFloat();
        m_directFwdWatts = std::pow(10.0f, dBm / 10.0f) / 1000.0f;
        meters = true;
    }
    if (kvs.contains(QStringLiteral("swr"))) {
        const float returnLossDb = kvs.value(QStringLiteral("swr")).toFloat();
        // Take the magnitude: the sign is this transport's convention, not a
        // measurement, and a firmware that dropped it would otherwise invert
        // the ratio.
        const float rho = std::pow(10.0f, -std::abs(returnLossDb) / 20.0f);
        m_directSwr = (rho < 0.999f) ? (1.0f + rho) / (1.0f - rho) : 99.9f;
        meters = true;
    }
    // Emitted unconditionally when the fields are present, never gated on the
    // value having moved: a meter that settles on one number is still a live
    // meter, and suppressing the repeat is what freezes a gauge (#1530).
    if (meters) emit directMetersChanged(m_directFwdWatts, m_directSwr);

    if (!kvs.contains(QStringLiteral("bandA")) && !kvs.contains(QStringLiteral("bandB"))) {
        return;   // an info or partial frame, not the per-port block
    }

    auto readPort = [&](QChar side, bool keyed) {
        AmpPortInfo p;
        p.band = bandName(kvs.value(QStringLiteral("band%1").arg(side)));
        p.bias = biasProfile(kvs.value(QStringLiteral("bias%1").arg(side)));
        p.source = kvs.value(QStringLiteral("flex%1").arg(side)).trimmed();
        p.live = !p.band.isEmpty();
        p.ptt = keyed;
        return p;
    };
    const AmpPortInfo a = readPort(QLatin1Char('A'),
                                   m_state == QLatin1String("TRANSMIT_A"));
    const AmpPortInfo b = readPort(QLatin1Char('B'),
                                   m_state == QLatin1String("TRANSMIT_B"));

    const bool first = !m_havePortInfo;
    if (first || a != m_portA || b != m_portB) {
        m_portA = a;
        m_portB = b;
        m_havePortInfo = true;
        emit portsChanged();
    }
}

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
        // "ANT1:PORTA,ANT2:PORTB" — which amplifier output each radio antenna
        // is wired to. Split exactly as FlexLib's Amplifier.ParseAntennaSettings
        // does: comma-separated pairs, a pair without a colon or with more than
        // two fields skipped rather than treated as an error.
        if (d.telemetry.contains(QStringLiteral("ant"))) {
            QMap<QString, QString> outputs;
            const auto pairs = d.telemetry.value(QStringLiteral("ant"))
                                   .split(QLatin1Char(','), Qt::SkipEmptyParts);
            for (const QString& pair : pairs) {
                const QStringList parts = pair.split(QLatin1Char(':'));
                if (parts.size() != 2) continue;
                outputs.insert(parts.at(0).trimmed(), parts.at(1).trimmed());
            }
            if (outputs != m_antennaOutputs) {
                m_antennaOutputs = outputs;
                emit antennaMapChanged();
            }
        }
        // The state word also arrives here, so the keying lamps and the panel's
        // state read the same on the relayed path as on the direct one.
        const QString state = d.telemetry.value(QStringLiteral("state"));
        if (!state.isEmpty()) applyStateWord(state);

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

    // Everything below describes an amplifier reached through a radio this
    // model is being torn down from. A state word, an antenna map or a port
    // block left standing outlives the thing that reported it — the same
    // reason the direct connection's `disconnected` handler clears them.
    if (!m_state.isEmpty()) {
        m_state.clear();
        emit ampStateChanged(m_state);
    }
    if (!m_alert.isEmpty()) {
        m_alert.clear();
        emit alertChanged(m_alert);
    }
    if (!m_antennaOutputs.isEmpty()) {
        m_antennaOutputs.clear();
        emit antennaMapChanged();
    }
    if (m_havePortInfo) {
        m_havePortInfo = false;
        m_portA = {};
        m_portB = {};
        emit portsChanged();
    }
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
