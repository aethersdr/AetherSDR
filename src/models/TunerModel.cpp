#include "TunerModel.h"
#include "core/TgxlConnection.h"
#include "core/LogManager.h"

#include <QDebug>
#include <cmath>

namespace MasterSDR {

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

void TunerModel::applyStatus(const QMap<QString, QString>& kvs)
{
    bool changed = false;

    for (auto it = kvs.constBegin(); it != kvs.constEnd(); ++it) {
        const QString& key = it.key();
        const QString& val = it.value();

        if (key == "serial_num") {
            if (m_serialNum != val) { m_serialNum = val; changed = true; }
        } else if (key == "model") {
            if (m_model != val) { m_model = val; changed = true; }
        } else if (key == "operate") {
            bool on = (val == "1");
            if (m_operate != on) { m_operate = on; changed = true; }
        } else if (key == "bypass") {
            bool on = (val == "1");
            if (m_bypass != on) { m_bypass = on; changed = true; }
        } else if (key == "tuning") {
            bool on = (val == "1");
            if (m_tuning != on) {
                m_tuning = on;
                changed = true;
                emit tuningChanged(m_tuning);
            }
        } else if (key == "relayC1") {
            int v = val.toInt();
            if (m_relayC1 != v) { m_relayC1 = v; changed = true; }
        } else if (key == "relayC2") {
            int v = val.toInt();
            if (m_relayC2 != v) { m_relayC2 = v; changed = true; }
        } else if (key == "relayL") {
            int v = val.toInt();
            if (m_relayL != v) { m_relayL = v; changed = true; }
        }
        else if (key == "antA") {
            int v = val.toInt();
            if (m_antennaA != v) { m_antennaA = v; changed = true; emit antennaAChanged(v); }
        } else if (key == "one_by_three") {
            bool v = val == "1";
            if (m_oneByThree != v) { m_oneByThree = v; changed = true; }
        } else if (key == "ip") {
            if (m_tgxlIp != val) { m_tgxlIp = val; changed = true; }
        }
        // nickname, version, ant, dhcp, netmask, gateway, ptta, pttb
        // are informational — ignore for now.
    }

    if (changed)
        emit stateChanged();
}

// ── Commands ─────────────────────────────────────────────────────────────────

void TunerModel::setOperate(bool on)
{
    if (m_handle.isEmpty()) {
        qCDebug(lcTuner) << "TunerModel::setOperate: no handle yet, ignoring";
        return;
    }
    const QString cmd = "tgxl set handle=" + m_handle + " mode=" + (on ? "1" : "0");
    qCDebug(lcTuner) << "TunerModel:" << cmd;
    emit commandReady(cmd);
}

void TunerModel::setBypass(bool on)
{
    if (m_handle.isEmpty()) {
        qCDebug(lcTuner) << "TunerModel::setBypass: no handle yet, ignoring";
        return;
    }
    const QString cmd = "tgxl set handle=" + m_handle + " bypass=" + (on ? "1" : "0");
    qCDebug(lcTuner) << "TunerModel:" << cmd;
    emit commandReady(cmd);
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
    const QString cmd = "tgxl autotune handle=" + m_handle;
    qCDebug(lcTuner) << "TunerModel:" << cmd;
    emit commandReady(cmd);
}

void TunerModel::setAntennaA(int ant)
{
    if (!m_directConn || !m_directConn->isConnected()) {
        qCDebug(lcTuner) << "TunerModel::setAntennaA: no direct connection";
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
            if (!isPresent())
                emit presenceChanged(false);
            emit directConnectionChanged(false);
        });
        // Update relay values from direct state pushes
        connect(m_directConn, &TgxlConnection::stateUpdated, this,
                [this](const QMap<QString, QString>& kvs) {
            bool changed = false;
            if (kvs.contains("relayC1")) {
                int v = kvs.value("relayC1").toInt();
                if (m_relayC1 != v) { m_relayC1 = v; changed = true; }
            }
            if (kvs.contains("relayL")) {
                int v = kvs.value("relayL").toInt();
                if (m_relayL != v) { m_relayL = v; changed = true; }
            }
            if (kvs.contains("relayC2")) {
                int v = kvs.value("relayC2").toInt();
                if (m_relayC2 != v) { m_relayC2 = v; changed = true; }
            }
            if (kvs.contains("antA")) {
                int v = kvs.value("antA").toInt();
                if (m_antennaA != v) { m_antennaA = v; changed = true; emit antennaAChanged(v); }
            }
            if (changed) emit stateChanged();
            // Forward power and SWR from direct TGXL connection (#625)
            // TGXL reports fwd in dBm and swr as return loss (negative dB).
            // Convert to watts and SWR ratio for the gauge.
            // Always emit when meter fields are present — suppressing identical
            // values caused meter-freeze when SWR settled to exactly 1.0 (#1530).
            bool meters = false;
            if (kvs.contains("fwd")) {
                float dBm = kvs.value("fwd").toFloat();
                float watts = std::pow(10.0f, dBm / 10.0f) / 1000.0f;
                m_fwdPower = watts;
                meters = true;
            }
            if (kvs.contains("swr")) {
                float rl = kvs.value("swr").toFloat();  // return loss in dB (negative from TGXL)
                float rho = std::pow(10.0f, rl / 20.0f);  // rl is already negative
                float ratio = (rho < 0.999f) ? (1.0f + rho) / (1.0f - rho) : 99.9f;
                m_swr = ratio;
                meters = true;
            }
            if (meters) emit metersChanged(m_fwdPower, m_swr);
        });
        // Also parse antA + meters from 1/sec status poll responses
        connect(m_directConn, &TgxlConnection::statusUpdated, this,
                [this](const QMap<QString, QString>& kvs) {
            if (kvs.contains("antA")) {
                int v = kvs.value("antA").toInt();
                if (m_antennaA != v) {
                    m_antennaA = v;
                    emit antennaAChanged(v);
                    emit stateChanged();
                }
            }
            // Forward power and SWR from direct TGXL status poll (#625)
            // Always emit — see #1530 for why equality suppression was removed.
            bool meters = false;
            if (kvs.contains("fwd")) {
                float dBm = kvs.value("fwd").toFloat();
                float watts = std::pow(10.0f, dBm / 10.0f) / 1000.0f;
                m_fwdPower = watts;
                meters = true;
            }
            if (kvs.contains("swr")) {
                float rl = kvs.value("swr").toFloat();  // return loss in dB (negative from TGXL)
                float rho = std::pow(10.0f, rl / 20.0f);  // rl is already negative
                float ratio = (rho < 0.999f) ? (1.0f + rho) / (1.0f - rho) : 99.9f;
                m_swr = ratio;
                meters = true;
            }
            if (meters) emit metersChanged(m_fwdPower, m_swr);
        });
    }
}

bool TunerModel::hasDirectConnection() const
{
    return m_directConn && m_directConn->isConnected();
}

void TunerModel::adjustRelay(int relay, int direction)
{
    if (!m_directConn || !m_directConn->isConnected()) {
        qCDebug(lcTuner) << "TunerModel::adjustRelay: no direct connection";
        return;
    }
    m_directConn->adjustRelay(relay, direction);
}

} // namespace MasterSDR
