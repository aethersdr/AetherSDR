#include "KenwoodBackend.h"

#include <QMetaType>
#include <QPointer>
#include <QScopeGuard>
#include <QStringList>

namespace AetherSDR::kenwood {
namespace {
// PC Command Reference January 2019, printed pp.44–46. IDs are scoped by
// message source: ME0/003 is EQ copy confirmation; ME1/003 is USB power fault.
QString messageText(int source, int id)
{
    if (id == 0) { return QStringLiteral("No message displayed"); }
    if (source == 1) {
        if (id >= 10 && id <= 21) { return QStringLiteral("DSP fault (%1)").arg(id); }
        const QMap<int, QString> labels{
            {1, "Program timer starting soon"}, {2, "Program timer recording"},
            {3, "USB bus power fault"}, {4, "Temperature protection"},
            {5, "Frequency unlock"}, {6, "High-temperature transmission protection"},
            {7, "Reference signal input fault"}, {9, "Backup data corruption"},
            {23, "Hardware fault"}, {24, "Display color adjustment"}, {25, "Timer power off"}};
        return labels.value(id);
    }
    const QMap<int, QString> labels{
        {1, "Emergency-frequency configuration"}, {2, "Confirm disabling power limiter"},
        {3, "Confirm equalizer copy"}, {4, "Confirm deleting all quick memories"},
        {5, "CW paddle registration queued"}, {6, "CW message registration"},
        {7, "Voice recording queued"}, {8, "Voice recording"}, {9, "Voice playback"},
        {10, "Voice playback transmission"}, {11, "Audio file playback"},
        {12, "Audio file cannot be played"}, {13, "Confirm file deletion"},
        {15, "COM operation change: normal"}, {16, "COM operation change: PSQ/PKS"},
        {17, "Program timer configuration complete"}, {18, "Clock not set"},
        {20, "NTP time acquired"}, {21, "NTP time acquisition failed"},
        {25, "Data loading complete"}, {26, "Data loading complete; restart"},
        {27, "Invalid loading file"}, {28, "Data loading failed"},
        {34, "Prepare USB flash drive"}, {35, "Data saved"}, {36, "Confirm format"},
        {37, "Confirm unmount (1)"}, {38, "Unmount complete"},
        {39, "Confirm standard reset"}, {40, "Confirm full reset"},
        {41, "Confirm VFO reset"}, {42, "Confirm memory reset"}, {43, "Confirm menu reset"},
        {44, "Reset running"}, {45, "Writing data"}, {46, "NTP or configuration processing"},
        {47, "USB flash drive not detected"}, {48, "USB flash drive save failed"},
        {49, "USB flash drive capacity insufficient"}, {50, "Program timer day unspecified"},
        {51, "Program timer time exceeded"}, {52, "Program timer times identical"},
        {57, "Format failed"}, {58, "Operating environment changed"}, {59, "File deletion failed"},
        {60, "Unmount failed"}, {61, "Firmware file transferring"},
        {62, "Firmware file not detected"}, {63, "Firmware version mismatch"},
        {64, "Firmware write failed"}, {65, "Firmware update succeeded"},
        {67, "File access failed"}, {68, "Display restriction"},
        {70, "Cannot change audio recording destination"}, {71, "Cannot change CW log destination"},
        {72, "Cannot change RTTY log destination"}, {73, "Cannot change PSK log destination"},
        {74, "No copy item selected"}, {75, "Preparing copy"}, {76, "Awaiting PC copy"},
        {77, "Cannot copy during audio recording"}, {78, "Cannot copy during CW logging"},
        {79, "Cannot copy during RTTY logging"}, {80, "Cannot copy during PSK logging"},
        {81, "Cannot copy during KNS logging"}, {82, "Confirm deleting all files"},
        {83, "No deletion item selected"}, {84, "Confirm loading image"},
        {85, "Cannot delete during audio recording"}, {86, "Cannot delete during CW logging"},
        {87, "Cannot delete during RTTY logging"}, {88, "Cannot delete during PSK logging"},
        {89, "Cannot delete during KNS logging"}, {90, "Confirm unmount (2)"},
        {91, "Copy stopped: too many files"}};
    return labels.value(id);
}

std::optional<int> threeDigits(const QByteArray& bytes)
{
    if (bytes.size() != 3) { return std::nullopt; }
    int value = 0;
    for (const char byte : bytes) {
        if (byte < '0' || byte > '9') { return std::nullopt; }
        value = value * 10 + byte - '0';
    }
    return value;
}
}

bool KenwoodBackend::radioBeepWritable() const
{
    return isConnected() && m_identityConfirmed && m_txStateKnown && !m_txObserved
        && !m_keyIntent && !m_cwIntent && !m_atuIntent && !m_tune
        && m_radioBeep.has_value() && !m_radioBeepAwaiting;
}

QVariantMap KenwoodBackend::radioMessagesStatus() const
{
    const bool connected = isConnected() && m_identityConfirmed;
    QVariantMap status{{"statusRevision", QString::number(m_radioMessagesStatusRevision)},
        {"context", QStringLiteral("messages:%1").arg(m_generation)}, {"connected", connected},
        {"reason", connected ? QString{} : QStringLiteral("Requires a connected, identified radio.")}};
    int active = 0;
    for (int source = 0; source < 2; ++source) {
        const std::optional<int> id = connected ? m_radioMessageIds[source] : std::nullopt;
        QVariantMap message{{"known", id.has_value()}};
        if (id) {
            const QString text = messageText(source, *id);
            message.insert("id", *id); message.insert("recognized", !text.isEmpty());
            message.insert("text", text.isEmpty() ? QStringLiteral("Unrecognized radio message %1").arg(*id, 3, 10, QLatin1Char('0')) : text);
            if (*id != 0 && (source == 1 || (m_radioMessageIds[1] && *m_radioMessageIds[1] == 0))) { active = source + 1; }
        }
        status.insert(source == 0 ? "message1" : "message2", message);
    }
    status.insert("activeMessage", active);
    const bool known = connected && m_radioBeep.has_value();
    const bool editable = radioBeepWritable();
    QString reason;
    if (!connected) { reason = QStringLiteral("Requires a connected, identified radio."); }
    else if (m_radioBeepAwaiting) { reason = m_radioBeepPending ? QStringLiteral("Waiting for beep-volume readback; last confirmed value retained.") : QStringLiteral("Beep-volume request is unverified. Read radio messages for fresh readback."); }
    else if (!known) { reason = QStringLiteral("Read radio messages to obtain beep-volume readback."); }
    else if (!editable) { reason = QStringLiteral("Requires confirmed receive state and no local transmit intent."); }
    QVariantMap beep{{"known", known}, {"revision", QString::number(m_radioBeepRevision)},
        {"editable", editable}, {"pending", m_radioBeepPending != 0}, {"reason", reason}};
    if (known) { beep.insert("value", *m_radioBeep); }
    status.insert("beep", beep);
    return status;
}

void KenwoodBackend::publishRadioMessages()
{
    m_radioMessagesPublishedWritable = radioBeepWritable();
    ++m_radioMessagesStatusRevision;
    emit extensionStatus(QStringLiteral("kenwood"), QStringLiteral("radioMessages"), radioMessagesStatus());
}

void KenwoodBackend::refreshRadioMessagesAvailability()
{
    if (m_radioMessagesInterested && m_radioMessagesPublishedWritable != radioBeepWritable()) { publishRadioMessages(); }
}

void KenwoodBackend::resetRadioMessages()
{
    m_radioMessageIds = {}; m_radioMessageBatches = {};
    m_radioBeep.reset(); ++m_radioBeepRevision;
    m_radioBeepPending = m_radioBeepPredispatch = 0;
    m_radioBeepMinimumBatch = m_controlIngressSequence;
    m_radioBeepDeadlineMs = 0; m_radioBeepAwaiting = false;
    m_radioMessagesInterested = m_radioMessagesQuerying = m_radioMessagesPublishedWritable = false;
}

bool KenwoodBackend::queryRadioMessages()
{
    if (!isConnected() || !m_identityConfirmed) { return false; }
    if (m_radioMessagesQuerying || m_radioBeepPredispatch) { return true; }
    const QPointer<KenwoodBackend> guard(this);
    const quint64 generation = m_generation;
    m_radioMessagesQuerying = true;
    const auto cleanup = qScopeGuard([guard, generation] {
        if (guard && generation == guard->m_generation) { guard->m_radioMessagesQuerying = false; }
    });
    for (const QByteArray& query : {QByteArray("ME0"), QByteArray("ME1"), QByteArray("EX00100")}) {
        if (!send(query) || !guard || generation != m_generation) { return false; }
    }
    return true;
}

void KenwoodBackend::adoptRadioMessage(const QByteArray& message, quint64 ingressBatch)
{
    if (!m_identityConfirmed) { return; }
    if (message.startsWith("ME0") || message.startsWith("ME1")) {
        const std::optional<int> value = threeDigits(message.sliced(3));
        const int source = message[2] - '0';
        if (!value || ingressBatch < m_radioMessageBatches[source]) { return; }
        m_radioMessageBatches[source] = ingressBatch;
        m_radioMessageIds[source] = *value;
        publishRadioMessages();
    } else if (message.startsWith("EX00100 ")) {
        const std::optional<int> value = threeDigits(message.sliced(8));
        if (!value || *value > 20 || ingressBatch < m_radioMessageBatches[2]
            || ingressBatch <= m_radioBeepMinimumBatch) { return; }
        // An unchanged pre-dispatch report cannot acknowledge an unsent write.
        // A changed report is authoritative and retires that obsolete draft.
        if (m_radioBeepPredispatch && m_radioBeep && *m_radioBeep == *value) { return; }
        m_radioMessageBatches[2] = ingressBatch;
        m_radioBeep = *value; ++m_radioBeepRevision;
        m_radioBeepPending = m_radioBeepPredispatch = 0;
        m_radioBeepAwaiting = false; m_radioBeepDeadlineMs = 0;
        publishRadioMessages();
    }
}

void KenwoodBackend::pollRadioMessageWrite(qint64 nowMs)
{
    const QPointer<KenwoodBackend> guard(this);
    const quint64 generation = m_generation;
    if (m_radioBeepPending && !m_radioBeepPredispatch && nowMs >= m_radioBeepDeadlineMs) {
        m_radioBeepPending = 0;
        publishRadioMessages();
    }
    if (guard && generation == m_generation) { refreshRadioMessagesAvailability(); }
}

bool KenwoodBackend::handleRadioMessagesExtension(const QString& verb, quint64 requestId, const QVariant& argument)
{
    if (verb != "radio.messages.get" && verb != "radio.beep.set" && verb != "radio.beep.reset") { return false; }
    const QPointer<KenwoodBackend> guard(this);
    const quint64 generation = m_generation;
    if (verb == "radio.messages.get") {
        if (argument.isValid() && (argument.typeId() != QMetaType::QVariantMap || !argument.toMap().isEmpty())) {
            emit extensionError(requestId, QStringLiteral("Radio message query accepts no arguments.")); return true;
        }
        m_radioMessagesInterested = true;
        const bool sent = queryRadioMessages();
        if (!guard || generation != m_generation) { return true; }
        if (sent) { emit extensionResult(requestId, radioMessagesStatus()); }
        else { emit extensionError(requestId, QStringLiteral("Radio message query could not be sent.")); }
        return true;
    }
    const QVariantMap args = argument.toMap();
    const bool reset = verb == QLatin1String("radio.beep.reset");
    if (argument.typeId() != QMetaType::QVariantMap || args.size() != (reset ? 2 : 3)
        || args.value("context").typeId() != QMetaType::QString
        || args.value("context") != radioMessagesStatus().value("context")
        || args.value("revision").typeId() != QMetaType::QString
        || args.value("revision").toString() != QString::number(m_radioBeepRevision)
        || (!reset && (args.value("value").typeId() != QMetaType::Int || args.value("value").toInt() < 0
            || args.value("value").toInt() > 20)) || !radioBeepWritable()) {
        emit extensionError(requestId, QStringLiteral("Beep volume requires current readback, an index from 0 to 20 and confirmed receive state.")); return true;
    }
    const int value = args.value("value").toInt();
    const quint64 revision = m_radioBeepRevision;
    const quint64 ticket = ++m_radioBeepTicket;
    m_radioBeepPending = m_radioBeepPredispatch = ticket;
    m_radioBeepAwaiting = true;
    publishRadioMessages();
    if (!guard || generation != m_generation) { return true; }
    if (m_radioBeepPending != ticket || m_radioBeepRevision != revision
        || !m_txStateKnown || m_txObserved || m_keyIntent || m_cwIntent || m_atuIntent || m_tune) {
        if (m_radioBeepPending == ticket) { m_radioBeepPending = m_radioBeepPredispatch = 0; m_radioBeepAwaiting = false; publishRadioMessages(); }
        if (guard && generation == m_generation) { emit extensionError(requestId, QStringLiteral("Beep-volume context changed before dispatch.")); }
        return true;
    }
    m_radioBeepMinimumBatch = m_controlIngressSequence;
    m_radioBeepPredispatch = 0;
    m_radioBeepDeadlineMs = m_clock.elapsed() + 2000;
    const bool sent = send(reset ? QByteArray("EX001009") : "EX00100 " + QByteArray::number(value).rightJustified(3, '0'));
    if (!guard || generation != m_generation) { return true; }
    bool queried = false;
    if (sent) { queried = send("EX00100"); }
    if (!guard || generation != m_generation) { return true; }
    if ((!sent || !queried) && m_radioBeepPending == ticket) {
        m_radioBeepPending = 0;
        if (!sent) { m_radioBeepAwaiting = false; }
        publishRadioMessages();
        if (!guard || generation != m_generation) { return true; }
    }
    if (sent) { emit extensionResult(requestId, QVariantMap{{"sent", true}, {"verified", false}}); }
    else { emit extensionError(requestId, QStringLiteral("Beep-volume request could not be sent.")); }
    return true;
}
} // namespace AetherSDR::kenwood
