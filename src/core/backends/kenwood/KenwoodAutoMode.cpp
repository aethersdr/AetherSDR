#include "KenwoodBackend.h"

#include <QChar>
#include <QMetaType>
#include <QPointer>
#include <algorithm>

namespace AetherSDR::kenwood {
namespace {
std::optional<qint64> autoDigits(const QByteArray& bytes, int width)
{
    if (bytes.size() != width) { return std::nullopt; }
    qint64 value = 0;
    for (const char byte : bytes) { if (byte < '0' || byte > '9') { return std::nullopt; } value = value * 10 + byte - '0'; }
    return value;
}
bool autoSequence(const QVariant& value)
{
    if (value.typeId() != QMetaType::QString || value.toString().isEmpty()) { return false; }
    bool ok = false; const quint64 number = value.toString().toULongLong(&ok);
    return ok && QString::number(number) == value.toString();
}
QVariantList autoRows()
{
    QVariantList result;
    for (int index = 0; index < 32; ++index) { result.append(QVariantMap{{"index", index}, {"state", "unread"}, {"revision", "0"}, {"reason", "Not read."}, {"rawUtf8Hex", QString{}}}); }
    return result;
}
QStringList autoKeys()
{
    QStringList result{"MF", "AM", "AS1"};
    for (int index = 0; index < 32; ++index) { result.append(QStringLiteral("AS2%1").arg(index, 2, 10, QLatin1Char('0'))); }
    return result;
}
QStringList autoPairs(const QVariantList& rows)
{
    QStringList result;
    for (const QVariant& item : rows) {
        const QVariantMap row = item.toMap();
        if (row.value("state") == QLatin1String("populated")) {
            result.append(QString::number(row.value("upperHz").toLongLong()) + QLatin1Char(':') + row.value("mode").toString());
        }
    }
    std::sort(result.begin(), result.end()); return result;
}
bool autoDense(const QVariantList& rows, int count)
{
    if (rows.size() != 32) { return false; }
    for (int index = 0; index < 32; ++index) {
        if (rows[index].toMap().value("state") != (index < count ? QLatin1String("populated") : QLatin1String("empty"))) { return false; }
    }
    return true;
}
}

QString KenwoodBackend::autoModeContext() const { return QStringLiteral("auto:%1:%2").arg(m_generation).arg(m_autoModeEpoch); }
bool KenwoodBackend::autoModeWritable() const
{
    return receiveControlsWritable() && !m_tune && m_autoModeBankKnown && m_autoModeBank.has_value() && m_autoModeEnabledKnown && !m_autoModeWriteTicket;
}
void KenwoodBackend::retireAutoModeWrite(const QString& reason)
{
    if (!m_autoModeWriteTicket) { return; }
    m_autoModeLastWrite.insert("state", m_autoModeHanded ? QStringLiteral("indeterminate") : QStringLiteral("rejected"));
    m_autoModeLastWrite.insert("reason", reason);
    m_autoModeWriteTicket = 0; m_autoModePredispatch = false; m_autoModeHanded = false;
    m_autoModeComplete = false; m_autoModeCoherent = false; m_autoModeSnapshotToken.clear();
}
void KenwoodBackend::refreshAutoModeSafety()
{
    const QString physicalMode = m_receiveVfo >= 0 && m_receiveVfo < 2 ? m_vfoModes[m_receiveVfo] : QString{};
    const QString signature = QString::number(isConnected()) + QLatin1Char(':') + QStringLiteral("%1:%2:%3:%4:%5:%6:%7:%8:%9:%10:%11:%12:%13")
        .arg(m_generation).arg(m_identityConfirmed).arg(m_receiveVfo).arg(m_mode).arg(physicalMode)
        .arg(m_tfSet ? (*m_tfSet ? 1 : 0) : -1).arg(m_txStateKnown).arg(m_txObserved)
        .arg(m_keyIntent).arg(m_cwIntent).arg(m_atuIntent).arg(m_tune.has_value()).arg(m_tune ? m_tune->serial : 0);
    if (signature == m_autoModeSafetySignature) { return; }
    m_autoModeSafetySignature = signature; ++m_autoModeSafetyEpoch; m_autoModeAvailabilityDirty = true;
    retireAutoModeWrite(QStringLiteral("Receive safety/source authority changed; operation is retired."));
}
QVariantMap KenwoodBackend::autoModeStatus() const
{
    const bool connected = isConnected() && m_identityConfirmed;
    const bool writable = autoModeWritable() && !m_autoModeReading;
    const QString unavailable = QStringLiteral("Requires confirmed receive state, current bank/AM and no pending operation.");
    const QVariantMap enabled{{"editable", writable}, {"pending", m_autoModeWriteTicket && m_autoModeLastWrite.value("operation") == QLatin1String("enabled")}, {"reason", writable ? QString{} : unavailable}};
    const bool table = writable && m_autoModeComplete && m_autoModeCoherent && m_autoModeCountKnown && !m_autoModeSnapshotToken.isEmpty();
    const QVariantList rows = m_autoModeRows.isEmpty() ? autoRows() : m_autoModeRows;
    const QVariantMap registration{{"editable", table && m_autoModeCount && *m_autoModeCount < 32}, {"pending", m_autoModeWriteTicket && m_autoModeLastWrite.value("operation") == QLatin1String("register")}, {"reason", table && m_autoModeCount && *m_autoModeCount < 32 ? QString{} : QStringLiteral("Requires a complete current table with fewer than 32 divisions.")}};
    const QVariantMap deletion{{"editable", table && m_autoModeCount && *m_autoModeCount > 0 && autoDense(rows, *m_autoModeCount)}, {"pending", m_autoModeWriteTicket && m_autoModeLastWrite.value("operation") == QLatin1String("delete")}, {"reason", table && m_autoModeCount && *m_autoModeCount > 0 && autoDense(rows, *m_autoModeCount) ? QString{} : QStringLiteral("Requires a complete dense table and a populated selected row.")}};
    QVariantList choices;
    for (const char mode : QByteArray("12345679ABCDEF")) { choices.append(QVariantMap{{"value", QString(QChar::fromLatin1(mode))}, {"label", decodeMode(mode).value_or(QStringLiteral("Unknown"))}}); }
    QVariantMap status{{"statusRevision", QString::number(m_autoModeStatusRevision)}, {"context", autoModeContext()}, {"connected", connected}, {"reason", connected ? QString{} : QStringLiteral("Requires a connected identified radio.")},
        {"bankKnown", connected && m_autoModeBankKnown && m_autoModeBank.has_value()}, {"enabledKnown", connected && m_autoModeEnabledKnown}, {"enabledRevision", QString::number(m_autoModeEnabledRevision)}, {"countKnown", connected && m_autoModeCountKnown}, {"rows", rows}, {"modeChoices", choices},
        {"hzDomain", QVariantMap{{"minimum", QVariant::fromValue<qint64>(0)}, {"maximum", QVariant::fromValue<qint64>(99999999999LL)}, {"step", QVariant::fromValue<qint64>(1)}, {"unit", "Hz"}}},
        {"snapshot", QVariantMap{{"token", m_autoModeSnapshotToken}, {"complete", m_autoModeComplete}, {"coherent", m_autoModeCoherent}, {"reason", m_autoModeComplete && m_autoModeCoherent ? QString{} : QStringLiteral("Complete observed table is required.")}}},
        {"refresh", QVariantMap{{"token", m_autoModeReadToken}, {"busy", m_autoModeReading}, {"total", m_autoModeTotal}, {"completed", m_autoModeCompleted}, {"observed", m_autoModeObserved}, {"failed", m_autoModeFailed}, {"pendingKey", m_autoModePendingKey}, {"reason", m_autoModeReadReason}}},
        {"operations", QVariantMap{{"enabled", enabled}, {"register", registration}, {"delete", deletion}}},
        {"lastWrite", m_autoModeLastWrite.isEmpty() ? QVariantMap{{"token", QString{}}, {"operation", QString{}}, {"state", "none"}, {"reason", QString{}}} : m_autoModeLastWrite}};
    if (m_autoModeBank) { status.insert("bank", *m_autoModeBank); }
    if (m_autoModeEnabled) { status.insert("enabled", *m_autoModeEnabled); }
    if (m_autoModeCount) { status.insert("count", *m_autoModeCount); }
    return status;
}
void KenwoodBackend::publishAutoMode() { m_autoModeAvailabilityDirty = false; ++m_autoModeStatusRevision; emit extensionStatus(QStringLiteral("kenwood"), QStringLiteral("autoMode"), autoModeStatus()); }
void KenwoodBackend::resetAutoMode()
{
    retireAutoModeWrite(QStringLiteral("Session or configuration bank retired; accepted writes cannot be undone."));
    ++m_autoModeEpoch; ++m_autoModeOperation; ++m_autoModeSerial;
    for (const QString& key : autoKeys()) { m_autoModeBatches[key] = std::max(m_autoModeBatches.value(key), m_controlIngressSequence); }
    m_autoModeRows = autoRows(); m_autoModeBank.reset(); m_autoModeEnabled.reset(); m_autoModeCount.reset();
    m_autoModeBankKnown = false; m_autoModeEnabledKnown = false; m_autoModeCountKnown = false;
    m_autoModeComplete = false; m_autoModeCoherent = false; m_autoModeSnapshotToken.clear();
    m_autoModeQueue.clear(); m_autoModePendingKey.clear(); m_autoModePendingTicket = 0; m_autoModeReading = false;
    m_autoModeTotal = m_autoModeCompleted = m_autoModeObserved = m_autoModeFailed = 0;
    m_autoModeReadReason = QStringLiteral("Not read."); m_autoModeReadKeys.clear();
}
void KenwoodBackend::preflightAutoModeBank(const QByteArray& message, quint64 batch)
{
    if (message != "MF0" && message != "MF1") { return; }
    if (batch <= m_autoModeBatches.value("MF")) { return; }
    const int bank = message[2] - '0';
    bool observedRows = false;
    for (const QVariant& item : m_autoModeRows) { if (!item.toMap().value("rawUtf8Hex").toString().isEmpty()) { observedRows = true; break; } }
    if ((m_autoModeBank && *m_autoModeBank != bank) || (!m_autoModeBank && observedRows)) { resetAutoMode(); }
    m_autoModeBank = bank; m_autoModeBankKnown = true; m_autoModeBankBatch = batch; m_autoModeBatches["MF"] = batch;
}
void KenwoodBackend::startAutoModeRead(bool enabledOnly)
{
    ++m_autoModeOperation;
    m_autoModeQueue = enabledOnly ? QStringList{QStringLiteral("AM")} : autoKeys();
    m_autoModeReadToken = QString::number(++m_autoModeSerial); m_autoModeReading = true;
    m_autoModeTotal = m_autoModeQueue.size(); m_autoModeCompleted = m_autoModeObserved = m_autoModeFailed = 0;
    m_autoModePendingKey.clear(); m_autoModePendingTicket = 0; m_autoModeReadKeys.clear();
    m_autoModeReadReason = QStringLiteral("Read queued; snapshots are not atomic.");
    if (!enabledOnly) { m_autoModeComplete = false; m_autoModeCoherent = false; m_autoModeSnapshotToken.clear(); }
}
void KenwoodBackend::completeAutoModeReadItem(bool observed, const QString& reason)
{
    const QString key = m_autoModePendingKey;
    if (observed) { ++m_autoModeObserved; m_autoModeReadKeys.insert(key); }
    else {
        ++m_autoModeFailed;
        retireAutoModeWrite(QStringLiteral("Verification read failed; accepted write outcome is indeterminate."));
        if (key.startsWith(QLatin1String("AS2")) && m_autoModeRows.size() == 32) {
            const int index = key.mid(3).toInt(); QVariantMap row = m_autoModeRows[index].toMap();
            row.insert("state", reason.contains(QLatin1String("timed out")) ? QStringLiteral("timeout") : QStringLiteral("invalid")); row.insert("reason", reason); m_autoModeRows[index] = row;
        }
        if (key == QLatin1String("MF")) { m_autoModeBankKnown = false; }
        if (key == QLatin1String("AM")) { m_autoModeEnabledKnown = false; }
        if (key == QLatin1String("AS1")) { m_autoModeCountKnown = false; }
    }
    ++m_autoModeCompleted; m_autoModePendingKey.clear(); m_autoModePendingTicket = 0; m_autoModeDispatched = false;
    if (!m_autoModeQueue.isEmpty()) { return; }
    m_autoModeReading = false;
    m_autoModeReadReason = m_autoModeFailed ? QStringLiteral("Read finished with failures; snapshot incomplete.") : QStringLiteral("Read finished; observations are not an atomic capture.");
    if (m_autoModeTotal == 35) {
        m_autoModeComplete = !m_autoModeFailed && m_autoModeReadKeys.size() == 35 && m_autoModeBankKnown && m_autoModeBank && m_autoModeEnabledKnown && m_autoModeCountKnown;
        m_autoModeCoherent = m_autoModeComplete && m_autoModeCount && autoPairs(m_autoModeRows).size() == *m_autoModeCount;
        if (m_autoModeComplete && m_autoModeCoherent) { m_autoModeSnapshotToken = QString::number(++m_autoModeSerial); }
    }
    if (!m_autoModeWriteTicket) { return; }
    bool consistent = false;
    const QString operation = m_autoModeLastWrite.value("operation").toString();
    if (operation == QLatin1String("enabled")) { consistent = !m_autoModeFailed && m_autoModeEnabledKnown && m_autoModeEnabled == std::optional<bool>(m_autoModeWriteArguments.value("enabled").toBool()); }
    else if (m_autoModeComplete && m_autoModeCoherent && m_autoModeCount) {
        if (operation == QLatin1String("register")) {
            QStringList expected = autoPairs(m_autoModeWriteRows);
            expected.append(QString::number(m_autoModeWriteArguments.value("upperHz").toLongLong()) + QLatin1Char(':') + m_autoModeWriteArguments.value("mode").toString()); std::sort(expected.begin(), expected.end());
            consistent = *m_autoModeCount == m_autoModeWriteCount + 1 && expected == autoPairs(m_autoModeRows);
        } else {
            QVariantList expected = m_autoModeWriteRows; expected.removeAt(m_autoModeWriteArguments.value("index").toInt()); expected.append(autoRows().last());
            consistent = *m_autoModeCount == m_autoModeWriteCount - 1;
            for (int index = 0; index < 32 && consistent; ++index) {
                const QVariantMap left = expected[index].toMap(), right = m_autoModeRows[index].toMap();
                consistent = left.value("state") == right.value("state") && left.value("upperHz") == right.value("upperHz") && left.value("mode") == right.value("mode");
                if (index == 31) { consistent = right.value("state") == QLatin1String("empty"); }
            }
        }
    }
    m_autoModeLastWrite.insert("state", consistent ? QStringLiteral("observed_consistent") : QStringLiteral("indeterminate"));
    m_autoModeLastWrite.insert("reason", consistent ? QStringLiteral("Fresh observations are consistent; no causal acknowledgement or CAS exists.") : QStringLiteral("Post-write observations are incomplete or differ from the captured baseline."));
    m_autoModeWriteTicket = 0; m_autoModeHanded = false;
}

void KenwoodBackend::adoptAutoMode(const QByteArray& message, quint64 batch)
{
    refreshAutoModeSafety();
    if (!isConnected() || !m_identityConfirmed) { return; }
    QString key; QVariantMap row; std::optional<int> count; std::optional<bool> enabled;
    bool valid = false; bool changed = false; int index = -1;
    if (message.startsWith("MF")) {
        if ((message != "MF0" && message != "MF1") || batch != m_autoModeBankBatch || !m_autoModeBank || *m_autoModeBank != message[2] - '0') { return; }
        key = "MF"; valid = true;
    } else if (message.startsWith("AM")) {
        key = "AM"; valid = message == "AM0" || message == "AM1";
        if (valid) { enabled = message[2] == '1'; changed = !m_autoModeEnabled || *m_autoModeEnabled != *enabled; }
    } else if (message.startsWith("AS1")) {
        key = "AS1"; const std::optional<qint64> value = autoDigits(message.mid(3), 2);
        valid = value && *value <= 32;
        if (valid) { count = static_cast<int>(*value); changed = !m_autoModeCount || *m_autoModeCount != *count; }
    } else if (message.startsWith("AS2") && message.size() >= 5) {
        const std::optional<qint64> slot = autoDigits(message.mid(3, 2), 2);
        if (!slot || *slot >= 32) { return; }
        index = static_cast<int>(*slot); key = QString::fromLatin1(message.left(5));
        const std::optional<qint64> hz = autoDigits(message.mid(5, 11), 11);
        const bool empty = message.size() == 17 && hz && *hz == 0 && message[16] == '0';
        const std::optional<QString> mode = message.size() == 17 ? decodeMode(message[16]) : std::nullopt;
        valid = message.size() == 17 && hz && (empty || mode.has_value());
        if (valid) {
            row = {{"index", index}, {"state", empty ? "empty" : "populated"}, {"revision", QStringLiteral("0")}, {"reason", QString{}},
                {"rawUtf8Hex", QString::fromLatin1(message.toHex())}, {"upperHz", QVariant::fromValue(*hz)},
                {"mode", QString(QChar::fromLatin1(message[16]))}, {"modeLabel", empty ? QStringLiteral("Empty") : *mode}};
            if (m_autoModeRows.isEmpty()) { m_autoModeRows = autoRows(); }
            const QVariantMap previous = m_autoModeRows[index].toMap();
            changed = previous.value("upperHz") != row.value("upperHz") || previous.value("mode") != row.value("mode") || previous.value("state") != row.value("state");
        }
    } else { return; }
    const bool matching = key == m_autoModePendingKey && m_autoModePendingTicket;
    if (key != QLatin1String("MF") && batch <= m_autoModeBatches.value(key)) { return; }
    if (matching && (!m_autoModeDispatched || batch <= m_autoModeMinimumBatch)) { return; }
    if (m_autoModePredispatch && key != QLatin1String("MF")) {
        if (!valid || !changed) { return; }
        retireAutoModeWrite(QStringLiteral("External auto-mode observation changed before setter handoff."));
    }
    if (!valid) {
        if (matching) { completeAutoModeReadItem(false, QStringLiteral("Malformed reply; last valid observation retained.")); publishAutoMode(); }
        return;
    }
    m_autoModeBatches[key] = batch;
    if (enabled) { m_autoModeEnabled = enabled; m_autoModeEnabledKnown = true; ++m_autoModeEnabledRevision; }
    if (count) {
        m_autoModeCount = count; m_autoModeCountKnown = true;
        if (changed) {
            for (int slot = 0; slot < 32; ++slot) {
                const QString slotKey = QStringLiteral("AS2%1").arg(slot, 2, 10, QLatin1Char('0'));
                m_autoModeBatches[slotKey] = std::max(m_autoModeBatches.value(slotKey), batch);
                m_autoModeReadKeys.remove(slotKey);
            }
        }
    }
    if (index >= 0) {
        const quint64 previous = m_autoModeRows[index].toMap().value("revision").toString().toULongLong();
        row.insert("revision", QString::number(previous + 1)); m_autoModeRows[index] = row;
    }
    if (changed && key != QLatin1String("AM")) { m_autoModeComplete = false; m_autoModeCoherent = false; m_autoModeSnapshotToken.clear(); }
    if (matching) { completeAutoModeReadItem(true, QString{}); }
    publishAutoMode();
}

void KenwoodBackend::pollAutoMode(qint64 nowMs)
{
    refreshAutoModeSafety();
    if (m_autoModeAvailabilityDirty) {
        const QPointer<KenwoodBackend> guard(this); const quint64 generation = m_generation;
        publishAutoMode();
        if (!guard || generation != m_generation) { return; }
    }
    if (!m_autoModeReading || !isConnected() || !m_identityConfirmed) { return; }
    if (!m_autoModePendingKey.isEmpty()) {
        if (m_autoModeDispatched && nowMs >= m_autoModeDeadline) {
            completeAutoModeReadItem(false, QStringLiteral("Query timed out; previous observation retained.")); publishAutoMode();
        }
        return;
    }
    if (m_autoModeQueue.isEmpty()) { return; }
    const QString key = m_autoModeQueue.takeFirst();
    const quint64 ticket = ++m_autoModeSerial; const quint64 generation = m_generation;
    const QString context = autoModeContext(); const QPointer<KenwoodBackend> guard(this);
    m_autoModePendingKey = key; m_autoModePendingTicket = ticket; m_autoModeDispatched = false;
    m_autoModeMinimumBatch = m_controlIngressSequence;
    publishAutoMode();
    if (!guard || generation != m_generation || context != autoModeContext() || ticket != m_autoModePendingTicket) { return; }
    m_autoModeMinimumBatch = m_controlIngressSequence; m_autoModeBatches[key] = std::max(m_autoModeBatches.value(key), m_controlIngressSequence);
    m_autoModeDeadline = nowMs + 2000; m_autoModeDispatched = true;
    const bool sent = send(key.toLatin1());
    if (!guard || generation != m_generation || context != autoModeContext() || ticket != m_autoModePendingTicket) { return; }
    if (!sent) { completeAutoModeReadItem(false, QStringLiteral("Read transport refused; previous observation retained.")); publishAutoMode(); }
}

bool KenwoodBackend::handleAutoModeExtension(const QString& verb, quint64 requestId, const QVariant& argument)
{
    if (!verb.startsWith(QLatin1String("auto.mode."))) { return false; }
    const QVariantMap args = argument.toMap(); const bool map = !argument.isValid() || argument.typeId() == QMetaType::QVariantMap;
    if (verb == QLatin1String("auto.mode.get")) {
        if (!map || !args.isEmpty()) { emit extensionError(requestId, QStringLiteral("Auto mode getter accepts no arguments.")); }
        else { emit extensionResult(requestId, autoModeStatus()); }
        return true;
    }
    refreshAutoModeSafety();
    if (!map || args.value("context").typeId() != QMetaType::QString || args.value("context").toString() != autoModeContext() || !isConnected() || !m_identityConfirmed) {
        emit extensionError(requestId, QStringLiteral("Requires the current connected auto-mode context.")); return true;
    }
    const QPointer<KenwoodBackend> guard(this); const quint64 generation = m_generation;
    if (verb == QLatin1String("auto.mode.read") || verb == QLatin1String("auto.mode.cancel")) {
        if (verb == QLatin1String("auto.mode.read")) {
            if (args.size() != 1 || m_autoModeReading || m_autoModeWriteTicket) { emit extensionError(requestId, QStringLiteral("Read already pending or malformed arguments.")); return true; }
            startAutoModeRead();
        } else {
            if (args.size() != 2 || !autoSequence(args.value("token")) || args.value("token").toString() != m_autoModeReadToken) { emit extensionError(requestId, QStringLiteral("Requires the current read token.")); return true; }
            ++m_autoModeOperation; retireAutoModeWrite(QStringLiteral("Verification canceled locally; an accepted write cannot be undone."));
            m_autoModeQueue.clear(); m_autoModePendingKey.clear(); m_autoModePendingTicket = 0; m_autoModeReading = false;
            m_autoModeComplete = false; m_autoModeCoherent = false; m_autoModeSnapshotToken.clear();
            m_autoModeReadReason = QStringLiteral("Read canceled locally; incomplete observations remain snapshots.");
        }
        const quint64 operation = m_autoModeOperation; const QString context = autoModeContext();
        publishAutoMode();
        if (guard && generation == m_generation) {
            if (operation == m_autoModeOperation && context == autoModeContext()) { emit extensionResult(requestId, autoModeStatus()); }
            else { emit extensionError(requestId, QStringLiteral("Auto-mode read request superseded during publication.")); }
        }
        return true;
    }
    if (!autoModeWritable() || m_autoModeReading) { emit extensionError(requestId, QStringLiteral("Requires current qualified receive, bank and AM authority without pending work.")); return true; }
    QByteArray command; QString operation;
    if (verb == QLatin1String("auto.mode.enabled.set")) {
        if (args.size() != 3 || args.value("enabled").typeId() != QMetaType::Bool || !autoSequence(args.value("enabledRevision")) || args.value("enabledRevision").toString() != QString::number(m_autoModeEnabledRevision)) {
            emit extensionError(requestId, QStringLiteral("Requires a boolean and current AM revision.")); return true;
        }
        operation = "enabled"; command = args.value("enabled").toBool() ? "AM1" : "AM0";
    } else {
        if (!m_autoModeComplete || !m_autoModeCoherent || !m_autoModeCountKnown || !m_autoModeCount || !autoSequence(args.value("snapshotToken")) || args.value("snapshotToken").toString() != m_autoModeSnapshotToken) {
            emit extensionError(requestId, QStringLiteral("Requires a complete current table snapshot.")); return true;
        }
        if (verb == QLatin1String("auto.mode.division.register")) {
            if (args.size() != 4 || args.value("upperHz").typeId() != QMetaType::LongLong || args.value("upperHz").toLongLong() < 0 || args.value("upperHz").toLongLong() > 99999999999LL
                || args.value("mode").typeId() != QMetaType::QString || args.value("mode").toString().size() != 1 || !decodeMode(args.value("mode").toString().at(0).toLatin1()) || *m_autoModeCount >= 32) {
                emit extensionError(requestId, QStringLiteral("Requires native Hz/mode values and an available division.")); return true;
            }
            for (const QVariant& item : m_autoModeRows) {
                const QVariantMap row = item.toMap();
                if (row.value("state") == QLatin1String("populated") && row.value("upperHz").toLongLong() == args.value("upperHz").toLongLong()) {
                    emit extensionError(requestId, QStringLiteral("Duplicate boundary behavior is unspecified; choose a new frequency.")); return true;
                }
            }
            operation = "register"; command = "AS0" + QByteArray::number(args.value("upperHz").toLongLong()).rightJustified(11, '0') + args.value("mode").toString().toLatin1();
        } else if (verb == QLatin1String("auto.mode.division.delete")) {
            if (args.size() != 4 || args.value("index").typeId() != QMetaType::Int || args.value("index").toInt() < 0 || args.value("index").toInt() >= *m_autoModeCount
                || !autoDense(m_autoModeRows, *m_autoModeCount) || !autoSequence(args.value("rowRevision"))
                || args.value("rowRevision").toString() != m_autoModeRows[args.value("index").toInt()].toMap().value("revision").toString()) {
                emit extensionError(requestId, QStringLiteral("Requires a current populated row in a dense table.")); return true;
            }
            operation = "delete"; command = "AS3" + QByteArray::number(args.value("index").toInt()).rightJustified(2, '0');
        } else { emit extensionError(requestId, QStringLiteral("Unknown auto-mode verb.")); return true; }
    }
    const quint64 ticket = ++m_autoModeSerial; const QString token = QString::number(ticket);
    m_autoModeWriteTicket = ticket; m_autoModePredispatch = true; m_autoModeHanded = false;
    m_autoModeWriteSafety = m_autoModeSafetyEpoch; m_autoModeWriteContext = autoModeContext();
    m_autoModeWriteRows = m_autoModeRows; m_autoModeWriteCount = m_autoModeCount.value_or(0); m_autoModeWriteArguments = args;
    m_autoModeLastWrite = {{"token", token}, {"operation", operation}, {"state", "pending"}, {"reason", QStringLiteral("Awaiting setter handoff; no change confirmed.")}};
    if (operation != QLatin1String("enabled")) { m_autoModeComplete = false; m_autoModeCoherent = false; m_autoModeSnapshotToken.clear(); }
    publishAutoMode();
    if (!guard) { return true; }
    refreshAutoModeSafety();
    if (generation != m_generation || ticket != m_autoModeWriteTicket || m_autoModeWriteSafety != m_autoModeSafetyEpoch || m_autoModeWriteContext != autoModeContext()) {
        if (generation == m_generation) { emit extensionError(requestId, QStringLiteral("Auto-mode mutation retired before handoff.")); }
        return true;
    }
    m_autoModePredispatch = false;
    for (const QString& key : operation == QLatin1String("enabled") ? QStringList{QStringLiteral("AM")} : autoKeys()) { m_autoModeBatches[key] = std::max(m_autoModeBatches.value(key), m_controlIngressSequence); }
    m_autoModeHanded = true;
    const bool sent = send(command);
    if (!guard || generation != m_generation) { return true; }
    if (ticket == m_autoModeWriteTicket) {
        if (sent) {
            m_autoModeLastWrite.insert("state", QStringLiteral("sent_unverified"));
            m_autoModeLastWrite.insert("reason", QStringLiteral("Request sent; no causal acknowledgement exists."));
            startAutoModeRead(operation == QLatin1String("enabled"));
        } else {
            m_autoModeHanded = false; retireAutoModeWrite(QStringLiteral("Setter transport refused."));
        }
        publishAutoMode();
        if (!guard || generation != m_generation) { return true; }
    }
    if (sent) { emit extensionResult(requestId, QVariantMap{{"sent", true}, {"verified", false}, {"token", token}}); }
    else { emit extensionError(requestId, QStringLiteral("Auto-mode setter could not be sent.")); }
    return true;
}
}
