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
struct ScanPreference { QString id; QString key; QString label; QStringList choices; QString applicability; };
const QList<ScanPreference> kScanPreferences{
    {"hold", "EX00404", "Scan Hold", {"Off", "On"}, "Applies to non-FM VFO scan; applicability does not restrict editing."},
    {"resume", "EX00405", "Scan Resume", {"Time-operated", "Carrier-operated"}, "Applies to FM scan; applicability does not restrict editing."}};
bool preferenceSequence(const QVariant& value)
{
    if (value.typeId() != QMetaType::QString || value.toString().isEmpty()) { return false; }
    bool ok = false; const quint64 number = value.toString().toULongLong(&ok);
    return ok && QString::number(number) == value.toString();
}
QString preferenceKey(const QString& id)
{
    for (const ScanPreference& row : kScanPreferences) { if (row.id == id) { return row.key; } }
    return {};
}
}
QString KenwoodBackend::scanPreferencesContext() const
{
    return QStringLiteral("scanPreferences:%1:%2").arg(m_generation).arg(m_scanPreferencesEpoch);
}
void KenwoodBackend::retireScanPreferencesWrite(const QString& reason)
{
    if (!m_scanPreferencesWriteTicket) { return; }
    m_scanPreferencesOutcomeRetired = true;
    m_scanPreferencesLastWrite.insert("state", m_scanPreferencesHanded ? QStringLiteral("indeterminate") : QStringLiteral("rejected"));
    m_scanPreferencesLastWrite.insert("reason", reason);
    if (!m_scanPreferencesHanded) { m_scanPreferencesWriteTicket = 0; m_scanPreferencesPredispatch = false; }
}
void KenwoodBackend::refreshScanPreferencesContext()
{
    const QString signature = QStringLiteral("%1:%2:%3:%4:%5:%6:%7:%8:%9:%10:%11")
        .arg(m_generation).arg(isConnected()).arg(m_identityConfirmed).arg(m_scanPreferencesBankKnown)
        .arg(m_scanPreferencesBank.value_or(-1)).arg(m_txStateKnown).arg(m_txObserved)
        .arg(m_keyIntent).arg(m_cwIntent).arg(m_atuIntent).arg(m_tune ? m_tune->serial : 0);
    if (signature == m_scanPreferencesSignature) { return; }
    m_scanPreferencesSignature = signature; ++m_scanPreferencesEpoch; m_scanPreferencesDirty = true;
    retireScanPreferencesWrite(QStringLiteral("Persistent scan preference intent retired by bank/session or receive/local-intent transition."));
}
QString KenwoodBackend::scanPreferencesUnavailable() const
{
    if (!isConnected() || !m_identityConfirmed) { return QStringLiteral("Requires a connected, identified radio."); }
    if (!m_scanPreferencesBankKnown) { return QStringLiteral("Read the current configuration bank before changing persistent preferences."); }
    if (m_scanPreferencesReading || m_scanPreferencesWriteTicket) { return QStringLiteral("Waiting for the current native preference request to finish."); }
    if (!m_txStateKnown || m_txObserved || m_keyIntent || m_cwIntent || m_atuIntent || m_tune) { return QStringLiteral("Requires confirmed receive state without local transmit or tune intent."); }
    return {};
}
QVariantMap KenwoodBackend::scanPreferencesStatus() const
{
    const bool connected = isConnected() && m_identityConfirmed;
    QVariantMap bank{{"known", connected && m_scanPreferencesBankKnown}, {"revision", QString::number(m_scanPreferencesBankRevision)},
        {"reason", connected && m_scanPreferencesBankKnown ? QString{} : QStringLiteral("Read the current native configuration bank; no bank is selected automatically.")}};
    if (m_scanPreferencesBank) { bank.insert("value", *m_scanPreferencesBank); }
    QVariantList rows;
    for (const ScanPreference& row : kScanPreferences) {
        const bool known = connected && m_scanPreferencesBankKnown && m_scanPreferencesKnown.contains(row.key);
        QString reason = scanPreferencesUnavailable();
        if (reason.isEmpty() && !known) { reason = QStringLiteral("Read this bank's native preference before applying or resetting it."); }
        QVariantList choices;
        for (int value = 0; value < row.choices.size(); ++value) { choices.append(QVariantMap{{"value", value}, {"label", row.choices[value]}}); }
        QVariantMap descriptor{{"id", row.id}, {"label", row.label}, {"known", known}, {"revision", QString::number(m_scanPreferencesRevisions.value(row.key))},
            {"choices", choices}, {"editable", reason.isEmpty()}, {"resettable", reason.isEmpty()},
            {"pending", m_scanPreferencesWriteTicket != 0 && m_scanPreferencesWriteId == row.id}, {"reason", reason}, {"applicability", row.applicability}};
        if (m_scanPreferencesValues.contains(row.key)) { descriptor.insert("value", m_scanPreferencesValues.value(row.key)); }
        rows.append(descriptor);
    }
    return {{"statusRevision", QString::number(m_scanPreferencesStatusRevision)}, {"context", scanPreferencesContext()}, {"connected", connected},
        {"reason", connected ? QString{} : QStringLiteral("Requires a connected, identified radio.")}, {"bank", bank}, {"rows", rows},
        {"refresh", QVariantMap{{"token", m_scanPreferencesReadToken}, {"busy", m_scanPreferencesReading}, {"total", m_scanPreferencesTotal},
            {"completed", m_scanPreferencesCompleted}, {"observed", m_scanPreferencesObserved}, {"failed", m_scanPreferencesFailed}, {"skipped", 0},
            {"pendingKey", m_scanPreferencesPending}, {"reason", m_scanPreferencesReadReason}}},
        {"lastWrite", m_scanPreferencesLastWrite.isEmpty() ? QVariantMap{{"token", QString{}}, {"id", QString{}}, {"operation", QString{}}, {"state", "none"}, {"reason", QString{}}} : m_scanPreferencesLastWrite},
        {"notice", QStringLiteral("Persistent radio preferences in the observed configuration bank. Reset requests the radio's initial value; no default, acknowledgement or automatic restoration is inferred. Hold applies outside FM and Resume in FM; editing is not mode-restricted.")}};
}
void KenwoodBackend::publishScanPreferences()
{
    m_scanPreferencesDirty = false; ++m_scanPreferencesStatusRevision;
    emit extensionStatus(QStringLiteral("kenwood"), QStringLiteral("scanPreferences"), scanPreferencesStatus());
}
void KenwoodBackend::resetScanPreferences()
{
    retireScanPreferencesWrite(QStringLiteral("Preference session retired; accepted persistent writes cannot be undone automatically."));
    m_scanPreferencesWriteTicket = 0; m_scanPreferencesPredispatch = m_scanPreferencesHanded = false;
    m_scanPreferencesBankKnown = false; ++m_scanPreferencesBankRevision; ++m_scanPreferencesEpoch; ++m_scanPreferencesSerial;
    m_scanPreferencesKnown.clear();
    for (const QString& key : {QStringLiteral("MF"), QStringLiteral("EX00404"), QStringLiteral("EX00405")}) {
        m_scanPreferencesFloors[key] = std::max(m_scanPreferencesFloors.value(key), m_controlIngressSequence);
        ++m_scanPreferencesRevisions[key];
    }
    m_scanPreferencesReading = m_scanPreferencesDispatched = m_scanPreferencesReadBankKnown = false;
    m_scanPreferencesReadBank.reset(); m_scanPreferencesPending.clear(); m_scanPreferencesPendingTicket = 0; m_scanPreferencesQueue.clear();
    m_scanPreferencesTotal = m_scanPreferencesCompleted = m_scanPreferencesObserved = m_scanPreferencesFailed = 0;
    m_scanPreferencesReadReason = QStringLiteral("Session retired; last observed values retained without bank authority."); m_scanPreferencesDirty = true;
}
void KenwoodBackend::beginScanPreferencesRead(const QStringList& keys)
{
    m_scanPreferencesQueue = keys; m_scanPreferencesReadToken = QString::number(++m_scanPreferencesSerial);
    m_scanPreferencesPending.clear(); m_scanPreferencesPendingTicket = 0; m_scanPreferencesDispatched = false;
    m_scanPreferencesReadBankKnown = false; m_scanPreferencesReadBank.reset(); m_scanPreferencesReading = true;
    m_scanPreferencesTotal = keys.size(); m_scanPreferencesCompleted = m_scanPreferencesObserved = m_scanPreferencesFailed = 0;
    m_scanPreferencesReadReason = QStringLiteral("Reading current-bank native preferences; no persistent setting is changed.");
}
void KenwoodBackend::completeScanPreferencesRead(bool observed, const QString& reason)
{
    const QString key = m_scanPreferencesPending;
    if (observed) { ++m_scanPreferencesObserved; }
    else {
        ++m_scanPreferencesFailed; m_scanPreferencesKnown.remove(key); ++m_scanPreferencesRevisions[key];
        if (key == QLatin1String("MF")) {
            m_scanPreferencesBankKnown = false; m_scanPreferencesReadBankKnown = false; ++m_scanPreferencesBankRevision;
            m_scanPreferencesKnown.clear();
        }
        retireScanPreferencesWrite(reason);
    }
    ++m_scanPreferencesCompleted; m_scanPreferencesPending.clear(); m_scanPreferencesPendingTicket = 0; m_scanPreferencesDispatched = false;
    if (!m_scanPreferencesQueue.isEmpty()) { return; }
    m_scanPreferencesReading = false;
    m_scanPreferencesReadReason = m_scanPreferencesFailed ? QStringLiteral("Read finished with unqualified observations; previous values retained.") : QStringLiteral("Fresh current-bank observations received.");
    if (!m_scanPreferencesWriteTicket) { return; }
    const QString writeKey = preferenceKey(m_scanPreferencesWriteId);
    const bool qualified = !m_scanPreferencesFailed && !m_scanPreferencesOutcomeRetired && m_scanPreferencesReadBankKnown
        && m_scanPreferencesReadBank == m_scanPreferencesWriteBank && m_scanPreferencesBankKnown && m_scanPreferencesBank == m_scanPreferencesWriteBank
        && m_scanPreferencesKnown.contains(writeKey);
    const bool consistent = qualified && !m_scanPreferencesWriteReset && m_scanPreferencesValues.value(writeKey) == m_scanPreferencesWriteValue;
    m_scanPreferencesLastWrite.insert("state", m_scanPreferencesWriteReset && qualified ? QStringLiteral("sent_unverified") : consistent ? QStringLiteral("observed_consistent") : QStringLiteral("indeterminate"));
    m_scanPreferencesLastWrite.insert("reason", m_scanPreferencesWriteReset && qualified ? QStringLiteral("Fresh native value observed after the reset request; default correspondence and reset causality remain unverified.")
        : consistent ? QStringLiteral("Fresh bank-qualified observation matches the request; no causal acknowledgement inferred.") : QStringLiteral("Persistent preference outcome remains indeterminate; no retry, inverse or restoration."));
    m_scanPreferencesWriteTicket = 0; m_scanPreferencesHanded = false;
}

