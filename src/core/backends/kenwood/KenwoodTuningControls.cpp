#include "KenwoodBackend.h"

#include <QMetaType>
#include <QPointer>
#include <QStringList>
#include <QVariant>

namespace AetherSDR::kenwood {
namespace {
QByteArray tuningCommand(const QString& field)
{
    if (field == QLatin1String("receiveFine")) { return "FS"; }
    if (field == QLatin1String("mhzStep")) { return "MH"; }
    if (field == QLatin1String("locked")) { return "LK"; }
    return {};
}
}

QString KenwoodBackend::tuningControlContext() const
{
    return QStringLiteral("%1:%2:%3:%4:%5").arg(receiveControlContext()).arg(m_transmitVfo)
        .arg(m_receiveVfo >= 0 && m_receiveVfo <= 1 && m_frequency[m_receiveVfo] > 0)
        .arg(m_tune.has_value()).arg(m_tuneSerial);
}

bool KenwoodBackend::tuningControlsWritable() const
{
    return isConnected() && m_identityConfirmed && m_receiveVfo >= 0 && m_receiveVfo <= 1
        && m_frequency[m_receiveVfo] > 0 && encodeMode(m_mode).has_value()
        && m_tfSet && !*m_tfSet && m_txStateKnown && !m_txObserved
        && !m_keyIntent && !m_cwIntent && !m_atuIntent && !m_tune
        && m_tuningControlContext == tuningControlContext();
}

QVariantMap KenwoodBackend::tuningControlStatus() const
{
    const bool ready = tuningControlsWritable();
    QVariantMap result{{"statusRevision", QString::number(m_tuningControlStatusRevision)}, {"context", QStringLiteral("%1:%2").arg(m_generation).arg(m_tuningControlRevision)},
        {"editable", ready},
        {"reason", ready ? QString{} : QStringLiteral("Requires confirmed receive operation, physical receive VFO, current mode and TF-SET off.")}};
    if (m_tuningControlContext == tuningControlContext()) {
        for (auto it = m_tuningControlValues.cbegin(); it != m_tuningControlValues.cend(); ++it) {
            result.insert(it.key(), it.value());
        }
    }
    QVariantMap pending;
    for (auto it = m_tuningControlPending.cbegin(); it != m_tuningControlPending.cend(); ++it) {
        pending.insert(it.key(), true);
    }
    result.insert(QStringLiteral("pending"), pending);
    return result;
}

void KenwoodBackend::publishTuningControls()
{
    ++m_tuningControlStatusRevision;
    emit extensionStatus(QStringLiteral("kenwood"), QStringLiteral("tuning"), tuningControlStatus());
}

void KenwoodBackend::resetTuningControls()
{
    ++m_tuningControlRevision;
    m_tuningControlContext.clear();
    m_tuningControlValues.clear();
    m_tuningControlPending.clear();
    m_tuningControlMinimumBatches.clear();
    m_tuningControlsInterested = false;
    m_tuningControlsQueried = false;
    m_tuningControlMinimumBatch = m_controlIngressSequence;
}

bool KenwoodBackend::refreshTuningControlContext()
{
    const QString context = tuningControlContext();
    if (context == m_tuningControlContext) { return false; }
    m_tuningControlContext = context;
    ++m_tuningControlRevision;
    m_tuningControlValues.clear();
    m_tuningControlPending.clear();
    m_tuningControlMinimumBatches.clear();
    m_tuningControlsQueried = false;
    m_tuningControlMinimumBatch = m_controlIngressSequence;
    const QPointer<KenwoodBackend> guard(this);
    const quint64 generation = m_generation;
    const quint64 revision = m_tuningControlRevision;
    publishTuningControls();
    if (guard && generation == m_generation && revision == m_tuningControlRevision) { queryTuningControls(); }
    return true;
}

void KenwoodBackend::queryTuningControls()
{
    if (!m_tuningControlsInterested || m_tuningControlsQueried || !tuningControlsWritable()) { return; }
    m_tuningControlsQueried = true;
    m_tuningControlMinimumBatch = m_controlIngressSequence;
    const QPointer<KenwoodBackend> guard(this);
    const quint64 generation = m_generation;
    const quint64 revision = m_tuningControlRevision;
    for (const QByteArray& command : {QByteArray("FS"), QByteArray("MH"), QByteArray("LK")}) {
        const bool accepted = send(command);
        if (!guard || generation != m_generation || revision != m_tuningControlRevision
            || !tuningControlsWritable()) { return; }
        if (!accepted) {
            m_tuningControlsQueried = false;
            return;
        }
    }
}

void KenwoodBackend::adoptTuningControl(const QByteArray& message, quint64 ingressBatch)
{
    const QPointer<KenwoodBackend> guard(this);
    const quint64 generation = m_generation;
    refreshTuningControlContext();
    if (!guard || generation != m_generation || !m_tuningControlsInterested
        || !tuningControlsWritable() || ingressBatch <= m_tuningControlMinimumBatch) { return; }
    QString field;
    bool value = false;
    // PC Command Reference 2019-01-30 p.34: FS setter has one bit;
    // the answer has independent RX and TX bits. Never write the TX bit.
    if (message.size() == 4 && message.startsWith("FS")
        && (message[2] == '0' || message[2] == '1') && (message[3] == '0' || message[3] == '1')) {
        field = QStringLiteral("receiveFine");
        value = message[2] == '1';
    } else if (message.size() == 3 && (message.startsWith("MH") || message.startsWith("LK"))
        && (message[2] == '0' || message[2] == '1')) {
        field = message.startsWith("MH") ? QStringLiteral("mhzStep") : QStringLiteral("locked");
        value = message[2] == '1';
    } else {
        return;
    }
    if (ingressBatch <= m_tuningControlMinimumBatches.value(field, 0)) { return; }
    m_tuningControlValues.insert(field, value);
    m_tuningControlPending.remove(field);
    if (field == QLatin1String("receiveFine")) {
        m_tuningControlValues.insert(QStringLiteral("transmitFine"), message[3] == '1');
    }
    publishTuningControls();
}

bool KenwoodBackend::handleTuningExtension(const QString& verb, quint64 requestId, const QVariant& argument)
{
    if (verb != QLatin1String("tuning.get") && verb != QLatin1String("tuning.set")) { return false; }
    const QPointer<KenwoodBackend> guard(this);
    const quint64 generation = m_generation;
    const bool alreadyInterested = m_tuningControlsInterested;
    const bool changed = refreshTuningControlContext();
    if (!guard || generation != m_generation) { return true; }
    if (verb == QLatin1String("tuning.get")) {
        if (argument.isValid()) {
            emit extensionError(requestId, QStringLiteral("Tuning query does not accept an argument."));
            return true;
        }
        m_tuningControlsInterested = true;
        // A changed interested context already dispatched its initial reads.
        // An explicit get in the same context always refreshes all three fields.
        if (!changed) { m_tuningControlsQueried = false; }
        if (!changed || !alreadyInterested) { queryTuningControls(); }
        if (guard && generation == m_generation) { emit extensionResult(requestId, tuningControlStatus()); }
        return true;
    }
    const QVariantMap fields = argument.toMap();
    const QString field = fields.value(QStringLiteral("field")).toString();
    const QByteArray command = tuningCommand(field);
    if (argument.typeId() != QMetaType::QVariantMap || fields.size() != 3
        || fields.value(QStringLiteral("context")).typeId() != QMetaType::QString
        || fields.value(QStringLiteral("field")).typeId() != QMetaType::QString
        || fields.value(QStringLiteral("value")).typeId() != QMetaType::Bool || command.isEmpty()
        || fields.value(QStringLiteral("context")) != tuningControlStatus().value(QStringLiteral("context"))
        || !tuningControlsWritable() || !m_tuningControlValues.contains(field)
        || m_tuningControlPending.contains(field)) {
        emit extensionError(requestId, QStringLiteral("Tuning request requires a current editable field and a boolean value."));
        return true;
    }
    const bool requested = fields.value(QStringLiteral("value")).toBool();
    const quint64 revision = m_tuningControlRevision;
    const quint64 ticket = ++m_tuningControlTicket;
    m_tuningControlPending.insert(field, ticket);
    m_tuningControlMinimumBatches.insert(field, m_controlIngressSequence);
    const auto current = [guard, generation, revision] {
        return guard && generation == guard->m_generation && revision == guard->m_tuningControlRevision
            && guard->tuningControlsWritable();
    };
    const auto abandon = [guard, field, ticket] {
        if (guard && guard->m_tuningControlPending.value(field, 0) == ticket) {
            guard->m_tuningControlPending.remove(field);
            guard->publishTuningControls();
        }
    };
    publishTuningControls();
    if (!current() || m_tuningControlPending.value(field, 0) != ticket) {
        abandon();
        if (guard && generation == m_generation) { emit extensionError(requestId, QStringLiteral("Receive tuning context changed before dispatch.")); }
        return true;
    }
    if (!send(command + (requested ? '1' : '0'))) {
        abandon();
        if (guard && generation == m_generation) { emit extensionError(requestId, QStringLiteral("Radio tuning command was not dispatched.")); }
        return true;
    }
    if (!current()) {
        abandon();
        if (guard && generation == m_generation) { emit extensionError(requestId, QStringLiteral("Receive tuning context changed during dispatch.")); }
        return true;
    }
    // A synchronous injected readback may already have completed this ticket.
    if (m_tuningControlPending.value(field, 0) == ticket && !send(command)) {
        abandon();
        if (guard && generation == m_generation) { emit extensionError(requestId, QStringLiteral("Radio tuning readback query was not dispatched.")); }
        return true;
    }
    if (current()) {
        emit extensionResult(requestId, true);
    } else if (guard && generation == m_generation) {
        emit extensionError(requestId, QStringLiteral("Receive tuning context changed during readback query."));
    }
    return true;
}
} // namespace AetherSDR::kenwood

