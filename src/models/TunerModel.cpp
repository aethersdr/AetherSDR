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
    // Losing the handle is how the relayed side says the tuner is gone (radio
    // disconnect, or the amplifier object being removed). A tune reported
    // before that goes unfinished as far as this client is concerned: the
    // tuner completes on its own and sits idle while the flag stays true.
    // Left latched it would pass abortTune()'s guard and start a tune on an
    // idle tuner. A direct connection that is still up re-reports the truth
    // on its next poll a second later.
    // Not while a direct connection is up: it is still watching the tune and
    // will report the end itself. Clearing here would drop the key back to
    // TUNE mid-tune, and pressing it then aborts.
    if (m_handle.isEmpty() && !hasDirectConnection()) {
        clearTuning();
    }
    if (wasPres != nowPres)
        emit presenceChanged(nowPres);
    emit stateChanged();
}

void TunerModel::applyChanges(const TunerDelta& d)
{
    // Apply only the present fields, change-gated — faithful to the prior
    // applyStatus (which iterated the wire kv-set). The SmartSDR key names and
    // "1"/toInt parsing now live in FlexBackend::decodeTunerStatus; informational
    // fields (nickname/version/dhcp/netmask/gateway) are dropped there.
    // Edge-signal emit order matches the old QMap key-sorted iteration:
    // antennaAChanged (key "antA") precedes tuningChanged (key "tuning").
    // pttChanged is new, so it has no legacy position to preserve; it is
    // emitted where its keys sort ("pttA"/"pttB", between the two) to keep
    // that one rule describing the whole function rather than most of it.
    const bool wasPresent = isPresent();
    bool changed = false;
    std::optional<int> pendingAntennaA;
    std::optional<bool> pendingTuning;
    bool pttMoved = false;

    if (d.handle && m_handle != *d.handle)           { m_handle = *d.handle;       changed = true; }
    if (d.serialNum && m_serialNum != *d.serialNum) { m_serialNum = *d.serialNum; changed = true; }
    if (d.model && m_model != *d.model)             { m_model = *d.model;         changed = true; }
    if (d.operate) {
        // If we have an in-flight operate command, apply the commanded value
        // rather than the echo until the echo confirms our command. This
        // prevents intermediate echoes (produced while the companion bypass
        // command is still in flight) from reverting the optimistic update and
        // flashing an unwanted intermediate state in the UI.
        bool effective = m_heldOperate ? m_heldOperateVal : *d.operate;
        if (m_heldOperate && *d.operate == m_heldOperateVal) m_heldOperate = false;
        if (m_operate != effective) { m_operate = effective; changed = true; }
    }
    if (d.bypass) {
        bool effective = m_heldBypass ? m_heldBypassVal : *d.bypass;
        if (m_heldBypass && *d.bypass == m_heldBypassVal) m_heldBypass = false;
        if (m_bypass != effective) { m_bypass = effective; changed = true; }
    }
    if (d.antennaA && m_antennaA != *d.antennaA) {
        m_antennaA = *d.antennaA;
        changed = true;
        pendingAntennaA = m_antennaA;
    }
    if (d.tuning && m_tuning != *d.tuning) {
        m_tuning = *d.tuning;
        changed = true;
        pendingTuning = m_tuning;
    }
    if (d.relayC1 && m_relayC1 != *d.relayC1) { m_relayC1 = *d.relayC1; changed = true; }
    if (d.relayC2 && m_relayC2 != *d.relayC2) { m_relayC2 = *d.relayC2; changed = true; }
    if (d.relayL && m_relayL != *d.relayL)    { m_relayL = *d.relayL;   changed = true; }
    if (d.oneByThree && m_oneByThree != *d.oneByThree) { m_oneByThree = *d.oneByThree; changed = true; }
    if (d.ip && m_tgxlIp != *d.ip)                     { m_tgxlIp = *d.ip;              changed = true; }
    if (d.portAAnt && m_portAAnt != *d.portAAnt) { m_portAAnt = *d.portAAnt; changed = true; }
    if (d.portBAnt && m_portBAnt != *d.portBAnt) { m_portBAnt = *d.portBAnt; changed = true; }
    if (d.pttA && m_pttA != *d.pttA) { m_pttA = *d.pttA; changed = true; pttMoved = true; }
    if (d.pttB && m_pttB != *d.pttB) { m_pttB = *d.pttB; changed = true; pttMoved = true; }

    const bool nowPresent = isPresent();
    if (wasPresent != nowPresent) {
        emit presenceChanged(nowPresent);
    }
    if (pendingAntennaA) {
        emit antennaAChanged(*pendingAntennaA);  // "antA" sorts before "tuning"
    }
    if (pttMoved) {
        emit pttChanged(m_pttA, m_pttB);
    }
    if (pendingTuning) {
        emit tuningChanged(*pendingTuning);
    }
    if (changed) {
        emit stateChanged();
    }
}

// ── Commands ─────────────────────────────────────────────────────────────────

