#include "KenwoodBackend.h"

#include <QByteArray>
#include <QList>
#include <QMap>
#include <QPair>
#include <QSet>
#include <QString>
#include <QStringList>
#include <QVariant>
#include <QVariantMap>
#include <QMetaType>
#include <QPointer>
#include <algorithm>

namespace AetherSDR::kenwood {
namespace {
struct ScanRow { QString id; QString key; QString label; QStringList labels; int minimum; };
const QList<ScanRow> kScanRows{
    {"enabled", "SC0", "VFO scan", {"Off", "On"}, 0},
    {"speed", "SC1", "Scan speed index", {"1", "2", "3", "4", "5", "6", "7", "8", "9"}, 1},
    {"tone", "SC2", "Tone scan", {"Off", "Tone", "CTCSS"}, 0},
    {"type", "SC3", "Scan selection", {"Program", "VFO"}, 0}};
const QStringList kScanReadKeys{"MF", "FR", "MV", "QR", "OM0", "TS", "SC0", "SC1", "SC2", "SC3", "TO", "TN", "CN"};
bool scanSequence(const QVariant& value)
{
    if (value.typeId() != QMetaType::QString || value.toString().isEmpty()) { return false; }
    bool ok = false; const quint64 number = value.toString().toULongLong(&ok);
    return ok && QString::number(number) == value.toString();
}
std::optional<int> scanDigits(const QByteArray& value, int width, int maximum)
{
    if (value.size() != width) { return std::nullopt; }
    int number = 0;
    for (const char digit : value) { if (digit < '0' || digit > '9') { return std::nullopt; } number = number * 10 + digit - '0'; }
    return number <= maximum ? std::optional<int>(number) : std::nullopt;
}
}

QString KenwoodBackend::vfoScanContext() const
{
    return QStringLiteral("vfoScan:%1:%2").arg(m_generation).arg(m_vfoScanEpoch);
}
void KenwoodBackend::retireVfoScanWrite(const QString& reason)
{
    if (!m_vfoScanWriteTicket) { return; }
    m_vfoScanOutcomeRetired = true;
    m_vfoScanLastWrite.insert("state", m_vfoScanHanded ? QStringLiteral("indeterminate") : QStringLiteral("rejected"));
    m_vfoScanLastWrite.insert("reason", reason);
    if (!m_vfoScanHanded) { m_vfoScanWriteTicket = 0; m_vfoScanPredispatch = false; }
}
void KenwoodBackend::refreshVfoScanContext()
{
    QStringList components{QString::number(m_generation), QString::number(isConnected()), QString::number(m_identityConfirmed),
        QString::number(m_txStateKnown), QString::number(m_txObserved), QString::number(m_keyIntent), QString::number(m_cwIntent),
        QString::number(m_atuIntent), QString::number(m_tune ? m_tune->serial : 0), QString::number(m_receiveVfo),
        m_mode, m_tfSet ? (*m_tfSet ? QStringLiteral("1") : QStringLiteral("0")) : QStringLiteral("unknown")};
    for (const QString& key : {QStringLiteral("MF"), QStringLiteral("FR"), QStringLiteral("MV"), QStringLiteral("QR"), QStringLiteral("OM0"), QStringLiteral("TS")}) {
        components.append(m_vfoScanKnown.contains(key) ? QString::number(m_vfoScanValues.value(key)) : QStringLiteral("unknown"));
    }
    const QString signature = components.join(QLatin1Char(':'));
    if (signature == m_vfoScanSignature) { return; }
    m_vfoScanSignature = signature; ++m_vfoScanEpoch; m_vfoScanDirty = true;
    retireVfoScanWrite(QStringLiteral("Scan intent retired by receive/source/mode context transition."));
}
QString KenwoodBackend::vfoScanUnavailable(const QString& id, int value) const
{
    if (!isConnected() || !m_identityConfirmed) { return QStringLiteral("Requires a connected, identified radio."); }
    if (id == QLatin1String("type") && value == 0) { return QStringLiteral("Program scan is unavailable because memory support is disabled in this TS-890S build."); }
    if (m_vfoScanReading || m_vfoScanWriteTicket) { return QStringLiteral("Waiting for the current native scan request to finish."); }
    if (!m_txStateKnown || m_txObserved || m_keyIntent || m_cwIntent || m_atuIntent || m_tune) { return QStringLiteral("Requires confirmed receive state without local transmit or tune intent."); }
    if (m_vfoScanKnown.contains("FR") && m_vfoScanValues.value("FR") != m_receiveVfo) {
        return QStringLiteral("The native scan source and current receive source are incoherent; read fresh source observations before applying.");
    }
    if (!m_vfoScanKnown.contains("FR") || m_vfoScanValues.value("FR") > 1 || m_receiveVfo < 0
        || !m_vfoScanKnown.contains("MV") || m_vfoScanValues.value("MV") != 0
        || !m_vfoScanKnown.contains("QR") || m_vfoScanValues.value("QR") / 10 != 0) {
        return QStringLiteral("Read a current physical VFO source with normal and quick memory inactive; this feature does not support memory scan.");
    }
    if (!m_vfoScanKnown.contains("TS")) { return QStringLiteral("Read the current TF-SET observation."); }
    if (id != QLatin1String("type") && m_vfoScanValues.value("TS") == 1) {
        return id == QLatin1String("enabled") ? QStringLiteral("The scan target during TF-SET is not yet qualified; this is not a native protocol prohibition.")
            : QStringLiteral("The effective receive mode during TF-SET is not yet qualified for this scan setting.");
    }
    if (id == QLatin1String("enabled") && (!m_vfoScanKnown.contains("SC3") || m_vfoScanValues.value("SC3") != 1)) { return QStringLiteral("Read or explicitly select VFO scan before starting or stopping this scoped VFO feature; Program scan is not supported."); }
    if (id == QLatin1String("speed") || id == QLatin1String("tone")) {
        if (!m_vfoScanKnown.contains("OM0")) { return QStringLiteral("Read the current native receive mode."); }
        const std::optional<QString> mode = decodeMode(static_cast<char>(m_vfoScanValues.value("OM0")));
        if (!mode) { return QStringLiteral("The native receive mode is unqualified."); }
        if (id == QLatin1String("speed") && (*mode == QLatin1String("FM") || *mode == QLatin1String("DFM"))) { return QStringLiteral("Native scan speed changes are supported in non-FM modes only."); }
        if (id == QLatin1String("tone") && *mode != QLatin1String("FM")) { return *mode == QLatin1String("DFM") ? QStringLiteral("Tone scan in data FM remains unqualified; it is not excluded from the remaining goal.") : QStringLiteral("Native Tone/CTCSS scan settings require standard FM mode."); }
    }
    return {};
}
QVariantMap KenwoodBackend::vfoScanStatus() const
{
    const bool connected = isConnected() && m_identityConfirmed;
    QVariantList rows;
    for (const ScanRow& row : kScanRows) {
        const bool known = connected && m_vfoScanKnown.contains(row.key);
        QVariantList choices; bool editable = false; QString reason;
        for (int index = 0; index < row.labels.size(); ++index) {
            const int value = row.minimum + index;
            QString unavailable = vfoScanUnavailable(row.id, value);
            if (!known && unavailable.isEmpty()) { unavailable = QStringLiteral("Read the native scan value before changing it."); }
            const bool allowed = unavailable.isEmpty(); editable = editable || allowed;
            if (reason.isEmpty() && !allowed) { reason = unavailable; }
            choices.append(QVariantMap{{"value", value}, {"label", row.labels[index]}, {"editable", allowed}, {"reason", unavailable}});
        }
        QVariantMap descriptor{{"id", row.id}, {"label", row.label}, {"revision", QString::number(m_vfoScanRevisions.value(row.key))},
            {"known", known}, {"choices", choices}, {"editable", editable}, {"pending", m_vfoScanWriteTicket != 0 && m_vfoScanWriteId == row.id}, {"reason", editable ? QString{} : reason}};
        if (m_vfoScanValues.contains(row.key)) { descriptor.insert("value", m_vfoScanValues.value(row.key)); }
        rows.append(descriptor);
    }
    const bool physical = connected && m_vfoScanKnown.contains("FR") && m_vfoScanValues.value("FR") < 2
        && m_vfoScanValues.value("FR") == m_receiveVfo;
    const std::optional<QString> mode = connected && m_vfoScanKnown.contains("OM0") ? decodeMode(static_cast<char>(m_vfoScanValues.value("OM0"))) : std::nullopt;
    QVariantMap source{{"physicalSource", physical ? m_vfoScanValues.value("FR") == 0 ? QStringLiteral("A") : QStringLiteral("B") : connected && m_vfoScanKnown.contains("FR") && m_vfoScanValues.value("FR") == 3 ? QStringLiteral("memory") : QStringLiteral("unknown")},
        {"mode", mode.value_or(QStringLiteral("Unknown"))}, {"normalMemoryKnown", connected && m_vfoScanKnown.contains("MV")}, {"quickMemoryKnown", connected && m_vfoScanKnown.contains("QR")},
        {"bankKnown", connected && m_vfoScanKnown.contains("MF")}, {"slowRangeKnown", connected && m_vfoScanKnown.contains("SC0")},
        {"tfSetKnown", connected && m_vfoScanKnown.contains("TS")}, {"qualification", QStringLiteral("VFO-only scan; TF-SET target/mode paths remain unqualified except mode-independent VFO selection.")}};
    if (m_vfoScanKnown.contains("SC0")) { source.insert("slowRange", m_vfoScanValues.value("slow") != 0); }
    if (m_vfoScanKnown.contains("TS")) { source.insert("tfSet", m_vfoScanValues.value("TS") != 0); }
    QVariantMap tones;
    for (const QPair<QString, QString>& item : {QPair<QString, QString>{"type", "TO"}, {"index", "TN"}, {"ctcssIndex", "CN"}}) {
        const bool known = connected && m_vfoScanKnown.contains(item.second);
        QVariantMap observation{{"known", known}, {"revision", QString::number(m_vfoScanRevisions.value(item.second))}, {"reason", known ? QString{} : QStringLiteral("Read the actual native tone setting; no preference is inferred or restored.")}};
        if (m_vfoScanValues.contains(item.second)) { observation.insert("value", m_vfoScanValues.value(item.second)); }
        tones.insert(item.first, observation);
    }
    return {{"statusRevision", QString::number(m_vfoScanStatusRevision)}, {"context", vfoScanContext()}, {"connected", connected}, {"reason", connected ? QString{} : QStringLiteral("Requires a connected, identified radio.")},
        {"rows", rows}, {"source", source}, {"toneObservations", tones},
        {"refresh", QVariantMap{{"token", m_vfoScanReadToken}, {"busy", m_vfoScanReading}, {"total", m_vfoScanTotal}, {"completed", m_vfoScanCompleted}, {"observed", m_vfoScanObserved}, {"failed", m_vfoScanFailed}, {"skipped", 0}, {"pendingKey", m_vfoScanPending}, {"reason", m_vfoScanReadReason}}},
        {"lastWrite", m_vfoScanLastWrite.isEmpty() ? QVariantMap{{"token", QString{}}, {"id", QString{}}, {"state", "none"}, {"reason", QString{}}} : m_vfoScanLastWrite},
        {"notice", QStringLiteral("Tone/CTCSS scan automatically enables the corresponding native tone function. No companion setting is restored automatically. Program/memory scan is excluded; scan readback is not causal verification.")}};
}
void KenwoodBackend::publishVfoScan()
{
    m_vfoScanDirty = false; ++m_vfoScanStatusRevision;
    emit extensionStatus(QStringLiteral("kenwood"), QStringLiteral("vfoScan"), vfoScanStatus());
}
void KenwoodBackend::resetVfoScan()
{
    retireVfoScanWrite(QStringLiteral("Native scan session retired; no stop or restoration was sent."));
    m_vfoScanWriteTicket = 0; m_vfoScanPredispatch = m_vfoScanHanded = false;
    ++m_vfoScanEpoch; ++m_vfoScanSerial; m_vfoScanKnown.clear(); m_vfoScanQueue.clear();
    for (const QString& key : kScanReadKeys) { m_vfoScanFloors[key] = std::max(m_vfoScanFloors.value(key), m_controlIngressSequence); ++m_vfoScanRevisions[key]; }
    m_vfoScanReading = m_vfoScanDispatched = false; m_vfoScanPending.clear(); m_vfoScanPendingTicket = 0;
    m_vfoScanCompleted = m_vfoScanTotal = m_vfoScanObserved = m_vfoScanFailed = 0;
    m_vfoScanReadReason = QStringLiteral("Session retired; last observed values retained without authority."); m_vfoScanDirty = true;
}
void KenwoodBackend::beginVfoScanRead(const QStringList& keys)
{
    m_vfoScanQueue = keys; m_vfoScanReadToken = QString::number(++m_vfoScanSerial);
    m_vfoScanReading = true; m_vfoScanPending.clear(); m_vfoScanPendingTicket = 0;
    m_vfoScanDispatched = false; m_vfoScanTotal = keys.size(); m_vfoScanCompleted = m_vfoScanObserved = m_vfoScanFailed = 0;
    m_vfoScanReadReason = QStringLiteral("Explicit native scan read in progress.");
}
void KenwoodBackend::completeVfoScanRead(bool observed, const QString& reason)
{
    const QString key = m_vfoScanPending;
    if (observed) { ++m_vfoScanObserved; }
    else { ++m_vfoScanFailed; m_vfoScanKnown.remove(key); ++m_vfoScanRevisions[key]; retireVfoScanWrite(reason); }
    ++m_vfoScanCompleted; m_vfoScanPending.clear(); m_vfoScanPendingTicket = 0; m_vfoScanDispatched = false;
    if (!m_vfoScanQueue.isEmpty()) { return; }
    m_vfoScanReading = false;
    m_vfoScanReadReason = m_vfoScanFailed ? QStringLiteral("Read finished with unqualified observations; previous values retained.") : QStringLiteral("Native scan observations received; no causal acknowledgement inferred.");
    if (!m_vfoScanWriteTicket) { return; }
    QString keyForWrite;
    for (const ScanRow& row : kScanRows) { if (row.id == m_vfoScanWriteId) { keyForWrite = row.key; } }
    const bool consistent = !m_vfoScanFailed && !m_vfoScanOutcomeRetired && m_vfoScanKnown.contains(keyForWrite) && m_vfoScanValues.value(keyForWrite) == m_vfoScanWriteValue;
    m_vfoScanLastWrite.insert("state", consistent ? QStringLiteral("observed_consistent") : QStringLiteral("indeterminate"));
    m_vfoScanLastWrite.insert("reason", consistent ? QStringLiteral("Native observation is consistent with the request; no causal verification inferred.") : QStringLiteral("Native outcome remains indeterminate; no automatic stop, retry or restoration."));
    m_vfoScanWriteTicket = 0; m_vfoScanHanded = false;
}

void KenwoodBackend::adoptVfoScan(const QByteArray& message, quint64 batch)
{
    if (!isConnected() || !m_identityConfirmed) { return; }
    if (const std::optional<VfoState> state = decodeVfoState(message)) {
        if (state->vfo == m_receiveVfo && m_vfoScanKnown.contains("OM0")) {
            const std::optional<QString> observedMode = decodeMode(static_cast<char>(m_vfoScanValues.value("OM0")));
            if (observedMode && *observedMode != state->mode) {
                m_vfoScanKnown.remove(QStringLiteral("OM0"));
                m_vfoScanFloors["OM0"] = std::max(m_vfoScanFloors.value("OM0"), batch);
                ++m_vfoScanRevisions["OM0"]; refreshVfoScanContext(); m_vfoScanDirty = true;
            }
        }
        return;
    }
    if (message == "?") {
        if (m_vfoScanPendingTicket && m_vfoScanDispatched && batch > m_vfoScanMinimumBatch) {
            completeVfoScanRead(false, QStringLiteral("Ambiguous radio error during native scan query; no causal attribution inferred.")); m_vfoScanDirty = true;
        }
        return;
    }
    QString key; std::optional<int> value; std::optional<int> slow;
    if (message.startsWith("SC0")) {
        key = "SC0";
        if (message.size() == 5) { value = scanDigits(message.mid(3, 1), 1, 1); slow = scanDigits(message.mid(4), 1, 1); }
        if (!slow) { value.reset(); }
    } else if (message.startsWith("SC1")) { key = "SC1"; value = scanDigits(message.mid(3), 1, 9); if (value == std::optional<int>(0)) { value.reset(); } }
    else if (message.startsWith("SC2")) { key = "SC2"; value = scanDigits(message.mid(3), 1, 2); }
    else if (message.startsWith("SC3")) { key = "SC3"; value = scanDigits(message.mid(3), 1, 1); }
    else if (message.startsWith("TO")) { key = "TO"; value = scanDigits(message.mid(2), 1, 3); }
    else if (message.startsWith("TN")) { key = "TN"; value = scanDigits(message.mid(2), 2, 50); }
    else if (message.startsWith("CN")) { key = "CN"; value = scanDigits(message.mid(2), 2, 49); }
    else if (message.startsWith("MF")) { key = "MF"; value = scanDigits(message.mid(2), 1, 1); }
    else if (message.startsWith("FR")) { key = "FR"; value = scanDigits(message.mid(2), 1, 3); if (value == std::optional<int>(2)) { value.reset(); } }
    else if (message.startsWith("MV")) { key = "MV"; value = scanDigits(message.mid(2), 1, 1); }
    else if (message.startsWith("QR")) {
        key = "QR";
        if (message.size() == 4) {
            const std::optional<int> state = scanDigits(message.mid(2, 1), 1, 1);
            const std::optional<int> channel = message[3] == ' ' ? std::optional<int>(0) : scanDigits(message.mid(3), 1, 9);
            if (state && channel) { value = *state * 10 + *channel; }
        }
    } else if (message.startsWith("OM0")) {
        key = "OM0";
        if (message.size() == 4 && decodeMode(message[3])) { value = static_cast<unsigned char>(message[3]); }
    } else if (message.startsWith("TS")) { key = "TS"; value = scanDigits(message.mid(2), 1, 1); }
    else { return; }
    if (batch < m_vfoScanBatches.value(key) || batch <= m_vfoScanFloors.value(key)) { return; }
    const bool matching = m_vfoScanPendingTicket && key == m_vfoScanPending;
    if (matching && (!m_vfoScanDispatched || batch <= m_vfoScanMinimumBatch)) { return; }
    if (!value) {
        if (matching) { completeVfoScanRead(false, QStringLiteral("Malformed native scan reply; last observed value retained without qualification.")); }
        m_vfoScanDirty = true; return;
    }
    const bool changed = !m_vfoScanKnown.contains(key) || m_vfoScanValues.value(key) != *value;
    if (m_vfoScanPredispatch) {
        QString writeKey;
        for (const ScanRow& row : kScanRows) { if (row.id == m_vfoScanWriteId) { writeKey = row.key; } }
        if (key == writeKey) {
            if (!changed) { return; }
            retireVfoScanWrite(QStringLiteral("A changed native scan observation superseded the unsent request."));
        }
    }
    if (changed && key == QLatin1String("SC3") && (m_vfoScanPredispatch || (m_vfoScanHanded && m_vfoScanWriteId == QLatin1String("enabled")))) {
        retireVfoScanWrite(QStringLiteral("Scan selection changed; the enabled request no longer owns a qualified VFO-scan outcome."));
    }
    m_vfoScanBatches[key] = batch;
    if (changed && (key == QLatin1String("MF") || key == QLatin1String("FR") || key == QLatin1String("MV") || key == QLatin1String("QR") || key == QLatin1String("OM0") || key == QLatin1String("TS"))) {
        if (key != QLatin1String("OM0") && m_vfoScanKnown.contains(key)) { m_vfoScanKnown.remove(QStringLiteral("OM0")); }
        for (const ScanRow& row : kScanRows) {
            m_vfoScanKnown.remove(row.key); ++m_vfoScanRevisions[row.key];
            m_vfoScanFloors[row.key] = std::max(m_vfoScanFloors.value(row.key), batch);
        }
        if (key == QLatin1String("MF") && m_vfoScanKnown.contains("MF")) {
            for (const QString& contextKey : {QStringLiteral("FR"), QStringLiteral("MV"), QStringLiteral("QR"), QStringLiteral("OM0"), QStringLiteral("TS")}) {
                m_vfoScanKnown.remove(contextKey); m_vfoScanFloors[contextKey] = std::max(m_vfoScanFloors.value(contextKey), batch);
            }
        }
    }
    m_vfoScanKnown.insert(key); m_vfoScanValues[key] = *value;
    if (slow) { m_vfoScanValues["slow"] = *slow; }
    if (changed) { ++m_vfoScanRevisions[key]; }
    if (matching) { completeVfoScanRead(true, {}); }
    refreshVfoScanContext(); m_vfoScanDirty = true;
}
void KenwoodBackend::pollVfoScan(qint64 nowMs)
{
    const QPointer<KenwoodBackend> guard(this); const quint64 generation = m_generation;
    refreshVfoScanContext();
    if (m_vfoScanDirty) { publishVfoScan(); if (!guard || generation != m_generation) { return; } }
    if (!m_vfoScanReading || !isConnected() || !m_identityConfirmed) { return; }
    if (m_vfoScanPendingTicket) {
        if (m_vfoScanDispatched && nowMs >= m_vfoScanDeadline) { completeVfoScanRead(false, QStringLiteral("Native scan query timed out; no retry or automatic stop.")); publishVfoScan(); }
        return;
    }
    if (m_vfoScanQueue.isEmpty()) { return; }
    const QString key = m_vfoScanQueue.takeFirst();
    const quint64 ticket = ++m_vfoScanSerial;
    m_vfoScanPending = key; m_vfoScanPendingTicket = ticket; m_vfoScanDispatched = false;
    publishVfoScan();
    if (!guard || generation != m_generation || ticket != m_vfoScanPendingTicket || !m_vfoScanReading) { return; }
    m_vfoScanMinimumBatch = m_controlIngressSequence;
    m_vfoScanFloors[key] = std::max(m_vfoScanFloors.value(key), m_controlIngressSequence);
    m_vfoScanDispatched = true; m_vfoScanDeadline = nowMs + 2000;
    const bool sent = send(key.toLatin1());
    if (!guard || generation != m_generation || ticket != m_vfoScanPendingTicket) { return; }
    if (!sent) { completeVfoScanRead(false, QStringLiteral("Native scan read transport refused; no retry or automatic stop.")); publishVfoScan(); }
}

bool KenwoodBackend::handleVfoScanExtension(const QString& verb, quint64 requestId, const QVariant& argument)
{
    if (!verb.startsWith(QLatin1String("scan.vfo."))) { return false; }
    const bool map = !argument.isValid() || argument.typeId() == QMetaType::QVariantMap;
    const QVariantMap args = argument.toMap();
    if (verb == QLatin1String("scan.vfo.get")) {
        if (!map || !args.isEmpty()) { emit extensionError(requestId, QStringLiteral("Native VFO scan getter accepts no arguments.")); }
        else { emit extensionResult(requestId, vfoScanStatus()); }
        return true;
    }
    refreshVfoScanContext();
    if (!map || args.value("context").typeId() != QMetaType::QString || args.value("context").toString() != vfoScanContext()) {
        emit extensionError(requestId, QStringLiteral("Requires the current native VFO scan context.")); return true;
    }
    if (!isConnected() || !m_identityConfirmed) { emit extensionError(requestId, QStringLiteral("Requires a connected, identified radio.")); return true; }
    const QPointer<KenwoodBackend> guard(this); const quint64 generation = m_generation;
    if (verb == QLatin1String("scan.vfo.read") || verb == QLatin1String("scan.vfo.cancel")) {
        if (verb == QLatin1String("scan.vfo.read")) {
            if (args.size() != 1 || m_vfoScanReading || m_vfoScanWriteTicket) { emit extensionError(requestId, QStringLiteral("A native VFO scan read or write is already pending, or arguments are invalid.")); return true; }
            beginVfoScanRead(kScanReadKeys);
        } else {
            if (args.size() != 2 || !scanSequence(args.value("token")) || args.value("token").toString() != m_vfoScanReadToken) { emit extensionError(requestId, QStringLiteral("Requires the current native VFO scan read token.")); return true; }
            retireVfoScanWrite(QStringLiteral("Verification canceled locally; the radio was not stopped or restored."));
            m_vfoScanWriteTicket = 0; m_vfoScanHanded = m_vfoScanPredispatch = false;
            m_vfoScanReading = m_vfoScanDispatched = false; m_vfoScanPending.clear(); m_vfoScanPendingTicket = 0; m_vfoScanQueue.clear();
            m_vfoScanReadReason = QStringLiteral("Read canceled locally; no automatic SC00 or companion restoration.");
        }
        const QString token = m_vfoScanReadToken; publishVfoScan();
        if (guard && generation == m_generation) {
            if (token == m_vfoScanReadToken) { emit extensionResult(requestId, vfoScanStatus()); }
            else { emit extensionError(requestId, QStringLiteral("Native VFO scan scheduling request was superseded.")); }
        }
        return true;
    }
    if (verb != QLatin1String("scan.vfo.set") || args.size() != 4 || args.value("id").typeId() != QMetaType::QString || args.value("value").typeId() != QMetaType::Int || !scanSequence(args.value("revision"))) {
        emit extensionError(requestId, QStringLiteral("Requires a native scan row ID, integer choice and current revision.")); return true;
    }
    const QString id = args.value("id").toString(); const int value = args.value("value").toInt(); QString key;
    for (const ScanRow& row : kScanRows) { if (row.id == id && value >= row.minimum && value < row.minimum + row.labels.size()) { key = row.key; } }
    if (key.isEmpty() || !m_vfoScanKnown.contains(key) || args.value("revision").toString() != QString::number(m_vfoScanRevisions.value(key))) {
        emit extensionError(requestId, QStringLiteral("Native scan choice or baseline revision is unqualified.")); return true;
    }
    const QString reason = vfoScanUnavailable(id, value);
    if (!reason.isEmpty()) { emit extensionError(requestId, reason); return true; }
    const quint64 ticket = ++m_vfoScanSerial; const QString token = QString::number(ticket); const QString context = vfoScanContext();
    m_vfoScanWriteTicket = ticket; m_vfoScanWriteContext = context; m_vfoScanWriteId = id; m_vfoScanWriteValue = value;
    m_vfoScanPredispatch = true; m_vfoScanHanded = m_vfoScanOutcomeRetired = false;
    m_vfoScanLastWrite = {{"token", token}, {"id", id}, {"state", "pending"}, {"reason", "Awaiting explicit native handoff."}};
    publishVfoScan();
    if (!guard || generation != m_generation) { return true; }
    refreshVfoScanContext();
    if (ticket != m_vfoScanWriteTicket || context != vfoScanContext()) {
        if (ticket == m_vfoScanWriteTicket) { retireVfoScanWrite(QStringLiteral("Native scan intent retired before handoff.")); }
        emit extensionError(requestId, QStringLiteral("Native scan intent was superseded before handoff.")); return true;
    }
    m_vfoScanFloors[key] = std::max(m_vfoScanFloors.value(key), m_controlIngressSequence);
    m_vfoScanPredispatch = false; m_vfoScanHanded = true;
    const bool sent = send(key.toLatin1() + QByteArray::number(value));
    if (!guard || generation != m_generation) { return true; }
    if (ticket == m_vfoScanWriteTicket) {
        if (sent) {
            m_vfoScanLastWrite.insert("state", m_vfoScanOutcomeRetired ? QStringLiteral("indeterminate") : QStringLiteral("sent_unverified"));
            m_vfoScanLastWrite.insert("reason", m_vfoScanOutcomeRetired ? QStringLiteral("Context changed during handoff; outcome remains indeterminate.") : QStringLiteral("Native scan request sent; no causal acknowledgement inferred."));
            beginVfoScanRead(id == QLatin1String("tone") ? QStringList{"SC2", "TO", "TN", "CN"} : QStringList{key});
        } else { m_vfoScanHanded = false; retireVfoScanWrite(QStringLiteral("Native scan write transport refused.")); }
        publishVfoScan(); if (!guard || generation != m_generation) { return true; }
    }
    if (sent) { emit extensionResult(requestId, QVariantMap{{"sent", true}, {"verified", false}, {"token", token}}); }
    else { emit extensionError(requestId, QStringLiteral("Native scan request could not be sent.")); }
    return true;
}
}
