#include "KenwoodBackend.h"

#include <QMetaType>
#include <QPointer>
#include <algorithm>
#include <limits>

namespace AetherSDR::kenwood {
namespace {
constexpr qint64 kTransverterMagnitudeMaximum = 99999999999LL;
constexpr qint64 kTransverterDisplayMaximum = 4294967295LL;
QStringList transverterReadKeys(const QString& scope)
{
    if (scope == QLatin1String("controls")) { return {"XV", "XO", "DS0"}; }
    if (scope == QLatin1String("companions")) { return {"FR", "FT", "FA", "FB", "SF0", "SF1", "IF", "MN", "MV", "QR", "MA70", "MA71"}; }
    return {"FR", "FT", "XV", "XO", "FA", "FB", "SF0", "SF1", "IF", "DS0", "MN", "MV", "QR", "MA70", "MA71"};
}
std::optional<qint64> transverterDigits(const QByteArray& bytes, int width)
{
    if (bytes.size() != width) { return std::nullopt; }
    qint64 value = 0;
    for (const char byte : bytes) { if (byte < '0' || byte > '9') { return std::nullopt; } value = value * 10 + byte - '0'; }
    return value;
}
bool transverterSequence(const QVariant& value)
{
    if (value.typeId() != QMetaType::QString || value.toString().isEmpty()) { return false; }
    bool valid = false; const quint64 number = value.toString().toULongLong(&valid);
    return valid && QString::number(number) == value.toString();
}
bool transverterOffsetArguments(const QVariantMap& args)
{
    return args.value("direction").typeId() == QMetaType::Int && args.value("direction").toInt() >= 0 && args.value("direction").toInt() <= 1
        && args.value("magnitudeHz").typeId() == QMetaType::LongLong && args.value("magnitudeHz").toLongLong() >= 0 && args.value("magnitudeHz").toLongLong() <= kTransverterMagnitudeMaximum;
}
QVariantMap transverterUnknown()
{
    return {{"known", false}, {"revision", QStringLiteral("0")}, {"reason", QStringLiteral("Read this native frequency observation.")}, {"rawUtf8Hex", QString{}}};
}
}

QString KenwoodBackend::transverterContext() const { return QStringLiteral("transverter:%1:%2").arg(m_generation).arg(m_transverterEpoch); }
bool KenwoodBackend::transverterWritable() const
{
    return isConnected() && m_identityConfirmed && m_txStateKnown && !m_txObserved && !m_keyIntent && !m_cwIntent && !m_atuIntent && !m_tune
        && !m_transverterReading && !m_transverterWriteTicket;
}
QVariantMap KenwoodBackend::transverterObservation(const QString& key) const
{
    QVariantMap observation = m_transverterObservations.value(key, transverterUnknown());
    if (!isConnected() || !m_identityConfirmed) { observation.insert("known", false); observation.insert("reason", QStringLiteral("Connection retired; last raw sample retained.")); }
    return observation;
}
QVariantMap KenwoodBackend::transverterBounds(int direction, qint64 magnitude, int vfo) const
{
    if (vfo < 0) { vfo = m_transverterRoleKnown && m_receiveVfo >= 0 && m_receiveVfo < 2 ? m_receiveVfo : -1; }
    const QString source = vfo == 0 ? QStringLiteral("FA") : vfo == 1 ? QStringLiteral("FB") : QStringLiteral("none");
    const QVariantMap observation = transverterObservation(source);
    QVariantMap bounds{{"known", false}, {"source", source}, {"basisRevision", observation.value("revision")}, {"state", QStringLiteral("unknown")},
        {"reason", QStringLiteral("Active native FA/FB basis is unqualified; result is not prevalidated and the radio may refuse.")}};
    if (vfo < 0 || vfo > 1 || !m_transverterFaQualified[vfo] || !observation.value("known").toBool() || !observation.contains("rawHz")) { return bounds; }
    const qint64 base = observation.value("rawHz").toLongLong();
    const qint64 offset = direction == 0 ? magnitude : -magnitude;
    if ((offset > 0 && base > std::numeric_limits<qint64>::max() - offset) || (offset < 0 && base < std::numeric_limits<qint64>::min() - offset)) { return bounds; }
    const qint64 candidate = base + offset;
    const bool valid = candidate >= 0 && candidate <= kTransverterDisplayMaximum;
    bounds.insert("known", true); bounds.insert("baseHz", QVariant::fromValue(base)); bounds.insert("proposedDisplayHz", QVariant::fromValue(candidate));
    bounds.insert("state", valid ? QStringLiteral("valid") : QStringLiteral("invalid"));
    bounds.insert("reason", valid ? QStringLiteral("Checked native FA/FB candidate arithmetic; actual RF/display coordinates remain unqualified with transverter On.") : QStringLiteral("Candidate display is outside 0 to 4294967295 Hz."));
    return bounds;
}
QVariantMap KenwoodBackend::transverterCandidate(int vfo, int direction, qint64 magnitude, bool offsetKnown) const
{
    const QVariantMap bounds = transverterBounds(direction, magnitude, vfo);
    const bool known = offsetKnown && bounds.value("known").toBool();
    QVariantMap candidate{{"known", false}, {"candidateKnown", known}, {"basis", vfo == 0 ? QStringLiteral("FA native raw") : QStringLiteral("FB native raw")},
        {"reason", known ? QStringLiteral("Computed candidate only; no universal RF/scope projection is qualified.") : QStringLiteral("Read fresh native frequency and offset companions; no value is invented.")}};
    if (known) { candidate.insert("candidateHz", bounds.value("proposedDisplayHz")); candidate.insert("candidateInRange", bounds.value("state") == QLatin1String("valid")); }
    return candidate;
}
QVariantMap KenwoodBackend::transverterFrequency() const
{
    QVariantList vfos;
    for (int vfo = 0; vfo < 2; ++vfo) {
        const QString key = vfo == 0 ? QStringLiteral("FA") : QStringLiteral("FB");
        const QVariantMap fa = transverterObservation(key);
        bool nativeIfKnown = m_transverterKnown && m_transverterEnabled == std::optional<bool>(false) && m_transverterFaQualified[vfo] && fa.value("known").toBool();
        if (nativeIfKnown) {
            const QByteArray raw = key.toLatin1() + QByteArray::number(fa.value("rawHz").toLongLong()).rightJustified(11, '0');
            nativeIfKnown = decodeFrequency(raw).has_value();
        }
        QVariantMap nativeIf{{"known", nativeIfKnown}, {"reason", nativeIfKnown ? QString{} : QStringLiteral("Native IF is not qualified here; raw FA/SF are retained separately, especially with transverter On.")}};
        if (nativeIfKnown) { nativeIf.insert("valueHz", fa.value("rawHz")); }
        vfos.append(QVariantMap{{"id", vfo == 0 ? QStringLiteral("A") : QStringLiteral("B")}, {"fa", fa},
            {"sf", transverterObservation(QStringLiteral("SF%1").arg(vfo))}, {"nativeIf", nativeIf},
            {"display", transverterCandidate(vfo, m_transverterDirection.value_or(0), m_transverterMagnitude.value_or(0), m_transverterOffsetKnown)}});
    }
    const QString selected = !isConnected() || !m_identityConfirmed || !m_transverterRoleKnown ? QStringLiteral("unknown") : m_receiveVfo == 0 ? QStringLiteral("A") : m_receiveVfo == 1 ? QStringLiteral("B") : m_receiveVfo == -2 ? QStringLiteral("memory") : QStringLiteral("unknown");
    QVariantMap bounds = transverterBounds(m_transverterDirection.value_or(0), m_transverterMagnitude.value_or(0));
    if (!m_transverterOffsetKnown) { bounds = {{"known", false}, {"source", "none"}, {"basisRevision", "0"}, {"state", "unknown"}, {"reason", "Read the native offset before evaluating its current candidate."}}; }
    return {{"revision", QString::number(m_transverterFrequencyRevision)}, {"selectedSource", selected}, {"activeBounds", bounds}, {"vfos", vfos},
        {"memory", QVariantMap{{"left", transverterObservation(QStringLiteral("MA70"))}, {"right", transverterObservation(QStringLiteral("MA71"))}, {"basisKnown", false},
            {"reason", QStringLiteral("Current-memory observations are not silently treated as native FA/FB or confirmed RF coordinates.")}}},
        {"compatibilityIf", transverterObservation(QStringLiteral("IF"))}};
}
void KenwoodBackend::retireTransverterWrite(const QString& reason)
{
    if (!m_transverterWriteTicket) { return; }
    m_transverterOutcomeRetired = true;
    m_transverterLastWrite.insert("state", m_transverterHanded ? QStringLiteral("indeterminate") : QStringLiteral("rejected")); m_transverterLastWrite.insert("reason", reason);
    if (!m_transverterHanded) { m_transverterWriteTicket = 0; m_transverterPredispatch = false; }
}
void KenwoodBackend::refreshTransverterContext()
{
    if (m_transverterPendingRole != -99 && m_receiveVfo == m_transverterPendingRole) { m_transverterRoleKnown = true; m_transverterPendingRole = -99; }
    const QString safety = QStringLiteral("%1:%2:%3:%4:%5:%6:%7:%8:%9:%10:%11:%12:%13:%14:%15:%16")
        .arg(m_generation).arg(isConnected()).arg(m_identityConfirmed).arg(m_txStateKnown).arg(m_txObserved).arg(m_keyIntent).arg(m_cwIntent).arg(m_atuIntent)
        .arg(m_tune ? m_tune->serial : 0).arg(m_receiveVfo).arg(m_mode).arg(m_tfSet ? (*m_tfSet ? 1 : 0) : -1).arg(m_transmitVfo)
        .arg(m_receiveVfo >= 0 && m_receiveVfo < 2 ? m_vfoModes[m_receiveVfo] : QString{})
        .arg(m_operatingDisplayBasicKnown).arg(m_operatingDisplayBasic.value_or(-1));
    if (safety != m_transverterSafetySignature) {
        m_transverterSafetySignature = safety; ++m_transverterSafetyEpoch; ++m_transverterStatusRevision; m_transverterDirty = true;
        retireTransverterWrite(QStringLiteral("Transverter intent retired by receive/source context transition."));
    }
    const qint64 activeFrequency = m_receiveVfo >= 0 && m_receiveVfo < 2 ? m_frequency[m_receiveVfo] : 0;
    const QString frequency = QStringLiteral("%1:%2:%3:%4:%5:%6").arg(m_generation).arg(isConnected()).arg(m_identityConfirmed).arg(m_receiveVfo).arg(m_transverterRoleKnown).arg(activeFrequency);
    if (frequency != m_transverterFrequencySignature) {
        m_transverterFrequencySignature = frequency; ++m_transverterFrequencyRevision; ++m_transverterSafetyEpoch; ++m_transverterStatusRevision; m_transverterDirty = true;
        if (m_transverterPredispatch) { retireTransverterWrite(QStringLiteral("Frequency basis changed before transverter handoff.")); }
    }
}
void KenwoodBackend::qualifyTransverterEnabled(std::optional<bool> previous, quint64 batch)
{
    if (batch < m_transverterObservationBatch || !m_transverterEnabled) { return; }
    if (!m_transverterKnown || previous != m_transverterEnabled) {
        ++m_transverterEnabledRevision; ++m_transverterSafetyEpoch; ++m_transverterFrequencyRevision; ++m_transverterStatusRevision;
        m_transverterFaQualified = {}; m_transverterDirty = true;
        if (m_transverterPredispatch) { retireTransverterWrite(QStringLiteral("Observed XV state changed before handoff.")); }
    }
    m_transverterKnown = true;
}
QVariantMap KenwoodBackend::transverterStatus() const
{
    const bool connected = isConnected() && m_identityConfirmed; const bool idle = false;
    const bool enabledEditable = idle && m_transverterKnown;
    const bool swl = m_operatingDisplayBasicKnown && m_operatingDisplayBasic == std::optional<int>(1);
    const bool offsetEditable = idle && m_transverterOffsetKnown;
    const QString unavailable = QStringLiteral("Transverter changes are deferred in this experimental RX build; native status is read-only.");
    const QVariantMap currentBounds = transverterBounds(m_transverterDirection.value_or(0), m_transverterMagnitude.value_or(0));
    QVariantMap status{{"statusRevision", QString::number(m_transverterStatusRevision)}, {"context", transverterContext()}, {"connected", connected}, {"reason", connected ? QString{} : QStringLiteral("Requires a connected identified radio.")},
        {"enabledKnown", connected && m_transverterKnown}, {"enabledRevision", QString::number(m_transverterEnabledRevision)}, {"offsetKnown", connected && m_transverterOffsetKnown}, {"offsetRevision", QString::number(m_transverterOffsetRevision)},
        {"enabledChoices", QVariantList{QVariantMap{{"value", false}, {"label", "Off"}, {"editable", enabledEditable}, {"reason", enabledEditable ? QString{} : unavailable}},
            QVariantMap{{"value", true}, {"label", "On"}, {"editable", enabledEditable && !swl}, {"reason", swl ? QStringLiteral("SWL mode prevents turning the transverter On.") : enabledEditable ? QString{} : unavailable}}}},
        {"directionChoices", QVariantList{QVariantMap{{"value", 0}, {"label", "Plus"}}, QVariantMap{{"value", 1}, {"label", "Minus"}}}},
        {"magnitudeDomain", QVariantMap{{"minimum", QVariant::fromValue<qint64>(0)}, {"maximum", QVariant::fromValue(kTransverterMagnitudeMaximum)}, {"step", QVariant::fromValue<qint64>(1)}, {"unit", "Hz"}}},
        {"displayDomain", QVariantMap{{"minimum", QVariant::fromValue<qint64>(0)}, {"maximum", QVariant::fromValue(kTransverterDisplayMaximum)}, {"unit", "Hz"}, {"qualification", "Displayed-result constraint, not a physical tuning capability."}}},
        {"frequency", transverterFrequency()},
        {"operations", QVariantMap{{"enabled", QVariantMap{{"editable", enabledEditable}, {"pending", m_transverterWriteTicket && m_transverterLastWrite.value("operation") == QLatin1String("enabled")}, {"reason", enabledEditable ? QString{} : unavailable}}},
            {"offset", QVariantMap{{"editable", offsetEditable}, {"pending", m_transverterWriteTicket && m_transverterLastWrite.value("operation") == QLatin1String("offset")}, {"reason", !offsetEditable ? unavailable : currentBounds.value("known").toBool() ? QString{} : QStringLiteral("Result is not prevalidated; the radio may refuse an explicit offset request.")}}}}},
        {"refresh", QVariantMap{{"token", m_transverterReadToken}, {"busy", m_transverterReading}, {"total", m_transverterTotal}, {"completed", m_transverterCompleted}, {"observed", m_transverterObserved}, {"failed", m_transverterFailed}, {"skipped", m_transverterSkipped}, {"pendingKey", m_transverterPending}, {"reason", m_transverterReadReason}}},
        {"lastWrite", m_transverterLastWrite.isEmpty() ? QVariantMap{{"token", QString{}}, {"operation", QString{}}, {"state", "none"}, {"reason", QString{}}, {"boundsPrevalidated", false}} : m_transverterLastWrite},
        {"qualificationNotice", QStringLiteral("Native IF/base, raw FA/SF and offset/display candidates are separate. With transverter On, ordinary tuning, memory and spectrum coordinate projection remain unqualified; no receiver range or axis is expanded.")}};
    if (m_transverterEnabled) { status.insert("enabled", *m_transverterEnabled); }
    if (m_transverterDirection && m_transverterMagnitude) {
        status.insert("direction", *m_transverterDirection); status.insert("magnitudeHz", QVariant::fromValue(*m_transverterMagnitude));
        status.insert("signedOffsetHz", QVariant::fromValue(*m_transverterDirection == 0 ? *m_transverterMagnitude : -*m_transverterMagnitude));
    }
    return status;
}
void KenwoodBackend::publishTransverter() { m_transverterDirty = false; ++m_transverterStatusRevision; emit extensionStatus(QStringLiteral("kenwood"), QStringLiteral("transverter"), transverterStatus()); }

void KenwoodBackend::resetTransverter()
{
    retireTransverterWrite(QStringLiteral("Transverter session/bank retired; accepted changes cannot be undone."));
    m_transverterWriteTicket = 0; m_transverterPredispatch = false; m_transverterHanded = false;
    ++m_transverterEpoch; ++m_transverterOperation; ++m_transverterSerial; ++m_transverterStatusRevision; ++m_transverterFrequencyRevision;
    QStringList keys = transverterReadKeys(QStringLiteral("all")); keys.append(QStringLiteral("MF"));
    for (const QString& key : keys) {
        m_transverterBatches[key] = std::max(m_transverterBatches.value(key), m_controlIngressSequence);
        m_transverterMinimumBatches[key] = std::max(m_transverterMinimumBatches.value(key), m_controlIngressSequence);
    }
    m_transverterKnown = false; m_transverterDirection.reset(); m_transverterMagnitude.reset(); m_transverterOffsetKnown = false;
    m_transverterObservations.clear(); m_transverterFaQualified = {}; m_transverterRoleKnown = false; m_transverterPendingRole = -99; m_transverterBank.reset();
    m_transverterMemoryView.clear(); ++m_transverterMemoryViewEpoch; m_transverterMemoryMinimumBatch = m_controlIngressSequence;
    m_transverterQueue.clear(); m_transverterPending.clear(); m_transverterPendingTicket = 0; m_transverterReading = false; m_transverterReadKeys.clear();
    m_transverterTotal = m_transverterCompleted = m_transverterObserved = m_transverterFailed = m_transverterSkipped = 0;
    m_transverterReadReason = QStringLiteral("Not read."); m_transverterDirty = true;
}
void KenwoodBackend::preflightTransverterBank(const QByteArray& message, quint64 batch)
{
    if ((message != "MF0" && message != "MF1") || batch <= m_transverterBatches.value("MF")) { return; }
    const int bank = message[2] - '0';
    if (m_transverterBank && *m_transverterBank != bank) { resetTransverter(); }
    m_transverterBank = bank; m_transverterBatches["MF"] = batch;
}
void KenwoodBackend::startTransverterRead(const QString& scope, bool verification)
{
    ++m_transverterOperation; m_transverterReadToken = QString::number(++m_transverterSerial);
    m_transverterQueue = verification ? QStringList{"XV", "XO", "FA", "FB", "DS0"} : transverterReadKeys(scope);
    m_transverterTotal = m_transverterQueue.size(); m_transverterCompleted = m_transverterObserved = m_transverterFailed = m_transverterSkipped = 0;
    m_transverterPending.clear(); m_transverterPendingTicket = 0; m_transverterReadKeys.clear(); m_transverterReading = true;
    m_transverterReadReason = QStringLiteral("Reading native observations; frequency companions are not an atomic RF-coordinate capture.");
}
void KenwoodBackend::completeTransverterReadItem(bool observed, bool skipped, const QString& reason)
{
    const QString key = m_transverterPending;
    if (!observed) { m_transverterReadKeys.remove(key); }
    if (observed) { ++m_transverterObserved; m_transverterReadKeys.insert(key); }
    else if (skipped) { ++m_transverterSkipped; }
    else {
        ++m_transverterFailed; retireTransverterWrite(reason);
        if (key == QLatin1String("XV")) { m_transverterKnown = false; ++m_transverterEnabledRevision; }
        if (key == QLatin1String("XO")) { m_transverterOffsetKnown = false; ++m_transverterOffsetRevision; }
        if (key == QLatin1String("FR")) { m_transverterRoleKnown = false; m_transverterPendingRole = -99; }
        if (m_transverterObservations.contains(key)) { QVariantMap row = m_transverterObservations.value(key); row.insert("known", false); row.insert("reason", reason); m_transverterObservations[key] = row; }
        if (key == QLatin1String("FA") || key == QLatin1String("FB")) { m_transverterFaQualified[key == QLatin1String("FA") ? 0 : 1] = false; }
        ++m_transverterFrequencyRevision;
    }
    ++m_transverterCompleted; m_transverterPending.clear(); m_transverterPendingTicket = 0; m_transverterDispatched = false;
    if (!m_transverterQueue.isEmpty()) { return; }
    m_transverterReading = false;
    m_transverterReadReason = m_transverterFailed || m_transverterSkipped ? QStringLiteral("Read finished with failed or unavailable companions; no complete projection is claimed.") : QStringLiteral("Native observations read; RF/scope coordinate qualification remains separate.");
    if (!m_transverterWriteTicket) { return; }
    const bool enabled = m_transverterLastWrite.value("operation") == QLatin1String("enabled");
    const bool matches = enabled ? m_transverterKnown && m_transverterEnabled == std::optional<bool>(m_transverterWriteArguments.value("value").toBool())
        : m_transverterOffsetKnown && m_transverterDirection == std::optional<int>(m_transverterWriteArguments.value("direction").toInt()) && m_transverterMagnitude == std::optional<qint64>(m_transverterWriteArguments.value("magnitudeHz").toLongLong());
    const bool success = matches && !m_transverterFailed && !m_transverterOutcomeRetired;
    m_transverterLastWrite.insert("state", success ? QStringLiteral("observed_consistent") : QStringLiteral("indeterminate"));
    m_transverterLastWrite.insert("reason", success ? QStringLiteral("Native readback matches; actual RF display/scope projection and causal verification remain unqualified.") : QStringLiteral("Readback failed, differed or was retired; no automatic restoration is performed."));
    m_transverterWriteTicket = 0; m_transverterHanded = false;
}

void KenwoodBackend::adoptTransverter(const QByteArray& message, quint64 batch)
{
    if (!isConnected() || !m_identityConfirmed) { return; }
    if (message == "?") {
        if (m_transverterReading && m_transverterDispatched && m_transverterPendingTicket && batch > m_transverterMinimumBatch) {
            completeTransverterReadItem(false, false, QStringLiteral("Ambiguous radio error while a native query was pending; no causal acknowledgment inferred."));
            ++m_transverterStatusRevision; m_transverterDirty = true;
        }
        return;
    }
    QString key; QVariantMap observation; QVariantMap view; std::optional<int> direction; std::optional<qint64> magnitude;
    bool valid = false; bool record = false; bool blank = false; int role = -99;
    if (message.startsWith("XV")) {
        key = "XV"; valid = (message == "XV0" || message == "XV1") && m_transverterEnabled == std::optional<bool>(message == "XV1") && batch == m_transverterObservationBatch;
    } else if (message.startsWith("XO")) {
        key = "XO"; const std::optional<qint64> sign = transverterDigits(message.mid(2, 1), 1); magnitude = transverterDigits(message.mid(3), 11);
        valid = message.size() == 14 && sign && *sign <= 1 && magnitude;
        if (valid) { direction = static_cast<int>(*sign); }
    } else if (message.startsWith("FR") || message.startsWith("FT")) {
        key = QString::fromLatin1(message.left(2)); const std::optional<qint64> value = transverterDigits(message.mid(2), 1);
        valid = value && (*value == 0 || *value == 1 || *value == 3);
        if (valid && key == QLatin1String("FR")) { role = *value == 3 ? -2 : static_cast<int>(*value); }
    } else if (message.startsWith("FA") || message.startsWith("FB")) {
        key = QString::fromLatin1(message.left(2)); magnitude = transverterDigits(message.mid(2), 11); valid = magnitude.has_value(); record = true;
    } else if (message.startsWith("SF") && message.size() >= 3 && (message[2] == '0' || message[2] == '1')) {
        key = QString::fromLatin1(message.left(3)); magnitude = transverterDigits(message.mid(3, 11), 11);
        const std::optional<QString> mode = message.size() == 15 ? decodeMode(message[14]) : std::nullopt;
        valid = message.size() == 15 && magnitude && mode; record = true;
        if (valid) { observation.insert("modeRaw", QString(QChar::fromLatin1(message[14]))); }
    } else if (message.startsWith("IF")) {
        key = "IF"; record = true; magnitude = transverterDigits(message.mid(2, 11), 11);
        valid = message.size() == 37 && magnitude;
        for (const char byte : message) { if (byte < 0x20 || byte > 0x7e) { valid = false; } }
    } else if (message.startsWith("MN")) {
        key = "MN"; const std::optional<qint64> value = transverterDigits(message.mid(2), 3); valid = value && *value <= 119;
        if (valid) { view.insert("selectedChannel", static_cast<int>(*value)); }
    } else if (message.startsWith("MV")) {
        key = "MV"; const std::optional<qint64> value = transverterDigits(message.mid(2), 1); valid = value && *value <= 1;
        if (valid) { view.insert("memoryMode", static_cast<int>(*value)); }
    } else if (message.startsWith("QR")) {
        key = "QR"; valid = message.size() == 4 && (message[2] == '0' || message[2] == '1') && ((message[3] >= '0' && message[3] <= '9') || message[3] == ' ');
        if (valid) { view.insert("quickMode", message[2] - '0'); if (message[3] != ' ') { view.insert("quickChannel", message[3] - '0'); } }
    } else if (message.startsWith("MA7") && message.size() >= 4 && (message[3] == '0' || message[3] == '1')) {
        key = QString::fromLatin1(message.left(4)); record = true;
        const bool memoryView = m_transverterMemoryView.contains("selectedChannel") && m_transverterMemoryView.value("memoryMode").toInt() == 1 && m_transverterMemoryView.contains("quickMode") && m_transverterMemoryView.value("quickMode").toInt() == 0;
        const bool ownedView = m_transverterReadKeys.contains(QStringLiteral("MN")) && m_transverterReadKeys.contains(QStringLiteral("MV")) && m_transverterReadKeys.contains(QStringLiteral("QR"));
        if (!memoryView || !ownedView || key != m_transverterPending || !m_transverterPendingTicket || !m_transverterDispatched || batch <= m_transverterMemoryMinimumBatch) { return; }
        if (key == m_transverterPending && m_transverterPendingTicket && m_transverterPendingViewEpoch != m_transverterMemoryViewEpoch) {
            completeTransverterReadItem(false, true, QStringLiteral("Temporary-memory query view retired before its reply."));
            ++m_transverterStatusRevision; m_transverterDirty = true; return;
        }
        blank = message.size() == 15 && message.mid(4) == QByteArray(11, ' '); magnitude = transverterDigits(message.mid(4), 11); valid = blank || magnitude.has_value();
    } else if (message.startsWith("DS0")) {
        key = "DS0"; const std::optional<qint64> value = transverterDigits(message.mid(3), 1); valid = value && *value <= 1;
    } else { return; }
    if (batch < m_transverterBatches.value(key) || batch <= m_transverterMinimumBatches.value(key)) { return; }
    const bool matching = key == m_transverterPending && m_transverterPendingTicket;
    if (matching && (!m_transverterDispatched || batch <= m_transverterMinimumBatch)) { return; }
    if (m_transverterPredispatch && key == QLatin1String("XO") && valid) {
        if (m_transverterOffsetKnown && m_transverterDirection == direction && m_transverterMagnitude == magnitude) { return; }
        retireTransverterWrite(QStringLiteral("Native offset changed before handoff."));
    }
    if (!valid) {
        if (matching) { completeTransverterReadItem(false, false, QStringLiteral("Malformed native transverter/companion reply; previous sample retained.")); }
        ++m_transverterStatusRevision; m_transverterDirty = true; return;
    }
    m_transverterBatches[key] = batch;
    if (key == QLatin1String("XO")) {
        const bool changed = !m_transverterOffsetKnown || m_transverterDirection != direction || m_transverterMagnitude != magnitude;
        m_transverterDirection = direction; m_transverterMagnitude = magnitude; m_transverterOffsetKnown = true;
        if (changed) { ++m_transverterOffsetRevision; ++m_transverterSafetyEpoch; }
    }
    if (role != -99) {
        const bool changed = !m_transverterRoleKnown || m_receiveVfo != role;
        if (m_receiveVfo == role) { m_transverterRoleKnown = true; m_transverterPendingRole = -99; }
        else { m_transverterRoleKnown = false; m_transverterPendingRole = role; }
        if (changed) {
            m_transverterFaQualified = {};
            ++m_transverterFrequencyRevision; ++m_transverterSafetyEpoch;
            if (m_transverterPredispatch) { retireTransverterWrite(QStringLiteral("Receive source observation changed before handoff.")); }
        }
    }
    if (!view.isEmpty()) {
        QVariantMap nextView = m_transverterMemoryView;
        if (key == QLatin1String("QR")) { nextView.remove("quickChannel"); }
        for (auto iterator = view.cbegin(); iterator != view.cend(); ++iterator) { nextView.insert(iterator.key(), iterator.value()); }
        const bool changed = nextView != m_transverterMemoryView;
        m_transverterMemoryView = nextView;
        if (changed) {
            if (!matching) {
                m_transverterReadKeys.remove(QStringLiteral("MN")); m_transverterReadKeys.remove(QStringLiteral("MV")); m_transverterReadKeys.remove(QStringLiteral("QR"));
                if (m_transverterPending.startsWith(QLatin1String("MA7")) && m_transverterPendingTicket) {
                    completeTransverterReadItem(false, true, QStringLiteral("Unowned memory-view transition retired this temporary-memory query."));
                }
            }
            for (const QString& temporary : {QStringLiteral("MA70"), QStringLiteral("MA71")}) {
                if (m_transverterObservations.contains(temporary)) {
                    QVariantMap sample = m_transverterObservations.value(temporary);
                    sample.insert("known", false); sample.insert("reason", QStringLiteral("Memory view changed; last native sample retained without current-view qualification."));
                    m_transverterObservations[temporary] = sample;
                }
            }
            ++m_transverterMemoryViewEpoch; ++m_transverterFrequencyRevision; m_transverterMemoryMinimumBatch = std::max(m_transverterMemoryMinimumBatch, batch);
            if (m_transverterPredispatch) { retireTransverterWrite(QStringLiteral("Current memory view changed before handoff.")); }
        }
    }
    if (record) {
        const QVariantMap previous = m_transverterObservations.value(key, transverterUnknown());
        observation.insert("known", !blank); observation.insert("rawUtf8Hex", QString::fromLatin1(message.toHex()));
        observation.insert("reason", blank ? QStringLiteral("Current temporary memory frequency is blank; no numeric value is invented.") : key == QLatin1String("IF") ? QStringLiteral("Compatibility IF coordinates remain firmware-specific and unqualified.") : QString{});
        if (!blank && magnitude) { observation.insert("rawHz", QVariant::fromValue(*magnitude)); }
        bool changed = previous.value("known") != observation.value("known") || previous.value("rawHz") != observation.value("rawHz") || previous.value("modeRaw") != observation.value("modeRaw");
        observation.insert("revision", QString::number(previous.value("revision").toString().toULongLong() + (changed ? 1 : 0)));
        m_transverterObservations[key] = observation;
        if (key == QLatin1String("FA") || key == QLatin1String("FB")) {
            const int vfo = key == QLatin1String("FA") ? 0 : 1; changed = changed || !m_transverterFaQualified[vfo]; m_transverterFaQualified[vfo] = true;
        }
        else if (key.startsWith(QLatin1String("SF"))) {
            const int vfo = key == QLatin1String("SF0") ? 0 : 1; const QVariantMap fa = transverterObservation(vfo == 0 ? QStringLiteral("FA") : QStringLiteral("FB"));
            if (changed && ((fa.contains("rawHz") && fa.value("rawHz") != observation.value("rawHz")) || (previous.contains("modeRaw") && previous.value("modeRaw") != observation.value("modeRaw")))) { m_transverterFaQualified[vfo] = false; }
        } else if (key == QLatin1String("IF") && m_receiveVfo >= 0 && m_receiveVfo < 2) {
            const QVariantMap fa = transverterObservation(m_receiveVfo == 0 ? QStringLiteral("FA") : QStringLiteral("FB"));
            if (changed && fa.contains("rawHz") && fa.value("rawHz") != observation.value("rawHz")) { m_transverterFaQualified[m_receiveVfo] = false; }
        }
        if (changed) { ++m_transverterFrequencyRevision; if (m_transverterPredispatch) { retireTransverterWrite(QStringLiteral("Frequency companion changed before handoff.")); } }
    }
    if (matching) { completeTransverterReadItem(true, false, QString{}); }
    refreshTransverterContext(); ++m_transverterStatusRevision; m_transverterDirty = true;
}

void KenwoodBackend::pollTransverter(qint64 nowMs)
{
    refreshTransverterContext(); const QPointer<KenwoodBackend> guard(this); const quint64 generation = m_generation;
    if (m_transverterDirty) { publishTransverter(); if (!guard || generation != m_generation) { return; } }
    if (!m_transverterReading || !isConnected() || !m_identityConfirmed) { return; }
    if (!m_transverterPending.isEmpty()) {
        if (m_transverterDispatched && nowMs >= m_transverterDeadline) { completeTransverterReadItem(false, false, QStringLiteral("Native query timed out; previous samples retained.")); publishTransverter(); }
        return;
    }
    if (m_transverterQueue.isEmpty()) { return; }
    const QString key = m_transverterQueue.takeFirst();
    m_transverterPending = key;
    const auto memoryReady = [this] {
        return m_transverterReadKeys.contains(QStringLiteral("MN")) && m_transverterReadKeys.contains(QStringLiteral("MV")) && m_transverterReadKeys.contains(QStringLiteral("QR"))
            && m_transverterMemoryView.contains("selectedChannel") && m_transverterMemoryView.value("memoryMode").toInt() == 1
            && m_transverterMemoryView.contains("quickMode") && m_transverterMemoryView.value("quickMode").toInt() == 0;
    };
    if (key.startsWith(QLatin1String("MA7")) && !memoryReady()) {
        completeTransverterReadItem(false, true, QStringLiteral("Temporary memory frequency requires successful current-read MN/MV/QR normal-memory qualification.")); publishTransverter(); return;
    }
    const quint64 ticket = ++m_transverterSerial; const quint64 viewEpoch = m_transverterMemoryViewEpoch; const QString context = transverterContext();
    m_transverterPendingTicket = ticket; m_transverterPendingViewEpoch = viewEpoch; m_transverterDispatched = false; m_transverterMinimumBatch = m_controlIngressSequence;
    publishTransverter();
    if (!guard || generation != m_generation || context != transverterContext() || ticket != m_transverterPendingTicket) { return; }
    if (key.startsWith(QLatin1String("MA7")) && (viewEpoch != m_transverterMemoryViewEpoch || !memoryReady())) {
        completeTransverterReadItem(false, true, QStringLiteral("Memory view changed before temporary frequency query handoff.")); publishTransverter(); return;
    }
    m_transverterMinimumBatch = m_controlIngressSequence; m_transverterMinimumBatches[key] = std::max(m_transverterMinimumBatches.value(key), m_controlIngressSequence);
    m_transverterDispatched = true; m_transverterDeadline = nowMs + 2000;
    const bool sent = send(key.toLatin1());
    if (!guard || generation != m_generation || context != transverterContext() || ticket != m_transverterPendingTicket) { return; }
    if (!sent) { completeTransverterReadItem(false, false, QStringLiteral("Read transport refused; result remains unqualified.")); publishTransverter(); }
}

bool KenwoodBackend::handleTransverterExtension(const QString& verb, quint64 requestId, const QVariant& argument)
{
    if (!verb.startsWith(QLatin1String("transverter."))) { return false; }
    const bool map = !argument.isValid() || argument.typeId() == QMetaType::QVariantMap; const QVariantMap args = argument.toMap();
    if (verb == QLatin1String("transverter.get")) {
        if (!map || !args.isEmpty()) { emit extensionError(requestId, QStringLiteral("Transverter getter accepts no arguments.")); }
        else { emit extensionResult(requestId, transverterStatus()); }
        return true;
    }
    if (!map || args.value("context").typeId() != QMetaType::QString || args.value("context").toString() != transverterContext()) {
        emit extensionError(requestId, QStringLiteral("Requires the current transverter cache context.")); return true;
    }
    if (verb == QLatin1String("transverter.offset.preview")) {
        if (args.size() != 4 || !transverterOffsetArguments(args) || !transverterSequence(args.value("frequencyRevision")) || args.value("frequencyRevision").toString() != QString::number(m_transverterFrequencyRevision)) {
            emit extensionError(requestId, QStringLiteral("Preview requires exact draft fields and the current frequency revision.")); return true;
        }
        const int direction = args.value("direction").toInt(); const qint64 magnitude = args.value("magnitudeHz").toLongLong();
        const QVariantMap bounds = transverterBounds(direction, magnitude);
        const QVariantList vfos{QVariantMap{{"id", "A"}, {"candidate", transverterCandidate(0, direction, magnitude, true)}}, QVariantMap{{"id", "B"}, {"candidate", transverterCandidate(1, direction, magnitude, true)}}};
        emit extensionResult(requestId, QVariantMap{{"frequencyRevision", QString::number(m_transverterFrequencyRevision)}, {"direction", direction}, {"magnitudeHz", QVariant::fromValue(magnitude)},
            {"activeBounds", bounds}, {"vfos", vfos}, {"reason", bounds.value("reason")}});
        return true;
    }
    refreshTransverterContext();
    if (!isConnected() || !m_identityConfirmed) { emit extensionError(requestId, QStringLiteral("Requires a connected identified radio.")); return true; }
    const QPointer<KenwoodBackend> guard(this); const quint64 generation = m_generation;
    if (verb == QLatin1String("transverter.read") || verb == QLatin1String("transverter.cancel")) {
        if (verb == QLatin1String("transverter.read")) {
            if (args.size() != 2 || args.value("scope").typeId() != QMetaType::QString || (args.value("scope") != QLatin1String("controls") && args.value("scope") != QLatin1String("companions") && args.value("scope") != QLatin1String("all")) || m_transverterReading || m_transverterWriteTicket) {
                emit extensionError(requestId, QStringLiteral("Requires a valid read scope and no pending native work.")); return true;
            }
            startTransverterRead(args.value("scope").toString());
        } else {
            if (args.size() != 2 || !transverterSequence(args.value("token")) || args.value("token").toString() != m_transverterReadToken) { emit extensionError(requestId, QStringLiteral("Requires the current native read token.")); return true; }
            ++m_transverterOperation; retireTransverterWrite(QStringLiteral("Native verification canceled; no state is restored."));
            m_transverterWriteTicket = 0; m_transverterPredispatch = false; m_transverterHanded = false;
            m_transverterQueue.clear(); m_transverterPending.clear(); m_transverterPendingTicket = 0; m_transverterReading = false;
            m_transverterReadReason = QStringLiteral("Read canceled locally; no transverter or frequency changes were undone.");
        }
        const quint64 operation = m_transverterOperation; const QString context = transverterContext(); publishTransverter();
        if (guard && generation == m_generation) {
            if (operation == m_transverterOperation && context == transverterContext()) { emit extensionResult(requestId, transverterStatus()); }
            else { emit extensionError(requestId, QStringLiteral("Native transverter request superseded during publication.")); }
        }
        return true;
    }
    if (verb == QLatin1String("transverter.enabled.set") || verb == QLatin1String("transverter.offset.set")) {
        emit extensionError(requestId, QStringLiteral("Transverter changes are deferred in this experimental RX build; no setting was changed."));
        return true;
    }
    if (!transverterWritable()) { emit extensionError(requestId, QStringLiteral("Requires confirmed receive state without local transmit/tune intent or pending work.")); return true; }
    QByteArray command; QString operation; bool boundsPrevalidated = false;
    if (verb == QLatin1String("transverter.enabled.set")) {
        if (args.size() != 3 || !m_transverterKnown || args.value("value").typeId() != QMetaType::Bool || !transverterSequence(args.value("enabledRevision")) || args.value("enabledRevision").toString() != QString::number(m_transverterEnabledRevision)) {
            emit extensionError(requestId, QStringLiteral("Requires a boolean value and current native XV revision.")); return true;
        }
        if (args.value("value").toBool() && m_operatingDisplayBasicKnown && m_operatingDisplayBasic == std::optional<int>(1)) {
            emit extensionError(requestId, QStringLiteral("SWL mode prevents turning the transverter On.")); return true;
        }
        operation = "enabled"; command = args.value("value").toBool() ? "XV1" : "XV0";
    } else if (verb == QLatin1String("transverter.offset.set")) {
        if (args.size() != 5 || !m_transverterOffsetKnown || !transverterOffsetArguments(args) || !transverterSequence(args.value("offsetRevision")) || args.value("offsetRevision").toString() != QString::number(m_transverterOffsetRevision)
            || !transverterSequence(args.value("frequencyRevision")) || args.value("frequencyRevision").toString() != QString::number(m_transverterFrequencyRevision)) {
            emit extensionError(requestId, QStringLiteral("Requires exact offset fields, native offset revision and current frequency snapshot.")); return true;
        }
        const QVariantMap bounds = transverterBounds(args.value("direction").toInt(), args.value("magnitudeHz").toLongLong());
        if (bounds.value("known").toBool() && bounds.value("state") == QLatin1String("invalid")) { emit extensionError(requestId, QStringLiteral("Candidate display is outside 0 to 4294967295 Hz.")); return true; }
        boundsPrevalidated = bounds.value("known").toBool(); operation = "offset";
        command = "XO" + QByteArray::number(args.value("direction").toInt()) + QByteArray::number(args.value("magnitudeHz").toLongLong()).rightJustified(11, '0');
    } else { emit extensionError(requestId, QStringLiteral("Unknown transverter extension verb.")); return true; }
    const quint64 ticket = ++m_transverterSerial; const QString token = QString::number(ticket);
    m_transverterWriteTicket = ticket; m_transverterPredispatch = true; m_transverterHanded = false; m_transverterOutcomeRetired = false;
    m_transverterWriteSafety = m_transverterSafetyEpoch; m_transverterWriteFrequency = m_transverterFrequencyRevision; m_transverterWriteContext = transverterContext(); m_transverterWriteArguments = args;
    m_transverterLastWrite = {{"token", token}, {"operation", operation}, {"state", "pending"}, {"reason", boundsPrevalidated ? QStringLiteral("Candidate arithmetic checked; awaiting native handoff.") : QStringLiteral("Awaiting explicit native handoff; result may require radio validation.")}, {"boundsPrevalidated", boundsPrevalidated}};
    publishTransverter();
    if (!guard) { return true; }
    refreshTransverterContext();
    if (generation != m_generation || ticket != m_transverterWriteTicket || m_transverterWriteSafety != m_transverterSafetyEpoch || m_transverterWriteFrequency != m_transverterFrequencyRevision || m_transverterWriteContext != transverterContext()) {
        if (generation == m_generation) {
            if (ticket == m_transverterWriteTicket) { retireTransverterWrite(QStringLiteral("Native transverter basis/context retired before handoff.")); }
            emit extensionError(requestId, QStringLiteral("Native transverter intent retired before handoff."));
        }
        return true;
    }
    if (operation == QLatin1String("enabled") && args.value("value").toBool() && m_operatingDisplayBasicKnown && m_operatingDisplayBasic == std::optional<int>(1)) {
        retireTransverterWrite(QStringLiteral("SWL mode prevents turning the transverter On."));
        emit extensionError(requestId, QStringLiteral("SWL mode prevents turning the transverter On.")); return true;
    }
    m_transverterPredispatch = false;
    const QString key = operation == QLatin1String("enabled") ? QStringLiteral("XV") : QStringLiteral("XO");
    m_transverterMinimumBatches[key] = std::max(m_transverterMinimumBatches.value(key), m_controlIngressSequence);
    m_transverterHanded = true; const bool sent = send(command);
    if (!guard || generation != m_generation) { return true; }
    if (ticket == m_transverterWriteTicket) {
        if (sent) {
            m_transverterLastWrite.insert("state", m_transverterOutcomeRetired ? QStringLiteral("indeterminate") : QStringLiteral("sent_unverified"));
            m_transverterLastWrite.insert("reason", m_transverterOutcomeRetired ? QStringLiteral("Context retired during handoff; outcome remains indeterminate.") : QStringLiteral("Native request sent; no causal or RF projection verification inferred."));
            startTransverterRead(QStringLiteral("controls"), true);
        } else { m_transverterHanded = false; retireTransverterWrite(QStringLiteral("Native setter transport refused.")); }
        publishTransverter(); if (!guard || generation != m_generation) { return true; }
    }
    if (sent) { emit extensionResult(requestId, QVariantMap{{"sent", true}, {"verified", false}, {"token", token}}); }
    else { emit extensionError(requestId, QStringLiteral("Native transverter request could not be sent.")); }
    return true;
}
}