namespace AetherSDR::kenwood {
namespace {
const QStringList kRelativeSurvey{"FR", "FT", "TS", "XV", "MV", "QR", "FA", "FB", "SF0", "SF1", "FS", "MH", "LK"};
bool relativeSequence(const QVariant& value)
{
    if (value.typeId() != QMetaType::QString || value.toString().isEmpty() || value.toString().size() > 20) { return false; }
    bool ok = false; const quint64 number = value.toString().toULongLong(&ok);
    return ok && value.toString() == QString::number(number);
}
bool relativeInteger(const QVariant& value, int maximum)
{
    return value.typeId() == QMetaType::Int && value.toInt() >= 0 && value.toInt() <= maximum;
}
}

void KenwoodBackend::refreshRelativeTuningContext()
{
    const QString safety = QStringLiteral("%1:%2:%3:%4:%5:%6:%7:%8:%9:%10")
        .arg(m_generation).arg(isConnected()).arg(m_identityConfirmed).arg(m_txStateKnown).arg(m_txObserved)
        .arg(m_keyIntent).arg(m_cwIntent).arg(m_atuIntent).arg(m_tune ? m_tune->serial : 0).arg(m_receiverOptionsEpoch);
    if (safety == m_relativeSafety) { return; }
    m_relativeSafety = safety; ++m_relativeEpoch; m_relativeOwned.clear();
    m_relativeQueue.clear(); m_relativePending.clear(); ++m_relativeReadTicket;
    if (m_relativeAction) {
        m_relativeLastAction = {{"state", m_relativeHanded ? "indeterminate" : "rejected"},
            {"reason", "Receive/session ownership changed; no retry or inverse."}};
        m_relativeAction = 0;
    }
    m_relativeReadReason = QStringLiteral("Receive/session ownership changed; explicitly Read again.");
    m_relativeDirty = true;
}

void KenwoodBackend::resetRelativeTuning()
{
    ++m_relativeEpoch; ++m_relativeReadTicket;
    m_relativeFrequencyOwned = {false, false}; m_relativeFrequencyFloor = {0, 0}; m_relativeFrequencyLatest = {0, 0};
    m_relativeOwned.clear(); m_relativeFacts.clear(); m_relativeBatches.clear(); m_relativeSafety.clear();
    m_relativeQueue.clear(); m_relativePending.clear(); m_relativeReadToken.clear(); m_relativeAction = 0;
    m_relativeLastAction.clear(); m_relativeCompleted = m_relativeTotal = m_relativeFailed = 0;
    m_relativeReadReason = QStringLiteral("Read native step context before tuning."); m_relativeDirty = true;
}

QString KenwoodBackend::relativeTuningUnavailable() const
{
    if (!isConnected() || !m_identityConfirmed) { return QStringLiteral("Requires a connected, identified TS-890S."); }
    if (!m_relativePending.isEmpty() || !m_relativeQueue.isEmpty() || m_relativeAction) { return QStringLiteral("Waiting for the current native step request."); }
    if (!m_txStateKnown || m_txObserved || m_keyIntent || m_cwIntent || m_atuIntent || m_tune) { return QStringLiteral("Requires confirmed receive operation without local transmit or tune intent."); }
    for (const QString& key : kRelativeSurvey) {
        if (!m_relativeOwned.contains(key)) { return QStringLiteral("Read current native step context before tuning."); }
    }
    if (m_relativeFacts.value("TS") != "TS0" || !m_tfSet || *m_tfSet) { return QStringLiteral("Native receive steps require TF-SET off."); }
    if (m_relativeFacts.value("XV") != "XV0" && m_relativeFacts.value("XV") != "XV1") { return QStringLiteral("Read the native transverter state before tuning."); }
    if (m_relativeFacts.value("MV") != "MV0" || !m_relativeFacts.value("QR").toString().startsWith("QR0")) { return QStringLiteral("Native memory functions are unsupported."); }
    const QString role = m_relativeFacts.value("FR").toString();
    const int target = role == "FR0" ? 0 : role == "FR1" ? 1 : -1;
    if (target < 0 || target != m_receiveVfo) { return QStringLiteral("Requires the current physical receive-selected VFO."); }
    const std::optional<VfoState> state = decodeVfoState(m_relativeFacts.value(target ? "SF1" : "SF0").toByteArray());
    const std::optional<qint64> frequency = decodeFrequency(m_relativeFacts.value(target ? "FB" : "FA").toByteArray());
    if (!state || !frequency || state->frequencyHz != *frequency || *frequency != m_frequency[target] || state->mode != m_mode) {
        return QStringLiteral("Receive frequency or mode changed; Read again.");
    }
    return {};
}

QVariantMap KenwoodBackend::relativeTuningStatus() const
{
    const QString role = m_relativeFacts.value("FR").toString();
    const int target = role == "FR0" ? 0 : role == "FR1" ? 1 : -1;
    const std::optional<qint64> frequency = target < 0 ? std::nullopt : decodeFrequency(m_relativeFacts.value(target ? "FB" : "FA").toByteArray());
    const QString reason = relativeTuningUnavailable();
    return {{"schemaVersion", 1}, {"statusRevision", QString::number(m_relativeStatusRevision)},
        {"context", QStringLiteral("relative:%1:%2").arg(m_generation).arg(m_relativeEpoch)}, {"revision", QString::number(m_relativeEpoch)},
        {"connected", isConnected() && m_identityConfirmed}, {"editable", reason.isEmpty()}, {"receiveVfo", target},
        {"frequencyHz", QVariant::fromValue<qint64>(frequency.value_or(0))}, {"reason", reason},
        {"read", QVariantMap{{"token", m_relativeReadToken}, {"busy", !m_relativePending.isEmpty() || !m_relativeQueue.isEmpty()},
            {"pending", m_relativePending.isEmpty() && !m_relativeQueue.isEmpty() ? m_relativeQueue.first() : m_relativePending}, {"completed", m_relativeCompleted}, {"total", m_relativeTotal}, {"reason", m_relativeReadReason}}},
        {"lastAction", m_relativeLastAction}};
}

void KenwoodBackend::publishRelativeTuning()
{
    m_relativeDirty = false; ++m_relativeStatusRevision;
    emit extensionStatus(QStringLiteral("kenwood"), QStringLiteral("relativeTuning"), relativeTuningStatus());
}

void KenwoodBackend::adoptRelativeTuning(const QByteArray& message, quint64 batch)
{
    refreshRelativeTuningContext();
    if (message == "?" && !m_relativePending.isEmpty() && m_relativeDispatched && batch > m_relativeMinimumBatch) {
        m_relativePending.clear(); m_relativeQueue.clear(); m_relativeOwned.clear(); ++m_relativeReadTicket;
        m_relativeReadReason = QStringLiteral("Native read refused; no retry.");
        if (m_relativeAction) { m_relativeLastAction = {{"state", "sent_unverified"}, {"reason", m_relativeReadReason}}; m_relativeAction = 0; }
        m_relativeDirty = true; return;
    }
    QString key; bool valid = false;
    if (message.startsWith("SF0") || message.startsWith("SF1")) {
        key = QString::fromLatin1(message.left(3)); valid = decodeVfoState(message).has_value();
    } else if (message.startsWith("OM0") || message.startsWith("OM1")) {
        key = QString::fromLatin1(message.left(3)); valid = message.size() == 4 && decodeMode(message[3]).has_value();
    } else {
        key = QString::fromLatin1(message.left(2));
        if (!kRelativeSurvey.contains(key)) { return; }
        if (key == "FA" || key == "FB") { valid = decodeFrequency(message).has_value(); }
        else if (key == "FS") { valid = message.size() == 4 && (message[2] == '0' || message[2] == '1') && (message[3] == '0' || message[3] == '1'); }
        else if (key == "QR") { valid = message.size() == 4 && (message[2] == '0' || message[2] == '1') && (message[3] == ' ' || (message[3] >= '0' && message[3] <= '9')); }
        else { valid = message.size() == 3 && (message[2] == '0' || message[2] == '1' || ((key == "FR" || key == "FT") && message[2] == '3')); }
    }
    if (batch <= m_relativeBatches.value(key)) { return; }
    const bool matching = key == m_relativePending && m_relativeDispatched && batch > m_relativeMinimumBatch;
    m_relativeBatches[key] = batch;
    const bool changed = !valid || m_relativeFacts.value(key).toString() != QString::fromLatin1(message);
    bool actionFrequency = false;
    if (valid && m_relativeAction && m_relativeHanded && m_relativeTarget >= 0 && m_relativeTarget < 2
        && batch > m_relativeFrequencyFloor[m_relativeTarget]) {
        const QString targetFrequency = m_relativeTarget == 0 ? QStringLiteral("FA") : QStringLiteral("FB");
        if (key == targetFrequency) {
            actionFrequency = true;
        } else if (const std::optional<VfoState> state = decodeVfoState(message)) {
            actionFrequency = state->vfo == m_relativeTarget && state->mode == m_relativeActionMode;
        }
    }
    if (changed) {
        ++m_relativeEpoch;
        // AI2 may report the actual target frequency before its queued query.
        // It does not complete that query or establish a causal acknowledgement.
        if ((m_relativeAction && m_relativeHanded) ? !actionFrequency : !matching) {
            m_relativeOwned.clear();
            if (!m_relativePending.isEmpty() || !m_relativeQueue.isEmpty()
                || (m_relativeAction && m_relativeHanded)) {
                m_relativePending.clear(); m_relativeQueue.clear(); ++m_relativeReadTicket;
                m_relativeReadReason = QStringLiteral("Context changed outside the owned read; Read again.");
                if (m_relativeAction && m_relativeHanded) {
                    m_relativeLastAction = {{"state", "indeterminate"}, {"reason", m_relativeReadReason}};
                    m_relativeAction = 0;
                }
            }
            if (m_relativeAction && !m_relativeHanded) {
                m_relativeLastAction = {{"state", "rejected"}, {"reason", "Native context changed before dispatch."}};
                m_relativeAction = 0;
            }
        }
    }
    if (valid) { m_relativeFacts[key] = QString::fromLatin1(message); }
    else { m_relativeFacts.remove(key); m_relativeOwned.remove(key); }
    if (matching) {
        m_relativePending.clear(); m_relativeDispatched = false;
        if (valid) { m_relativeOwned.insert(key); ++m_relativeCompleted; }
        else {
            m_relativeOwned.clear(); m_relativeQueue.clear(); m_relativeReadReason = QStringLiteral("Malformed native reply; Read again.");
            if (m_relativeAction) { m_relativeLastAction = {{"state", "sent_unverified"}, {"reason", m_relativeReadReason}}; m_relativeAction = 0; }
        }
    }
    m_relativeDirty = true;
}

void KenwoodBackend::pollRelativeTuning(qint64 now)
{
    refreshRelativeTuningContext();
    const QPointer<KenwoodBackend> guard(this); const quint64 generation = m_generation;
    if (!m_relativePending.isEmpty() && m_relativeDispatched && now >= m_relativeDeadline) {
        m_relativePending.clear(); m_relativeQueue.clear(); m_relativeOwned.clear(); ++m_relativeReadTicket;
        m_relativeReadReason = QStringLiteral("Native read timed out; no retry.");
        if (m_relativeAction) { m_relativeLastAction = {{"state", "sent_unverified"}, {"reason", m_relativeReadReason}}; m_relativeAction = 0; }
        m_relativeDirty = true;
    }
    if (m_relativePending.isEmpty() && !m_relativeQueue.isEmpty()) {
        if (m_relativeAction && (m_relativeFacts.value("FR") != QStringLiteral("FR%1").arg(m_relativeTarget)
            || m_relativeFacts.value("TS") != "TS0" || m_relativeFacts.value("XV").toString() != m_relativeActionXv)) {
            m_relativeQueue.clear(); m_relativeOwned.clear(); m_relativeAction = 0;
            m_relativeLastAction = {{"state", "indeterminate"}, {"reason", "Receive target changed after handoff; readback stopped."}};
        } else {
            m_relativePending = m_relativeQueue.takeFirst(); m_relativeDispatched = false;
            const QString key = m_relativePending; const quint64 ticket = ++m_relativeReadTicket;
            m_relativeMinimumBatch = m_controlIngressSequence;
            publishRelativeTuning();
            if (!guard || generation != m_generation || ticket != m_relativeReadTicket || key != m_relativePending) { return; }
            m_relativeMinimumBatch = m_controlIngressSequence;
            m_relativeDispatched = true; m_relativeDeadline = now + 1500;
            const bool sent = send(key.toLatin1());
            if (!guard || generation != m_generation) { return; }
            if (!sent && ticket == m_relativeReadTicket) {
                m_relativePending.clear(); m_relativeQueue.clear(); m_relativeOwned.clear(); ++m_relativeReadTicket;
                m_relativeReadReason = QStringLiteral("Native query transport refused; no retry.");
                if (m_relativeAction) { m_relativeLastAction = {{"state", "sent_unverified"}, {"reason", m_relativeReadReason}}; m_relativeAction = 0; }
                m_relativeDirty = true;
            }
        }
    }
    if (m_relativePending.isEmpty() && m_relativeQueue.isEmpty() && m_relativeAction && m_relativeHanded) {
        m_relativeAction = 0;
        const QString unavailable = relativeTuningUnavailable();
        const bool coherent = unavailable.isEmpty() && m_mode == m_relativeActionMode;
        m_relativeLastAction = {{"state", coherent ? "observed" : "indeterminate"},
            {"reason", coherent ? QStringLiteral("Readback finished; the radio owns the result. No predicted step or causal acknowledgement.")
                : unavailable.isEmpty() ? QStringLiteral("Receive mode changed; Read again.") : unavailable}};
        m_relativeDirty = true;
    }
    if (m_relativeDirty) { publishRelativeTuning(); }
}

bool KenwoodBackend::handleRelativeTuningExtension(const QString& verb, quint64 request, const QVariant& argument)
{
    if (!verb.startsWith("tuning.relative.")) { return false; }
    const QPointer<KenwoodBackend> guard(this); const quint64 generation = m_generation;
    refreshRelativeTuningContext();
    if (verb == "tuning.relative.get") {
        if (argument.isValid()) { emit extensionError(request, QStringLiteral("Cached get takes no argument.")); }
        else { ++m_relativeStatusRevision; emit extensionResult(request, relativeTuningStatus()); } return true;
    }
    if (verb == "tuning.relative.cancel") {
        const QVariantMap args = argument.toMap();
        if (argument.typeId() != QMetaType::QVariantMap || args.size() != 1 || args.value("token") != m_relativeReadToken
            || args.value("token").typeId() != QMetaType::QString || m_relativeReadToken.isEmpty()) {
            emit extensionError(request, QStringLiteral("Cancel requires the current read token.")); return true;
        }
        m_relativePending.clear(); m_relativeQueue.clear(); m_relativeOwned.clear(); ++m_relativeReadTicket; ++m_relativeEpoch;
        if (m_relativeAction) { m_relativeLastAction = {{"state", m_relativeHanded ? "sent_unverified" : "rejected"}, {"reason", "Cancelled locally; no native inverse."}}; }
        m_relativeAction = 0; publishRelativeTuning();
        if (guard && generation == m_generation) { emit extensionResult(request, relativeTuningStatus()); } return true;
    }
    if (verb == "tuning.relative.read") {
        if (argument.isValid() || !isConnected() || !m_identityConfirmed || !m_relativePending.isEmpty() || !m_relativeQueue.isEmpty() || m_relativeAction) {
            emit extensionError(request, QStringLiteral("Read requires an identified connection and no pending native work.")); return true;
        }
        m_relativeOwned.clear(); m_relativeQueue = kRelativeSurvey; m_relativeCompleted = 0; m_relativeTotal = kRelativeSurvey.size();
        m_relativeReadToken = QString::number(++m_relativeSerial); m_relativeReadReason = QStringLiteral("Reading native step context; no setting changed.");
        pollRelativeTuning(m_clock.elapsed());
        if (guard && generation == m_generation) { emit extensionResult(request, relativeTuningStatus()); } return true;
    }
    const bool fc = verb == "tuning.relative.fc"; const bool ud = verb == "tuning.relative.ud";
    const QVariantMap args = argument.toMap();
    const int expected = fc ? 4 : (args.contains("steps") ? 5 : 4);
    const QString context = relativeTuningStatus().value("context").toString();
    if ((!fc && !ud) || argument.typeId() != QMetaType::QVariantMap || args.size() != expected
        || args.value("context").typeId() != QMetaType::QString || args.value("context") != context
        || !relativeSequence(args.value("revision")) || args.value("revision").toString() != QString::number(m_relativeEpoch)
        || !relativeInteger(args.value("direction"), 1) || !relativeTuningUnavailable().isEmpty()
        || (fc && !relativeInteger(args.value("multiplier"), 5))
        || (ud && (!relativeInteger(args.value("target"), 1) || args.value("target").toInt() != m_receiveVfo
            || (args.contains("steps") && !relativeInteger(args.value("steps"), 99))))) {
        emit extensionError(request, QStringLiteral("Native step requires exact arguments and current receive-selected context.")); return true;
    }
    QByteArray command = fc ? QByteArray("FC") : QByteArray("UD") + QByteArray::number(m_receiveVfo);
    command += QByteArray::number(args.value("direction").toInt());
    if (fc) { command += QByteArray::number(args.value("multiplier").toInt()); }
    else if (args.contains("steps")) { command += QByteArray::number(args.value("steps").toInt()).rightJustified(2, '0'); }
    const quint64 epoch = m_relativeEpoch; const quint64 ticket = ++m_relativeSerial; m_relativeAction = ticket;
    m_relativeTarget = m_receiveVfo; m_relativeActionXv = m_relativeFacts.value("XV").toString(); m_relativeHanded = false;
    m_relativeLastAction = {{"state", "pending"}, {"reason", "Awaiting native step handoff."}};
    publishRelativeTuning();
    if (!guard || generation != m_generation) { return true; }
    refreshRelativeTuningContext();
    if (m_relativeAction != ticket) { emit extensionError(request, QStringLiteral("Native step retired before handoff.")); return true; }
    m_relativeAction = 0;
    const bool current = epoch == m_relativeEpoch && relativeTuningUnavailable().isEmpty();
    m_relativeAction = ticket;
    if (!current) {
        m_relativeAction = 0; m_relativeLastAction = {{"state", "rejected"}, {"reason", "Native step context changed before handoff."}};
        publishRelativeTuning(); if (guard && generation == m_generation) { emit extensionError(request, QStringLiteral("Native step context changed before handoff.")); } return true;
    }
    m_relativeActionMode = m_mode;
    m_relativeHanded = true; m_relativeOwned.clear();
    m_relativeFrequencyOwned[m_relativeTarget] = true;
    m_relativeFrequencyFloor[m_relativeTarget] = m_controlIngressSequence;
    const bool sent = send(command);
    if (!guard || generation != m_generation) { return true; }
    if (m_relativeAction == ticket) {
        m_relativeLastAction = {{"state", sent ? "sent_unverified" : "rejected"}, {"reason", sent ? "Native step sent; waiting for actual radio observations." : "Transport refused native step; no retry."}};
        if (sent) {
            m_relativeQueue = kRelativeSurvey; m_relativeReadToken = QString::number(++m_relativeSerial);
            m_relativeCompleted = 0; m_relativeTotal = kRelativeSurvey.size(); m_relativeReadReason = QStringLiteral("Reading actual native step result.");
            pollRelativeTuning(m_clock.elapsed());
        } else { m_relativeAction = 0; }
    }
    if (!guard || generation != m_generation) { return true; }
    publishRelativeTuning();
    if (guard && generation == m_generation) { emit extensionResult(request, QVariantMap{{"sent", sent}, {"verified", false}}); }
    return true;
}
} // namespace AetherSDR::kenwood