void TunerModel::applyDirectTuning(const QMap<QString, QString>& kvs)
{
    if (!kvs.contains(QStringLiteral("tuning"))) return;
    const bool tuning = kvs.value(QStringLiteral("tuning")) == QLatin1String("1");
    if (m_tuning == tuning) return;
    m_tuning = tuning;
    emit tuningChanged(m_tuning);
    emit stateChanged();
}

void TunerModel::clearTuning()
{
    if (!m_tuning) return;
    m_tuning = false;
    emit tuningChanged(false);
    emit stateChanged();
}

void TunerModel::setOperate(bool on)
{
    if (m_handle.isEmpty()) {
        qCDebug(lcTuner) << "TunerModel::setOperate: no handle yet, ignoring";
        return;
    }
    // A new single-command transition supersedes any combined-command hold that
    // may still be armed from a previous setOperateAndBypass() call. Without
    // this, a stale m_heldBypass (e.g., val=true from STANDBY→BYPASS) would
    // block the bypass=0 echo that arrives when the tuner returns to OPERATE,
    // leaving m_bypass stuck at true and the UI showing BYPASS indefinitely.
    m_heldOperate = false;
    m_heldBypass  = false;
    // Neutral intent → Flex "tgxl set handle=<h> mode=" wire (via RadioModel).
    emit operateRequested(on);
    // Optimistic update: reflect the commanded state immediately so the
    // button label stays in sync even before the radio echoes back.
    if (m_operate != on) { m_operate = on; emit stateChanged(); }
}

void TunerModel::setBypass(bool on)
{
    if (m_handle.isEmpty()) {
        qCDebug(lcTuner) << "TunerModel::setBypass: no handle yet, ignoring";
        return;
    }
    // Same hold-clearing rationale as setOperate(): a new single-command
    // transition must not be masked by a hold armed in a prior two-command
    // sequence that never received its confirmation echo.
    m_heldOperate = false;
    m_heldBypass  = false;
    // Neutral intent → Flex "tgxl set handle=<h> bypass=" wire (via RadioModel).
    emit bypassRequested(on);
    // Optimistic update: reflect the commanded state immediately so the
    // button label stays in sync even before the radio echoes back.
    if (m_bypass != on) { m_bypass = on; emit stateChanged(); }
}

