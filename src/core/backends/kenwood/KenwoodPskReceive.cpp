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
struct PskPreference { QString id; QString key; QString label; QStringList choices; QString applicability; };
const QList<PskPreference> kPskReceive{
    {"afcRange", "EX00211", "PSK AFC range", {QString::fromUtf8("±8 Hz"), QString::fromUtf8("±15 Hz")}, "Changing this range does not enable AFC or NET."}};
bool preferenceSequence(const QVariant& value)
{
    if (value.typeId() != QMetaType::QString || value.toString().isEmpty()) { return false; }
    bool ok = false; const quint64 number = value.toString().toULongLong(&ok);
    return ok && QString::number(number) == value.toString();
}
QString preferenceKey(const QString& id)
{
    for (const PskPreference& row : kPskReceive) { if (row.id == id) { return row.key; } }
    return {};
}
}
QString KenwoodBackend::pskReceiveContext() const
{
    return QStringLiteral("pskReceive:%1:%2").arg(m_generation).arg(m_pskReceiveEpoch);
}
void KenwoodBackend::retirePskReceiveWrite(const QString& reason)
{
    if (!m_pskReceiveWriteTicket) { return; }
    m_pskReceiveOutcomeRetired = true;
    m_pskReceiveLastWrite.insert("state", m_pskReceiveHanded ? QStringLiteral("indeterminate") : QStringLiteral("rejected"));
    m_pskReceiveLastWrite.insert("reason", reason);
    if (!m_pskReceiveHanded) { m_pskReceiveWriteTicket = 0; m_pskReceivePredispatch = false; }
}
void KenwoodBackend::refreshPskReceiveContext()
{
    const QString signature = QStringLiteral("%1:%2:%3:%4:%5:%6:%7:%8:%9:%10:%11")
        .arg(m_generation).arg(isConnected()).arg(m_identityConfirmed).arg(m_pskReceiveBankKnown)
        .arg(m_pskReceiveBank.value_or(-1)).arg(m_txStateKnown).arg(m_txObserved)
        .arg(m_keyIntent).arg(m_cwIntent).arg(m_atuIntent).arg(m_tune ? m_tune->serial : 0);
    if (signature == m_pskReceiveSignature) { return; }
    m_pskReceiveSignature = signature; ++m_pskReceiveEpoch; m_pskReceiveDirty = true;
    retirePskReceiveWrite(QStringLiteral("Persistent PSK range intent retired by bank/session or receive/local-intent transition."));
}
QString KenwoodBackend::pskReceiveUnavailable() const
{
    if (!isConnected() || !m_identityConfirmed) { return QStringLiteral("Requires a connected, identified radio."); }
    if (!m_pskReceiveBankKnown) { return QStringLiteral("Read the current configuration bank before changing persistent preferences."); }
    if (m_pskReceiveReading || m_pskReceiveWriteTicket) { return QStringLiteral("Waiting for the current native preference request to finish."); }
    if (!m_txStateKnown || m_txObserved || m_keyIntent || m_cwIntent || m_atuIntent || m_tune) { return QStringLiteral("Requires confirmed receive state without local transmit or tune intent."); }
    return {};
}
QVariantMap KenwoodBackend::pskReceiveStatus() const
{
    const bool connected = isConnected() && m_identityConfirmed;
    QVariantMap bank{{"known", connected && m_pskReceiveBankKnown}, {"revision", QString::number(m_pskReceiveBankRevision)},
        {"reason", connected && m_pskReceiveBankKnown ? QString{} : QStringLiteral("Read the current native configuration bank; no bank is selected automatically.")}};
    if (m_pskReceiveBank) { bank.insert("value", *m_pskReceiveBank); }
    QVariantList rows;
    for (const PskPreference& row : kPskReceive) {
        const bool known = connected && m_pskReceiveBankKnown && m_pskReceiveKnown.contains(row.key);
        QString reason = pskReceiveUnavailable();
        if (reason.isEmpty() && !known) { reason = QStringLiteral("Read this bank's native preference before applying or resetting it."); }
        QVariantList choices;
        for (int value = 0; value < row.choices.size(); ++value) { choices.append(QVariantMap{{"value", value}, {"label", row.choices[value]}}); }
        QVariantMap descriptor{{"id", row.id}, {"label", row.label}, {"known", known}, {"revision", QString::number(m_pskReceiveRevisions.value(row.key))},
            {"choices", choices}, {"editable", reason.isEmpty()}, {"resettable", reason.isEmpty()},
            {"pending", m_pskReceiveWriteTicket != 0 && m_pskReceiveWriteId == row.id}, {"reason", reason}, {"applicability", row.applicability}};
        if (m_pskReceiveValues.contains(row.key)) { descriptor.insert("value", m_pskReceiveValues.value(row.key)); }
        rows.append(descriptor);
    }
    return {{"statusRevision", QString::number(m_pskReceiveStatusRevision)}, {"context", pskReceiveContext()}, {"connected", connected},
        {"reason", connected ? QString{} : QStringLiteral("Requires a connected, identified radio.")}, {"bank", bank}, {"rows", rows},
        {"refresh", QVariantMap{{"token", m_pskReceiveReadToken}, {"busy", m_pskReceiveReading}, {"total", m_pskReceiveTotal},
            {"completed", m_pskReceiveCompleted}, {"observed", m_pskReceiveObserved}, {"failed", m_pskReceiveFailed}, {"skipped", 0},
            {"pendingKey", m_pskReceivePending}, {"reason", m_pskReceiveReadReason}}},
        {"lastWrite", m_pskReceiveLastWrite.isEmpty() ? QVariantMap{{"token", QString{}}, {"id", QString{}}, {"operation", QString{}}, {"state", "none"}, {"reason", QString{}}} : m_pskReceiveLastWrite},
        {"notice", QStringLiteral("This preference is stored in the displayed radio configuration. Read again after changing configurations. Reset requests the radio's initial value; a later observation alone cannot confirm that it is the initial value. Nothing is restored automatically.")}};
}
void KenwoodBackend::publishPskReceive()
{
    m_pskReceiveDirty = false; ++m_pskReceiveStatusRevision;
    emit extensionStatus(QStringLiteral("kenwood"), QStringLiteral("pskReceive"), pskReceiveStatus());
}
void KenwoodBackend::resetPskReceive()
{
    retirePskReceiveWrite(QStringLiteral("Preference session retired; accepted persistent writes cannot be undone automatically."));
    m_pskReceiveWriteTicket = 0; m_pskReceivePredispatch = m_pskReceiveHanded = false;
    m_pskReceiveBankKnown = false; ++m_pskReceiveBankRevision; ++m_pskReceiveEpoch; ++m_pskReceiveSerial;
    m_pskReceiveKnown.clear();
    for (const QString& key : {QStringLiteral("MF"), QStringLiteral("EX00211")}) {
        m_pskReceiveFloors[key] = std::max(m_pskReceiveFloors.value(key), m_controlIngressSequence);
        ++m_pskReceiveRevisions[key];
    }
    m_pskReceiveReading = m_pskReceiveDispatched = m_pskReceiveReadBankKnown = false;
    m_pskReceiveReadBank.reset(); m_pskReceivePending.clear(); m_pskReceivePendingTicket = 0; m_pskReceiveQueue.clear();
    m_pskReceiveTotal = m_pskReceiveCompleted = m_pskReceiveObserved = m_pskReceiveFailed = 0;
    m_pskReceiveReadReason = QStringLiteral("Session retired; last observed values retained without bank authority."); m_pskReceiveDirty = true;
}
void KenwoodBackend::beginPskReceiveRead(const QStringList& keys)
{
    m_pskReceiveQueue = keys; m_pskReceiveReadToken = QString::number(++m_pskReceiveSerial);
    m_pskReceivePending.clear(); m_pskReceivePendingTicket = 0; m_pskReceiveDispatched = false;
    m_pskReceiveReadBankKnown = false; m_pskReceiveReadBank.reset(); m_pskReceiveReading = true;
    m_pskReceiveTotal = keys.size(); m_pskReceiveCompleted = m_pskReceiveObserved = m_pskReceiveFailed = 0;
    m_pskReceiveReadReason = QStringLiteral("Reading current-bank native preferences; no persistent setting is changed.");
}
void KenwoodBackend::completePskReceiveRead(bool observed, const QString& reason)
{
    const QString key = m_pskReceivePending;
    if (observed) { ++m_pskReceiveObserved; }
    else {
        ++m_pskReceiveFailed; m_pskReceiveKnown.remove(key); ++m_pskReceiveRevisions[key];
        if (key == QLatin1String("MF")) {
            m_pskReceiveBankKnown = false; m_pskReceiveReadBankKnown = false; ++m_pskReceiveBankRevision;
            m_pskReceiveKnown.clear();
        }
        retirePskReceiveWrite(reason);
    }
    ++m_pskReceiveCompleted; m_pskReceivePending.clear(); m_pskReceivePendingTicket = 0; m_pskReceiveDispatched = false;
    if (!m_pskReceiveQueue.isEmpty()) { return; }
    m_pskReceiveReading = false;
    m_pskReceiveReadReason = m_pskReceiveFailed ? QStringLiteral("Read finished with unqualified observations; previous values retained.") : QStringLiteral("Fresh current-bank observations received.");
    if (!m_pskReceiveWriteTicket) { return; }
    const QString writeKey = preferenceKey(m_pskReceiveWriteId);
    const bool qualified = !m_pskReceiveFailed && !m_pskReceiveOutcomeRetired && m_pskReceiveReadBankKnown
        && m_pskReceiveReadBank == m_pskReceiveWriteBank && m_pskReceiveBankKnown && m_pskReceiveBank == m_pskReceiveWriteBank
        && m_pskReceiveKnown.contains(writeKey);
    const bool consistent = qualified && !m_pskReceiveWriteReset && m_pskReceiveValues.value(writeKey) == m_pskReceiveWriteValue;
    m_pskReceiveLastWrite.insert("state", m_pskReceiveWriteReset && qualified ? QStringLiteral("sent_unverified") : consistent ? QStringLiteral("observed_consistent") : QStringLiteral("indeterminate"));
    m_pskReceiveLastWrite.insert("reason", m_pskReceiveWriteReset && qualified ? QStringLiteral("Fresh native value observed after the reset request; default correspondence and reset causality remain unverified.")
        : consistent ? QStringLiteral("Fresh bank-qualified observation matches the request; no causal acknowledgement inferred.") : QStringLiteral("Persistent preference outcome remains indeterminate; no retry, inverse or restoration."));
    m_pskReceiveWriteTicket = 0; m_pskReceiveHanded = false;
}

