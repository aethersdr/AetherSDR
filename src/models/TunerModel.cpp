#include "TunerModel.h"
#include "core/TgxlConnection.h"
#include "core/LogManager.h"

#include <QDebug>
#include <cmath>

namespace AetherSDR {

TunerModel::TunerModel(QObject* parent)
    : QObject(parent)
{
}

// ── Status parsing ──────────────────────────────────────────────────────────

void TunerModel::setHandle(const QString& handle)
{
    if (m_handle == handle) return;
    bool wasPres = isPresent();
    m_handle = handle;
    bool nowPres = isPresent();
    qCDebug(lcTuner) << "TunerModel: handle set to" << m_handle;
    if (wasPres != nowPres)
        emit presenceChanged(nowPres);
    emit stateChanged();
}

void TunerModel::applyChanges(const TunerDelta& d)
{
    // Apply only the present fields, change-gated — faithful to the prior
    // applyStatus (which iterated the wire kv-set). The SmartSDR key names and
    // "1"/toInt parsing now live in FlexBackend::decodeTunerStatus; informational
    // fields (nickname/version/ant/dhcp/netmask/gateway/ptta/pttb) are dropped there.
    // Edge-signal emit order matches the old QMap key-sorted iteration:
    // antennaAChanged (key "antA") precedes tuningChanged (key "tuning").
    bool changed = false;

    if (d.serialNum && m_serialNum != *d.serialNum) { m_serialNum = *d.serialNum; changed = true; }
    if (d.model && m_model != *d.model)             { m_model = *d.model;         changed = true; }
    if (d.operate && m_operate != *d.operate)       { m_operate = *d.operate;     changed = true; }
    if (d.bypass && m_bypass != *d.bypass)          { m_bypass = *d.bypass;       changed = true; }
    if (d.antennaA && m_antennaA != *d.antennaA) {
        m_antennaA = *d.antennaA;
        changed = true;
        emit antennaAChanged(m_antennaA);        // "antA" sorts before "tuning"
    }
    if (d.tuning && m_tuning != *d.tuning) {
        m_tuning = *d.tuning;
        changed = true;
        emit tuningChanged(m_tuning);
    }
    if (d.relayC1 && m_relayC1 != *d.relayC1) { m_relayC1 = *d.relayC1; changed = true; }
    if (d.relayC2 && m_relayC2 != *d.relayC2) { m_relayC2 = *d.relayC2; changed = true; }
    if (d.relayL && m_relayL != *d.relayL)    { m_relayL = *d.relayL;   changed = true; }
    if (d.oneByThree && m_oneByThree != *d.oneByThree) { m_oneByThree = *d.oneByThree; changed = true; }
    if (d.ip && m_tgxlIp != *d.ip)                     { m_tgxlIp = *d.ip;              changed = true; }

    if (changed)
        emit stateChanged();
}

// ── Commands ─────────────────────────────────────────────────────────────────

void TunerModel::setOperate(bool on)
{
    // Direct port-9010 channel first, for the same reason autoTune prefers it:
    // it is the only path that exists for a TGXL the radio never reports as an
    // amplifier object (#2250), and there is no handle to address in that case.
    // "operate set=1" brings the tuner online, "operate set=0" is standby.
    const bool direct = m_directConn && m_directConn->isConnected();
    if (direct) {
        qCDebug(lcTuner) << "TunerModel::setOperate: using direct TGXL path, on=" << on;
        m_directConn->setOperate(on);
    } else if (m_handle.isEmpty()) {
        qCDebug(lcTuner) << "TunerModel::setOperate: no direct conn and no handle, ignoring";
        return;
    } else {
        // Neutral intent → Flex "tgxl set handle=<h> mode=" wire (via RadioModel).
        emit operateRequested(on);
    }
    // Optimistic update: reflect the commanded state immediately so the
    // button label stays in sync even before the tuner echoes back.
    if (m_operate != on) { m_operate = on; emit stateChanged(); }
}

void TunerModel::setBypass(bool on)
{
    if (m_handle.isEmpty()) {
        qCDebug(lcTuner) << "TunerModel::setBypass: no handle yet, ignoring";
        return;
    }
    // Neutral intent → Flex "tgxl set handle=<h> bypass=" wire (via RadioModel).
    emit bypassRequested(on);
    // Optimistic update: reflect the commanded state immediately so the
    // button label stays in sync even before the radio echoes back.
    if (m_bypass != on) { m_bypass = on; emit stateChanged(); }
}

void TunerModel::autoTune()
{
    // Prefer the direct port-9010 channel when available: bypasses the radio's
    // `tgxl autotune` command path, which broke for some users in firmware 4.2.
    // The TGXL drives radio PTT via its hardware interlock cable, so we don't
    // need to key the radio from the client.
    if (m_directConn && m_directConn->isConnected()) {
        qCDebug(lcTuner) << "TunerModel::autoTune: using direct TGXL path";
        m_directConn->requestAutotune();
        return;
    }
    if (m_handle.isEmpty()) {
        qCDebug(lcTuner) << "TunerModel::autoTune: no direct conn and no handle, ignoring";
        return;
    }
    // Neutral intent → Flex "tgxl autotune handle=<h>" wire. RadioModel applies
    // the TX interlock gate before dispatching (was a commandReady string-sniff).
    emit autotuneRequested();
}

void TunerModel::setAntennaA(int ant)
{
    if (!m_directConn || !m_directConn->isConnected()) {
        qCDebug(lcTuner) << "TunerModel::setAntennaA: no direct connection";
        return;
    }
    // Only the 3x1 models have somewhere to switch to. On a single-port tuner
    // the command is meaningless, so it is not sent at all.
    if (!hasAntennaSwitch()) {
        qCDebug(lcTuner) << "TunerModel::setAntennaA: tuner has no antenna switch";
        return;
    }
    if (ant < 1 || ant > 3) return;
    qCDebug(lcTuner) << "TunerModel: activate ant=" << ant;
    m_directConn->sendCommand(QString("activate ant=%1").arg(ant));
}

// ── Direct TGXL connection (port 9010) ──────────────────────────────────────

void TunerModel::setDirectConnection(TgxlConnection* conn)
{
    if (m_directConn == conn) return;
    if (m_directConn) {
        disconnect(m_directConn, nullptr, this, nullptr);
    }
    m_directConn = conn;
    if (m_directConn) {
        connect(m_directConn, &TgxlConnection::connected, this, [this]() {
            qCDebug(lcTuner) << "TunerModel: direct TGXL connection established";
            bool wasPres = isPresent();
            m_directPresence = true;
            if (!wasPres)
                emit presenceChanged(true);
            emit directConnectionChanged(true);
        });
        connect(m_directConn, &TgxlConnection::disconnected, this, [this]() {
            qCDebug(lcTuner) << "TunerModel: direct TGXL connection lost";
            m_directPresence = false;
            // The 3way capability came from this device's info reply. A tuner
            // without the switch omits the key entirely, so a stale true would
            // survive a reconnect to a different device — clear it here.
            if (m_threeWay) { m_threeWay = false; emit stateChanged(); }
            if (!isPresent())
                emit presenceChanged(false);
            emit directConnectionChanged(false);
        });
        // Every direct message kind — the unsolicited state push, the 1/sec
        // status poll reply, and the one-shot info reply — carries an
        // overlapping set of the same keys, so they all go through one apply.
        connect(m_directConn, &TgxlConnection::stateUpdated, this,
                [this](const QMap<QString, QString>& kvs) {
            applyDirectKvs(kvs, DirectSource::StatePush);
        });
        connect(m_directConn, &TgxlConnection::statusUpdated, this,
                [this](const QMap<QString, QString>& kvs) {
            applyDirectKvs(kvs, DirectSource::StatusReply);
        });
        connect(m_directConn, &TgxlConnection::infoUpdated, this,
                [this](const QMap<QString, QString>& kvs) {
            applyDirectKvs(kvs, DirectSource::InfoReply);
        });
    }
}

void TunerModel::applyDirectKvs(const QMap<QString, QString>& kvs, DirectSource source)
{
    bool changed = false;

    // Pi-network relay positions. These arrive on the status poll as well as on
    // the state push; parsing them only on the push left the C1 / L / C2 bars
    // frozen at 0 for direct-only tuners (#4551).
    if (kvs.contains(QStringLiteral("relayC1"))) {
        const int v = kvs.value(QStringLiteral("relayC1")).toInt();
        if (m_relayC1 != v) { m_relayC1 = v; changed = true; }
    }
    if (kvs.contains(QStringLiteral("relayL"))) {
        const int v = kvs.value(QStringLiteral("relayL")).toInt();
        if (m_relayL != v) { m_relayL = v; changed = true; }
    }
    if (kvs.contains(QStringLiteral("relayC2"))) {
        const int v = kvs.value(QStringLiteral("relayC2")).toInt();
        if (m_relayC2 != v) { m_relayC2 = v; changed = true; }
    }

    // Antenna-switch capability. The direct info reply spells it "3way"; the
    // radio-relayed status spells it "one_by_three" (handled in applyChanges).
    if (kvs.contains(QStringLiteral("3way"))) {
        const bool v = (kvs.value(QStringLiteral("3way")) == QLatin1String("1"));
        if (m_threeWay != v) { m_threeWay = v; changed = true; }
    }

    // Operate / standby. Reading this is what gives the button its state at
    // startup: m_operate defaults to false, so without it the tuner always
    // came up showing STANDBY however it was actually set, and only became
    // truthful once the operator clicked it (#4552).
    //
    // The status reply calls the flag "state" — state=1 online, state=0
    // standby. The command keeps its own spelling ("operate set=0|1"); the two
    // are not symmetric. Read only from the status reply: the unsolicited push
    // is itself the "state" object, so a key of that name in any other message
    // is something else entirely.
    if (source == DirectSource::StatusReply && kvs.contains(QStringLiteral("state"))) {
        const bool v = (kvs.value(QStringLiteral("state")) == QLatin1String("1"));
        if (m_operate != v) { m_operate = v; changed = true; }
    }
    if (kvs.contains(QStringLiteral("bypass"))) {
        const bool v = (kvs.value(QStringLiteral("bypass")) == QLatin1String("1"));
        if (m_bypass != v) { m_bypass = v; changed = true; }
    }
    if (kvs.contains(QStringLiteral("tuning"))) {
        const bool v = (kvs.value(QStringLiteral("tuning")) == QLatin1String("1"));
        if (m_tuning != v) { m_tuning = v; changed = true; emit tuningChanged(v); }
    }

    // Selected antenna port (0-indexed on the wire: 0=ANT1).
    if (kvs.contains(QStringLiteral("antA"))) {
        const int v = kvs.value(QStringLiteral("antA")).toInt();
        if (m_antennaA != v) { m_antennaA = v; changed = true; emit antennaAChanged(v); }
    }

    if (changed)
        emit stateChanged();

    // Forward power and SWR from the direct TGXL connection (#625).
    // TGXL reports fwd in dBm and swr as return loss (negative dB).
    // Convert to watts and SWR ratio for the gauge.
    // Always emit when meter fields are present — suppressing identical
    // values caused meter-freeze when SWR settled to exactly 1.0 (#1530).
    bool meters = false;
    if (kvs.contains(QStringLiteral("fwd"))) {
        const float dBm = kvs.value(QStringLiteral("fwd")).toFloat();
        m_fwdPower = std::pow(10.0f, dBm / 10.0f) / 1000.0f;
        meters = true;
    }
    if (kvs.contains(QStringLiteral("swr"))) {
        const float rl = kvs.value(QStringLiteral("swr")).toFloat();  // return loss in dB (negative from TGXL)
        const float rho = std::pow(10.0f, rl / 20.0f);                // rl is already negative
        m_swr = (rho < 0.999f) ? (1.0f + rho) / (1.0f - rho) : 99.9f;
        meters = true;
    }
    if (meters)
        emit metersChanged(m_fwdPower, m_swr);
}

bool TunerModel::hasDirectConnection() const
{
    return m_directConn && m_directConn->isConnected();
}

void TunerModel::adjustRelay(int relay, int steps)
{
    if (!m_directConn || !m_directConn->isConnected()) {
        qCDebug(lcTuner) << "TunerModel::adjustRelay: no direct connection";
        return;
    }
    m_directConn->adjustRelay(relay, steps);
}

} // namespace AetherSDR
