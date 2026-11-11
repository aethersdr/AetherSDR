#include "KenwoodBackend.h"
#include "KenwoodPowerLimits.h"

#include <QPointer>
#include <QScopeGuard>

#include <utility>

namespace AetherSDR::kenwood {
namespace {
constexpr unsigned kFreshFt = 1U;
constexpr unsigned kFreshSf = 2U;
constexpr unsigned kFreshAn = 4U;
constexpr unsigned kFreshXv = 8U;
constexpr unsigned kFreshXt = 16U;
constexpr unsigned kFreshRf = 32U;
constexpr unsigned kFreshLp2 = 64U;
constexpr unsigned kFreshTarget = 127U;

std::optional<int> incrementalOffset(const QByteArray& message)
{
    if (message.size() != 7 || !message.startsWith("RF") || (message[2] != '0' && message[2] != '1')) { return std::nullopt; }
    int value = 0;
    for (qsizetype index = 3; index < 7; ++index) {
        if (message[index] < '0' || message[index] > '9') { return std::nullopt; }
        value = value * 10 + message[index] - '0';
    }
    return message[2] == '1' ? -value : value;
}
}

bool KenwoodBackend::tuneTargetCurrent() const
{
    if (!m_tune || !isConnected() || !capabilities().canTransmit) { return false; }
    const QPointer<const KenwoodBackend> guard(this);
    const quint64 generation = m_generation;
    const quint64 serial = m_tune->serial;
    const TxCoordinator::Operation operation = m_tune->operation;
    if (!operation.permitsDispatch(TxCoordinator::monotonicMs()) || !guard || generation != m_generation
        || !m_tune || m_tune->serial != serial) { return false; }
    const TuneRequest& request = *m_tune;
    return m_transmitVfo == request.vfo && m_frequency[request.vfo] == request.frequencyHz
        && m_vfoModes[request.vfo] == request.mode && tunePowerBand() == request.band
        && m_xitEnabled == request.xit && (!request.xit || m_incrementalOffsetHz == request.offsetHz)
        && m_powerLimiterEnabled == request.limiter;
}

void KenwoodBackend::setTune(bool on, int power, const TxCoordinator::Operation& operation,
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
    if (!on) {
        if (m_tune) {
            if (original.permitsCleanupOf(m_tune->operation)
                && (!original.hasLocalIntentRetirement() || original.sameLocalIntent(m_tune->operation))) { cancelTune(); }
            completion.finish();
        } else { setKeying(false, original, completion); }
        return;
    }
    const QPointer<KenwoodBackend> guard(this);
    const quint64 generation = m_generation;
    const quint64 previousSerial = m_tuneSerial;
    if (m_tune && original.sameLocalIntent(m_tune->operation)) {
        // Repeated on updates only UI retirement identity, never the original
        // transmit authority, target snapshot or deadline.
        m_tune->retirementReceipt = original;
        completion.finish();
        return;
    }
    const bool admitted = original.permitsDispatch(TxCoordinator::monotonicMs());
    if (!guard || generation != m_generation) {
        completion.finish();
        original.retireLocalIntent(TxCoordinator::LocalIntentEnd::BeforeKeying);
        return;
    }
    const std::optional<int> band = tunePowerBand();
    if (m_tune || m_tuneObservationDepth || !band || !capabilities().canTransmit
        || !m_powerLimiterEnabled || !m_powerLimits.contains(60 + *band)
        || power < 5 || power > 100 || !admitted || !original.hasLocalIntentRetirement()
        || !m_txStateKnown || m_txObserved || m_keyIntent || m_atuIntent || m_cwIntent) {
        completion.finish();
        original.retireLocalIntent(TxCoordinator::LocalIntentEnd::BeforeKeying);
        if (guard && generation == m_generation && previousSerial == m_tuneSerial && isConnected()) {
            emit configurationWarning(QStringLiteral("TUNE refused: requires a current transmit operation, confirmed RX, physical TX target and native power/source readback; stop an existing transmit intent first."));
        }
        return;
    }
    const quint64 serial = ++m_tuneSerial;
    m_tune = TuneRequest{serial, m_transmitVfo, *band, m_frequency[m_transmitVfo], m_vfoModes[m_transmitVfo],
        *m_xitEnabled, m_xitEnabled.value() ? *m_incrementalOffsetHz : 0, *m_powerLimiterEnabled,
        power, m_clock.elapsed() + 3000, m_controlIngressSequence, 0, {}, TuneRequest::Phase::Snapshot, original, original, completion};
    m_keyIntent = true; refreshAutoModeSafety(); refreshOperatingDisplaySafety(); refreshCwReceiveContext(); refreshTransverterContext(); refreshVfoScanContext(); refreshScanPreferencesContext(); refreshReceiverOptionsContext(); refreshRelativeTuningContext(); refreshPskReceiveContext();
    m_txOperation = original;
    clearTxAudio();
    publishPowerLimits();
    if (!guard || generation != m_generation || !m_tune || m_tune->serial != serial) { return; }
    const QList<QByteArray> queries{"FT", "SF" + QByteArray::number(m_tune->vfo), "AN", "XV", "XT", "RF", "LP2"};
    for (const QByteArray& query : queries) {
        if (!send(query)) {
            if (guard && generation == m_generation && m_tune && m_tune->serial == serial) { cancelTune(QStringLiteral("TUNE preflight query could not be sent.")); }
            return;
        }
        if (!guard || generation != m_generation || !m_tune || m_tune->serial != serial) { return; }
    }
}

void KenwoodBackend::cancelTune(const QString& reason)
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
    if (!m_tune) { return; }
    const QPointer<KenwoodBackend> guard(this);
    const quint64 generation = m_generation;
    TuneRequest retired = std::move(*m_tune);
    m_tune.reset(); refreshAutoModeSafety(); refreshOperatingDisplaySafety(); refreshCwReceiveContext(); refreshTransverterContext(); refreshVfoScanContext(); refreshScanPreferencesContext(); refreshReceiverOptionsContext(); refreshRelativeTuningContext(); refreshPskReceiveContext();
    const quint64 serial = ++m_tuneSerial;
    m_keyIntent = false; refreshAutoModeSafety(); refreshOperatingDisplaySafety(); refreshCwReceiveContext(); refreshTransverterContext(); refreshVfoScanContext(); refreshScanPreferencesContext(); refreshReceiverOptionsContext(); refreshRelativeTuningContext(); refreshPskReceiveContext();
    clearTxAudio();
    const bool handed = retired.phase == TuneRequest::Phase::Handoff || retired.phase == TuneRequest::Phase::Active;
    if (handed) {
        // A failed cleanup write or lost authority cannot establish radio idle.
        m_txStateKnown = false; refreshAutoModeSafety(); refreshOperatingDisplaySafety(); refreshCwReceiveContext(); refreshTransverterContext(); refreshVfoScanContext(); refreshScanPreferencesContext(); refreshReceiverOptionsContext(); refreshRelativeTuningContext(); refreshPskReceiveContext();
        dispatchKeying("RX", false, retired.operation);
    }
    if (guard && generation == m_generation && serial == m_tuneSerial) { publishPowerLimits(); }
    retired.completion.finish();
    retired.operation.retireLocalIntent(handed ? TxCoordinator::LocalIntentEnd::MayHaveKeyed : TxCoordinator::LocalIntentEnd::BeforeKeying);
    retired.retirementReceipt.retireLocalIntent(handed ? TxCoordinator::LocalIntentEnd::MayHaveKeyed : TxCoordinator::LocalIntentEnd::BeforeKeying);
    if (guard && generation == m_generation && serial == m_tuneSerial && isConnected() && !reason.isEmpty()) {
        emit configurationWarning(reason);
    }
}

