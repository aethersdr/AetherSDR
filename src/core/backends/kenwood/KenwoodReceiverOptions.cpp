#include "KenwoodBackend.h"

#include <QByteArray>
#include <QList>
#include <QMap>
#include <QMetaType>
#include <QPointer>
#include <QSet>
#include <QString>
#include <QStringList>
#include <QVariant>
#include <QVariantMap>
#include <algorithm>
#include <optional>

namespace AetherSDR::kenwood {
namespace {
struct ReceiverOption { QString id; QString key; QString label; bool readonly; };
const QList<ReceiverOption> kReceiverOptions{
    {"speakerMute", "QS", "Physical speaker mute", false},
    {"signalBlanking", "BK", "Receiver signal blanking", false},
    {"busy", "BY", "Busy LED", true}};
bool preferenceSequence(const QVariant& value)
{
    if (value.typeId() != QMetaType::QString || value.toString().isEmpty()) { return false; }
    bool ok = false; const quint64 number = value.toString().toULongLong(&ok);
    return ok && QString::number(number) == value.toString();
}
QString preferenceKey(const QString& id)
{
    for (const ReceiverOption& row : kReceiverOptions) { if (row.id == id) { return row.key; } }
    return {};
}
}
QString KenwoodBackend::receiverOptionsContext() const
{
    return QStringLiteral("receiverOptions:%1:%2").arg(m_generation).arg(m_receiverOptionsEpoch);
}
void KenwoodBackend::retireReceiverOptionsWrite(const QString& reason)
{
    if (!m_receiverOptionsWriteTicket) { return; }
    m_receiverOptionsOutcomeRetired = true;
    m_receiverOptionsLastWrite.insert("state", m_receiverOptionsHanded ? QStringLiteral("indeterminate") : QStringLiteral("rejected"));
    m_receiverOptionsLastWrite.insert("reason", reason);
    if (!m_receiverOptionsHanded) { m_receiverOptionsWriteTicket = 0; m_receiverOptionsPredispatch = false; }
}
void KenwoodBackend::refreshReceiverOptionsContext()
{
    const QString signature = QStringLiteral("%1:%2:%3:%4:%5:%6:%7:%8:%9")
        .arg(m_generation).arg(isConnected()).arg(m_identityConfirmed).arg(m_txStateKnown).arg(m_txObserved)
        .arg(m_keyIntent).arg(m_cwIntent).arg(m_atuIntent).arg(m_tune ? m_tune->serial : 0);
    if (signature == m_receiverOptionsSignature) { return; }
    m_receiverOptionsSignature = signature; ++m_receiverOptionsEpoch; m_receiverOptionsDirty = true;
    retireReceiverOptionsWrite(QStringLiteral("Receiver option write intent retired by session or receive/local-intent transition."));
}
QString KenwoodBackend::receiverOptionsUnavailable() const
{
    if (!isConnected() || !m_identityConfirmed) { return QStringLiteral("Requires a connected, identified radio."); }
    if (m_receiverOptionsReading || m_receiverOptionsWriteTicket) { return QStringLiteral("Waiting for the current receiver option request to finish."); }
    if (!m_txStateKnown || m_txObserved || m_keyIntent || m_cwIntent || m_atuIntent || m_tune) { return QStringLiteral("Requires confirmed receive state without local transmit or tune intent."); }
    return {};
}
QVariantMap KenwoodBackend::receiverOptionsStatus() const
{
    const bool connected = isConnected() && m_identityConfirmed;
    const qint64 now = std::max(m_receiverOptionsNowMs, m_clock.isValid() ? m_clock.elapsed() : qint64(0));
    QVariantList rows;
    for (const ReceiverOption& row : kReceiverOptions) {
        const qint64 age = m_receiverOptionsTimes.contains(row.key) ? std::max(qint64(0), now - m_receiverOptionsTimes.value(row.key)) : qint64(-1);
        const bool known = connected && m_receiverOptionsKnown.contains(row.key) && (row.key != QLatin1String("BY") || age <= 1500);
        QString reason = row.readonly ? QStringLiteral("Busy LED is read-only; a sample expires after 1500 ms.") : receiverOptionsUnavailable();
        if (!row.readonly && reason.isEmpty() && !known) { reason = QStringLiteral("Explicitly read a native baseline before Apply."); }
        QVariantMap descriptor{{"id", row.id}, {"label", row.label}, {"known", known}, {"revision", QString::number(m_receiverOptionsRevisions.value(row.key))},
            {"editable", !row.readonly && reason.isEmpty()}, {"readonly", row.readonly},
            {"pending", m_receiverOptionsWriteTicket != 0 && m_receiverOptionsWriteId == row.id}, {"reason", reason}, {"ageMs", QVariant::fromValue<qint64>(age)}};
        if (m_receiverOptionsValues.contains(row.key)) { descriptor.insert("value", m_receiverOptionsValues.value(row.key)); }
        rows.append(descriptor);
    }
    QVariantMap recording{{"known", connected && m_receiverRecordingKnown},
        {"revision", QString::number(m_receiverOptionsRevisions.value("RE"))}, {"reason", m_receiverRecordingReason}};
    if (m_receiverRecordingState && m_receiverRecordingProgress) {
        recording.insert("state", *m_receiverRecordingState);
        recording.insert("progress", *m_receiverRecordingProgress);
    }
    return {{"recording", recording}, {"statusRevision", QString::number(m_receiverOptionsStatusRevision)}, {"context", receiverOptionsContext()}, {"connected", connected},
        {"reason", connected ? QString{} : QStringLiteral("Requires a connected, identified radio.")}, {"rows", rows},
        {"refresh", QVariantMap{{"token", m_receiverOptionsReadToken}, {"busy", m_receiverOptionsReading}, {"total", m_receiverOptionsTotal},
            {"completed", m_receiverOptionsCompleted}, {"observed", m_receiverOptionsObserved}, {"failed", m_receiverOptionsFailed},
            {"pendingKey", m_receiverOptionsPending}, {"reason", m_receiverOptionsReadReason}}},
        {"lastWrite", m_receiverOptionsLastWrite.isEmpty() ? QVariantMap{{"token", QString{}}, {"id", QString{}}, {"state", "none"}, {"reason", QString{}}} : m_receiverOptionsLastWrite},
        {"notice", QStringLiteral("Values follow radio observations and may change at the radio. BUSY is a short-lived observation. Read again to refresh. Requests are not automatically retried or restored.")}};
}
void KenwoodBackend::publishReceiverOptions()
{
    m_receiverOptionsDirty = false; ++m_receiverOptionsStatusRevision;
    emit extensionStatus(QStringLiteral("kenwood"), QStringLiteral("receiverOptions"), receiverOptionsStatus());
}
void KenwoodBackend::resetReceiverOptions()
{
    retireReceiverOptionsWrite(QStringLiteral("Session retired; an accepted write cannot be undone automatically."));
    m_receiverOptionsWriteTicket = 0; m_receiverOptionsPredispatch = m_receiverOptionsHanded = false;
    ++m_receiverOptionsEpoch; ++m_receiverOptionsSerial;
    m_receiverOptionsNowMs = 0;
    m_receiverRecordingKnown = false; m_receiverRecordingState.reset(); m_receiverRecordingProgress.reset();
    m_receiverRecordingReason = QStringLiteral("Session retired; explicitly read native recording/playback status.");
    m_receiverOptionsFloors["RE"] = std::max(m_receiverOptionsFloors.value("RE"), m_controlIngressSequence);
    ++m_receiverOptionsRevisions["RE"];
    m_receiverOptionsKnown.clear(); m_receiverOptionsValues.clear(); m_receiverOptionsTimes.clear();
    for (const ReceiverOption& row : kReceiverOptions) {
        m_receiverOptionsFloors[row.key] = std::max(m_receiverOptionsFloors.value(row.key), m_controlIngressSequence);
        ++m_receiverOptionsRevisions[row.key];
    }
    m_receiverOptionsReading = m_receiverOptionsDispatched = false;
    m_receiverOptionsPending.clear(); m_receiverOptionsPendingTicket = 0; m_receiverOptionsQueue.clear();
    m_receiverOptionsReadReason = QStringLiteral("Session retired; explicit Read is required."); m_receiverOptionsDirty = true;
}
void KenwoodBackend::beginReceiverOptionsRead(const QStringList& keys)
{
    m_receiverOptionsQueue = keys; m_receiverOptionsReadToken = QString::number(++m_receiverOptionsSerial);
    m_receiverOptionsPending.clear(); m_receiverOptionsPendingTicket = 0; m_receiverOptionsDispatched = false;
    m_receiverOptionsReading = true;
    m_receiverOptionsTotal = keys.size(); m_receiverOptionsCompleted = m_receiverOptionsObserved = m_receiverOptionsFailed = 0;
    m_receiverOptionsReadReason = QStringLiteral("Reading native global options; no setting is changed.");
}
void KenwoodBackend::completeReceiverOptionsRead(bool observed, const QString& reason)
{
    const QString key = m_receiverOptionsPending;
    if (observed) { ++m_receiverOptionsObserved; }
    else {
        if (key == QLatin1String("RE")) { m_receiverRecordingKnown = false; m_receiverRecordingReason = reason; }
        ++m_receiverOptionsFailed; m_receiverOptionsKnown.remove(key); ++m_receiverOptionsRevisions[key];
        retireReceiverOptionsWrite(reason);
    }
    ++m_receiverOptionsCompleted; m_receiverOptionsPending.clear(); m_receiverOptionsPendingTicket = 0; m_receiverOptionsDispatched = false;
    if (!m_receiverOptionsQueue.isEmpty()) { return; }
    m_receiverOptionsReading = false;
    m_receiverOptionsReadReason = m_receiverOptionsFailed ? QStringLiteral("Read finished with unqualified observations; previous values retained.") : QStringLiteral("Fresh native observations received.");
    if (!m_receiverOptionsWriteTicket) { return; }
    const QString writeKey = preferenceKey(m_receiverOptionsWriteId);
    const bool consistent = !m_receiverOptionsFailed && !m_receiverOptionsOutcomeRetired
        && m_receiverOptionsKnown.contains(writeKey) && m_receiverOptionsValues.value(writeKey) == m_receiverOptionsWriteValue;
    m_receiverOptionsLastWrite.insert("state", consistent ? QStringLiteral("observed_consistent") : QStringLiteral("indeterminate"));
    m_receiverOptionsLastWrite.insert("reason", consistent ? QStringLiteral("Fresh observation matches the request; no causal acknowledgement inferred.") : QStringLiteral("Outcome remains indeterminate; no retry, inverse or restoration."));
    m_receiverOptionsWriteTicket = 0; m_receiverOptionsHanded = false;
}

void KenwoodBackend::adoptReceiverOptions(const QByteArray& message, quint64 batch)
{
    if (!isConnected() || !m_identityConfirmed) { return; }
    if (message == "?") {
        if (m_receiverOptionsPendingTicket && m_receiverOptionsDispatched && batch > m_receiverOptionsMinimumBatch) {
            completeReceiverOptionsRead(false, QStringLiteral("Ambiguous radio error while a receiver option read was pending."));
            refreshReceiverOptionsContext(); m_receiverOptionsDirty = true;
        }
        return;
    }
    QString key = message.startsWith("RE") ? QStringLiteral("RE") : QString{};
    for (const ReceiverOption& row : kReceiverOptions) { if (message.startsWith(row.key.toLatin1())) { key = row.key; break; } }
    if (key.isEmpty() || batch < m_receiverOptionsBatches.value(key) || batch <= m_receiverOptionsFloors.value(key)) { return; }
    const bool matching = m_receiverOptionsPendingTicket && m_receiverOptionsPending == key
        && m_receiverOptionsDispatched && batch > m_receiverOptionsMinimumBatch;
    if (key == QLatin1String("RE")) {
        // Even an invalid newer report retires older copied observations.
        m_receiverOptionsBatches[key] = batch;
        bool valid = message.size() == 6 && QByteArray("0134567").contains(message[2]);
        int progress = 0;
        if (valid) {
            for (int i = 3; i < 6; ++i) {
                if (message[i] < '0' || message[i] > '9') { valid = false; break; }
                progress = progress * 10 + message[i] - '0';
            }
            valid = valid && progress <= 100;
        }
        if (!valid) {
            m_receiverRecordingKnown = false;
            m_receiverRecordingReason = QStringLiteral("Malformed native recording/playback report; last observation retained without authority.");
            ++m_receiverOptionsRevisions[key];
            if (matching) { completeReceiverOptionsRead(false, m_receiverRecordingReason); }
        } else {
            m_receiverRecordingState = message[2] - '0'; m_receiverRecordingProgress = progress;
            m_receiverRecordingKnown = true; ++m_receiverOptionsRevisions[key];
            m_receiverRecordingReason = QStringLiteral("Last reported native state and playback progression; not a causal acknowledgement or live activity guarantee.");
            if (matching) { completeReceiverOptionsRead(true, {}); }
        }
        refreshReceiverOptionsContext(); m_receiverOptionsDirty = true; return;
    }
    std::optional<bool> value;
    if (message.size() == 3 && (message[2] == '0' || message[2] == '1')) { value = message[2] == '1'; }
    if (!value) {
        if (matching) { completeReceiverOptionsRead(false, QStringLiteral("Malformed receiver option reply; previous observation retained without authority.")); }
        refreshReceiverOptionsContext(); m_receiverOptionsDirty = true; return;
    }
    if (m_receiverOptionsPredispatch && key == preferenceKey(m_receiverOptionsWriteId)) {
        const bool unchanged = m_receiverOptionsKnown.contains(key) && m_receiverOptionsValues.value(key) == *value;
        if (unchanged) {
            m_receiverOptionsBatches[key] = batch;
            m_receiverOptionsTimes[key] = std::max(m_receiverOptionsNowMs, m_clock.isValid() ? m_clock.elapsed() : qint64(0));
            m_receiverOptionsDirty = true;
            return;
        }
        retireReceiverOptionsWrite(QStringLiteral("A changed receiver option superseded the unsent request."));
    }
    m_receiverOptionsBatches[key] = batch;
    const bool changed = !m_receiverOptionsKnown.contains(key) || m_receiverOptionsValues.value(key) != *value;
    m_receiverOptionsValues[key] = *value; m_receiverOptionsKnown.insert(key);
    m_receiverOptionsTimes[key] = std::max(m_receiverOptionsNowMs, m_clock.isValid() ? m_clock.elapsed() : qint64(0));
    if (changed) { ++m_receiverOptionsRevisions[key]; }
    if (matching) { completeReceiverOptionsRead(true, {}); }
    refreshReceiverOptionsContext(); m_receiverOptionsDirty = true;
}
void KenwoodBackend::pollReceiverOptions(qint64 nowMs)
{
    const QPointer<KenwoodBackend> guard(this); const quint64 generation = m_generation;
    m_receiverOptionsNowMs = nowMs;
    if (m_receiverOptionsKnown.contains("BY") && nowMs - m_receiverOptionsTimes.value("BY") > 1500) {
        m_receiverOptionsKnown.remove("BY"); ++m_receiverOptionsRevisions["BY"]; m_receiverOptionsDirty = true;
    }
    refreshReceiverOptionsContext();
    if (m_receiverOptionsDirty) { publishReceiverOptions(); if (!guard || generation != m_generation) { return; } }
    if (!m_receiverOptionsReading || !isConnected() || !m_identityConfirmed) { return; }
    if (m_receiverOptionsPendingTicket) {
        if (m_receiverOptionsDispatched && nowMs >= m_receiverOptionsDeadline) {
            completeReceiverOptionsRead(false, QStringLiteral("Receiver option read timed out; no retry or restoration."));
            refreshReceiverOptionsContext(); publishReceiverOptions();
        }
        return;
    }
    if (m_receiverOptionsQueue.isEmpty()) { return; }
    const QString key = m_receiverOptionsQueue.takeFirst(); const quint64 ticket = ++m_receiverOptionsSerial;
    m_receiverOptionsPending = key; m_receiverOptionsPendingTicket = ticket; m_receiverOptionsDispatched = false;
    m_receiverOptionsMinimumBatch = m_controlIngressSequence;
    publishReceiverOptions();
    if (!guard || generation != m_generation || ticket != m_receiverOptionsPendingTicket || !m_receiverOptionsReading) { return; }
    m_receiverOptionsMinimumBatch = m_controlIngressSequence;
    m_receiverOptionsFloors[key] = std::max(m_receiverOptionsFloors.value(key), m_controlIngressSequence);
    m_receiverOptionsDispatched = true; m_receiverOptionsDeadline = nowMs + 2000;
    const bool sent = send(key.toLatin1());
    if (!guard || generation != m_generation || ticket != m_receiverOptionsPendingTicket) { return; }
    if (!sent) {
        completeReceiverOptionsRead(false, QStringLiteral("Receiver option read transport refused; no retry."));
        refreshReceiverOptionsContext(); publishReceiverOptions();
    }
}
bool KenwoodBackend::handleReceiverOptionsExtension(const QString& verb, quint64 requestId, const QVariant& argument)
{
    if (!verb.startsWith(QLatin1String("receiver.options."))) { return false; }
    const bool map = !argument.isValid() || argument.typeId() == QMetaType::QVariantMap;
    const QVariantMap args = argument.toMap();
    if (verb == QLatin1String("receiver.options.get")) {
        if (!map || !args.isEmpty()) { emit extensionError(requestId, QStringLiteral("Receiver option getter accepts no arguments.")); }
        else { emit extensionResult(requestId, receiverOptionsStatus()); }
        return true;
    }
    refreshReceiverOptionsContext();
    if (!map || args.value("context").typeId() != QMetaType::QString || args.value("context").toString() != receiverOptionsContext()) {
        emit extensionError(requestId, QStringLiteral("Requires the current native receiver option context.")); return true;
    }
    if (!isConnected() || !m_identityConfirmed) { emit extensionError(requestId, QStringLiteral("Requires a connected, identified radio.")); return true; }
    const QPointer<KenwoodBackend> guard(this); const quint64 generation = m_generation;
    if (verb == QLatin1String("receiver.options.read") || verb == QLatin1String("receiver.options.cancel")) {
        if (verb == QLatin1String("receiver.options.read")) {
            if (args.size() != 1 || m_receiverOptionsReading || m_receiverOptionsWriteTicket) { emit extensionError(requestId, QStringLiteral("Invalid read arguments or receiver option work is pending.")); return true; }
            beginReceiverOptionsRead({"QS", "BK", "BY", "RE"});
        } else {
            if (!m_receiverOptionsReading || args.size() != 2 || args.value("token").toString() == QLatin1String("0") || !preferenceSequence(args.value("token")) || args.value("token").toString() != m_receiverOptionsReadToken) { emit extensionError(requestId, QStringLiteral("Requires the current receiver option read token.")); return true; }
            retireReceiverOptionsWrite(QStringLiteral("Verification canceled locally; the request was not undone."));
            m_receiverOptionsWriteTicket = 0; m_receiverOptionsHanded = m_receiverOptionsPredispatch = false;
            m_receiverOptionsReading = m_receiverOptionsDispatched = false; m_receiverOptionsQueue.clear(); m_receiverOptionsPending.clear(); m_receiverOptionsPendingTicket = 0;
            for (const ReceiverOption& row : kReceiverOptions) { m_receiverOptionsFloors[row.key] = std::max(m_receiverOptionsFloors.value(row.key), m_controlIngressSequence); }
            m_receiverOptionsFloors["RE"] = std::max(m_receiverOptionsFloors.value("RE"), m_controlIngressSequence);
            ++m_receiverOptionsSerial; m_receiverOptionsReadReason = QStringLiteral("Read canceled locally; no inverse or restoration.");
        }
        const quint64 serial = m_receiverOptionsSerial; publishReceiverOptions();
        if (guard && generation == m_generation) {
            if (serial == m_receiverOptionsSerial) { emit extensionResult(requestId, verb == QLatin1String("receiver.options.cancel") ? QVariantMap{{"cancelled", true}} : QVariantMap{{"token", m_receiverOptionsReadToken}}); }
            else { emit extensionError(requestId, QStringLiteral("Receiver option scheduling request was superseded.")); }
        }
        return true;
    }
    if (verb != QLatin1String("receiver.options.set") || args.size() != 4
        || args.value("id").typeId() != QMetaType::QString || !preferenceSequence(args.value("revision"))
        || args.value("value").typeId() != QMetaType::Bool) {
        emit extensionError(requestId, QStringLiteral("Requires an exact native option ID, revision and Boolean value.")); return true;
    }
    const QString id = args.value("id").toString(); const QString key = preferenceKey(id);
    if (key.isEmpty() || key == QLatin1String("BY") || !m_receiverOptionsKnown.contains(key) || args.value("revision").toString() != QString::number(m_receiverOptionsRevisions.value(key))) {
        emit extensionError(requestId, QStringLiteral("Read the native option baseline before Apply.")); return true;
    }
    const QString reason = receiverOptionsUnavailable();
    if (!reason.isEmpty()) { emit extensionError(requestId, reason); return true; }
    const quint64 ticket = ++m_receiverOptionsSerial; const QString token = QString::number(ticket); const QString context = receiverOptionsContext();
    m_receiverOptionsWriteTicket = ticket; m_receiverOptionsWriteId = id;
    m_receiverOptionsWriteValue = args.value("value").toBool();
    m_receiverOptionsPredispatch = true; m_receiverOptionsHanded = m_receiverOptionsOutcomeRetired = false;
    m_receiverOptionsLastWrite = {{"token", token}, {"id", id}, {"state", "pending"}, {"reason", "Awaiting explicit native handoff."}};
    publishReceiverOptions();
    if (!guard || generation != m_generation) { return true; }
    refreshReceiverOptionsContext();
    if (ticket != m_receiverOptionsWriteTicket || context != receiverOptionsContext()) {
        if (ticket == m_receiverOptionsWriteTicket) { retireReceiverOptionsWrite(QStringLiteral("preference intent retired before handoff.")); }
        emit extensionError(requestId, QStringLiteral("preference intent was superseded before handoff.")); return true;
    }
    m_receiverOptionsFloors[key] = std::max(m_receiverOptionsFloors.value(key), m_controlIngressSequence);
    m_receiverOptionsPredispatch = false; m_receiverOptionsHanded = true;
    const QByteArray command = key.toLatin1() + (m_receiverOptionsWriteValue ? "1" : "0");
    const bool sent = send(command);
    if (!guard || generation != m_generation) { return true; }
    if (ticket == m_receiverOptionsWriteTicket) {
        if (sent) {
            m_receiverOptionsLastWrite.insert("state", m_receiverOptionsOutcomeRetired ? QStringLiteral("indeterminate") : QStringLiteral("sent_unverified"));
            m_receiverOptionsLastWrite.insert("reason", m_receiverOptionsOutcomeRetired ? QStringLiteral("Context retired during handoff; outcome remains indeterminate.") : QStringLiteral("request sent; no default or causal acknowledgement inferred."));
            beginReceiverOptionsRead({key});
        } else { m_receiverOptionsHanded = false; retireReceiverOptionsWrite(QStringLiteral("native write transport refused.")); }
        publishReceiverOptions(); if (!guard || generation != m_generation) { return true; }
    }
    if (sent) { emit extensionResult(requestId, QVariantMap{{"sent", true}, {"verified", false}, {"token", token}}); }
    else { emit extensionError(requestId, QStringLiteral("receiver option request could not be sent.")); }
    return true;
}
}