void KenwoodBackend::adoptPskReceive(const QByteArray& message, quint64 batch)
{
    if (!isConnected() || !m_identityConfirmed) { return; }
    if (message == "?") {
        if (m_pskReceivePendingTicket && m_pskReceiveDispatched && batch > m_pskReceiveMinimumBatch) {
            completePskReceiveRead(false, QStringLiteral("Ambiguous radio error while a native preference read was pending."));
            refreshPskReceiveContext(); m_pskReceiveDirty = true;
        }
        return;
    }
    QString key;
    if (message.startsWith("MF")) { key = "MF"; }
    else if (message.startsWith("EX00211")) { key = "EX00211"; }
    else { return; }
    if (batch < m_pskReceiveBatches.value(key) || batch <= m_pskReceiveFloors.value(key)) { return; }
    const bool pendingMatch = m_pskReceivePendingTicket && m_pskReceivePending == key;
    if (pendingMatch && batch <= m_pskReceiveMinimumBatch) { return; }
    const bool matching = pendingMatch && m_pskReceiveDispatched && batch > m_pskReceiveMinimumBatch;
    std::optional<int> value;
    if (key == QLatin1String("MF")) {
        if (message == "MF0" || message == "MF1") { value = message[2] - '0'; }
    } else if (message.size() == 11 && message.mid(7, 3) == " 00" && (message[10] == '0' || message[10] == '1')) { value = message[10] - '0'; }
    if (!value) {
        if (matching) { completePskReceiveRead(false, QStringLiteral("Malformed native preference reply; previous observation retained without authority.")); }
        refreshPskReceiveContext(); m_pskReceiveDirty = true; return;
    }
    if (m_pskReceivePredispatch && key == preferenceKey(m_pskReceiveWriteId)) {
        const bool unchanged = m_pskReceiveKnown.contains(key) && m_pskReceiveValues.value(key) == *value;
        if (unchanged) { m_pskReceiveBatches[key] = batch; return; }
        retirePskReceiveWrite(QStringLiteral("A changed native preference superseded the unsent persistent request."));
    }
    m_pskReceiveBatches[key] = batch;
    if (key == QLatin1String("MF")) {
        const bool newlyQualified = !m_pskReceiveBankKnown;
        const bool ownedInitialAcquisition = newlyQualified && matching && m_pskReceiveDispatched && !m_pskReceiveWriteTicket;
        const bool bankChanged = m_pskReceiveBank && *m_pskReceiveBank != *value && !ownedInitialAcquisition;
        if (bankChanged || newlyQualified) {
            m_pskReceiveKnown.clear();
            for (const PskPreference& row : kPskReceive) {
                ++m_pskReceiveRevisions[row.key];
                m_pskReceiveFloors[row.key] = std::max(m_pskReceiveFloors.value(row.key), batch);
            }
            ++m_pskReceiveBankRevision;
        }
        if (bankChanged) {
            retirePskReceiveWrite(QStringLiteral("Configuration bank changed; persistent outcome cannot be attributed or restored automatically."));
            m_pskReceiveWriteTicket = 0; m_pskReceiveHanded = m_pskReceivePredispatch = false;
            ++m_pskReceiveSerial; m_pskReceiveReading = m_pskReceiveDispatched = m_pskReceiveReadBankKnown = false;
            m_pskReceiveQueue.clear(); m_pskReceivePending.clear(); m_pskReceivePendingTicket = 0;
            m_pskReceiveReadReason = QStringLiteral("Bank changed; read the new bank deliberately.");
        }
        m_pskReceiveBank = *value; m_pskReceiveBankKnown = true;
        if (matching && !bankChanged) {
            m_pskReceiveReadBank = *value; m_pskReceiveReadBankKnown = true;
            completePskReceiveRead(true, {});
        }
    } else {
        const bool ownedBank = m_pskReceiveBankKnown && (!m_pskReceiveReading
            || (m_pskReceiveReadBankKnown && m_pskReceiveReadBank == m_pskReceiveBank));
        if (!ownedBank) {
            if (matching) { completePskReceiveRead(false, QStringLiteral("This read has no successful current-bank MF observation; preference remains unqualified.")); }
            refreshPskReceiveContext(); m_pskReceiveDirty = true; return;
        }
        const bool changed = !m_pskReceiveKnown.contains(key) || m_pskReceiveValues.value(key) != *value;
        m_pskReceiveValues[key] = *value; m_pskReceiveKnown.insert(key);
        if (changed) { ++m_pskReceiveRevisions[key]; }
        if (matching) {
            completePskReceiveRead(true, {});
        }
    }
    refreshPskReceiveContext(); m_pskReceiveDirty = true;
}
void KenwoodBackend::pollPskReceive(qint64 nowMs)
{
    const QPointer<KenwoodBackend> guard(this); const quint64 generation = m_generation;
    refreshPskReceiveContext();
    if (m_pskReceiveDirty) { publishPskReceive(); if (!guard || generation != m_generation) { return; } }
    if (!m_pskReceiveReading || !isConnected() || !m_identityConfirmed) { return; }
    if (m_pskReceivePendingTicket) {
        if (m_pskReceiveDispatched && nowMs >= m_pskReceiveDeadline) {
            completePskReceiveRead(false, QStringLiteral("Native preference read timed out; no retry or restoration."));
            refreshPskReceiveContext(); publishPskReceive();
        }
        return;
    }
    if (m_pskReceiveQueue.isEmpty()) { return; }
    const QString key = m_pskReceiveQueue.takeFirst(); const quint64 ticket = ++m_pskReceiveSerial;
    m_pskReceivePending = key; m_pskReceivePendingTicket = ticket; m_pskReceiveDispatched = false;
    m_pskReceiveMinimumBatch = m_controlIngressSequence;
    publishPskReceive();
    if (!guard || generation != m_generation || ticket != m_pskReceivePendingTicket || !m_pskReceiveReading) { return; }
    m_pskReceiveMinimumBatch = m_controlIngressSequence;
    m_pskReceiveFloors[key] = std::max(m_pskReceiveFloors.value(key), m_controlIngressSequence);
    m_pskReceiveDispatched = true; m_pskReceiveDeadline = nowMs + 2000;
    const bool sent = send(key.toLatin1());
    if (!guard || generation != m_generation || ticket != m_pskReceivePendingTicket) { return; }
    if (!sent) {
        completePskReceiveRead(false, QStringLiteral("Native preference read transport refused; no retry."));
        refreshPskReceiveContext(); publishPskReceive();
    }
}
bool KenwoodBackend::handlePskReceiveExtension(const QString& verb, quint64 requestId, const QVariant& argument)
{
    if (!verb.startsWith(QLatin1String("psk.receive."))) { return false; }
    const bool map = !argument.isValid() || argument.typeId() == QMetaType::QVariantMap;
    const QVariantMap args = argument.toMap();
    if (verb == QLatin1String("psk.receive.get")) {
        if (!map || !args.isEmpty()) { emit extensionError(requestId, QStringLiteral("PSK receive getter accepts no arguments.")); }
        else { emit extensionResult(requestId, pskReceiveStatus()); }
        return true;
    }
    refreshPskReceiveContext();
    if (!map || args.value("context").typeId() != QMetaType::QString || args.value("context").toString() != pskReceiveContext()) {
        emit extensionError(requestId, QStringLiteral("Requires the current native PSK receive context.")); return true;
    }
    if (!isConnected() || !m_identityConfirmed) { emit extensionError(requestId, QStringLiteral("Requires a connected, identified radio.")); return true; }
    const QPointer<KenwoodBackend> guard(this); const quint64 generation = m_generation;
    if (verb == QLatin1String("psk.receive.read") || verb == QLatin1String("psk.receive.cancel")) {
        if (verb == QLatin1String("psk.receive.read")) {
            if (args.size() != 1 || m_pskReceiveReading || m_pskReceiveWriteTicket) { emit extensionError(requestId, QStringLiteral("Invalid read arguments or native preference work is pending.")); return true; }
            beginPskReceiveRead({"MF", "EX00211"});
        } else {
            if (!m_pskReceiveReading || args.size() != 2 || args.value("token").toString() == QLatin1String("0") || !preferenceSequence(args.value("token")) || args.value("token").toString() != m_pskReceiveReadToken) { emit extensionError(requestId, QStringLiteral("Requires the current native preference read token.")); return true; }
            retirePskReceiveWrite(QStringLiteral("Verification canceled locally; the persistent request was not undone."));
            m_pskReceiveWriteTicket = 0; m_pskReceiveHanded = m_pskReceivePredispatch = false;
            m_pskReceiveReading = m_pskReceiveDispatched = false; m_pskReceiveQueue.clear(); m_pskReceivePending.clear(); m_pskReceivePendingTicket = 0;
            ++m_pskReceiveSerial; m_pskReceiveReadReason = QStringLiteral("Read canceled locally; no bank switch or preference restoration.");
        }
        const quint64 serial = m_pskReceiveSerial; publishPskReceive();
        if (guard && generation == m_generation) {
            if (serial == m_pskReceiveSerial) { emit extensionResult(requestId, pskReceiveStatus()); }
            else { emit extensionError(requestId, QStringLiteral("Native preference scheduling request was superseded.")); }
        }
        return true;
    }
    const bool reset = verb == QLatin1String("psk.receive.reset");
    if ((!reset && verb != QLatin1String("psk.receive.set")) || args.size() != (reset ? 3 : 4)
        || args.value("id").typeId() != QMetaType::QString || !preferenceSequence(args.value("revision"))
        || (!reset && (args.value("value").typeId() != QMetaType::Int || args.value("value").toInt() < 0 || args.value("value").toInt() > 1))) {
        emit extensionError(requestId, QStringLiteral("Requires a native preference ID, exact revision and integer choice for Apply.")); return true;
    }
    const QString id = args.value("id").toString(); const QString key = preferenceKey(id);
    if (key.isEmpty() || !m_pskReceiveKnown.contains(key) || args.value("revision").toString() != QString::number(m_pskReceiveRevisions.value(key))) {
        emit extensionError(requestId, QStringLiteral("Read this bank's native preference baseline before Apply or Reset.")); return true;
    }
    const QString reason = pskReceiveUnavailable();
    if (!reason.isEmpty()) { emit extensionError(requestId, reason); return true; }
    const quint64 ticket = ++m_pskReceiveSerial; const QString token = QString::number(ticket); const QString context = pskReceiveContext();
    m_pskReceiveWriteTicket = ticket; m_pskReceiveWriteId = id; m_pskReceiveWriteBank = m_pskReceiveBank;
    m_pskReceiveWriteReset = reset; m_pskReceiveWriteValue = args.value("value").toInt();
    m_pskReceivePredispatch = true; m_pskReceiveHanded = m_pskReceiveOutcomeRetired = false;
    m_pskReceiveLastWrite = {{"token", token}, {"id", id}, {"operation", reset ? QStringLiteral("reset") : QStringLiteral("set")}, {"state", "pending"}, {"reason", "Awaiting explicit persistent native handoff."}};
    publishPskReceive();
    if (!guard || generation != m_generation) { return true; }
    refreshPskReceiveContext();
    if (ticket != m_pskReceiveWriteTicket || context != pskReceiveContext() || !m_pskReceiveBankKnown || m_pskReceiveBank != m_pskReceiveWriteBank) {
        if (ticket == m_pskReceiveWriteTicket) { retirePskReceiveWrite(QStringLiteral("Persistent preference intent retired before handoff.")); }
        emit extensionError(requestId, QStringLiteral("Persistent preference intent was superseded before handoff.")); return true;
    }
    m_pskReceiveFloors[key] = std::max(m_pskReceiveFloors.value(key), m_controlIngressSequence);
    m_pskReceivePredispatch = false; m_pskReceiveHanded = true;
    const QByteArray command = key.toLatin1() + (reset ? QByteArray("9") : QByteArray(" ") + QByteArray::number(m_pskReceiveWriteValue).rightJustified(3, '0'));
    const bool sent = send(command);
    if (!guard || generation != m_generation) { return true; }
    if (ticket == m_pskReceiveWriteTicket) {
        if (sent) {
            m_pskReceiveLastWrite.insert("state", m_pskReceiveOutcomeRetired ? QStringLiteral("indeterminate") : QStringLiteral("sent_unverified"));
            m_pskReceiveLastWrite.insert("reason", m_pskReceiveOutcomeRetired ? QStringLiteral("Context retired during handoff; persistent outcome remains indeterminate.") : QStringLiteral("Persistent request sent; no default or causal acknowledgement inferred."));
            beginPskReceiveRead({"MF", key});
        } else { m_pskReceiveHanded = false; retirePskReceiveWrite(QStringLiteral("Persistent native write transport refused.")); }
        publishPskReceive(); if (!guard || generation != m_generation) { return true; }
    }
    if (sent) { emit extensionResult(requestId, QVariantMap{{"sent", true}, {"verified", false}, {"token", token}}); }
    else { emit extensionError(requestId, QStringLiteral("Persistent native preference request could not be sent.")); }
    return true;
}
}