bool KenwoodBackend::tuneMutation(const QByteArray& command) const
{
    if (!m_tune) { return false; }
    const TuneRequest& request = *m_tune;
    return ((command.startsWith("FA") || command.startsWith("FB")) && command.size() == 13
            && command[1] == (request.vfo == 0 ? 'A' : 'B'))
        || (command.startsWith("FT") && command.size() == 3)
        || (command.startsWith("OM0") && command.size() == 4 && m_receiveVfo == request.vfo)
        || (command.startsWith("XT") && command.size() == 3)
        || (command.startsWith("RF") && command.size() == 7 && request.xit)
        || (command.startsWith("AN") && command.size() == 6)
        || (command.startsWith("XV") && command.size() == 3);
}

void KenwoodBackend::observeTuneMessage(const QByteArray& message, quint64 ingressBatch)
{
    if (!m_tune) { return; }
    bool changed = false;
    unsigned fresh = 0;
    const TuneRequest& request = *m_tune;
    const bool subsequentBatch = ingressBatch > request.minimumReplyBatch;
    if (message == "FT0" || message == "FT1") {
        changed = message[2] - '0' != request.vfo;
        fresh = kFreshFt;
    } else if (message == "FT3" || message == "FR3" || message == "?" || message == "TX0" || message == "TX1") {
        changed = true;
    } else if (message == "TX2") {
        changed = request.phase != TuneRequest::Phase::Handoff && request.phase != TuneRequest::Phase::Active;
    } else if (message == "RX" && request.phase == TuneRequest::Phase::Active) {
        changed = true;
    } else if (const std::optional<VfoState> state = decodeVfoState(message); state && state->vfo == request.vfo) {
        changed = state->frequencyHz != request.frequencyHz || state->mode != request.mode;
        fresh = kFreshSf;
    } else if (const std::optional<qint64> frequency = decodeFrequency(message);
               frequency && message[1] == (request.vfo == 0 ? 'A' : 'B')) {
        changed = *frequency != request.frequencyHz;
    } else if ((message.startsWith("OM0") || message.startsWith("OM1")) && message.size() == 4 && decodeMode(message[3])) {
        const int display = message[2] - '0';
        changed = m_displayModes[display] != *decodeMode(message[3]);
    } else if (message.startsWith("AN") && message.size() == 6 && (message[2] == '1' || message[2] == '2')
               && (message[3] == '0' || message[3] == '1') && (message[4] == '0' || message[4] == '1')
               && (message[5] == '0' || message[5] == '1')) {
        changed = message[4] != '0';
        fresh = kFreshAn;
    } else if (message == "XV0" || message == "XV1") {
        changed = message != "XV0";
        fresh = kFreshXv;
    } else if (message == "XT0" || message == "XT1") {
        changed = (message == "XT1") != request.xit;
        fresh = kFreshXt;
    } else if (const std::optional<int> offset = incrementalOffset(message)) {
        changed = request.xit && *offset != request.offsetHz;
        fresh = kFreshRf;
    } else if (message == "LP20" || message == "LP21") {
        changed = (message == "LP21") != request.limiter;
        fresh = kFreshLp2;
    } else if (const std::optional<PowerLimit> limit = decodePowerLimit(message);
               limit && limit->type == 5 && limit->band == request.band) {
        if (subsequentBatch && (request.phase == TuneRequest::Phase::PowerRead || request.phase == TuneRequest::Phase::PowerWrite)) {
            m_tune->powerReply = limit->watts;
        } else if (request.phase == TuneRequest::Phase::Handoff || request.phase == TuneRequest::Phase::Active) {
            changed = limit->watts != request.watts;
        }
    }
    if (changed) {
        cancelTune(QStringLiteral("TUNE cancelled because radio state or its transmit target changed."));
    } else if (subsequentBatch) { m_tune->fresh |= fresh; }
}