void KenwoodBackend::adoptScanPreferences(const QByteArray& message, quint64 batch)
{
    if (!isConnected() || !m_identityConfirmed) { return; }
    if (message == "?") {
        if (m_scanPreferencesPendingTicket && m_scanPreferencesDispatched && batch > m_scanPreferencesMinimumBatch) {
            completeScanPreferencesRead(false, QStringLiteral("Ambiguous radio error while a native preference read was pending."));
            refreshScanPreferencesContext(); m_scanPreferencesDirty = true;
        }
        return;
    }
    QString key;
    if (message.startsWith("MF")) { key = "MF"; }
    else if (message.startsWith("EX00404")) { key = "EX00404"; }
    else if (message.startsWith("EX00405")) { key = "EX00405"; }
    else { return; }
    if (batch < m_scanPreferencesBatches.value(key) || batch <= m_scanPreferencesFloors.value(key)) { return; }
    const bool pendingMatch = m_scanPreferencesPendingTicket && m_scanPreferencesPending == key;
    if (pendingMatch && batch <= m_scanPreferencesMinimumBatch) { return; }
    const bool matching = pendingMatch && m_scanPreferencesDispatched;
    std::optional<int> value;
    if (key == QLatin1String("MF")) {
        if (message == "MF0" || message == "MF1") { value = message[2] - '0'; }
    } else if (message.size() == 11 && message.mid(7, 3) == " 00" && (message[10] == '0' || message[10] == '1')) { value = message[10] - '0'; }
    if (!value) {
        if (matching) { completeScanPreferencesRead(false, QStringLiteral("Malformed native preference reply; previous observation retained without authority.")); }
        refreshScanPreferencesContext(); m_scanPreferencesDirty = true; return;
    }
    if (m_scanPreferencesPredispatch && key == preferenceKey(m_scanPreferencesWriteId)) {
        const bool unchanged = m_scanPreferencesKnown.contains(key) && m_scanPreferencesValues.value(key) == *value;
        if (unchanged) { return; }
        retireScanPreferencesWrite(QStringLiteral("A changed native preference superseded the unsent persistent request."));
    }
    m_scanPreferencesBatches[key] = batch;
    if (key == QLatin1String("MF")) {
        const bool newlyQualified = !m_scanPreferencesBankKnown;
        const bool ownedInitialAcquisition = newlyQualified && matching && m_scanPreferencesDispatched && !m_scanPreferencesWriteTicket;
        const bool bankChanged = m_scanPreferencesBank && *m_scanPreferencesBank != *value && !ownedInitialAcquisition;
        if (bankChanged || newlyQualified) {
            m_scanPreferencesKnown.clear();
            for (const ScanPreference& row : kScanPreferences) {
                ++m_scanPreferencesRevisions[row.key];
                m_scanPreferencesFloors[row.key] = std::max(m_scanPreferencesFloors.value(row.key), batch);
            }
            ++m_scanPreferencesBankRevision;
        }
        if (bankChanged) {
            retireScanPreferencesWrite(QStringLiteral("Configuration bank changed; persistent outcome cannot be attributed or restored automatically."));
            m_scanPreferencesWriteTicket = 0; m_scanPreferencesHanded = m_scanPreferencesPredispatch = false;
            ++m_scanPreferencesSerial; m_scanPreferencesReading = m_scanPreferencesDispatched = m_scanPreferencesReadBankKnown = false;
            m_scanPreferencesQueue.clear(); m_scanPreferencesPending.clear(); m_scanPreferencesPendingTicket = 0;
            m_scanPreferencesReadReason = QStringLiteral("Bank changed; read the new bank deliberately.");
        }
        m_scanPreferencesBank = *value; m_scanPreferencesBankKnown = true;
        if (matching && !bankChanged) {
            m_scanPreferencesReadBank = *value; m_scanPreferencesReadBankKnown = true;
            completeScanPreferencesRead(true, {});
        }
    } else {
        const bool ownedBank = !m_scanPreferencesReading || (m_scanPreferencesReadBankKnown && m_scanPreferencesReadBank == m_scanPreferencesBank);
        if (!m_scanPreferencesBankKnown || !ownedBank) {
            if (matching) { completeScanPreferencesRead(false, QStringLiteral("This read has no successful current-bank MF observation; preference remains unqualified.")); }
            refreshScanPreferencesContext(); m_scanPreferencesDirty = true; return;
        }
        const bool changed = !m_scanPreferencesKnown.contains(key) || m_scanPreferencesValues.value(key) != *value;
        m_scanPreferencesValues[key] = *value; m_scanPreferencesKnown.insert(key);
        if (changed) { ++m_scanPreferencesRevisions[key]; }
        if (matching) { completeScanPreferencesRead(true, {}); }
    }
    refreshScanPreferencesContext(); m_scanPreferencesDirty = true;
}
void KenwoodBackend::pollScanPreferences(qint64 nowMs)
{
    const QPointer<KenwoodBackend> guard(this); const quint64 generation = m_generation;
    refreshScanPreferencesContext();
    if (m_scanPreferencesDirty) { publishScanPreferences(); if (!guard || generation != m_generation) { return; } }
    if (!m_scanPreferencesReading || !isConnected() || !m_identityConfirmed) { return; }
    if (m_scanPreferencesPendingTicket) {
        if (m_scanPreferencesDispatched && nowMs >= m_scanPreferencesDeadline) {
            completeScanPreferencesRead(false, QStringLiteral("Native preference read timed out; no retry or restoration."));
            refreshScanPreferencesContext(); publishScanPreferences();
        }
        return;
    }
    if (m_scanPreferencesQueue.isEmpty()) { return; }
    const QString key = m_scanPreferencesQueue.takeFirst(); const quint64 ticket = ++m_scanPreferencesSerial;
    m_scanPreferencesPending = key; m_scanPreferencesPendingTicket = ticket; m_scanPreferencesDispatched = false;
    m_scanPreferencesMinimumBatch = m_controlIngressSequence;
    publishScanPreferences();
    if (!guard || generation != m_generation || ticket != m_scanPreferencesPendingTicket || !m_scanPreferencesReading) { return; }
    m_scanPreferencesMinimumBatch = m_controlIngressSequence;
    m_scanPreferencesFloors[key] = std::max(m_scanPreferencesFloors.value(key), m_controlIngressSequence);
    m_scanPreferencesDispatched = true; m_scanPreferencesDeadline = nowMs + 2000;
    const bool sent = send(key.toLatin1());
    if (!guard || generation != m_generation || ticket != m_scanPreferencesPendingTicket) { return; }
    if (!sent) {
        completeScanPreferencesRead(false, QStringLiteral("Native preference read transport refused; no retry."));
        refreshScanPreferencesContext(); publishScanPreferences();
    }
}
bool KenwoodBackend::handleScanPreferencesExtension(const QString& verb, quint64 requestId, const QVariant& argument)
{
    if (!verb.startsWith(QLatin1String("scan.preferences."))) { return false; }
    const bool map = !argument.isValid() || argument.typeId() == QMetaType::QVariantMap;
    const QVariantMap args = argument.toMap();
    if (verb == QLatin1String("scan.preferences.get")) {
        if (!map || !args.isEmpty()) { emit extensionError(requestId, QStringLiteral("Scan preference getter accepts no arguments.")); }
        else { emit extensionResult(requestId, scanPreferencesStatus()); }
        return true;
    }
    refreshScanPreferencesContext();
    if (!map || args.value("context").typeId() != QMetaType::QString || args.value("context").toString() != scanPreferencesContext()) {
        emit extensionError(requestId, QStringLiteral("Requires the current native scan preference context.")); return true;
    }
    if (!isConnected() || !m_identityConfirmed) { emit extensionError(requestId, QStringLiteral("Requires a connected, identified radio.")); return true; }
    const QPointer<KenwoodBackend> guard(this); const quint64 generation = m_generation;
    if (verb == QLatin1String("scan.preferences.read") || verb == QLatin1String("scan.preferences.cancel")) {
        if (verb == QLatin1String("scan.preferences.read")) {
            if (args.size() != 1 || m_scanPreferencesReading || m_scanPreferencesWriteTicket) { emit extensionError(requestId, QStringLiteral("Invalid read arguments or native preference work is pending.")); return true; }
            beginScanPreferencesRead({"MF", "EX00404", "EX00405"});
        } else {
            if (args.size() != 2 || !preferenceSequence(args.value("token")) || args.value("token").toString() != m_scanPreferencesReadToken) { emit extensionError(requestId, QStringLiteral("Requires the current native preference read token.")); return true; }
            retireScanPreferencesWrite(QStringLiteral("Verification canceled locally; the persistent request was not undone."));
            m_scanPreferencesWriteTicket = 0; m_scanPreferencesHanded = m_scanPreferencesPredispatch = false;
            m_scanPreferencesReading = m_scanPreferencesDispatched = false; m_scanPreferencesQueue.clear(); m_scanPreferencesPending.clear(); m_scanPreferencesPendingTicket = 0;
            ++m_scanPreferencesSerial; m_scanPreferencesReadReason = QStringLiteral("Read canceled locally; no bank switch or preference restoration.");
        }
        const quint64 serial = m_scanPreferencesSerial; publishScanPreferences();
        if (guard && generation == m_generation) {
            if (serial == m_scanPreferencesSerial) { emit extensionResult(requestId, scanPreferencesStatus()); }
            else { emit extensionError(requestId, QStringLiteral("Native preference scheduling request was superseded.")); }
        }
        return true;
    }
    const bool reset = verb == QLatin1String("scan.preferences.reset");
    if ((!reset && verb != QLatin1String("scan.preferences.set")) || args.size() != (reset ? 3 : 4)
        || args.value("id").typeId() != QMetaType::QString || !preferenceSequence(args.value("revision"))
        || (!reset && (args.value("value").typeId() != QMetaType::Int || args.value("value").toInt() < 0 || args.value("value").toInt() > 1))) {
        emit extensionError(requestId, QStringLiteral("Requires a native preference ID, exact revision and integer choice for Apply.")); return true;
    }
    const QString id = args.value("id").toString(); const QString key = preferenceKey(id);
    if (key.isEmpty() || !m_scanPreferencesKnown.contains(key) || args.value("revision").toString() != QString::number(m_scanPreferencesRevisions.value(key))) {
        emit extensionError(requestId, QStringLiteral("Read this bank's native preference baseline before Apply or Reset.")); return true;
    }
    const QString reason = scanPreferencesUnavailable();
    if (!reason.isEmpty()) { emit extensionError(requestId, reason); return true; }
    const quint64 ticket = ++m_scanPreferencesSerial; const QString token = QString::number(ticket); const QString context = scanPreferencesContext();
    m_scanPreferencesWriteTicket = ticket; m_scanPreferencesWriteId = id; m_scanPreferencesWriteBank = m_scanPreferencesBank;
    m_scanPreferencesWriteReset = reset; m_scanPreferencesWriteValue = args.value("value").toInt();
    m_scanPreferencesPredispatch = true; m_scanPreferencesHanded = m_scanPreferencesOutcomeRetired = false;
    m_scanPreferencesLastWrite = {{"token", token}, {"id", id}, {"operation", reset ? QStringLiteral("reset") : QStringLiteral("set")}, {"state", "pending"}, {"reason", "Awaiting explicit persistent native handoff."}};
    publishScanPreferences();
    if (!guard || generation != m_generation) { return true; }
    refreshScanPreferencesContext();
    if (ticket != m_scanPreferencesWriteTicket || context != scanPreferencesContext() || !m_scanPreferencesBankKnown || m_scanPreferencesBank != m_scanPreferencesWriteBank) {
        if (ticket == m_scanPreferencesWriteTicket) { retireScanPreferencesWrite(QStringLiteral("Persistent preference intent retired before handoff.")); }
        emit extensionError(requestId, QStringLiteral("Persistent preference intent was superseded before handoff.")); return true;
    }
    m_scanPreferencesFloors[key] = std::max(m_scanPreferencesFloors.value(key), m_controlIngressSequence);
    m_scanPreferencesPredispatch = false; m_scanPreferencesHanded = true;
    const QByteArray command = key.toLatin1() + (reset ? QByteArray("9") : QByteArray(" ") + QByteArray::number(m_scanPreferencesWriteValue).rightJustified(3, '0'));
    const bool sent = send(command);
    if (!guard || generation != m_generation) { return true; }
    if (ticket == m_scanPreferencesWriteTicket) {
        if (sent) {
            m_scanPreferencesLastWrite.insert("state", m_scanPreferencesOutcomeRetired ? QStringLiteral("indeterminate") : QStringLiteral("sent_unverified"));
            m_scanPreferencesLastWrite.insert("reason", m_scanPreferencesOutcomeRetired ? QStringLiteral("Context retired during handoff; persistent outcome remains indeterminate.") : QStringLiteral("Persistent request sent; no default or causal acknowledgement inferred."));
            beginScanPreferencesRead({"MF", key});
        } else { m_scanPreferencesHanded = false; retireScanPreferencesWrite(QStringLiteral("Persistent native write transport refused.")); }
        publishScanPreferences(); if (!guard || generation != m_generation) { return true; }
    }
    if (sent) { emit extensionResult(requestId, QVariantMap{{"sent", true}, {"verified", false}, {"token", token}}); }
    else { emit extensionError(requestId, QStringLiteral("Persistent native preference request could not be sent.")); }
    return true;
}
}
