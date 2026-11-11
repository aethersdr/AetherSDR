#include "KenwoodBackend.h"

#include <QChar>
#include <QMetaType>
#include <QPointer>
#include <QStringDecoder>

namespace AetherSDR::kenwood {
namespace {
std::optional<qint64> memoryDigits(const QByteArray& value, int width)
{
    if (value.size() != width) { return std::nullopt; }
    qint64 result = 0;
    for (const char byte : value) {
        if (byte < '0' || byte > '9') { return std::nullopt; }
        result = result * 10 + byte - '0';
    }
    return result;
}
QVariantMap memoryRow(int index, bool quick)
{
    const QString kind = quick ? QStringLiteral("quick") : index < 100 ? QStringLiteral("standard")
        : index < 110 ? QStringLiteral("program") : QStringLiteral("extended");
    return {{"index", index}, {"kind", kind}, {"state", QStringLiteral("unread")},
        {"revision", QStringLiteral("0")}, {"reason", QStringLiteral("Not read in this session.")},
        {"rawUtf8Hex", QString{}}, {"fields", QVariantMap{}}};
}
QVariantList memoryRows(bool quick)
{
    QVariantList rows;
    for (int index = 0; index < (quick ? 10 : 120); ++index) { rows.append(memoryRow(index, quick)); }
    return rows;
}
bool memoryMode(char mode) { return QByteArray("12345679ABCDEF").contains(mode); }
bool memoryBlank(const QByteArray& field) { return !field.isEmpty() && field == QByteArray(field.size(), ' '); }

bool parseMemoryRecord(const QByteArray& body, bool quick, QVariantMap& fields, QString& state)
{
    if (quick) {
        if (body.size() != 28) { return false; }
        const std::array<int, 5> starts{3, 14, 15, 26, 27};
        const std::array<int, 5> widths{11, 1, 11, 1, 1};
        const std::array<QString, 5> names{QStringLiteral("leftFrequencyHz"), QStringLiteral("leftMode"),
            QStringLiteral("rightFrequencyHz"), QStringLiteral("rightMode"), QStringLiteral("split")};
        int known = 0;
        for (int index = 0; index < 5; ++index) {
            const QByteArray field = body.mid(starts[index], widths[index]);
            if (memoryBlank(field)) { continue; }
            if (index == 1 || index == 3) {
                if (!memoryMode(field[0])) { return false; }
                fields.insert(names[index], QString::fromLatin1(field));
                fields.insert(names[index] + QStringLiteral("Label"), decodeMode(field[0]).value_or(QStringLiteral("Unknown")));
            } else {
                const std::optional<qint64> value = memoryDigits(field, widths[index]);
                if (!value || (index == 4 && *value > 1)) { return false; }
                fields.insert(names[index], index == 4 ? QVariant(static_cast<int>(*value)) : QVariant::fromValue(*value));
            }
            ++known;
        }
        state = known == 0 ? QStringLiteral("empty") : known == 5 ? QStringLiteral("populated") : QStringLiteral("partial");
        return true;
    }
    if (body.size() < 39 || body.size() > 79) { return false; }
    const QByteArray nameBytes = body.mid(39);
    QStringDecoder decoder(QStringDecoder::Utf8);
    const QString name = decoder(nameBytes);
    if (decoder.hasError() || name.toUtf8() != nameBytes || name.toUcs4().size() > 10) { return false; }
    for (const char32_t scalar : name.toUcs4()) {
        if (scalar < 0x20 || scalar == 0x7f || QChar::category(scalar) == QChar::Other_Control) { return false; }
    }
    fields.insert(QStringLiteral("name"), name);
    fields.insert(QStringLiteral("nameUtf8Hex"), QString::fromLatin1(nameBytes.toHex()));
    if (memoryBlank(body.mid(6, 33))) { state = QStringLiteral("empty"); return true; }
    const std::optional<qint64> first = memoryDigits(body.mid(6, 11), 11);
    const std::optional<qint64> second = memoryDigits(body.mid(24, 11), 11);
    const std::optional<qint64> tone = memoryDigits(body.mid(20, 2), 2);
    const std::optional<qint64> ctcss = memoryDigits(body.mid(22, 2), 2);
    if (!first || !second || !tone || !ctcss || *tone > 50 || *ctcss > 49 || !memoryMode(body[17])
        || (body[18] != '0' && body[18] != '1') || body[19] < '0' || body[19] > '3'
        || (body[36] != '0' && body[36] != '1') || (body[37] != '0' && body[37] != '1')
        || (body[38] != '0' && body[38] != '1')) { return false; }
    const bool unused = body[37] == '0' && *second == 0 && body[35] == '0' && body[36] == '0';
    if (!unused && !memoryMode(body[35])) { return false; }
    fields.insert(QStringLiteral("frequency1Hz"), QVariant::fromValue(*first));
    fields.insert(QStringLiteral("frequency2Hz"), QVariant::fromValue(*second));
    fields.insert(QStringLiteral("mode1"), QString(QChar::fromLatin1(body[17])));
    fields.insert(QStringLiteral("mode2"), QString(QChar::fromLatin1(body[35])));
    fields.insert(QStringLiteral("mode1Label"), decodeMode(body[17]).value_or(QStringLiteral("Unknown")));
    fields.insert(QStringLiteral("mode2Label"), unused ? QStringLiteral("Unused") : decodeMode(body[35]).value_or(QStringLiteral("Unknown")));
    fields.insert(QStringLiteral("fmWidth1"), body[18] - '0');
    fields.insert(QStringLiteral("fmWidth2"), body[36] - '0');
    fields.insert(QStringLiteral("toneType"), body[19] - '0');
    fields.insert(QStringLiteral("toneIndex"), static_cast<int>(*tone));
    fields.insert(QStringLiteral("ctcssIndex"), static_cast<int>(*ctcss));
    fields.insert(QStringLiteral("split"), body[37] - '0');
    fields.insert(QStringLiteral("lockout"), body[38] - '0');
    state = QStringLiteral("populated");
    return true;
}
}

QString KenwoodBackend::memoryInventoryContext() const
{
    return QStringLiteral("memory:%1:%2").arg(m_generation).arg(m_memoryInventoryEpoch);
}
QVariantMap KenwoodBackend::memoryInventoryStatus() const
{
    const bool connected = isConnected() && m_identityConfirmed;
    const QVariantMap refresh{{"token", m_memoryInventoryToken}, {"busy", m_memoryInventoryBusy},
        {"completed", m_memoryInventoryCompleted}, {"total", m_memoryInventoryTotal},
        {"observed", m_memoryInventoryObserved}, {"failed", m_memoryInventoryFailed},
        {"skipped", m_memoryInventorySkipped}, {"pendingKey", m_memoryInventoryPending}, {"reason", m_memoryInventoryReason}};
    return {{"statusRevision", QString::number(m_memoryInventoryStatusRevision)}, {"context", memoryInventoryContext()},
        {"connected", connected}, {"readOnly", true}, {"reason", connected ? QString{} : QStringLiteral("Requires a connected, identified radio.")},
        {"refresh", refresh}, {"metadata", m_memoryInventoryMetadata},
        {"channels", m_memoryInventoryChannels.isEmpty() ? memoryRows(false) : m_memoryInventoryChannels},
        {"quickChannels", m_memoryInventoryQuick.isEmpty() ? memoryRows(true) : m_memoryInventoryQuick}};
}
void KenwoodBackend::publishMemoryInventory()
{
    ++m_memoryInventoryStatusRevision;
    emit extensionStatus(QStringLiteral("kenwood"), QStringLiteral("memoryRead"), memoryInventoryStatus());
}
void KenwoodBackend::resetMemoryInventory()
{
    QStringList retiredKeys{QStringLiteral("MF"), QStringLiteral("MN"), QStringLiteral("MV"), QStringLiteral("QR"),
        QStringLiteral("EX00400"), QStringLiteral("SC0"), QStringLiteral("SC1"), QStringLiteral("SC2"),
        QStringLiteral("SC3"), QStringLiteral("MA70"), QStringLiteral("MA71")};
    for (int index = 0; index < 120; ++index) {
        retiredKeys.append(QStringLiteral("MA0%1").arg(index, 3, 10, QLatin1Char('0')));
    }
    for (int index = 0; index < 10; ++index) { retiredKeys.append(QStringLiteral("QA%1").arg(index)); }
    for (const QString& key : retiredKeys) {
        const quint64 previous = m_memoryInventoryBatches.value(key);
        m_memoryInventoryBatches.insert(key, previous > m_controlIngressSequence ? previous : m_controlIngressSequence);
    }
    ++m_memoryInventoryEpoch;
    ++m_memoryInventoryOperationEpoch;
    ++m_memoryInventoryTicket;
    m_memoryInventoryPendingTicket = 0;
    m_memoryInventoryPending.clear(); m_memoryInventoryQueue.clear();
    m_memoryInventoryBusy = false; m_memoryInventoryDispatched = false;
    m_memoryInventoryChannels = memoryRows(false); m_memoryInventoryQuick = memoryRows(true);
    m_memoryInventoryMetadata.clear(); m_memoryInventoryBank.reset(); m_memoryInventoryObservedKeys.clear();
    m_memoryInventoryTotal = 0; m_memoryInventoryCompleted = 0; m_memoryInventoryObserved = 0;
    m_memoryInventoryFailed = 0; m_memoryInventorySkipped = 0;
    m_memoryInventoryReason = QStringLiteral("Inventory not read in this session.");
    m_memoryInventoryTemporaryFloor = m_controlIngressSequence;
}
void KenwoodBackend::finishMemoryInventoryRequest(const QString& outcome, const QString& reason)
{
    const QString key = m_memoryInventoryPending;
    if (key.startsWith(QLatin1String("MA0")) || key.startsWith(QLatin1String("QA"))) {
        const bool quick = key.startsWith(QLatin1String("QA"));
        const int index = key.mid(quick ? 2 : 3).toInt();
        QVariantList& rows = quick ? m_memoryInventoryQuick : m_memoryInventoryChannels;
        if (rows.isEmpty()) { rows = memoryRows(quick); }
        if (outcome != QLatin1String("observed") && index >= 0 && index < rows.size()) {
            QVariantMap row = rows[index].toMap();
            row.insert("state", outcome == QLatin1String("timeout") ? QStringLiteral("timeout") : QStringLiteral("invalid"));
            row.insert("reason", reason); rows[index] = row;
        }
    }
    ++m_memoryInventoryCompleted;
    if (outcome == QLatin1String("observed")) { ++m_memoryInventoryObserved; }
    else if (outcome == QLatin1String("skipped")) { ++m_memoryInventorySkipped; }
    else { ++m_memoryInventoryFailed; }
    m_memoryInventoryPending.clear(); m_memoryInventoryPendingTicket = 0; m_memoryInventoryDispatched = false;
    if (m_memoryInventoryQueue.isEmpty()) {
        m_memoryInventoryBusy = false;
        m_memoryInventoryReason = m_memoryInventoryFailed || m_memoryInventorySkipped
            ? QStringLiteral("Read finished with failed or skipped items; snapshot is partial.") : QStringLiteral("Read finished; records are observed snapshots, not an atomic capture.");
    }
}

bool KenwoodBackend::handleMemoryInventoryExtension(const QString& verb, quint64 requestId, const QVariant& argument)
{
    if (!verb.startsWith(QLatin1String("memory.read."))) { return false; }
    const QVariantMap args = argument.toMap();
    const bool map = !argument.isValid() || argument.metaType().id() == QMetaType::QVariantMap;
    if (verb == QLatin1String("memory.read.get")) {
        if (!map || !args.isEmpty()) { emit extensionError(requestId, QStringLiteral("Memory getter accepts no arguments.")); }
        else { emit extensionResult(requestId, memoryInventoryStatus()); }
        return true;
    }
    const bool context = map && args.value("context").metaType().id() == QMetaType::QString
        && args.value("context").toString() == memoryInventoryContext();
    if (!context || !isConnected() || !m_identityConfirmed) {
        emit extensionError(requestId, QStringLiteral("Requires the current connected memory context.")); return true;
    }
    if (verb == QLatin1String("memory.read.cancel")) {
        if (args.size() != 2 || args.value("token").metaType().id() != QMetaType::QString
            || args.value("token").toString() != m_memoryInventoryToken) {
            emit extensionError(requestId, QStringLiteral("Requires the current refresh token.")); return true;
        }
        ++m_memoryInventoryOperationEpoch;
        ++m_memoryInventoryTicket; m_memoryInventoryQueue.clear(); m_memoryInventoryPending.clear();
        m_memoryInventoryPendingTicket = 0; m_memoryInventoryBusy = false; m_memoryInventoryDispatched = false;
        m_memoryInventoryReason = QStringLiteral("Read canceled locally; already received observations remain snapshots.");
    } else {
        if (m_memoryInventoryBusy) { emit extensionError(requestId, QStringLiteral("A read is already running; cancel it first.")); return true; }
        QStringList queue;
        if (verb == QLatin1String("memory.read.refresh")) {
            if (args.size() != 2 || args.value("scope").metaType().id() != QMetaType::QString) {
                emit extensionError(requestId, QStringLiteral("Read scope is required.")); return true;
            }
            const QString scope = args.value("scope").toString();
            if (scope != QLatin1String("all") && scope != QLatin1String("normal") && scope != QLatin1String("quick") && scope != QLatin1String("metadata")) {
                emit extensionError(requestId, QStringLiteral("Unknown read scope.")); return true;
            }
            queue = {"MF", "MN", "MV", "QR", "EX00400", "SC0", "SC1", "SC2", "SC3", "MA70", "MA71"};
            if (scope == QLatin1String("all") || scope == QLatin1String("normal")) {
                for (int index = 0; index < 120; ++index) { queue.append(QStringLiteral("MA0%1").arg(index, 3, 10, QLatin1Char('0'))); }
            }
            if (scope == QLatin1String("all") || scope == QLatin1String("quick")) {
                for (int index = 0; index < 10; ++index) { queue.append(QStringLiteral("QA%1").arg(index)); }
            }
        } else if (verb == QLatin1String("memory.read.channel") || verb == QLatin1String("memory.read.quick")) {
            const bool quick = verb.endsWith(QLatin1String("quick"));
            if (args.size() != 2 || args.value("channel").metaType().id() != QMetaType::Int
                || args.value("channel").toInt() < 0 || args.value("channel").toInt() >= (quick ? 10 : 120)) {
                emit extensionError(requestId, QStringLiteral("Channel must be an integer in the native range.")); return true;
            }
            queue.append(quick ? QStringLiteral("QA%1").arg(args.value("channel").toInt())
                : QStringLiteral("MA0%1").arg(args.value("channel").toInt(), 3, 10, QLatin1Char('0')));
        } else { emit extensionError(requestId, QStringLiteral("Unknown memory read verb.")); return true; }
        ++m_memoryInventoryOperationEpoch;
        m_memoryInventoryObservedKeys.clear();
        m_memoryInventoryQueue = queue; m_memoryInventoryTotal = queue.size();
        m_memoryInventoryCompleted = 0; m_memoryInventoryObserved = 0; m_memoryInventoryFailed = 0; m_memoryInventorySkipped = 0;
        m_memoryInventoryBusy = true; m_memoryInventoryReason = QStringLiteral("Read queued; no recall or settings changes.");
        m_memoryInventoryToken = QString::number(++m_memoryInventoryTicket);
    }
    const QPointer<KenwoodBackend> guard(this);
    const quint64 generation = m_generation;
    const QString token = m_memoryInventoryToken;
    const QString contextKey = memoryInventoryContext();
    const quint64 operation = m_memoryInventoryOperationEpoch;
    publishMemoryInventory();
    if (guard && generation == m_generation) {
        if (operation != m_memoryInventoryOperationEpoch || contextKey != memoryInventoryContext() || token != m_memoryInventoryToken) {
            emit extensionError(requestId, QStringLiteral("Memory read request was superseded or retired during submission."));
        } else { emit extensionResult(requestId, memoryInventoryStatus()); }
    }
    return true;
}

void KenwoodBackend::pollMemoryInventory(qint64 nowMs)
{
    if (!m_memoryInventoryBusy || !isConnected() || !m_identityConfirmed) { return; }
    if (!m_memoryInventoryPending.isEmpty()) {
        if (m_memoryInventoryDispatched && nowMs >= m_memoryInventoryDeadline) {
            finishMemoryInventoryRequest(QStringLiteral("timeout"), QStringLiteral("Read timed out; previous observation retained."));
            publishMemoryInventory();
        }
        return;
    }
    if (m_memoryInventoryQueue.isEmpty()) { return; }
    m_memoryInventoryPending = m_memoryInventoryQueue.takeFirst();
    const QString key = m_memoryInventoryPending;
    if (key.startsWith(QLatin1String("MA7"))
        && (!m_memoryInventoryObservedKeys.contains(QStringLiteral("MN"))
            || !m_memoryInventoryObservedKeys.contains(QStringLiteral("MV"))
            || !m_memoryInventoryObservedKeys.contains(QStringLiteral("QR"))
            || m_memoryInventoryMetadata.value("memoryMode").toInt() != 1
            || !m_memoryInventoryMetadata.contains("selectedChannel") || !m_memoryInventoryMetadata.contains("quickMode")
            || m_memoryInventoryMetadata.value("quickMode").toInt() != 0)) {
        finishMemoryInventoryRequest(QStringLiteral("skipped"), QStringLiteral("Temporary memory frequency requires confirmed normal-memory view."));
        publishMemoryInventory(); return;
    }
    const quint64 ticket = ++m_memoryInventoryTicket;
    m_memoryInventoryPendingTicket = ticket; m_memoryInventoryDispatched = false;
    m_memoryInventoryMinimumBatch = m_controlIngressSequence;
    const quint64 generation = m_generation;
    const QString context = memoryInventoryContext();
    const quint64 temporaryFloor = m_memoryInventoryTemporaryFloor;
    const QVariant selectedChannel = m_memoryInventoryMetadata.value("selectedChannel");
    const QVariant memoryModeValue = m_memoryInventoryMetadata.value("memoryMode");
    const QVariant quickModeValue = m_memoryInventoryMetadata.value("quickMode");
    const QVariant quickChannel = m_memoryInventoryMetadata.value("quickChannel");
    const QPointer<KenwoodBackend> guard(this);
    publishMemoryInventory();
    if (!guard || generation != m_generation || context != memoryInventoryContext() || ticket != m_memoryInventoryPendingTicket) { return; }
    if (key.startsWith(QLatin1String("MA7"))
        && (temporaryFloor != m_memoryInventoryTemporaryFloor
            || selectedChannel != m_memoryInventoryMetadata.value("selectedChannel")
            || memoryModeValue != m_memoryInventoryMetadata.value("memoryMode")
            || quickModeValue != m_memoryInventoryMetadata.value("quickMode")
            || quickChannel != m_memoryInventoryMetadata.value("quickChannel"))) {
        finishMemoryInventoryRequest(QStringLiteral("skipped"), QStringLiteral("Temporary memory view changed before query dispatch."));
        publishMemoryInventory();
        return;
    }
    m_memoryInventoryMinimumBatch = m_controlIngressSequence;
    m_memoryInventoryBatches.insert(key, m_controlIngressSequence);
    m_memoryInventoryDeadline = nowMs + 2000; m_memoryInventoryDispatched = true;
    const bool sent = send(key.toLatin1());
    if (!guard || generation != m_generation || context != memoryInventoryContext() || ticket != m_memoryInventoryPendingTicket) { return; }
    if (!sent) {
        finishMemoryInventoryRequest(QStringLiteral("failed"), QStringLiteral("Query transport refused; previous observation retained."));
        publishMemoryInventory();
    }
}

void KenwoodBackend::adoptMemoryInventory(const QByteArray& message, quint64 ingressBatch)
{
    if (!isConnected() || !m_identityConfirmed) { return; }
    QString key;
    QString metadataName;
    QVariant metadataValue;
    QVariantMap fields;
    QString state;
    bool valid = false;
    bool record = false;
    bool quick = false;
    int index = -1;
    if (message.startsWith("MA0") && message.size() >= 6) {
        const std::optional<qint64> channel = memoryDigits(message.mid(3, 3), 3);
        if (!channel || *channel >= 120) { return; }
        index = static_cast<int>(*channel); key = QString::fromLatin1(message.left(6)); record = true;
        valid = parseMemoryRecord(message, false, fields, state);
        if (index >= 100 && index < 110 && valid) { fields.insert("frequency2Role", QStringLiteral("unqualified-program-field")); }
    } else if (message.startsWith("QA") && message.size() >= 3) {
        const std::optional<qint64> channel = memoryDigits(message.mid(2, 1), 1);
        if (!channel) { return; }
        index = static_cast<int>(*channel); key = QString::fromLatin1(message.left(3)); record = true; quick = true;
        valid = parseMemoryRecord(message, true, fields, state);
    } else if (message.startsWith("MA7") && message.size() >= 4 && (message[3] == '0' || message[3] == '1')) {
        key = QString::fromLatin1(message.left(4));
        metadataName = message[3] == '0' ? QStringLiteral("temporaryLeftHz") : QStringLiteral("temporaryRightHz");
        if (ingressBatch <= m_memoryInventoryTemporaryFloor || m_memoryInventoryMetadata.value("memoryMode").toInt() != 1
            || !m_memoryInventoryMetadata.contains("selectedChannel") || !m_memoryInventoryMetadata.contains("quickMode")
            || m_memoryInventoryMetadata.value("quickMode").toInt() != 0) { return; }
        if (message.size() == 15 && memoryBlank(message.mid(4))) { valid = true; }
        else {
            const std::optional<qint64> frequency = memoryDigits(message.mid(4), 11);
            valid = frequency.has_value();
            if (frequency) { metadataValue = QVariant::fromValue(*frequency); }
        }
    } else {
        int minimum = 0;
        int maximum = 0;
        int width = 1;
        int prefix = 2;
        if (message.startsWith("MF")) { key = "MF"; metadataName = "configBank"; maximum = 1; }
        else if (message.startsWith("MN")) { key = "MN"; metadataName = "selectedChannel"; maximum = 119; width = 3; }
        else if (message.startsWith("MV")) { key = "MV"; metadataName = "memoryMode"; maximum = 1; }
        else if (message.startsWith("QR")) {
            key = "QR"; metadataName = "quickMode";
            valid = message.size() == 4 && (message[2] == '0' || message[2] == '1')
                && ((message[3] >= '0' && message[3] <= '9') || message[3] == ' ');
            if (valid) {
                fields.insert("quickMode", message[2] - '0');
                if (message[3] != ' ') { fields.insert("quickChannel", message[3] - '0'); }
            }
        } else if (message.startsWith("EX00400")) { key = "EX00400"; metadataName = "quickCapacityRaw"; prefix = 8; width = 3; maximum = 2;
            if (message.size() < 8 || message[7] != ' ') { return; }
        } else if (message.startsWith("SC0")) {
            key = "SC0"; metadataName = "scanActive";
            valid = message.size() == 5 && (message[3] == '0' || message[3] == '1') && (message[4] == '0' || message[4] == '1');
            if (valid) { fields.insert("scanActive", message[3] - '0'); fields.insert("programSlowRange", message[4] - '0'); }
        } else if (message.startsWith("SC1")) { key = "SC1"; metadataName = "scanSpeed"; prefix = 3; minimum = 1; maximum = 9; }
        else if (message.startsWith("SC2")) { key = "SC2"; metadataName = "toneScan"; prefix = 3; maximum = 2; }
        else if (message.startsWith("SC3")) { key = "SC3"; metadataName = "scanType"; prefix = 3; maximum = 1; }
        else { return; }
        if (key != QLatin1String("QR") && key != QLatin1String("SC0")) {
            const std::optional<qint64> value = memoryDigits(message.mid(prefix), width);
            valid = value && *value >= minimum && *value <= maximum;
            if (valid) { metadataValue = static_cast<int>(*value); }
        }
    }
    if (m_memoryInventoryBatches.contains(key) && ingressBatch <= m_memoryInventoryBatches.value(key)) { return; }
    const bool matching = key == m_memoryInventoryPending && m_memoryInventoryPendingTicket != 0;
    if (matching && (!m_memoryInventoryDispatched || ingressBatch <= m_memoryInventoryMinimumBatch)) { return; }
    if (!valid) {
        if (matching) {
            finishMemoryInventoryRequest(QStringLiteral("invalid"), QStringLiteral("Malformed reply; last valid observation retained."));
            publishMemoryInventory();
        }
        return;
    }
    m_memoryInventoryBatches.insert(key, ingressBatch);
    if (key == QLatin1String("MF")) {
        const int bank = metadataValue.toInt();
        bool oldRecords = false;
        for (const QVariant& row : m_memoryInventoryChannels) { if (!row.toMap().value("rawUtf8Hex").toString().isEmpty()) { oldRecords = true; break; } }
        if (!oldRecords) { for (const QVariant& row : m_memoryInventoryQuick) { if (!row.toMap().value("rawUtf8Hex").toString().isEmpty()) { oldRecords = true; break; } } }
        if ((m_memoryInventoryBank && *m_memoryInventoryBank != bank) || (!m_memoryInventoryBank && oldRecords)) {
            resetMemoryInventory();
            m_memoryInventoryBank = bank; m_memoryInventoryMetadata.insert("configBank", bank);
            m_memoryInventoryReason = QStringLiteral("Configuration bank changed; explicit new Read is required.");
            publishMemoryInventory(); return;
        }
        m_memoryInventoryBank = bank;
    }
    if (record) {
        QVariantList& rows = quick ? m_memoryInventoryQuick : m_memoryInventoryChannels;
        if (rows.isEmpty()) { rows = memoryRows(quick); }
        QVariantMap row = rows[index].toMap();
        row.insert("fields", fields); row.insert("state", state); row.insert("reason", QString{});
        row.insert("rawUtf8Hex", QString::fromLatin1(message.toHex()));
        row.insert("revision", QString::number(row.value("revision").toString().toULongLong() + 1));
        rows[index] = row;
    } else {
        bool changedCurrent = false;
        if (key == QLatin1String("QR")) {
            changedCurrent = m_memoryInventoryMetadata.value("quickMode") != fields.value("quickMode")
                || m_memoryInventoryMetadata.value("quickChannel") != fields.value("quickChannel");
            m_memoryInventoryMetadata.remove("quickChannel");
        } else if (key == QLatin1String("MN") || key == QLatin1String("MV")) {
            changedCurrent = m_memoryInventoryMetadata.value(metadataName) != metadataValue;
        }
        if (changedCurrent) {
            m_memoryInventoryTemporaryFloor = ingressBatch;
            m_memoryInventoryMetadata.remove("temporaryLeftHz"); m_memoryInventoryMetadata.remove("temporaryRightHz");
        }
        if (!fields.isEmpty()) {
            for (auto iterator = fields.cbegin(); iterator != fields.cend(); ++iterator) { m_memoryInventoryMetadata.insert(iterator.key(), iterator.value()); }
        } else if (metadataValue.isValid()) { m_memoryInventoryMetadata.insert(metadataName, metadataValue); }
        else { m_memoryInventoryMetadata.remove(metadataName); }
    }
    if (matching) {
        m_memoryInventoryObservedKeys.insert(key);
        finishMemoryInventoryRequest(QStringLiteral("observed"), QString{});
    }
    publishMemoryInventory();
}
}
