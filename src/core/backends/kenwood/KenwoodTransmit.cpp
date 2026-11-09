#include "KenwoodBackend.h"
#include "KenwoodFilter.h"
#include "KenwoodPowerLimits.h"

#include "core/Resampler.h"

#include <QtEndian>
#include <QPointer>
#include <QScopeGuard>

#include <algorithm>
#include <cmath>
#include <cstring>

namespace AetherSDR::kenwood {
namespace {
// Generated audio supplies a 120 ms FIFO cushion. Leave bounded room for
// packet rounding and the converter tail without replaying a stalled pump.
constexpr qsizetype kTxQueueSamples = 3200;
constexpr qint64 kTxQueueAgeMs = 200;
}

ReceiveDispatch KenwoodBackend::requestPcAudioRouting(bool on)
{
    if (!isConnected()) { return ReceiveDispatch::Unsupported; }
    if (m_pcAudioWanted != on || !on) { clearTxAudio(); }
    m_pcAudioWanted = on;
    if (!on) {
        // TX0 uses the radio's ordinary SEND/PTT input. Leaving PC Audio
        // must not replace that radio-owned selection with a microphone.
        return send("MS0") ? ReceiveDispatch::Dispatched : ReceiveDispatch::Unsupported;
    }
    // MS is radio-persisted: only this explicit operator edit writes it.
    // Kenwood PC Command Reference (2019-01-30), p.48, MS.
    const QPointer<KenwoodBackend> guard(this);
    const quint64 generation = m_generation;
    clearTxAudio();
    m_audioInputSelection[1] = -1;
    m_audioInputPending[1] = true;
    m_dataSourceKnown = m_dataLan = false;
    const bool sent = sendObserved("MS103", "MS1");
    if (guard && generation == m_generation) { publishTransmitInputs(); }
    return sent ? ReceiveDispatch::Dispatched : ReceiveDispatch::Unsupported;
}

void KenwoodBackend::observePcAudioRouting(bool on)
{
    if (m_pcAudioWanted != on || !on) { clearTxAudio(); }
    m_pcAudioWanted = on;
    if (isConnected() && on && m_dataSourceKnown && !m_dataLan) {
        emit configurationWarning(QStringLiteral("PC Audio requires DATA SEND audio input LAN. Toggle PC Audio off and on to select it explicitly."));
    }
}

bool KenwoodBackend::dispatchKeying(const QByteArray& command, bool key,
                                   const TxCoordinator::Operation& operation, quint64 tuneSerial)
{
    const QPointer<KenwoodBackend> guard(this);
    const quint64 generation = m_generation;
    if (!isConnected() || (key && !capabilities().canTransmit)) { return false; }
    const TxCoordinator::Command request{operation, key};
    const TxCoordinator::Dispatch dispatch = request.beginDispatch(TxCoordinator::monotonicMs());
    if (!dispatch || !guard || generation != m_generation || !isConnected()
        || (key && !capabilities().canTransmit)
        || (tuneSerial && (!m_tune || m_tune->serial != tuneSerial))) { return false; }
    return send(command) && guard && generation == m_generation && isConnected();
}

void KenwoodBackend::setKeying(bool key, const TxCoordinator::Operation& operation,
                              const TxCoordinator::Completion& completion)
{
    const QPointer<KenwoodBackend> tuningGuard(this);
    const quint64 tuningGeneration = m_generation;
    const auto tuningPublication = qScopeGuard([tuningGuard, tuningGeneration] {
        if (tuningGuard && tuningGeneration == tuningGuard->m_generation) {
            tuningGuard->adoptTuningControl({}, tuningGuard->m_controlIngressSequence);
        }
        if (tuningGuard && tuningGeneration == tuningGuard->m_generation) {
            tuningGuard->adoptFilterBank({}, tuningGuard->m_controlIngressSequence);
        }
        if (tuningGuard && tuningGeneration == tuningGuard->m_generation) {
            tuningGuard->adoptReceiveEq({}, tuningGuard->m_controlIngressSequence);
        }
        if (tuningGuard && tuningGeneration == tuningGuard->m_generation) {
            tuningGuard->adoptReceiveAgc({}, tuningGuard->m_controlIngressSequence);
        }
        if (tuningGuard && tuningGeneration == tuningGuard->m_generation) {
            tuningGuard->adoptReceiveRoutingContext();
        }
        if (tuningGuard && tuningGeneration == tuningGuard->m_generation) {
            tuningGuard->adoptScopeControlsContext();
        }
    });
    const QPointer<KenwoodBackend> guard(this);
    const quint64 generation = m_generation;
    if (m_tune) {
        if (!key) { setTune(false, 0, operation, completion); }
        else { completion.finish(); }
        return;
    }
    if (key && m_audioInputPending[m_pcAudioWanted ? 1 : 0]) {
        completion.finish();
        if (guard && generation == m_generation && isConnected()) { emit configurationWarning(QStringLiteral("Transmit refused: audio input readback is pending.")); }
        return;
    }
    if (key && m_pcAudioWanted && !m_dataLan) {
        completion.finish();
        if (guard && generation == m_generation && isConnected()) { emit configurationWarning(QStringLiteral("Transmit refused: DATA SEND must select LAN for PC Audio.")); }
        return;
    }
    if (!key) {
        if (!operation.permitsCleanup()) {
            if (operation.sameOperation(m_txOperation)) {
                m_keyIntent = false; refreshAutoModeSafety(); refreshOperatingDisplaySafety(); refreshCwReceiveContext(); refreshTransverterContext(); refreshVfoScanContext(); refreshScanPreferencesContext(); refreshReceiverOptionsContext(); refreshRelativeTuningContext(); refreshPskReceiveContext();
                m_txStateKnown = false; refreshAutoModeSafety(); refreshOperatingDisplaySafety(); refreshCwReceiveContext(); refreshTransverterContext(); refreshVfoScanContext(); refreshScanPreferencesContext(); refreshReceiverOptionsContext(); refreshRelativeTuningContext(); refreshPskReceiveContext();
                clearTxAudio();
            }
            if (guard && generation == m_generation) { publishPowerLimits(); }
            completion.finish();
            return;
        }
        m_keyIntent = false; refreshAutoModeSafety(); refreshOperatingDisplaySafety(); refreshCwReceiveContext(); refreshTransverterContext(); refreshVfoScanContext(); refreshScanPreferencesContext(); refreshReceiverOptionsContext(); refreshRelativeTuningContext(); refreshPskReceiveContext();
        clearTxAudio();
    }
    const bool sent = dispatchKeying(key ? (m_pcAudioWanted ? "TX1" : "TX0") : "RX", key, operation);
    if (sent && key) {
        m_txOperation = operation;
        m_keyIntent = true; refreshAutoModeSafety(); refreshOperatingDisplaySafety(); refreshCwReceiveContext(); refreshTransverterContext(); refreshVfoScanContext(); refreshScanPreferencesContext(); refreshReceiverOptionsContext(); refreshRelativeTuningContext(); refreshPskReceiveContext();
        m_lastTxTick = m_clock.elapsed();
        m_txTimer.start();
    }
    if (guard && generation == m_generation) { publishPowerLimits(); }
    // This only completes local handoff, never establishes radio idle.
    completion.finish();
}

void KenwoodBackend::retireAtuIntent()
{
    const QPointer<KenwoodBackend> tuningGuard(this);
    const quint64 tuningGeneration = m_generation;
    const auto tuningPublication = qScopeGuard([tuningGuard, tuningGeneration] {
        if (tuningGuard && tuningGeneration == tuningGuard->m_generation) {
            tuningGuard->adoptTuningControl({}, tuningGuard->m_controlIngressSequence);
        }
        if (tuningGuard && tuningGeneration == tuningGuard->m_generation) {
            tuningGuard->adoptFilterBank({}, tuningGuard->m_controlIngressSequence);
        }
        if (tuningGuard && tuningGeneration == tuningGuard->m_generation) {
            tuningGuard->adoptReceiveEq({}, tuningGuard->m_controlIngressSequence);
        }
        if (tuningGuard && tuningGeneration == tuningGuard->m_generation) {
            tuningGuard->adoptReceiveAgc({}, tuningGuard->m_controlIngressSequence);
        }
        if (tuningGuard && tuningGeneration == tuningGuard->m_generation) {
            tuningGuard->adoptReceiveRoutingContext();
        }
        if (tuningGuard && tuningGeneration == tuningGuard->m_generation) {
            tuningGuard->adoptScopeControlsContext();
        }
    });
    const TxCoordinator::Operation retired = m_atuOperation;
    m_atuIntent = false; refreshAutoModeSafety(); refreshOperatingDisplaySafety(); refreshCwReceiveContext(); refreshTransverterContext(); refreshVfoScanContext(); refreshScanPreferencesContext(); refreshReceiverOptionsContext(); refreshRelativeTuningContext(); refreshPskReceiveContext();
    m_atuProgressObserved = false;
    m_atuOperation = {};
    ++m_atuSerial;
    retired.retireLocalIntent(TxCoordinator::LocalIntentEnd::MayHaveKeyed);
}

void KenwoodBackend::setAtu(bool start, const TxCoordinator::Operation& operation,
                           const TxCoordinator::Completion& completion)
{
    const QPointer<KenwoodBackend> tuningGuard(this);
    const quint64 tuningGeneration = m_generation;
    const auto tuningPublication = qScopeGuard([tuningGuard, tuningGeneration] {
        if (tuningGuard && tuningGeneration == tuningGuard->m_generation) {
            tuningGuard->adoptTuningControl({}, tuningGuard->m_controlIngressSequence);
        }
        if (tuningGuard && tuningGeneration == tuningGuard->m_generation) {
            tuningGuard->adoptFilterBank({}, tuningGuard->m_controlIngressSequence);
        }
        if (tuningGuard && tuningGeneration == tuningGuard->m_generation) {
            tuningGuard->adoptReceiveEq({}, tuningGuard->m_controlIngressSequence);
        }
        if (tuningGuard && tuningGeneration == tuningGuard->m_generation) {
            tuningGuard->adoptReceiveAgc({}, tuningGuard->m_controlIngressSequence);
        }
        if (tuningGuard && tuningGeneration == tuningGuard->m_generation) {
            tuningGuard->adoptReceiveRoutingContext();
        }
        if (tuningGuard && tuningGeneration == tuningGuard->m_generation) {
            tuningGuard->adoptScopeControlsContext();
        }
    });
    const TxCoordinator::Operation original = operation;
    const QPointer<KenwoodBackend> guard(this);
    const quint64 generation = m_generation;
    if (start && m_tune) {
        completion.finish();
        original.retireLocalIntent(TxCoordinator::LocalIntentEnd::BeforeKeying);
        return;
    }
    if (m_atuIntent) {
        const bool sameInput = original.sameLocalIntent(m_atuOperation)
            || (!original.hasLocalIntentRetirement() && !m_atuOperation.hasLocalIntentRetirement()
                && original.sameAuthority(m_atuOperation));
        if (start) {
            completion.finish();
            if (!sameInput) { original.retireLocalIntent(TxCoordinator::LocalIntentEnd::BeforeKeying); }
            return;
        }
        if (!sameInput && (!original.permitsCleanupOf(m_atuOperation)
            || original.hasLocalIntentRetirement())) { completion.finish(); return; }
    }
    const bool retiring = !start && m_atuIntent;
    const TxCoordinator::Operation retired = start ? TxCoordinator::Operation{} : m_atuOperation;
    const quint64 serial = ++m_atuSerial;
    m_atuIntent = start; refreshAutoModeSafety(); refreshOperatingDisplaySafety(); refreshCwReceiveContext(); refreshTransverterContext(); refreshVfoScanContext(); refreshScanPreferencesContext(); refreshReceiverOptionsContext(); refreshRelativeTuningContext(); refreshPskReceiveContext();
    m_atuProgressObserved = false;
    m_atuOperation = start ? original : TxCoordinator::Operation{};
    m_atuMinimumReplyBatch = m_controlIngressSequence;
    const bool accepted = dispatchKeying(start ? "AC111" : "AC110", start, original);
    if (guard && generation == m_generation && serial == m_atuSerial) {
        if (start && !accepted) {
            retireAtuIntent();
        } else if (!start && !accepted && retiring) {
            m_txStateKnown = false; refreshAutoModeSafety(); refreshOperatingDisplaySafety(); refreshCwReceiveContext(); refreshTransverterContext(); refreshVfoScanContext(); refreshScanPreferencesContext(); refreshReceiverOptionsContext(); refreshRelativeTuningContext(); refreshPskReceiveContext();
        }
    }
    retired.retireLocalIntent(TxCoordinator::LocalIntentEnd::MayHaveKeyed);
    if (guard && generation == m_generation && serial == m_atuSerial) { publishPowerLimits(); }
    completion.finish();
    if (guard && generation == m_generation && serial == m_atuSerial && isConnected()) { send("AC"); }
}

std::optional<int> KenwoodBackend::maximumNormalPower() const
{
    if (!m_identityConfirmed || !m_driveOutput || *m_driveOutput
        || m_transmitVfo < 0 || m_transmitVfo > 1 || m_vfoModes[m_transmitVfo].isEmpty()) { return std::nullopt; }
    const std::optional<int> band = powerLimitBandForFrequency(m_frequency[m_transmitVfo]);
    if (!band) { return std::nullopt; }
    // PC Command Reference (2019-01-30), p.53: 013 represents 12.5 W
    // for 70 MHz AM. These command ceilings do not authorize transmission.
    const bool am = m_vfoModes[m_transmitVfo] == QLatin1String("AM")
        || m_vfoModes[m_transmitVfo] == QLatin1String("DAM");
    return *band == 11 ? (am ? 13 : 50) : (am ? 25 : 100);
}

void KenwoodBackend::setTxPower(int percent)
{
    const std::optional<int> maximum = maximumNormalPower();
    if (!maximum || percent < 5 || percent > *maximum) { return; }
    sendObserved("PC" + QByteArray::number(percent).rightJustified(3, '0'), "PC");
}

void KenwoodBackend::setMicGain(int percent)
{
    if (percent < 0 || percent > 100) { return; }
    sendObserved("MG" + QByteArray::number(percent).rightJustified(3, '0'), "MG");
}

void KenwoodBackend::setTxMonitor(bool on, int level)
{
    if (level < 0 || level > 100) { return; }
    // PC Command Reference (2019-01-30), p.46: ML is 0..20,
    // independent of the MO0 enable switch.
    if (!sendObserved("ML" + QByteArray::number(qRound(level / 5.0)).rightJustified(3, '0'), "ML")) { return; }
    sendObserved(on ? "MO01" : "MO00", "MO0");
}

ReceiveDispatch KenwoodBackend::requestTransmitControl(const TransmitControlRequest& request,
                                                      TunePowerContext context)
{
    if (!isConnected() || !request.valid()) { return ReceiveDispatch::Unsupported; }
    if (!capabilities().radioTransmitSettings->fields.contains(request.field)) { return ReceiveDispatch::Unsupported; }
    if (request.field == TransmitControlRequest::Field::TunePower) {
        const std::optional<int> band = tunePowerBand();
        const int watts = std::get<int>(request.value);
        if (!band || watts < 5 || context != TunePowerContext::Deferred || !m_txStateKnown || m_txObserved
            || m_keyIntent || m_atuIntent || m_cwIntent) { return ReceiveDispatch::Unsupported; }
        const QByteArray query = *queryPowerLimit(5, *band);
        const QByteArray command = *encodePowerLimit({5, *band, watts});
        const QPointer<KenwoodBackend> guard(this);
        const quint64 generation = m_generation;
        m_powerLimits.remove(60 + *band);
        const bool sent = sendObserved(command, query);
        if (guard && generation == m_generation) { publishPowerLimits(); }
        return sent ? ReceiveDispatch::Dispatched : ReceiveDispatch::Unsupported;
    }
    if (request.field == TransmitControlRequest::Field::Filter) {
        if (!txFilterKnown()) { return ReceiveDispatch::Unsupported; }
        const QString mode = m_vfoModes[m_transmitVfo];
        const int vfo = m_transmitVfo;
        const int group = *txFilterGroup(mode);
        const TxPassband edges = std::get<TxPassband>(request.value);
        const std::optional<QList<QByteArray>> commands = encodeTxFilter(group, edges.lowHz, edges.highHz);
        if (!commands) { return ReceiveDispatch::Unsupported; }
        const QList<QByteArray> queries = queryTxFilter(group);
        const QPointer<KenwoodBackend> guard(this);
        const quint64 generation = m_generation;
        for (int axis = 0; axis < 2; ++axis) {
            const int current = axis == 0 ? *m_txFilterLow[group] : *m_txFilterHigh[group];
            const int requested = axis == 0 ? edges.lowHz : edges.highHz;
            if (current != requested && !sendObserved((*commands)[axis], queries[axis])) { return ReceiveDispatch::Unsupported; }
            if (!guard || generation != m_generation || m_transmitVfo != vfo || m_vfoModes[vfo] != mode) {
                return ReceiveDispatch::Unsupported;
            }
        }
        return ReceiveDispatch::Dispatched;
    }
    if (request.field == TransmitControlRequest::Field::RfPower) {
        const int percent = std::get<int>(request.value);
        const std::optional<int> maximum = maximumNormalPower();
        if (!maximum || percent < 5 || percent > *maximum) { return ReceiveDispatch::Unsupported; }
        setTxPower(percent);
        return ReceiveDispatch::Dispatched;
    }
    if (request.field == TransmitControlRequest::Field::MicGain) {
        setMicGain(std::get<int>(request.value));
        return ReceiveDispatch::Dispatched;
    }
    if (request.field == TransmitControlRequest::Field::MonitorEnabled) {
        const MonitorSetting monitor = std::get<MonitorSetting>(request.value);
        return sendObserved(monitor.enabled ? "MO01" : "MO00", "MO0")
            ? ReceiveDispatch::Dispatched : ReceiveDispatch::Unsupported;
    }
    if (request.field == TransmitControlRequest::Field::MonitorLevel) {
        const MonitorSetting monitor = std::get<MonitorSetting>(request.value);
        return sendObserved("ML" + QByteArray::number(qRound(monitor.level / 5.0)).rightJustified(3, '0'), "ML")
            ? ReceiveDispatch::Dispatched : ReceiveDispatch::Unsupported;
    }
    if (request.field == TransmitControlRequest::Field::CwlEnabled) {
        if (!m_cwLowerSideband.has_value()) { return ReceiveDispatch::Unsupported; }
        const bool lower = std::get<bool>(request.value);
        return sendObserved(lower ? "EX00505 001" : "EX00505 000", "EX00505")
            ? ReceiveDispatch::Dispatched : ReceiveDispatch::Unsupported;
    }
    if (request.field == TransmitControlRequest::Field::CwSidetone) {
        if (m_radioSidetone < 0) { return ReceiveDispatch::Unsupported; }
        const bool enabled = std::get<bool>(request.value);
        if (enabled == (m_radioSidetone != 1)) {
            return send("EX00102") ? ReceiveDispatch::Dispatched : ReceiveDispatch::Unsupported;
        }
        // PC Command Reference (2019-01-30), p.25: 000 follows monitor,
        // 001 is Off, and 002..021 are fixed levels 1..20.
        return sendObserved(enabled ? "EX00102 000" : "EX00102 001", "EX00102")
            ? ReceiveDispatch::Dispatched : ReceiveDispatch::Unsupported;
    }
    if (request.field == TransmitControlRequest::Field::CwSpeed) {
        const int wpm = std::get<int>(request.value);
        if (wpm < 5 || wpm > 60) { return ReceiveDispatch::Unsupported; }
        setCwSpeed(wpm);
        return ReceiveDispatch::Dispatched;
    }
    if (request.field == TransmitControlRequest::Field::CwPitch) {
        const int hz = std::get<int>(request.value);
        if (hz < 300 || hz > 1100 || hz % 5 != 0) { return ReceiveDispatch::Unsupported; }
        setCwPitch(hz);
        return ReceiveDispatch::Dispatched;
    }
    if (request.field == TransmitControlRequest::Field::CwBreakIn) {
        setCwBreakIn(std::get<bool>(request.value));
        return ReceiveDispatch::Dispatched;
    }
    if (request.field == TransmitControlRequest::Field::CwDelay) {
        const int ms = std::get<int>(request.value);
        if (ms > 1000) { return ReceiveDispatch::Unsupported; }
        const int delay = qRound(ms / 50.0) * 50;
        return sendObserved("SD" + QByteArray::number(delay).rightJustified(4, '0'), "SD")
            ? ReceiveDispatch::Dispatched : ReceiveDispatch::Unsupported;
    }
    if (request.field == TransmitControlRequest::Field::CwIambicMode) {
        const int mode = std::get<int>(request.value);
        return sendObserved("EX00502 " + QByteArray::number(mode).rightJustified(3, '0'), "EX00502")
            ? ReceiveDispatch::Dispatched : ReceiveDispatch::Unsupported;
    }
    if (request.field == TransmitControlRequest::Field::CwSwap) {
        return sendObserved(std::get<bool>(request.value) ? "EX00503 001" : "EX00503 000", "EX00503")
            ? ReceiveDispatch::Dispatched : ReceiveDispatch::Unsupported;
    }
    if (request.field == TransmitControlRequest::Field::ProcessorEnabled) {
        const ProcessorSetting processor = std::get<ProcessorSetting>(request.value);
        return sendObserved(processor.enabled ? "PR01" : "PR00", "PR0")
            ? ReceiveDispatch::Dispatched : ReceiveDispatch::Unsupported;
    }
    if (request.field == TransmitControlRequest::Field::ProcessorLevel) {
        if (m_processorOutput < 0) { return ReceiveDispatch::Unsupported; }
        const ProcessorSetting processor = std::get<ProcessorSetting>(request.value);
        return sendObserved("PL" + QByteArray::number(processor.level).rightJustified(3, '0')
            + QByteArray::number(m_processorOutput).rightJustified(3, '0'), "PL")
            ? ReceiveDispatch::Dispatched : ReceiveDispatch::Unsupported;
    }
    return ReceiveDispatch::Unsupported;
}

QString KenwoodBackend::sendCwText(const QString& text, const TxCoordinator::Operation& operation,
                                 const TxCoordinator::Completion& completion)
{
    const QPointer<KenwoodBackend> tuningGuard(this);
    const quint64 tuningGeneration = m_generation;
    const auto tuningPublication = qScopeGuard([tuningGuard, tuningGeneration] {
        if (tuningGuard && tuningGeneration == tuningGuard->m_generation) {
            tuningGuard->adoptTuningControl({}, tuningGuard->m_controlIngressSequence);
        }
        if (tuningGuard && tuningGeneration == tuningGuard->m_generation) {
            tuningGuard->adoptFilterBank({}, tuningGuard->m_controlIngressSequence);
        }
        if (tuningGuard && tuningGeneration == tuningGuard->m_generation) {
            tuningGuard->adoptReceiveEq({}, tuningGuard->m_controlIngressSequence);
        }
        if (tuningGuard && tuningGeneration == tuningGuard->m_generation) {
            tuningGuard->adoptReceiveAgc({}, tuningGuard->m_controlIngressSequence);
        }
        if (tuningGuard && tuningGeneration == tuningGuard->m_generation) {
            tuningGuard->adoptReceiveRoutingContext();
        }
        if (tuningGuard && tuningGeneration == tuningGuard->m_generation) {
            tuningGuard->adoptScopeControlsContext();
        }
    });
    const QPointer<KenwoodBackend> guard(this);
    const quint64 generation = m_generation;
    const RadioCapabilities caps = capabilities();
    if (m_tune || !caps.canTransmit || m_transmitVfo < 0
        || (m_vfoModes[m_transmitVfo] != QLatin1String("CW") && m_vfoModes[m_transmitVfo] != QLatin1String("CWR"))) {
        completion.finish();
        return QStringLiteral("The radio must permit transmission and confirm CW or CWR on the transmit VFO.");
    }
    if (text.isEmpty() || text.size() > 24) {
        completion.finish();
        return QStringLiteral("TS-890S text keyer messages require 1–24 characters.");
    }
    for (QChar character : text) {
        if (!caps.cwTextAllowedCharacters.contains(character)) {
            completion.finish();
            return QStringLiteral("The message contains a character the TS-890S keyer cannot send.");
        }
    }
    if (m_cwIntent && !operation.sameLocalIntent(m_cwOperation)
        && (operation.hasLocalIntentRetirement() || m_cwOperation.hasLocalIntentRetirement()
            || !operation.sameAuthority(m_cwOperation))) {
        completion.finish();
        return QStringLiteral("The native CW queue is owned by another input; cancel it before sending.");
    }
    const quint64 serial = ++m_cwSerial;
    // KY reports buffer availability, not qualified drain. Keep this exact
    // input until explicit abort, authority loss or teardown; RX may be a QSK gap.
    m_cwIntent = true; refreshAutoModeSafety(); refreshOperatingDisplaySafety(); refreshCwReceiveContext(); refreshTransverterContext(); refreshVfoScanContext(); refreshScanPreferencesContext(); refreshReceiverOptionsContext(); refreshRelativeTuningContext(); refreshPskReceiveContext();
    m_cwOperation = operation;
    const bool sent = dispatchKeying("KY2" + text.toLatin1(), true, operation);
    if (!guard || generation != m_generation || serial != m_cwSerial) {
        completion.finish();
        return QStringLiteral("The CW input was cancelled during control handoff.");
    }
    if (!sent) { abortCwText(operation); }
    if (guard && generation == m_generation && serial == m_cwSerial) { publishPowerLimits(); }
    completion.finish();
    return sent ? QString() : QStringLiteral("The transmit operation is no longer admitted.");
}

void KenwoodBackend::abortCwText(const TxCoordinator::Operation& operation,
                               const TxCoordinator::Completion& completion)
{
    const QPointer<KenwoodBackend> tuningGuard(this);
    const quint64 tuningGeneration = m_generation;
    const auto tuningPublication = qScopeGuard([tuningGuard, tuningGeneration] {
        if (tuningGuard && tuningGeneration == tuningGuard->m_generation) {
            tuningGuard->adoptTuningControl({}, tuningGuard->m_controlIngressSequence);
        }
        if (tuningGuard && tuningGeneration == tuningGuard->m_generation) {
            tuningGuard->adoptFilterBank({}, tuningGuard->m_controlIngressSequence);
        }
        if (tuningGuard && tuningGeneration == tuningGuard->m_generation) {
            tuningGuard->adoptReceiveEq({}, tuningGuard->m_controlIngressSequence);
        }
        if (tuningGuard && tuningGeneration == tuningGuard->m_generation) {
            tuningGuard->adoptReceiveAgc({}, tuningGuard->m_controlIngressSequence);
        }
        if (tuningGuard && tuningGeneration == tuningGuard->m_generation) {
            tuningGuard->adoptReceiveRoutingContext();
        }
        if (tuningGuard && tuningGeneration == tuningGuard->m_generation) {
            tuningGuard->adoptScopeControlsContext();
        }
    });
    const TxCoordinator::Operation original = operation;
    const QPointer<KenwoodBackend> guard(this);
    const quint64 generation = m_generation;
    if (m_cwIntent) {
        const bool sameInput = original.sameLocalIntent(m_cwOperation)
            || (!original.hasLocalIntentRetirement() && !m_cwOperation.hasLocalIntentRetirement()
                && original.sameAuthority(m_cwOperation));
        if (!sameInput && (!original.permitsCleanupOf(m_cwOperation)
            || original.hasLocalIntentRetirement())) { completion.finish(); return; }
    }
    const bool retiring = m_cwIntent;
    const TxCoordinator::Operation retired = m_cwOperation;
    const quint64 serial = ++m_cwSerial;
    m_cwIntent = false; refreshAutoModeSafety(); refreshOperatingDisplaySafety(); refreshCwReceiveContext(); refreshTransverterContext(); refreshVfoScanContext(); refreshScanPreferencesContext(); refreshReceiverOptionsContext(); refreshRelativeTuningContext(); refreshPskReceiveContext();
    m_cwOperation = {};
    const bool accepted = dispatchKeying("KY0", false, original);
    if (guard && generation == m_generation && serial == m_cwSerial
        && !accepted && retiring) { m_txStateKnown = false; refreshAutoModeSafety(); refreshOperatingDisplaySafety(); refreshCwReceiveContext(); refreshTransverterContext(); refreshVfoScanContext(); refreshScanPreferencesContext(); refreshReceiverOptionsContext(); refreshRelativeTuningContext(); refreshPskReceiveContext(); }
    retired.retireLocalIntent(TxCoordinator::LocalIntentEnd::MayHaveKeyed);
    if (guard && generation == m_generation && serial == m_cwSerial) { publishPowerLimits(); }
    completion.finish();
}

void KenwoodBackend::setCwSpeed(int wpm)
{
    if (wpm < 5 || wpm > 60) { return; }
    sendObserved("KS" + QByteArray::number(wpm).rightJustified(3, '0'), "KS");
}

void KenwoodBackend::setCwPitch(int hz)
{
    if (hz < 300 || hz > 1100 || hz % 5 != 0) { return; }
    sendObserved("PT" + QByteArray::number((hz - 300) / 5).rightJustified(3, '0'), "PT");
}

void KenwoodBackend::setCwBreakIn(bool on)
{
    sendObserved(on ? "BI1" : "BI0", "BI");
}

void KenwoodBackend::setSpeechProcessor(bool on, int level)
{
    if (level < 0 || level > 100 || m_processorOutput < 0) { return; }
    const quint64 generation = m_generation;
    if (!sendObserved("PL" + QByteArray::number(level).rightJustified(3, '0')
        + QByteArray::number(m_processorOutput).rightJustified(3, '0'), "PL") || generation != m_generation) { return; }
    sendObserved(on ? "PR01" : "PR00", "PR0");
}

void KenwoodBackend::clearTxAudio()
{
    m_txTimer.stop();
    m_txQueue.clear();
    m_txQueuedSamples = 0;
    m_txRemainder.clear();
    m_audioContext = {};
    m_txResampler.reset();
    m_txInputRate = 0;
}

void KenwoodBackend::submitTxAudio(const QByteArray& stereo, int rate, TxAudioSource source,
                                  const TxCoordinator::Context& context)
{
    Q_UNUSED(source);
    if (!isConnected() || m_tune || !m_keyIntent || !m_pcAudioWanted || !m_dataLan
        || !context.permitsDispatch(TxCoordinator::monotonicMs())
        || !m_txOperation.permitsDispatch(TxCoordinator::monotonicMs())
        || (rate != 24000 && rate != 48000) || stereo.isEmpty()
        || stereo.size() % 4 != 0 || stereo.size() > 32768) { return; }
    if (!m_audioContext.sameContext(context) || m_txInputRate != rate) {
        clearTxAudio();
        m_audioContext = context;
        m_txInputRate = rate;
        // A 10% transition keeps finite-stream delay below one 20 ms packet
        // at both input rates, within the existing bounded queue budget.
        m_txResampler = std::make_unique<Resampler>(rate, 16000, 8192, 10.0);
    }
    const qsizetype frames = stereo.size() / 4;
    QVector<float> mono(frames);
    for (qsizetype index = 0; index < frames; ++index) {
        qint16 left;
        qint16 right;
        std::memcpy(&left, stereo.constData() + index * 4, sizeof(left));
        std::memcpy(&right, stereo.constData() + index * 4 + 2, sizeof(right));
        mono[index] = (static_cast<float>(left) + static_cast<float>(right)) / 65536.0f;
    }
    const QByteArray converted = m_txResampler->process(mono.constData(), mono.size());
    const qsizetype count = converted.size() / sizeof(float);
    if (count + m_txQueuedSamples + m_txRemainder.size() > kTxQueueSamples) {
        // Drop the complete old conversion epoch, never replay a stale tail.
        clearTxAudio();
        return;
    }
    const qsizetype previous = m_txRemainder.size();
    m_txRemainder.resize(previous + count);
    std::memcpy(m_txRemainder.data() + previous, converted.constData(), converted.size());
    while (m_txRemainder.size() >= 320) {
        m_txQueue.push_back({m_txRemainder.first(320), context, m_clock.elapsed()});
        m_txQueuedSamples += 320;
        m_txRemainder.remove(0, 320);
    }
    if (!m_txTimer.isActive()) {
        m_lastTxTick = m_clock.elapsed();
        m_txTimer.start();
    }
}

void KenwoodBackend::pumpTxAudio()
{
    const QPointer<KenwoodBackend> guard(this);
    if (m_txQueue.empty()) { m_lastTxTick = m_clock.elapsed(); return; }
    const qint64 now = m_clock.elapsed();
    if (!isConnected() || !m_keyIntent || !m_pcAudioWanted || !m_dataLan
        || !capabilities().canTransmit || now - m_lastTxTick > 60
        || now - m_txQueue.front().queuedMs > kTxQueueAgeMs) {
        clearTxAudio();
        return;
    }
    m_lastTxTick = now;
    AudioChunk chunk = std::move(m_txQueue.front());
    m_txQueue.pop_front();
    m_txQueuedSamples -= chunk.samples.size();
    const TxCoordinator::Dispatch authority = m_txOperation.beginDispatch(TxCoordinator::monotonicMs());
    const TxCoordinator::Dispatch media = chunk.context.beginDispatch(TxCoordinator::monotonicMs());
    if (!authority || !media) { clearTxAudio(); return; }
    const std::optional<QByteArray> packet = encodeTxAudio(chunk.samples, m_txSequence++, 0.01f);
    if (!packet) { clearTxAudio(); return; }
    if (m_transport.audio) {
        const std::function<bool(const QByteArray&)> sink = m_transport.audio;
        const bool accepted = sink(*packet);
        if (guard && !accepted) { clearTxAudio(); }
    } else if (!m_txUdp || m_txUdp->writeDatagram(*packet, m_peer, m_audioPort) != packet->size()) {
        clearTxAudio();
    }
}

int KenwoodBackend::finishTxAudio(const TxCoordinator::Context& context)
{
    if (!context.sameContext(m_audioContext) || !m_keyIntent
        || !context.permitsDispatch(TxCoordinator::monotonicMs())
        || !m_txOperation.permitsDispatch(TxCoordinator::monotonicMs())) { return 0; }
    if (!m_txResampler) { return static_cast<int>(m_txQueue.size() * 20); }
    const QByteArray tail = m_txResampler->drain();
    m_txResampler.reset();
    m_txInputRate = 0;
    const qsizetype samples = tail.size() / sizeof(float);
    const qsizetype padded = ((samples + m_txRemainder.size() + 319) / 320) * 320;
    if (padded + m_txQueuedSamples > kTxQueueSamples) { clearTxAudio(); return 0; }
    const qsizetype previous = m_txRemainder.size();
    m_txRemainder.resize(previous + samples);
    std::memcpy(m_txRemainder.data() + previous, tail.constData(), tail.size());
    while (!m_txRemainder.isEmpty()) {
        QVector<float> packet = m_txRemainder.first(std::min<qsizetype>(320, m_txRemainder.size()));
        m_txRemainder.remove(0, packet.size());
        packet.resize(320); // Silence pads this same finite context, never another operation.
        m_txQueue.push_back({std::move(packet), context, m_clock.elapsed()});
        m_txQueuedSamples += 320;
    }
    return static_cast<int>(m_txQueue.size() * 20);
}
} // namespace AetherSDR::kenwood