void KenwoodBackend::advanceTunePreflight()
{
    if (!m_tune || m_tuneObservationDepth) { return; }
    const QPointer<KenwoodBackend> guard(this);
    const quint64 generation = m_generation;
    const quint64 serial = m_tune->serial;
    const bool current = tuneTargetCurrent();
    if (!guard || generation != m_generation || !m_tune || m_tune->serial != serial) { return; }
    if (!current) {
        cancelTune(QStringLiteral("TUNE cancelled because its original operation or target is no longer current."));
        return;
    }
    if (m_tune->phase == TuneRequest::Phase::Handoff || m_tune->phase == TuneRequest::Phase::Active) { return; }
    if (!m_txStateKnown || m_txObserved || m_clock.elapsed() >= m_tune->deadlineMs) {
        cancelTune(QStringLiteral("TUNE preflight timed out or receive operation is no longer confirmed."));
        return;
    }
    const QByteArray query = *queryPowerLimit(5, m_tune->band);
    if (m_tune->phase == TuneRequest::Phase::Snapshot) {
        if (m_tune->fresh != kFreshTarget) { return; }
        m_tune->phase = TuneRequest::Phase::PowerRead;
        m_tune->minimumReplyBatch = m_controlIngressSequence;
        if (!send(query) && guard && generation == m_generation && m_tune && m_tune->serial == serial) {
            cancelTune(QStringLiteral("TUNE power preflight could not be sent."));
        }
        return;
    }
    if (!m_tune->powerReply) { return; }
    if (*m_tune->powerReply != m_tune->watts) {
        if (m_tune->phase == TuneRequest::Phase::PowerWrite) {
            cancelTune(QStringLiteral("TUNE refused: the radio did not confirm the requested tuning power."));
            return;
        }
        const QByteArray command = *encodePowerLimit({5, m_tune->band, m_tune->watts});
        m_tune->phase = TuneRequest::Phase::PowerWrite;
        m_tune->minimumReplyBatch = m_controlIngressSequence;
        m_tune->powerReply.reset();
        m_powerLimits.remove(60 + m_tune->band);
        if (!sendObserved(command, query) && guard && generation == m_generation && m_tune && m_tune->serial == serial) {
            cancelTune(QStringLiteral("TUNE power request could not be sent."));
        }
        if (guard && generation == m_generation && m_tune && m_tune->serial == serial) { publishPowerLimits(); }
        return;
    }
    const TxCoordinator::Operation operation = m_tune->operation;
    const TxCoordinator::Completion completion = m_tune->completion;
    m_tune->phase = TuneRequest::Phase::Handoff;
    const bool sent = dispatchKeying("TX2", true, operation, serial);
    if (!guard || generation != m_generation || !m_tune || m_tune->serial != serial) { return; }
    if (!sent) { cancelTune(QStringLiteral("TUNE could not be dispatched through its original transmit operation.")); return; }
    m_tune->phase = TuneRequest::Phase::Active;
    publishPowerLimits();
    completion.finish();
}

void KenwoodBackend::pollTunePreflight(qint64 nowMs)
{
    if (!m_tune) { return; }
    if (m_tune->phase != TuneRequest::Phase::Active && nowMs >= m_tune->deadlineMs) {
        cancelTune(QStringLiteral("TUNE preflight or control handoff timed out; check radio state."));
    } else { advanceTunePreflight(); }
}
} // namespace AetherSDR::kenwood