void TunerModel::setOperateAndBypass(bool operate, bool bypass)
{
    if (m_handle.isEmpty()) {
        qCDebug(lcTuner) << "TunerModel::setOperateAndBypass: no handle yet, ignoring";
        return;
    }
    // Send only the commands that actually change state.
    const bool opChanged = (m_operate != operate);
    const bool byChanged = (m_bypass  != bypass);
    if (!opChanged && !byChanged) return;

    // Arm the holds BEFORE emitting the intents so that any synchronous echo
    // that arrives (unlikely on a LAN but correct to guard) does not slip past.
    if (opChanged) { m_heldOperate = true; m_heldOperateVal = operate; }
    if (byChanged) { m_heldBypass  = true; m_heldBypassVal  = bypass;  }

    // Emit the neutral intents — RadioModel translates these to the Flex wire.
    // Operate must be commanded before bypass so the hardware state machine
    // sees a valid intermediate state (operate=1) before bypass=1 is applied.
    if (opChanged) emit operateRequested(operate);
    if (byChanged) emit bypassRequested(bypass);

    // Single optimistic update for the combined transition.
    if (opChanged) m_operate = operate;
    if (byChanged) m_bypass  = bypass;
    emit stateChanged();
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

void TunerModel::abortTune()
{
    if (!m_tuning) return;

    // `autotune` is a toggle, not a start. Sent while the tuner is idle it
    // begins a cycle; sent while tuning=1 it aborts the one running, and the
    // tuner acknowledges that with a bare R<seq>|0| rather than the state
    // push a start gets. Captured off the wire between the 4O3A
    // TunerGeniusDesk application and the tuner (three aborts, three for
    // three); FlexLib documents none of this — it exposes one tuner command
    // and an ATUTuneStatus.TGXL_Aborted that nothing ever produces.
    //
    // So an abort is the same command as a start, and the guard above is what
    // separates them. It is the tuning flag the firmware itself is keying
    // off, so the two agree by construction: no tune running, nothing to
    // abort, and no risk of this starting one instead.
    //
    // Crucially the tuner stays in OPERATE across an abort — bypass is never
    // touched — so there is no state to restore afterwards.
    qCDebug(lcTuner) << "TunerModel::abortTune: re-sending autotune to abort";
    if (m_directConn && m_directConn->isConnected()) {
        m_directConn->requestAutotune();
        return;
    }
    if (m_handle.isEmpty()) {
        qCDebug(lcTuner) << "TunerModel::abortTune: no direct conn and no handle, ignoring";
        return;
    }
    // Relayed path: the radio passes `tgxl autotune` to the same firmware,
    // which has no separate notion of start-vs-abort to lose in translation.
    emit autotuneRequested();
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
        // Alerts are pushed to every client, so this arrives whether or not
        // it was this client that asked for the tune.
        connect(m_directConn, &TgxlConnection::alertChanged, this,
                [this](const QString& text) {
            if (m_alert == text) return;
            m_alert = text;
            emit alertChanged(m_alert);
        });
        // Clear a stale alert on disconnect — it describes a tuner we can no
        // longer see, and the tuner's own clear can never reach us now.
        connect(m_directConn, &TgxlConnection::disconnected, this, [this]() {
            if (!m_alert.isEmpty()) {
                m_alert.clear();
                emit alertChanged(m_alert);
            }
            // And the tune: a latched `tuning` is not merely stale display,
            // it is what unlocks abortTune() — which on this transport sends
            // `autotune`, and `autotune` on an idle tuner starts one.
            clearTuning();
            // Same for the port readings: without the direct connection they
            // stop being refreshed, and a frozen frequency is worse than
            // falling back to what the radio can still tell us.
            if (m_havePortInfo) {
                m_havePortInfo = false;
                m_portA = {};
                m_portB = {};
                emit portsChanged();
            }
        });

        // Update relay values from direct state pushes
        connect(m_directConn, &TgxlConnection::stateUpdated, this,
                [this](const QMap<QString, QString>& kvs) {
            applyDirectTuning(kvs);
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
            // The device's rolling peak. Measured on a live transmission it
            // holds for about a second after the last peak, then re-arms --
            // a window, not a latch (`max` is the latch, and nothing here
            // reads it). Our poll is slower than that window, so this is
            // read every time it arrives rather than tracked for changes.
            if (kvs.contains("peak")) {
                float dBm = kvs.value("peak").toFloat();
                m_fwdPeak = std::pow(10.0f, dBm / 10.0f) / 1000.0f;
                meters = true;
            }
            if (kvs.contains("swr")) {
                float rl = kvs.value("swr").toFloat();  // return loss in dB (negative from TGXL)
                float rho = std::pow(10.0f, rl / 20.0f);  // rl is already negative
                float ratio = (rho < 0.999f) ? (1.0f + rho) / (1.0f - rho) : 99.9f;
                m_swr = ratio;
                meters = true;
            }
            if (meters) emit metersChanged(m_fwdPower, m_swr, m_fwdPeak);
        });
        // Also parse antA + meters + the per-port block from 1/sec status
        // poll responses. The port fields appear only in `status`, never in
        // the `state` push, so this is their one arrival point.
        connect(m_directConn, &TgxlConnection::statusUpdated, this,
                [this](const QMap<QString, QString>& kvs) {
            applyDirectTuning(kvs);
            if (kvs.contains(QStringLiteral("modeA"))
                || kvs.contains(QStringLiteral("modeB"))) {
                auto readPort = [&kvs](QChar side) {
                    TunerPortInfo p;
                    p.live = kvs.value(QStringLiteral("mode%1").arg(side)) == QLatin1String("1");
                    p.source = kvs.value(QStringLiteral("flex%1").arg(side));
                    p.freqKhz = kvs.value(QStringLiteral("freq%1").arg(side)).toDouble();
                    p.ptt = kvs.value(QStringLiteral("ptt%1").arg(side)) == QLatin1String("1");
                    return p;
                };
                const TunerPortInfo a = readPort(QLatin1Char('A'));
                const TunerPortInfo b = readPort(QLatin1Char('B'));
                const bool first = !m_havePortInfo;
                if (first || a != m_portA || b != m_portB) {
                    m_portA = a;
                    m_portB = b;
                    m_havePortInfo = true;
                    emit portsChanged();
                }
                // The direct status carries keying for both ports too, so the
                // lamps track without waiting on the radio to relay it.
                if (m_pttA != a.ptt || m_pttB != b.ptt) {
                    m_pttA = a.ptt;
                    m_pttB = b.ptt;
                    emit pttChanged(m_pttA, m_pttB);
                    emit stateChanged();
                }
            }
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
            // The device's rolling peak. Measured on a live transmission it
            // holds for about a second after the last peak, then re-arms --
            // a window, not a latch (`max` is the latch, and nothing here
            // reads it). Our poll is slower than that window, so this is
            // read every time it arrives rather than tracked for changes.
            if (kvs.contains("peak")) {
                float dBm = kvs.value("peak").toFloat();
                m_fwdPeak = std::pow(10.0f, dBm / 10.0f) / 1000.0f;
                meters = true;
            }
            if (kvs.contains("swr")) {
                float rl = kvs.value("swr").toFloat();  // return loss in dB (negative from TGXL)
                float rho = std::pow(10.0f, rl / 20.0f);  // rl is already negative
                float ratio = (rho < 0.999f) ? (1.0f + rho) / (1.0f - rho) : 99.9f;
                m_swr = ratio;
                meters = true;
            }
            if (meters) emit metersChanged(m_fwdPower, m_swr, m_fwdPeak);
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

} // namespace AetherSDR
