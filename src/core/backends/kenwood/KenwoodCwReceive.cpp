#include "KenwoodBackend.h"

#include <QMetaType>
#include <QPointer>
#include <algorithm>
#include <limits>

namespace AetherSDR::kenwood {
namespace {
struct CwReceiveControl { QString id; QByteArray command; QString label; int minimum; int maximum; QStringList choices; };
const std::array<CwReceiveControl, 4> kCwReceiveControls{{
    {"screen", "CD0", "CW communication screen", 0, 1, {"Closed", "Open"}},
    {"threshold", "CD1", "Native decode threshold", 1, 30, {}},
    {"filter", "CD3", "Native decode filter", 0, 2, {"Off", "Normal", "Narrow"}},
    {"enabled", "CD5", "Native CW decoding", 0, 1, {"Stopped", "Decode"}}
}};
QStringList cwReadKeys() { return {"AI", "CD0", "CD1", "CD3", "CD5", "DS0", "CA"}; }
const CwReceiveControl* cwControl(const QString& id)
{
    for (const CwReceiveControl& item : kCwReceiveControls) { if (item.id == id) { return &item; } }
    return nullptr;
}
bool cwSequence(const QVariant& value)
{
    if (value.typeId() != QMetaType::QString || value.toString().isEmpty()) { return false; }
    bool valid = false; const quint64 number = value.toString().toULongLong(&valid);
    return valid && QString::number(number) == value.toString();
}
std::optional<int> cwDigits(const QByteArray& bytes, int width)
{
    if (bytes.size() != width) { return std::nullopt; }
    int result = 0;
    for (const char byte : bytes) { if (byte < '0' || byte > '9') { return std::nullopt; } result = result * 10 + byte - '0'; }
    return result;
}
void cwIncrement(int& value) { if (value < std::numeric_limits<int>::max()) { ++value; } }
}

QString KenwoodBackend::cwReceiveContext() const { return QStringLiteral("cw:%1:%2").arg(m_generation).arg(m_cwReceiveEpoch); }
bool KenwoodBackend::cwReceiveWritable() const
{
    return isConnected() && m_identityConfirmed && m_txStateKnown && !m_txObserved && !m_keyIntent
        && !m_cwIntent && !m_atuIntent && !m_tune && !m_cwReceiveWriteTicket && !m_cwReceiveReading;
}
bool KenwoodBackend::cwReceiveValueWritable(const QString& id, int value) const
{
    const CwReceiveControl* control = cwControl(id);
    if (!control || value < control->minimum || value > control->maximum || !cwReceiveWritable() || !m_cwReceiveKnown.contains(id)) { return false; }
    const QString observedMode = m_receiveVfo >= 0 && m_receiveVfo < 2 && !m_vfoModes[m_receiveVfo].isEmpty() ? m_vfoModes[m_receiveVfo] : m_mode;
    if (value == 1 && (id == QLatin1String("screen") || id == QLatin1String("enabled")) && observedMode != QLatin1String("CW") && observedMode != QLatin1String("CWR")) { return false; }
    const bool swl = m_operatingDisplayBasicKnown && m_operatingDisplayBasic == std::optional<int>(1);
    return !(id == QLatin1String("enabled") && value == 1 && swl);
}
void KenwoodBackend::retireCwReceiveWrite(const QString& reason)
{
    if (!m_cwReceiveWriteTicket) { return; }
    m_cwReceiveOutcomeRetired = true;
    m_cwReceiveLastWrite.insert("state", m_cwReceiveHanded ? QStringLiteral("indeterminate") : QStringLiteral("rejected"));
    m_cwReceiveLastWrite.insert("reason", reason);
    if (!m_cwReceiveHanded) { m_cwReceiveWriteTicket = 0; m_cwReceivePredispatch = false; }
}
void KenwoodBackend::refreshCwReceiveContext()
{
    const bool connected = isConnected() && m_identityConfirmed;
    const bool rx = connected && m_txStateKnown && !m_txObserved && !m_keyIntent && !m_cwIntent && !m_atuIntent && !m_tune;
    const QString physicalMode = m_receiveVfo >= 0 && m_receiveVfo < 2 ? m_vfoModes[m_receiveVfo] : QString{};
    const QString observedMode = physicalMode.isEmpty() ? m_mode : physicalMode;
    const bool cw = observedMode == QLatin1String("CW") || observedMode == QLatin1String("CWR");
    const bool swl = m_operatingDisplayBasicKnown && m_operatingDisplayBasic == std::optional<int>(1);
    const bool aiOff = m_cwReceiveAiKnown && m_cwReceiveAi == std::optional<int>(0);
    const bool accepting = rx && cw && !swl && !aiOff;
    const qint64 frequency = m_receiveVfo >= 0 && m_receiveVfo < 2 ? m_frequency[m_receiveVfo] : 0;
    const QString signature = QStringLiteral("%1:%2:%3:%4:%5:%6:%7:%8:%9:%10:%11:%12:%13:%14:%15:%16")
        .arg(m_generation).arg(connected).arg(m_txStateKnown).arg(m_txObserved).arg(m_keyIntent).arg(m_cwIntent).arg(m_atuIntent)
        .arg(m_tune ? m_tune->serial : 0).arg(m_receiveVfo).arg(m_mode).arg(m_tfSet ? (*m_tfSet ? 1 : 0) : -1)
        .arg(m_operatingDisplayBasicKnown).arg(m_operatingDisplayBasic.value_or(-1)).arg(m_cwReceiveAiKnown).arg(m_cwReceiveAi.value_or(-1)).arg(physicalMode);
    if (signature != m_cwReceiveSafetySignature) {
        m_cwReceiveSafetySignature = signature; ++m_cwReceiveSafetyEpoch; ++m_cwReceiveStatusRevision; m_cwReceiveDirty = true;
        if (m_cwReceiveWriteTicket) { retireCwReceiveWrite(QStringLiteral("Native CW intent retired by a context transition.")); }
    }
    const QString streamSignature = QStringLiteral("%1:%2:%3:%4:%5:%6:%7")
        .arg(m_generation).arg(rx).arg(m_receiveVfo).arg(observedMode).arg(frequency).arg(swl).arg(aiOff);
    if (streamSignature != m_cwReceiveStreamSignature) {
        if (m_cwReceiveAccepting) { m_cwReceiveStreamMinimumBatch = std::max(m_cwReceiveStreamMinimumBatch, m_controlIngressSequence); }
        m_cwReceiveStreamSignature = streamSignature; ++m_cwReceiveStreamEpoch;
        m_cwReceiveNativeObserved = false; m_cwReceiveLastEventMs.reset();
        ++m_cwReceiveTextRevision; m_cwReceiveTextDirty = true;
    }
    m_cwReceiveAccepting = accepting;
    const bool ready = accepting && m_cwReceiveAiKnown && m_cwReceiveAi && (*m_cwReceiveAi == 2 || *m_cwReceiveAi == 4);
    if (ready != m_cwReceiveReady) { m_cwReceiveReady = ready; ++m_cwReceiveTextRevision; m_cwReceiveTextDirty = true; }
}
QVariantMap KenwoodBackend::cwReceiveStream() const
{
    const bool ready = m_cwReceiveAccepting && m_cwReceiveAiKnown && m_cwReceiveAi && (*m_cwReceiveAi == 2 || *m_cwReceiveAi == 4);
    QString reason;
    if (!m_cwReceiveAccepting) { reason = QStringLiteral("Native receive text requires confirmed RX in CW/CWR without local transmit intent or known SWL mode."); }
    else if (!ready) { reason = QStringLiteral("AI On is not qualified; received native events remain observations, not an inferred AI setting."); }
    QVariantMap stream{{"available", ready}, {"reason", reason}, {"aiKnown", isConnected() && m_identityConfirmed && m_cwReceiveAiKnown}, {"nativeDecoderObserved", m_cwReceiveNativeObserved},
        {"context", QStringLiteral("cw-text:%1:%2").arg(m_generation).arg(m_cwReceiveStreamEpoch)}, {"revision", QString::number(m_cwReceiveTextRevision)}};
    if (m_cwReceiveAi) { stream.insert("ai", *m_cwReceiveAi); }
    if (m_cwReceiveLastEventMs) { stream.insert("lastEventElapsedMs", QVariant::fromValue(*m_cwReceiveLastEventMs)); }
    return stream;
}
QVariantMap KenwoodBackend::cwReceiveTranscript() const
{
    return {{"revision", QString::number(m_cwReceiveTextRevision)}, {"events", m_cwReceiveEvents}, {"dropped", m_cwReceiveDropped}, {"invalid", m_cwReceiveInvalid},
        {"reason", QStringLiteral("Last received native CW; pauses do not establish decoder failure. Historical events retain their original context.")}};
}
QVariantMap KenwoodBackend::cwReceiveStatus() const
{
    QVariantList controls;
    for (const CwReceiveControl& control : kCwReceiveControls) {
        const bool known = isConnected() && m_identityConfirmed && m_cwReceiveKnown.contains(control.id);
        bool editable = false; QVariantList choices;
        for (int value = control.minimum; value <= control.maximum; ++value) {
            const bool permitted = cwReceiveValueWritable(control.id, value); editable = editable || permitted;
            if (!control.choices.isEmpty()) { choices.append(QVariantMap{{"value", value}, {"label", control.choices[value]}, {"editable", permitted}, {"reason", permitted ? QString{} : QStringLiteral("Requires current native value and qualified receive context for this choice.")}}); }
        }
        QVariantMap item{{"id", control.id}, {"label", control.label}, {"unit", control.id == QLatin1String("threshold") ? QStringLiteral("index") : QString{}},
            {"minimum", control.minimum}, {"maximum", control.maximum}, {"step", 1}, {"known", known}, {"revision", QString::number(m_cwReceiveRevisions.value(control.id))},
            {"editable", editable}, {"pending", m_cwReceiveWriteTicket && m_cwReceiveWriteId == control.id}, {"reason", editable ? QString{} : QStringLiteral("Read the native value; writes require RX without local transmit intent. Screen/Decode On require CW/CWR.")}, {"choices", choices}};
        if (m_cwReceiveValues.contains(control.id)) { item.insert("value", m_cwReceiveValues.value(control.id)); }
        controls.append(item);
    }
    QVariantMap autoZero{{"known", isConnected() && m_identityConfirmed && m_cwReceiveAutoZeroKnown},
        {"reason", QStringLiteral("Last reported native CW auto zero-in activity; inactivity does not establish alignment success. Start and Stop are not implemented.")}};
    if (m_cwReceiveAutoZero) { autoZero.insert("active", *m_cwReceiveAutoZero); }
    return {{"autoZero", autoZero}, {"statusRevision", QString::number(m_cwReceiveStatusRevision)}, {"context", cwReceiveContext()}, {"connected", isConnected() && m_identityConfirmed}, {"reason", isConnected() && m_identityConfirmed ? QString{} : QStringLiteral("Requires a connected identified radio.")}, {"controls", controls},
        {"refresh", QVariantMap{{"token", m_cwReceiveReadToken}, {"busy", m_cwReceiveReading}, {"total", m_cwReceiveTotal}, {"completed", m_cwReceiveCompleted}, {"observed", m_cwReceiveObserved}, {"failed", m_cwReceiveFailed}, {"pendingKey", m_cwReceivePending}, {"reason", m_cwReceiveReadReason}}},
        {"lastWrite", m_cwReceiveLastWrite.isEmpty() ? QVariantMap{{"token", QString{}}, {"id", QString{}}, {"state", "none"}, {"reason", QString{}}} : m_cwReceiveLastWrite},
        {"stream", cwReceiveStream()}, {"transcript", cwReceiveTranscript()},
        {"restrictionNotice", QStringLiteral("During native CW decoding, native NR1/NR2/APF/RX DSP EQ do not function. This notice does not rewrite their observed preferences or host DSP. CD5 On alone does not prove active decoding. This page has no sending or Quick controls.")}};
}
void KenwoodBackend::publishCwReceive()
{
    m_cwReceiveDirty = false; ++m_cwReceiveStatusRevision;
    emit extensionStatus(QStringLiteral("kenwood"), QStringLiteral("cwReceive"), cwReceiveStatus());
}
void KenwoodBackend::publishCwReceiveText()
{
    m_cwReceiveTextDirty = false; ++m_cwReceiveTextRevision;
    emit extensionStatus(QStringLiteral("kenwood"), QStringLiteral("cwReceiveText"), QVariantMap{{"statusRevision", QString::number(m_cwReceiveTextRevision)}, {"stream", cwReceiveStream()}, {"transcript", cwReceiveTranscript()}});
}
void KenwoodBackend::resetCwReceive()
{
    retireCwReceiveWrite(QStringLiteral("Native CW session/configuration retired; sent changes cannot be undone."));
    m_cwReceiveWriteTicket = 0; m_cwReceivePredispatch = false; m_cwReceiveHanded = false;
    ++m_cwReceiveEpoch; ++m_cwReceiveOperation; ++m_cwReceiveSerial; ++m_cwReceiveStatusRevision;
    QStringList retiredKeys = cwReadKeys(); retiredKeys.append(QStringLiteral("MF"));
    for (const QString& key : retiredKeys) {
        m_cwReceiveBatches[key] = std::max(m_cwReceiveBatches.value(key), m_controlIngressSequence);
        m_cwReceiveMinimumBatches[key] = std::max(m_cwReceiveMinimumBatches.value(key), m_controlIngressSequence);
    }
    m_cwReceiveValues.clear(); m_cwReceiveKnown.clear(); m_cwReceiveAi.reset(); m_cwReceiveAiKnown = false; m_cwReceiveAutoZero.reset(); m_cwReceiveAutoZeroKnown = false; m_cwReceiveBank.reset();
    m_cwReceiveQueue.clear(); m_cwReceivePending.clear(); m_cwReceivePendingTicket = 0; m_cwReceiveReading = false;
    m_cwReceiveTotal = m_cwReceiveCompleted = m_cwReceiveObserved = m_cwReceiveFailed = 0;
    m_cwReceiveReadReason = QStringLiteral("Not read.");
    m_cwReceiveStreamMinimumBatch = std::max(m_cwReceiveStreamMinimumBatch, m_controlIngressSequence);
    m_cwReceiveAccepting = false; m_cwReceiveNativeObserved = false; m_cwReceiveLastEventMs.reset();
    ++m_cwReceiveStreamEpoch; ++m_cwReceiveTextRevision; m_cwReceiveTextDirty = true; m_cwReceiveDirty = true;
}
void KenwoodBackend::preflightCwReceiveBank(const QByteArray& message, quint64 batch)
{
    if (message != "MF0" && message != "MF1") { return; }
    if (batch <= m_cwReceiveBatches.value("MF")) { return; }
    const int bank = message[2] - '0';
    if (m_cwReceiveBank && *m_cwReceiveBank != bank) { resetCwReceive(); }
    m_cwReceiveBank = bank; m_cwReceiveBatches["MF"] = batch;
}
void KenwoodBackend::startCwReceiveRead(const QString& id)
{
    ++m_cwReceiveOperation; m_cwReceiveReadToken = QString::number(++m_cwReceiveSerial);
    const CwReceiveControl* control = cwControl(id);
    m_cwReceiveQueue = control ? QStringList{QString::fromLatin1(control->command)} : cwReadKeys();
    m_cwReceiveTotal = m_cwReceiveQueue.size(); m_cwReceiveCompleted = m_cwReceiveObserved = m_cwReceiveFailed = 0;
    m_cwReceivePending.clear(); m_cwReceivePendingTicket = 0; m_cwReceiveReading = true;
    m_cwReceiveReadReason = QStringLiteral("Native values are observed snapshots; no causal acknowledgement is implied.");
}
void KenwoodBackend::completeCwReceiveReadItem(bool observed, const QString& reason)
{
    if (observed) { ++m_cwReceiveObserved; }
    else {
        ++m_cwReceiveFailed; retireCwReceiveWrite(reason);
        if (m_cwReceivePending == QLatin1String("CA")) { m_cwReceiveAutoZeroKnown = false; }
        if (m_cwReceivePending == QLatin1String("AI")) { m_cwReceiveAiKnown = false; }
        for (const CwReceiveControl& control : kCwReceiveControls) { if (control.command == m_cwReceivePending.toLatin1()) { m_cwReceiveKnown.remove(control.id); } }
    }
    ++m_cwReceiveCompleted; m_cwReceivePending.clear(); m_cwReceivePendingTicket = 0; m_cwReceiveDispatched = false;
    if (!m_cwReceiveQueue.isEmpty()) { return; }
    m_cwReceiveReading = false; m_cwReceiveReadReason = m_cwReceiveFailed ? QStringLiteral("Read finished with failures; last values are not all qualified.") : QStringLiteral("Read complete; native state observed.");
    if (!m_cwReceiveWriteTicket) { return; }
    const bool matches = !m_cwReceiveFailed && !m_cwReceiveOutcomeRetired && m_cwReceiveKnown.contains(m_cwReceiveWriteId) && m_cwReceiveValues.value(m_cwReceiveWriteId) == m_cwReceiveWriteValue;
    m_cwReceiveLastWrite.insert("state", matches ? QStringLiteral("observed_consistent") : QStringLiteral("indeterminate"));
    m_cwReceiveLastWrite.insert("reason", matches ? QStringLiteral("Fresh native value matches; causal verification is unavailable.") : QStringLiteral("Request outcome differs, failed or was retired."));
    m_cwReceiveWriteTicket = 0; m_cwReceiveHanded = false;
}

void KenwoodBackend::adoptCwReceive(const QByteArray& message, quint64 batch)
{
    refreshCwReceiveContext();
    if (!isConnected() || !m_identityConfirmed) { return; }
    if (message.startsWith("CD2")) {
        const QByteArray bytes = message.mid(3);
        const QStringList prosigns{"BT", "AR", "AS", "HH", "SK", "KN", "BK", "SN"};
        const bool single = bytes.size() == 1 && bytes[0] >= 0x20 && bytes[0] <= 0x7e && bytes[0] != ';';
        const bool prosign = bytes.size() == 2 && prosigns.contains(QString::fromLatin1(bytes));
        if (!single && !prosign) {
            cwIncrement(m_cwReceiveInvalid); ++m_cwReceiveTextRevision; m_cwReceiveTextDirty = true;
            return;
        }
        if (!m_cwReceiveAccepting || batch <= m_cwReceiveStreamMinimumBatch) { return; }
        // Adoption is silent: capture provenance and append before any external
        // publication. A later context change cannot relabel this observation.
        const QString context = QStringLiteral("cw-text:%1:%2").arg(m_generation).arg(m_cwReceiveStreamEpoch);
        const QString observedMode = m_receiveVfo >= 0 && m_receiveVfo < 2 && !m_vfoModes[m_receiveVfo].isEmpty() ? m_vfoModes[m_receiveVfo] : m_mode;
        const qint64 frequency = m_receiveVfo >= 0 && m_receiveVfo < 2 ? m_frequency[m_receiveVfo] : 0;
        const QString frequencyLabel = frequency > 0 ? QStringLiteral("%1 Hz").arg(frequency) : QStringLiteral("Unknown");
        const QString sourceLabel = m_receiveVfo == 0 ? QStringLiteral("A") : m_receiveVfo == 1 ? QStringLiteral("B") : QStringLiteral("Unmapped/memory");
        const QString label = QStringLiteral("Native RX %1; frequency %2; source %3").arg(observedMode, frequencyLabel, sourceLabel);
        const QString text = prosign ? QStringLiteral("<%1>").arg(QString::fromLatin1(bytes)) : QString::fromLatin1(bytes);
        const qint64 elapsed = std::max<qint64>(0, m_clock.elapsed());
        const QVariantMap event{{"sequence", QString::number(++m_cwReceiveEventSequence)}, {"context", context}, {"label", label},
            {"kind", prosign ? QStringLiteral("prosign") : bytes == "_" ? QStringLiteral("undecodable") : QStringLiteral("character")},
            {"text", text}, {"rawAsciiHex", QString::fromLatin1(bytes.toHex())}, {"elapsedMs", QVariant::fromValue(elapsed)}};
        m_cwReceiveEvents.append(event);
        qsizetype characters = 0;
        for (const QVariant& item : m_cwReceiveEvents) { characters += item.toMap().value("text").toString().size(); }
        while (m_cwReceiveEvents.size() > 1024 || characters > 8192) {
            characters -= m_cwReceiveEvents.first().toMap().value("text").toString().size();
            m_cwReceiveEvents.removeFirst(); cwIncrement(m_cwReceiveDropped);
        }
        m_cwReceiveNativeObserved = true; m_cwReceiveLastEventMs = elapsed;
        ++m_cwReceiveTextRevision; m_cwReceiveTextDirty = true;
        return;
    }
    QString key; const CwReceiveControl* control = nullptr; std::optional<int> value; bool valid = false;
    if (message.startsWith("AI")) {
        key = "AI"; value = cwDigits(message.mid(2), 1);
        valid = value && (*value == 0 || *value == 2 || *value == 4);
    } else if (message.startsWith("CA")) {
        key = "CA"; value = cwDigits(message.mid(2), 1); valid = value && *value <= 1;
    } else if (message.startsWith("DS0")) {
        key = "DS0"; value = cwDigits(message.mid(3), 1); valid = value && *value <= 1;
    } else {
        for (const CwReceiveControl& item : kCwReceiveControls) {
            if (message.startsWith(item.command)) { control = &item; key = QString::fromLatin1(item.command); break; }
        }
        if (!control) { return; }
        value = cwDigits(message.mid(3), control->id == QLatin1String("threshold") ? 3 : 1);
        valid = value && *value >= control->minimum && *value <= control->maximum;
    }
    if (batch < m_cwReceiveBatches.value(key) || batch <= m_cwReceiveMinimumBatches.value(key)) { return; }
    const bool matching = key == m_cwReceivePending && m_cwReceivePendingTicket;
    if (matching && (!m_cwReceiveDispatched || batch <= m_cwReceiveMinimumBatch)) { return; }
    if (m_cwReceivePredispatch && control && valid) {
        const bool unchanged = m_cwReceiveKnown.contains(control->id) && m_cwReceiveValues.value(control->id) == *value;
        if (unchanged) { return; }
        retireCwReceiveWrite(QStringLiteral("Native decoder configuration changed before handoff."));
    }
    if (!valid) {
        if (key == QLatin1String("CA")) {
            m_cwReceiveBatches[key] = batch;
            m_cwReceiveAutoZeroKnown = false;
        }
        if (key == QLatin1String("AI")) {
            if (m_cwReceiveAiKnown) { ++m_cwReceiveTextRevision; m_cwReceiveTextDirty = true; }
            m_cwReceiveAiKnown = false; refreshCwReceiveContext();
        }
        if (matching) { completeCwReceiveReadItem(false, QStringLiteral("Malformed or reserved native reply; previous value retained.")); }
        ++m_cwReceiveStatusRevision; m_cwReceiveDirty = true;
        return;
    }
    m_cwReceiveBatches[key] = batch;
    if (key == QLatin1String("CA")) { m_cwReceiveAutoZero = *value == 1; m_cwReceiveAutoZeroKnown = true; }
    if (key == QLatin1String("AI")) {
        if (!m_cwReceiveAiKnown || m_cwReceiveAi != value) { ++m_cwReceiveTextRevision; m_cwReceiveTextDirty = true; }
        m_cwReceiveAi = value; m_cwReceiveAiKnown = true;
    }
    if (control) {
        const bool changed = !m_cwReceiveKnown.contains(control->id) || m_cwReceiveValues.value(control->id) != *value;
        m_cwReceiveValues[control->id] = *value; m_cwReceiveKnown.insert(control->id); ++m_cwReceiveRevisions[control->id];
        if (changed) { ++m_cwReceiveSafetyEpoch; }
    }
    refreshCwReceiveContext();
    if (matching) { completeCwReceiveReadItem(true, QString{}); }
    ++m_cwReceiveStatusRevision; m_cwReceiveDirty = true;
}

void KenwoodBackend::pollCwReceive(qint64 nowMs)
{
    refreshCwReceiveContext();
    const QPointer<KenwoodBackend> guard(this); const quint64 generation = m_generation;
    if (m_cwReceiveDirty) { publishCwReceive(); if (!guard || generation != m_generation) { return; } }
    if (m_cwReceiveTextDirty) { publishCwReceiveText(); if (!guard || generation != m_generation) { return; } }
    if (!m_cwReceiveReading || !isConnected() || !m_identityConfirmed) { return; }
    if (!m_cwReceivePending.isEmpty()) {
        if (m_cwReceiveDispatched && nowMs >= m_cwReceiveDeadline) {
            completeCwReceiveReadItem(false, QStringLiteral("Native query timed out; outcome remains unverified.")); refreshCwReceiveContext(); publishCwReceive();
        }
        return;
    }
    if (m_cwReceiveQueue.isEmpty()) { return; }
    const QString key = m_cwReceiveQueue.takeFirst(); const QString context = cwReceiveContext();
    const quint64 ticket = ++m_cwReceiveSerial;
    m_cwReceivePending = key; m_cwReceivePendingTicket = ticket; m_cwReceiveDispatched = false; m_cwReceiveMinimumBatch = m_controlIngressSequence;
    publishCwReceive();
    if (!guard || generation != m_generation || context != cwReceiveContext() || ticket != m_cwReceivePendingTicket) { return; }
    m_cwReceiveMinimumBatch = m_controlIngressSequence; m_cwReceiveBatches[key] = std::max(m_cwReceiveBatches.value(key), m_controlIngressSequence);
    m_cwReceiveMinimumBatches[key] = std::max(m_cwReceiveMinimumBatches.value(key), m_controlIngressSequence);
    m_cwReceiveDispatched = true; m_cwReceiveDeadline = nowMs + 2000;
    const bool sent = send(key.toLatin1());
    if (!guard || generation != m_generation || context != cwReceiveContext() || ticket != m_cwReceivePendingTicket) { return; }
    if (!sent) { completeCwReceiveReadItem(false, QStringLiteral("Native read transport refused; previous values retained.")); refreshCwReceiveContext(); publishCwReceive(); }
}

bool KenwoodBackend::handleCwReceiveExtension(const QString& verb, quint64 requestId, const QVariant& argument)
{
    if (!verb.startsWith(QLatin1String("cw.receive."))) { return false; }
    const bool map = !argument.isValid() || argument.typeId() == QMetaType::QVariantMap; const QVariantMap args = argument.toMap();
    if (verb == QLatin1String("cw.receive.get")) {
        if (!map || !args.isEmpty()) { emit extensionError(requestId, QStringLiteral("Native CW getter accepts no arguments.")); }
        else { emit extensionResult(requestId, cwReceiveStatus()); }
        return true;
    }
    if (!map || args.value("context").typeId() != QMetaType::QString || args.value("context").toString() != cwReceiveContext()) {
        emit extensionError(requestId, QStringLiteral("Requires the current native CW cache context.")); return true;
    }
    const QPointer<KenwoodBackend> guard(this); const quint64 generation = m_generation;
    if (verb == QLatin1String("cw.receive.clear")) {
        if (args.size() != 2 || !cwSequence(args.value("transcriptRevision")) || args.value("transcriptRevision").toString() != QString::number(m_cwReceiveTextRevision)) {
            emit extensionError(requestId, QStringLiteral("Requires the current transcript revision.")); return true;
        }
        m_cwReceiveEvents.clear(); m_cwReceiveDropped = 0; m_cwReceiveInvalid = 0;
        m_cwReceiveStreamMinimumBatch = std::max(m_cwReceiveStreamMinimumBatch, m_controlIngressSequence);
        ++m_cwReceiveStreamEpoch; m_cwReceiveNativeObserved = false; m_cwReceiveLastEventMs.reset();
        ++m_cwReceiveTextRevision; m_cwReceiveTextDirty = true;
        publishCwReceiveText();
        if (guard && generation == m_generation) { emit extensionResult(requestId, cwReceiveStatus()); }
        return true;
    }
    refreshCwReceiveContext();
    if (!isConnected() || !m_identityConfirmed) { emit extensionError(requestId, QStringLiteral("Requires a connected identified radio.")); return true; }
    if (verb == QLatin1String("cw.receive.read") || verb == QLatin1String("cw.receive.cancel")) {
        if (verb == QLatin1String("cw.receive.read")) {
            if (args.size() != 1 || m_cwReceiveReading || m_cwReceiveWriteTicket) { emit extensionError(requestId, QStringLiteral("Read already pending or malformed arguments.")); return true; }
            startCwReceiveRead();
        } else {
            if (args.size() != 2 || !cwSequence(args.value("token")) || args.value("token").toString() != m_cwReceiveReadToken) { emit extensionError(requestId, QStringLiteral("Requires the current read token.")); return true; }
            ++m_cwReceiveOperation; retireCwReceiveWrite(QStringLiteral("Verification canceled; no native state restored."));
            m_cwReceiveWriteTicket = 0; m_cwReceivePredispatch = false; m_cwReceiveHanded = false;
            m_cwReceiveQueue.clear(); m_cwReceivePending.clear(); m_cwReceivePendingTicket = 0; m_cwReceiveReading = false;
            m_cwReceiveReadReason = QStringLiteral("Native reads canceled locally.");
        }
        const quint64 operation = m_cwReceiveOperation; const QString context = cwReceiveContext(); publishCwReceive();
        if (guard && generation == m_generation) {
            if (operation == m_cwReceiveOperation && context == cwReceiveContext()) { emit extensionResult(requestId, cwReceiveStatus()); }
            else { emit extensionError(requestId, QStringLiteral("Native CW request superseded during publication.")); }
        }
        return true;
    }
    if (verb != QLatin1String("cw.receive.set") || args.size() != 4 || args.value("id").typeId() != QMetaType::QString || args.value("value").typeId() != QMetaType::Int || !cwSequence(args.value("revision"))) {
        emit extensionError(requestId, QStringLiteral("Native CW setter requires typed field, value and revision.")); return true;
    }
    const QString id = args.value("id").toString(); const int value = args.value("value").toInt(); const CwReceiveControl* control = cwControl(id);
    if (!control || !cwReceiveValueWritable(id, value) || args.value("revision").toString() != QString::number(m_cwReceiveRevisions.value(id))) {
        emit extensionError(requestId, QStringLiteral("Requires the current native field revision and eligible receive context.")); return true;
    }
    const QByteArray command = control->command + QByteArray::number(value).rightJustified(id == QLatin1String("threshold") ? 3 : 1, '0');
    const quint64 ticket = ++m_cwReceiveSerial; const QString token = QString::number(ticket);
    m_cwReceiveWriteTicket = ticket; m_cwReceivePredispatch = true; m_cwReceiveHanded = false; m_cwReceiveOutcomeRetired = false;
    m_cwReceiveWriteSafety = m_cwReceiveSafetyEpoch; m_cwReceiveWriteContext = cwReceiveContext(); m_cwReceiveWriteId = id; m_cwReceiveWriteValue = value;
    m_cwReceiveLastWrite = {{"token", token}, {"id", id}, {"state", "pending"}, {"reason", QStringLiteral("Awaiting explicit native setter handoff.")}};
    publishCwReceive();
    if (!guard) { return true; }
    refreshCwReceiveContext();
    if (generation != m_generation || ticket != m_cwReceiveWriteTicket || m_cwReceiveWriteSafety != m_cwReceiveSafetyEpoch || m_cwReceiveWriteContext != cwReceiveContext()) {
        if (generation == m_generation) { emit extensionError(requestId, QStringLiteral("Native CW intent retired before handoff.")); }
        return true;
    }
    m_cwReceivePredispatch = false; m_cwReceiveBatches[QString::fromLatin1(control->command)] = std::max(m_cwReceiveBatches.value(QString::fromLatin1(control->command)), m_controlIngressSequence);
    m_cwReceiveMinimumBatches[QString::fromLatin1(control->command)] = std::max(m_cwReceiveMinimumBatches.value(QString::fromLatin1(control->command)), m_controlIngressSequence);
    m_cwReceiveHanded = true; const bool sent = send(command);
    if (!guard || generation != m_generation) { return true; }
    if (ticket == m_cwReceiveWriteTicket) {
        if (sent) { m_cwReceiveLastWrite.insert("state", m_cwReceiveOutcomeRetired ? QStringLiteral("indeterminate") : QStringLiteral("sent_unverified")); m_cwReceiveLastWrite.insert("reason", m_cwReceiveOutcomeRetired ? QStringLiteral("Context retired during handoff; native outcome remains indeterminate.") : QStringLiteral("Native request sent; no causal acknowledgement inferred.")); startCwReceiveRead(id); }
        else { m_cwReceiveHanded = false; retireCwReceiveWrite(QStringLiteral("Native setter transport refused.")); }
        publishCwReceive(); if (!guard || generation != m_generation) { return true; }
    }
    if (sent) { emit extensionResult(requestId, QVariantMap{{"sent", true}, {"verified", false}, {"token", token}}); }
    else { emit extensionError(requestId, QStringLiteral("Native CW setter could not be sent.")); }
    return true;
}
}
