#include "KenwoodBackend.h"

#include <QMetaType>
#include <QPointer>
#include <algorithm>

namespace AetherSDR::kenwood {
namespace {
// PC Command Reference January 2019, printed p.21. ID175 is unlisted;
// numeric observations outside this table remain visible without invented labels.
const QMap<int, QString> kDisplayScreens{
    {0, QStringLiteral("No Setting Screen")},
    {1, QStringLiteral("Unused")},
    {2, QStringLiteral("Unused")},
    {3, QStringLiteral("AGC Setting Screen")},
    {4, QStringLiteral("AGC Quick Recovery Setting Screen")},
    {5, QStringLiteral("Transmission Voice Input Sound Source Setting Screen")},
    {6, QStringLiteral("Transmission Output Limit Setting Screen")},
    {7, QStringLiteral("Carrier Level Setting Screen")},
    {8, QStringLiteral("VOX Level Setting Screen")},
    {9, QStringLiteral("Transmission Monitor Level Setting Screen")},
    {10, QStringLiteral("Speech Processor Setting Screen")},
    {23, QStringLiteral("FM Tone Setting Screen")},
    {24, QStringLiteral("FM CTCSS Setting Screen")},
    {25, QStringLiteral("FM Cross Tone Setting Screen")},
    {26, QStringLiteral("Reception Filter Setting Screen")},
    {27, QStringLiteral("Audio Peak Filter Setting Screen")},
    {28, QStringLiteral("NB1 Level Setting Screen")},
    {29, QStringLiteral("NB2 Level Setting Screen")},
    {30, QStringLiteral("NR1 Level Setting Screen")},
    {31, QStringLiteral("NR2 Level Setting Screen")},
    {32, QStringLiteral("Fixed Mode Scope Range Setting Screen")},
    {34, QStringLiteral("VFO/Program Scan Section Setting Screen")},
    {35, QStringLiteral("Program Slow Scan Point Setting Screen")},
    {36, QStringLiteral("Memory Scan Group Setting Screen")},
    {37, QStringLiteral("Voice Message Screen")},
    {128, QStringLiteral("Reception Equalizer Screen")},
    {129, QStringLiteral("Reception Equalizer Adjustment Screen")},
    {130, QStringLiteral("Transmission Equalizer Screen")},
    {131, QStringLiteral("Transmission Adjustment Screen")},
    {167, QStringLiteral("Dimmer Setting Screen")},
    {168, QStringLiteral("Linear Amplifier Menu Setting Screen")},
    {173, QStringLiteral("Frequency Marker List Screen")},
    {11, QStringLiteral("CW Message Screen")},
    {12, QStringLiteral("CW Message Screen")},
    {13, QStringLiteral("CW Message Screen")},
    {14, QStringLiteral("CW Message Screen")},
    {15, QStringLiteral("CW Communication Screen")},
    {16, QStringLiteral("CW Communication Screen")},
    {132, QStringLiteral("CW Communication Screen")},
    {17, QStringLiteral("RTTY Communication Screen")},
    {133, QStringLiteral("RTTY Communication Screen")},
    {18, QStringLiteral("RTTY Message Screen")},
    {19, QStringLiteral("RTTY Message Screen")},
    {20, QStringLiteral("PSK Communication Screen")},
    {134, QStringLiteral("PSK Communication Screen")},
    {21, QStringLiteral("PSK Message Screen")},
    {22, QStringLiteral("PSK Message Screen")},
    {33, QStringLiteral("Memory Channel List Screen")},
    {135, QStringLiteral("Memory Channel List Screen")},
    {38, QStringLiteral("File Selection Screen")},
    {39, QStringLiteral("File Selection Screen")},
    {40, QStringLiteral("File Selection Screen")},
    {41, QStringLiteral("File Selection Screen")},
    {42, QStringLiteral("File Selection Screen")},
    {43, QStringLiteral("File Selection Screen")},
    {45, QStringLiteral("File Selection Screen")},
    {46, QStringLiteral("File Selection Screen")},
    {47, QStringLiteral("File Selection Screen")},
    {48, QStringLiteral("File Selection Screen")},
    {161, QStringLiteral("File Selection Screen")},
    {162, QStringLiteral("File Selection Screen")},
    {163, QStringLiteral("File Selection Screen")},
    {164, QStringLiteral("File Selection Screen")},
    {174, QStringLiteral("File Selection Screen")},
    {176, QStringLiteral("File Selection Screen")},
    {178, QStringLiteral("File Selection Screen")},
    {179, QStringLiteral("File Selection Screen")},
    {180, QStringLiteral("File Selection Screen")},
    {181, QStringLiteral("File Selection Screen")},
    {182, QStringLiteral("File Selection Screen")},
    {183, QStringLiteral("File Selection Screen")},
    {44, QStringLiteral("LOG View Screen")},
    {177, QStringLiteral("LOG View Screen")},
    {49, QStringLiteral("Unused")},
    {50, QStringLiteral("Unused")},
    {51, QStringLiteral("Unused")},
    {52, QStringLiteral("Unused")},
    {53, QStringLiteral("Unused")},
    {54, QStringLiteral("Unused")},
    {55, QStringLiteral("Unused")},
    {56, QStringLiteral("Unused")},
    {57, QStringLiteral("Unused")},
    {58, QStringLiteral("Unused")},
    {59, QStringLiteral("Unused")},
    {60, QStringLiteral("Unused")},
    {61, QStringLiteral("Unused")},
    {62, QStringLiteral("Unused")},
    {63, QStringLiteral("Unused")},
    {64, QStringLiteral("Unused")},
    {65, QStringLiteral("Unused")},
    {66, QStringLiteral("Unused")},
    {67, QStringLiteral("Unused")},
    {68, QStringLiteral("Unused")},
    {69, QStringLiteral("Unused")},
    {70, QStringLiteral("Unused")},
    {71, QStringLiteral("Unused")},
    {72, QStringLiteral("Unused")},
    {73, QStringLiteral("Unused")},
    {74, QStringLiteral("Unused")},
    {75, QStringLiteral("Unused")},
    {76, QStringLiteral("Unused")},
    {77, QStringLiteral("Unused")},
    {78, QStringLiteral("Unused")},
    {79, QStringLiteral("Unused")},
    {80, QStringLiteral("Unused")},
    {81, QStringLiteral("Unused")},
    {82, QStringLiteral("Unused")},
    {83, QStringLiteral("Unused")},
    {84, QStringLiteral("Unused")},
    {85, QStringLiteral("Unused")},
    {86, QStringLiteral("Unused")},
    {87, QStringLiteral("Unused")},
    {88, QStringLiteral("Unused")},
    {89, QStringLiteral("Unused")},
    {90, QStringLiteral("Unused")},
    {91, QStringLiteral("Unused")},
    {92, QStringLiteral("Unused")},
    {93, QStringLiteral("Unused")},
    {94, QStringLiteral("Unused")},
    {95, QStringLiteral("Unused")},
    {96, QStringLiteral("Unused")},
    {97, QStringLiteral("Unused")},
    {98, QStringLiteral("Unused")},
    {99, QStringLiteral("Unused")},
    {100, QStringLiteral("Unused")},
    {101, QStringLiteral("Unused")},
    {102, QStringLiteral("Unused")},
    {103, QStringLiteral("Unused")},
    {104, QStringLiteral("Unused")},
    {105, QStringLiteral("Unused")},
    {106, QStringLiteral("Unused")},
    {107, QStringLiteral("Unused")},
    {108, QStringLiteral("Unused")},
    {109, QStringLiteral("Unused")},
    {110, QStringLiteral("Unused")},
    {111, QStringLiteral("Unused")},
    {112, QStringLiteral("Unused")},
    {113, QStringLiteral("Unused")},
    {114, QStringLiteral("Unused")},
    {115, QStringLiteral("Unused")},
    {116, QStringLiteral("Unused")},
    {117, QStringLiteral("Unused")},
    {118, QStringLiteral("Unused")},
    {119, QStringLiteral("Unused")},
    {120, QStringLiteral("Unused")},
    {121, QStringLiteral("Unused")},
    {122, QStringLiteral("Unused")},
    {123, QStringLiteral("Unused")},
    {124, QStringLiteral("Unused")},
    {125, QStringLiteral("Unused")},
    {126, QStringLiteral("Unused")},
    {127, QStringLiteral("Unused")},
    {136, QStringLiteral("Menu Related Screen")},
    {137, QStringLiteral("Menu Related Screen")},
    {138, QStringLiteral("Menu Related Screen")},
    {139, QStringLiteral("Menu Related Screen")},
    {140, QStringLiteral("Menu Related Screen")},
    {141, QStringLiteral("Menu Related Screen")},
    {142, QStringLiteral("Menu Related Screen")},
    {143, QStringLiteral("Menu Related Screen")},
    {144, QStringLiteral("Menu Related Screen")},
    {145, QStringLiteral("Menu Related Screen")},
    {146, QStringLiteral("Menu Related Screen")},
    {147, QStringLiteral("Menu Related Screen")},
    {148, QStringLiteral("Menu Related Screen")},
    {149, QStringLiteral("Menu Related Screen")},
    {150, QStringLiteral("Menu Related Screen")},
    {151, QStringLiteral("Menu Related Screen")},
    {152, QStringLiteral("Menu Related Screen")},
    {153, QStringLiteral("Menu Related Screen")},
    {154, QStringLiteral("Menu Related Screen")},
    {155, QStringLiteral("Menu Related Screen")},
    {156, QStringLiteral("Menu Related Screen")},
    {157, QStringLiteral("Menu Related Screen")},
    {158, QStringLiteral("Menu Related Screen")},
    {159, QStringLiteral("Menu Related Screen")},
    {165, QStringLiteral("Timer Setting Screen")},
    {166, QStringLiteral("Timer Setting Screen")},
    {169, QStringLiteral("KNS Related Screen")},
    {170, QStringLiteral("KNS Related Screen")},
    {171, QStringLiteral("KNS Related Screen")},
    {172, QStringLiteral("KNS Related Screen")},
    {160, QStringLiteral("USB/File Management Menu Related Screen")},
    {184, QStringLiteral("USB/File Management Menu Related Screen")},
    {185, QStringLiteral("USB/File Management Menu Related Screen")},
    {186, QStringLiteral("USB/File Management Menu Related Screen")},
};
const QStringList kDisplayEditLabels{QStringLiteral("Edit screen off"), QStringLiteral("Frequency entry"),
    QStringLiteral("Frequency entry log"), QStringLiteral("Channel number entry"),
    QStringLiteral("Character string editing"), QStringLiteral("Memory channel registration")};
QStringList displayReadKeys() { return {"MF", "XV", "DS0", "DS1", "DS2"}; }
bool displaySequence(const QVariant& value)
{
    if (value.typeId() != QMetaType::QString || value.toString().isEmpty()) { return false; }
    bool valid = false; const quint64 number = value.toString().toULongLong(&valid);
    return valid && QString::number(number) == value.toString();
}
std::optional<int> displayDigits(const QByteArray& bytes, int width)
{
    if (bytes.size() != width) { return std::nullopt; }
    int result = 0;
    for (const char byte : bytes) { if (byte < '0' || byte > '9') { return std::nullopt; } result = result * 10 + byte - '0'; }
    return result;
}
}

QString KenwoodBackend::operatingDisplayContext() const { return QStringLiteral("display:%1:%2").arg(m_generation).arg(m_operatingDisplayEpoch); }
bool KenwoodBackend::operatingDisplayWritable() const
{
    return isConnected() && m_identityConfirmed && m_txStateKnown && !m_txObserved
        && !m_keyIntent && !m_cwIntent && !m_atuIntent && !m_tune && !m_operatingDisplayWriteTicket && !m_operatingDisplayReading;
}
void KenwoodBackend::retireOperatingDisplayWrite(const QString& reason)
{
    if (!m_operatingDisplayWriteTicket) { return; }
    m_operatingDisplayLastWrite.insert("state", m_operatingDisplayHanded ? QStringLiteral("indeterminate") : QStringLiteral("rejected"));
    m_operatingDisplayLastWrite.insert("reason", reason); m_operatingDisplayOutcomeRetired = true;
    if (!m_operatingDisplayHanded) { m_operatingDisplayWriteTicket = 0; m_operatingDisplayPredispatch = false; }
}
void KenwoodBackend::refreshOperatingDisplaySafety()
{
    const QString physicalMode = m_receiveVfo >= 0 && m_receiveVfo < 2 ? m_vfoModes[m_receiveVfo] : QString{};
    const QString signature = QStringLiteral("%1:%2:%3:%4:%5:%6:%7:%8:%9:%10:%11:%12:%13:%14:%15")
        .arg(m_generation).arg(isConnected()).arg(m_identityConfirmed).arg(m_txStateKnown).arg(m_txObserved)
        .arg(m_keyIntent).arg(m_cwIntent).arg(m_atuIntent).arg(m_tune ? m_tune->serial : 0)
        .arg(m_receiveVfo).arg(m_mode).arg(physicalMode).arg(m_tfSet ? (*m_tfSet ? 1 : 0) : -1)
        .arg(m_operatingDisplayXvKnown).arg(m_transverterEnabled ? (*m_transverterEnabled ? 1 : 0) : -1);
    const QString diagnostic = QStringLiteral("%1:%2:%3:%4:%5:%6")
        .arg(m_operatingDisplayBasicKnown).arg(m_operatingDisplayBasic.value_or(-1))
        .arg(m_operatingDisplayScreenKnown).arg(m_operatingDisplayScreen.value_or(-1))
        .arg(m_operatingDisplayEditKnown).arg(m_operatingDisplayEdit.value_or(-1));
    const bool safetyChanged = signature != m_operatingDisplaySafetySignature;
    const bool diagnosticChanged = diagnostic != m_operatingDisplayDiagnosticSignature;
    if (!safetyChanged && !diagnosticChanged) { return; }
    m_operatingDisplaySafetySignature = signature; m_operatingDisplayDiagnosticSignature = diagnostic;
    ++m_operatingDisplaySafetyEpoch; m_operatingDisplayDirty = true;
    if (safetyChanged || m_operatingDisplayPredispatch) { retireOperatingDisplayWrite(QStringLiteral("Operating display intent retired by an observed context transition.")); }
}
QVariantMap KenwoodBackend::operatingDisplayStatus() const
{
    const bool connected = isConnected() && m_identityConfirmed;
    const bool idle = operatingDisplayWritable();
    const bool basic = idle && m_operatingDisplayBasicKnown;
    const bool swl = basic && m_operatingDisplayXvKnown && m_transverterEnabled == std::optional<bool>(false);
    const bool escape = idle && m_operatingDisplayScreenKnown && m_operatingDisplayEditKnown;
    const QString unavailable = QStringLiteral("Requires confirmed receive state, current readback and no pending operation.");
    QVariantList basicChoices{QVariantMap{{"value", 0}, {"label", "Standard"}, {"editable", basic}, {"reason", basic ? QString{} : unavailable}},
        QVariantMap{{"value", 1}, {"label", "SWL"}, {"editable", swl}, {"reason", swl ? QString{} : QStringLiteral("SWL entry requires confirmed transverter Off and current receive/display authority.")}}};
    QVariantList editChoices;
    for (int index = 0; index < 6; ++index) { editChoices.append(QVariantMap{{"value", index}, {"label", kDisplayEditLabels[index]}}); }
    const bool screenKnown = connected && m_operatingDisplayScreenKnown;
    const int screenId = m_operatingDisplayScreen.value_or(-1);
    const QString label = kDisplayScreens.value(screenId, QStringLiteral("Unknown screen (%1)").arg(screenId, 3, 10, QLatin1Char('0')));
    QVariantMap screen{{"known", screenKnown}, {"recognized", screenKnown && kDisplayScreens.contains(screenId)}, {"reserved", screenKnown && label == QLatin1String("Unused")},
        {"label", screenKnown ? label : QStringLiteral("Unknown")}, {"reason", screenKnown ? QString{} : QStringLiteral("Read function-screen identity; prior values are not qualified.")}};
    if (m_operatingDisplayScreen) { screen.insert("id", *m_operatingDisplayScreen); screen.insert("raw", QStringLiteral("%1").arg(*m_operatingDisplayScreen, 3, 10, QLatin1Char('0'))); }
    const bool editKnown = connected && m_operatingDisplayEditKnown;
    QVariantMap edit{{"known", editKnown}, {"label", editKnown ? kDisplayEditLabels[*m_operatingDisplayEdit] : QStringLiteral("Unknown")}, {"reason", editKnown ? QString{} : QStringLiteral("Read edit-screen state.")}};
    if (m_operatingDisplayEdit) { edit.insert("value", *m_operatingDisplayEdit); }
    QVariantMap status{{"statusRevision", QString::number(m_operatingDisplayStatusRevision)}, {"context", operatingDisplayContext()}, {"connected", connected}, {"reason", connected ? QString{} : QStringLiteral("Requires a connected identified radio.")},
        {"bankKnown", connected && m_operatingDisplayBankKnown}, {"xvKnown", connected && m_operatingDisplayXvKnown}, {"basicKnown", connected && m_operatingDisplayBasicKnown}, {"basicRevision", QString::number(m_operatingDisplayBasicRevision)},
        {"screenKnown", screenKnown}, {"screenRevision", QString::number(m_operatingDisplayScreenRevision)}, {"editKnown", editKnown}, {"editRevision", QString::number(m_operatingDisplayEditRevision)},
        {"basicChoices", basicChoices}, {"editChoices", editChoices}, {"screen", screen}, {"edit", edit},
        {"operation", QVariantMap{{"basic", QVariantMap{{"editable", basic}, {"pending", m_operatingDisplayWriteTicket && m_operatingDisplayLastWrite.value("operation") == QLatin1String("set")}, {"reason", basic ? QString{} : unavailable}}},
            {"escape", QVariantMap{{"editable", escape}, {"pending", m_operatingDisplayWriteTicket && m_operatingDisplayLastWrite.value("operation") == QLatin1String("escape")}, {"reason", escape ? QString{} : QStringLiteral("Requires current function/edit-screen revisions and confirmed receive state.")}}}}},
        {"refresh", QVariantMap{{"token", m_operatingDisplayReadToken}, {"busy", m_operatingDisplayReading}, {"total", m_operatingDisplayTotal}, {"completed", m_operatingDisplayCompleted}, {"observed", m_operatingDisplayObserved}, {"failed", m_operatingDisplayFailed}, {"pendingKey", m_operatingDisplayPending}, {"reason", m_operatingDisplayReadReason}}},
        {"lastWrite", m_operatingDisplayLastWrite.isEmpty() ? QVariantMap{{"token", QString{}}, {"operation", QString{}}, {"state", "none"}, {"reason", QString{}}} : m_operatingDisplayLastWrite},
        {"restrictionNotice", QStringLiteral("SWL is a radio operating state, not a client theme. It restricts transmission, voice/CW messaging, decoding/encoding, TX limiter, meter/TX-filter switching, transverter enabling, FM tone, antenna tuning and TX EQ. ESC can discard an edit; no automatic restoration is provided.")}};
    if (m_operatingDisplayBank) { status.insert("bank", *m_operatingDisplayBank); }
    if (m_transverterEnabled) { status.insert("xvEnabled", *m_transverterEnabled); }
    if (m_operatingDisplayBasic) { status.insert("basic", *m_operatingDisplayBasic); }
    return status;
}
void KenwoodBackend::publishOperatingDisplay() { m_operatingDisplayDirty = false; ++m_operatingDisplayStatusRevision; emit extensionStatus(QStringLiteral("kenwood"), QStringLiteral("operatingDisplay"), operatingDisplayStatus()); }
void KenwoodBackend::resetOperatingDisplay()
{
    retireOperatingDisplayWrite(QStringLiteral("Session/bank retired; an accepted operation cannot be undone."));
    m_operatingDisplayWriteTicket = 0; m_operatingDisplayPredispatch = false; m_operatingDisplayHanded = false;
    ++m_operatingDisplayEpoch; ++m_operatingDisplayOperation; ++m_operatingDisplaySerial;
    for (const QString& key : displayReadKeys()) { m_operatingDisplayBatches[key] = std::max(m_operatingDisplayBatches.value(key), m_controlIngressSequence); }
    m_transverterObservationBatch = std::max(m_transverterObservationBatch, m_controlIngressSequence); ++m_transverterObservationSerial;
    m_operatingDisplayBank.reset(); m_operatingDisplayBasic.reset(); m_operatingDisplayScreen.reset(); m_operatingDisplayEdit.reset();
    m_operatingDisplayBankKnown = m_operatingDisplayXvKnown = m_operatingDisplayBasicKnown = m_operatingDisplayScreenKnown = m_operatingDisplayEditKnown = false;
    m_operatingDisplayQueue.clear(); m_operatingDisplayPending.clear(); m_operatingDisplayPendingTicket = 0; m_operatingDisplayReading = false;
    m_operatingDisplayTotal = m_operatingDisplayCompleted = m_operatingDisplayObserved = m_operatingDisplayFailed = 0; m_operatingDisplayReadReason = QStringLiteral("Not read.");
}
quint64 KenwoodBackend::preflightTransverterObservation(const QByteArray& message, quint64 batch)
{
    if ((message != "XV0" && message != "XV1") || !m_identityConfirmed || batch < m_transverterObservationBatch) { return 0; }
    m_transverterObservationBatch = batch; ++m_transverterObservationSerial;
    m_transverterEnabled = message == "XV1";
    if (batch > m_operatingDisplayBatches.value("XV")) { m_operatingDisplayXvKnown = true; }
    refreshOperatingDisplaySafety();
    return m_transverterObservationSerial;
}
void KenwoodBackend::preflightOperatingDisplayBank(const QByteArray& message, quint64 batch)
{
    if ((message != "MF0" && message != "MF1") || batch <= m_operatingDisplayBatches.value("MF")) { return; }
    const int bank = message[2] - '0';
    const bool qualificationChanged = !m_operatingDisplayBankKnown || m_operatingDisplayBank != std::optional<int>(bank);
    if ((m_operatingDisplayBank && *m_operatingDisplayBank != bank) || (!m_operatingDisplayBank && (m_operatingDisplayBasicKnown || m_operatingDisplayScreenKnown || m_operatingDisplayEditKnown))) { resetOperatingDisplay(); }
    m_operatingDisplayBank = bank; m_operatingDisplayBankKnown = true; m_operatingDisplayBankBatch = batch; m_operatingDisplayBatches["MF"] = batch;
    if (qualificationChanged) {
        ++m_operatingDisplaySafetyEpoch; m_operatingDisplayDirty = true;
        retireOperatingDisplayWrite(QStringLiteral("Observed bank context changed during display intent."));
    }
}
void KenwoodBackend::startOperatingDisplayRead(bool verification)
{
    ++m_operatingDisplayOperation; m_operatingDisplayReadToken = QString::number(++m_operatingDisplaySerial);
    m_operatingDisplayQueue = verification ? QStringList{"DS0", "DS1", "DS2"} : displayReadKeys();
    m_operatingDisplayTotal = m_operatingDisplayQueue.size(); m_operatingDisplayCompleted = m_operatingDisplayObserved = m_operatingDisplayFailed = 0;
    m_operatingDisplayPending.clear(); m_operatingDisplayPendingTicket = 0; m_operatingDisplayReading = true; m_operatingDisplayReadReason = QStringLiteral("Reading observed display state; no causal acknowledgement is implied.");
}
void KenwoodBackend::finishOperatingDisplayReadItem(bool valid, const QString& reason)
{
    if (valid) { ++m_operatingDisplayObserved; }
    else {
        ++m_operatingDisplayFailed;
        if (m_operatingDisplayPending == QLatin1String("MF")) { m_operatingDisplayBankKnown = false; }
        if (m_operatingDisplayPending == QLatin1String("XV")) { m_operatingDisplayXvKnown = false; }
        if (m_operatingDisplayPending == QLatin1String("DS0")) { m_operatingDisplayBasicKnown = false; }
        if (m_operatingDisplayPending == QLatin1String("DS1")) { m_operatingDisplayScreenKnown = false; }
        if (m_operatingDisplayPending == QLatin1String("DS2")) { m_operatingDisplayEditKnown = false; }
        retireOperatingDisplayWrite(reason);
    }
    ++m_operatingDisplayCompleted; m_operatingDisplayPending.clear(); m_operatingDisplayPendingTicket = 0; m_operatingDisplayDispatched = false;
    if (!m_operatingDisplayQueue.isEmpty()) { return; }
    m_operatingDisplayReading = false; m_operatingDisplayReadReason = m_operatingDisplayFailed ? QStringLiteral("Read completed with failures; prior values are not qualified.") : QStringLiteral("Read complete; states are observed snapshots.");
    if (!m_operatingDisplayWriteTicket) { return; }
    const bool escape = m_operatingDisplayLastWrite.value("operation") == QLatin1String("escape");
    const bool success = !m_operatingDisplayFailed && !m_operatingDisplayOutcomeRetired && m_operatingDisplayBasicKnown && m_operatingDisplayScreenKnown && m_operatingDisplayEditKnown;
    const bool matches = escape || m_operatingDisplayBasic == std::optional<int>(m_operatingDisplayWriteBaseline.value("value").toInt());
    m_operatingDisplayLastWrite.insert("state", success && matches ? (escape ? QStringLiteral("sent_unverified") : QStringLiteral("observed_consistent")) : QStringLiteral("indeterminate"));
    m_operatingDisplayLastWrite.insert("reason", success && matches ? (escape ? QStringLiteral("Post-ESC states observed; no acknowledgement or all-screens-closed guarantee exists.") : QStringLiteral("Fresh observations match the request; causal verification is unavailable.")) : QStringLiteral("Outcome retired, incomplete or different from the requested state."));
    m_operatingDisplayWriteTicket = 0; m_operatingDisplayHanded = false;
}

void KenwoodBackend::adoptOperatingDisplay(const QByteArray& message, quint64 batch)
{
    if (!isConnected() || !m_identityConfirmed) { return; }
    QString key; std::optional<int> value; bool valid = false;
    if (message.startsWith("MF")) {
        if ((message != "MF0" && message != "MF1") || batch != m_operatingDisplayBankBatch || !m_operatingDisplayBank || *m_operatingDisplayBank != message[2] - '0') { return; }
        key = "MF"; valid = true;
    } else if (message.startsWith("XV")) {
        key = "XV"; valid = (message == "XV0" || message == "XV1") && batch == m_transverterObservationBatch && m_transverterEnabled == std::optional<bool>(message == "XV1");
    } else if (message.startsWith("DS0")) { key = "DS0"; value = displayDigits(message.mid(3), 1); valid = value && *value <= 1; }
    else if (message.startsWith("DS1")) { key = "DS1"; value = displayDigits(message.mid(3), 3); valid = value.has_value(); }
    else if (message.startsWith("DS2")) { key = "DS2"; value = displayDigits(message.mid(3), 1); valid = value && *value <= 5; }
    else { return; }
    const bool matching = key == m_operatingDisplayPending && m_operatingDisplayPendingTicket;
    if (key != QLatin1String("MF") && batch <= m_operatingDisplayBatches.value(key)) { return; }
    if (matching && (!m_operatingDisplayDispatched || batch <= m_operatingDisplayMinimumBatch)) { return; }
    if (m_operatingDisplayPredispatch && valid && key != QLatin1String("MF") && key != QLatin1String("XV")) {
        const std::optional<int> previous = key == QLatin1String("DS0") ? m_operatingDisplayBasic : key == QLatin1String("DS1") ? m_operatingDisplayScreen : m_operatingDisplayEdit;
        const bool previousKnown = key == QLatin1String("DS0") ? m_operatingDisplayBasicKnown : key == QLatin1String("DS1") ? m_operatingDisplayScreenKnown : m_operatingDisplayEditKnown;
        if (previousKnown && previous == value) { return; }
    }
    if (!valid) {
        if (matching) { finishOperatingDisplayReadItem(false, QStringLiteral("Malformed reply; previous observation retained.")); refreshOperatingDisplaySafety(); m_operatingDisplayDirty = true; }
        return;
    }
    m_operatingDisplayBatches[key] = batch;
    if (key == QLatin1String("XV")) { m_operatingDisplayXvKnown = true; }
    if (key == QLatin1String("DS0")) { m_operatingDisplayBasic = value; m_operatingDisplayBasicKnown = true; ++m_operatingDisplayBasicRevision; }
    if (key == QLatin1String("DS1")) { m_operatingDisplayScreen = value; m_operatingDisplayScreenKnown = true; ++m_operatingDisplayScreenRevision; }
    if (key == QLatin1String("DS2")) { m_operatingDisplayEdit = value; m_operatingDisplayEditKnown = true; ++m_operatingDisplayEditRevision; }
    refreshOperatingDisplaySafety();
    if (matching) { finishOperatingDisplayReadItem(true, QString{}); }
    m_operatingDisplayDirty = true;
}
void KenwoodBackend::pollOperatingDisplay(qint64 nowMs)
{
    refreshOperatingDisplaySafety();
    if (m_operatingDisplayDirty) {
        const QPointer<KenwoodBackend> guard(this); const quint64 generation = m_generation; publishOperatingDisplay();
        if (!guard || generation != m_generation) { return; }
    }
    if (!isConnected() || !m_identityConfirmed || !m_operatingDisplayReading) { return; }
    if (!m_operatingDisplayPending.isEmpty()) {
        if (m_operatingDisplayDispatched && nowMs >= m_operatingDisplayDeadline) { finishOperatingDisplayReadItem(false, QStringLiteral("Read timed out; outcome remains unverified.")); refreshOperatingDisplaySafety(); publishOperatingDisplay(); }
        return;
    }
    if (m_operatingDisplayQueue.isEmpty()) { return; }
    const QString key = m_operatingDisplayQueue.takeFirst(); const QString context = operatingDisplayContext();
    const quint64 ticket = ++m_operatingDisplaySerial; const quint64 generation = m_generation;
    const QPointer<KenwoodBackend> guard(this); m_operatingDisplayPending = key; m_operatingDisplayPendingTicket = ticket;
    m_operatingDisplayDispatched = false; m_operatingDisplayMinimumBatch = m_controlIngressSequence;
    publishOperatingDisplay();
    if (!guard || generation != m_generation || context != operatingDisplayContext() || ticket != m_operatingDisplayPendingTicket) { return; }
    m_operatingDisplayMinimumBatch = m_controlIngressSequence; m_operatingDisplayBatches[key] = std::max(m_operatingDisplayBatches.value(key), m_controlIngressSequence);
    m_operatingDisplayDispatched = true; m_operatingDisplayDeadline = nowMs + 2000;
    const bool sent = send(key.toLatin1());
    if (!guard || generation != m_generation || context != operatingDisplayContext() || ticket != m_operatingDisplayPendingTicket) { return; }
    if (!sent) { finishOperatingDisplayReadItem(false, QStringLiteral("Read transport refused; outcome remains unverified.")); refreshOperatingDisplaySafety(); publishOperatingDisplay(); }
}

bool KenwoodBackend::handleOperatingDisplayExtension(const QString& verb, quint64 requestId, const QVariant& argument)
{
    if (!verb.startsWith(QLatin1String("operating.display."))) { return false; }
    const bool map = !argument.isValid() || argument.typeId() == QMetaType::QVariantMap; const QVariantMap args = argument.toMap();
    if (verb == QLatin1String("operating.display.get")) {
        if (!map || !args.isEmpty()) { emit extensionError(requestId, QStringLiteral("Operating display getter accepts no arguments.")); }
        else { emit extensionResult(requestId, operatingDisplayStatus()); }
        return true;
    }
    refreshOperatingDisplaySafety();
    if (!map || args.value("context").typeId() != QMetaType::QString || args.value("context").toString() != operatingDisplayContext() || !isConnected() || !m_identityConfirmed) {
        emit extensionError(requestId, QStringLiteral("Requires the current connected operating display context.")); return true;
    }
    const QPointer<KenwoodBackend> guard(this); const quint64 generation = m_generation;
    if (verb == QLatin1String("operating.display.read") || verb == QLatin1String("operating.display.cancel")) {
        if (verb.endsWith(QLatin1String("read"))) {
            if (args.size() != 1 || m_operatingDisplayReading || m_operatingDisplayWriteTicket) { emit extensionError(requestId, QStringLiteral("Read already pending or malformed request.")); return true; }
            startOperatingDisplayRead();
        } else {
            if (args.size() != 2 || !displaySequence(args.value("token")) || args.value("token").toString() != m_operatingDisplayReadToken) { emit extensionError(requestId, QStringLiteral("Requires the current read token.")); return true; }
            ++m_operatingDisplayOperation; retireOperatingDisplayWrite(QStringLiteral("Verification canceled; a sent display action cannot be undone."));
            m_operatingDisplayWriteTicket = 0; m_operatingDisplayPredispatch = false; m_operatingDisplayHanded = false;
            m_operatingDisplayQueue.clear(); m_operatingDisplayPending.clear(); m_operatingDisplayPendingTicket = 0; m_operatingDisplayReading = false;
            m_operatingDisplayReadReason = QStringLiteral("Read canceled locally; no radio state restored.");
        }
        const quint64 operation = m_operatingDisplayOperation; const QString context = operatingDisplayContext(); publishOperatingDisplay();
        if (guard && generation == m_generation) {
            if (operation == m_operatingDisplayOperation && context == operatingDisplayContext()) { emit extensionResult(requestId, operatingDisplayStatus()); }
            else { emit extensionError(requestId, QStringLiteral("Operating display read superseded during publication.")); }
        }
        return true;
    }
    if (!operatingDisplayWritable()) { emit extensionError(requestId, QStringLiteral("Requires confirmed receive state and no local transmit/tune intent or pending work.")); return true; }
    const bool escape = verb == QLatin1String("operating.display.escape"); QByteArray command;
    if (escape) {
        if (args.size() != 3 || !m_operatingDisplayScreenKnown || !m_operatingDisplayEditKnown || !displaySequence(args.value("screenRevision")) || !displaySequence(args.value("editRevision"))
            || args.value("screenRevision").toString() != QString::number(m_operatingDisplayScreenRevision) || args.value("editRevision").toString() != QString::number(m_operatingDisplayEditRevision)) {
            emit extensionError(requestId, QStringLiteral("ESC requires the current observed function/edit-screen revisions.")); return true;
        }
        command = "DS3";
    } else if (verb == QLatin1String("operating.display.set")) {
        if (args.size() != 3 || !m_operatingDisplayBasicKnown || !displaySequence(args.value("basicRevision")) || args.value("basicRevision").toString() != QString::number(m_operatingDisplayBasicRevision)
            || args.value("value").typeId() != QMetaType::Int || args.value("value").toInt() < 0 || args.value("value").toInt() > 1) {
            emit extensionError(requestId, QStringLiteral("Requires a native display value and current basic-display revision.")); return true;
        }
        if (args.value("value").toInt() == 1 && (!m_operatingDisplayXvKnown || m_transverterEnabled != std::optional<bool>(false))) {
            emit extensionError(requestId, QStringLiteral("SWL entry requires confirmed transverter Off.")); return true;
        }
        command = args.value("value").toInt() ? "DS01" : "DS00";
    } else { emit extensionError(requestId, QStringLiteral("Unknown operating display verb.")); return true; }
    const quint64 ticket = ++m_operatingDisplaySerial; const QString token = QString::number(ticket);
    m_operatingDisplayWriteTicket = ticket; m_operatingDisplayPredispatch = true; m_operatingDisplayHanded = false; m_operatingDisplayOutcomeRetired = false;
    m_operatingDisplayWriteSafety = m_operatingDisplaySafetyEpoch; m_operatingDisplayWriteContext = operatingDisplayContext();
    m_operatingDisplayWriteBaseline = args;
    m_operatingDisplayWriteBaseline.insert("screen", m_operatingDisplayScreen.value_or(-1)); m_operatingDisplayWriteBaseline.insert("edit", m_operatingDisplayEdit.value_or(-1));
    m_operatingDisplayLastWrite = {{"token", token}, {"operation", escape ? "escape" : "set"}, {"state", "pending"}, {"reason", QStringLiteral("Awaiting explicit radio action handoff.")}};
    publishOperatingDisplay();
    if (!guard) { return true; }
    refreshOperatingDisplaySafety();
    if (generation != m_generation || ticket != m_operatingDisplayWriteTicket || m_operatingDisplayWriteSafety != m_operatingDisplaySafetyEpoch || m_operatingDisplayWriteContext != operatingDisplayContext()) {
        if (generation == m_generation) { emit extensionError(requestId, QStringLiteral("Operating display intent retired before handoff.")); }
        return true;
    }
    m_operatingDisplayPredispatch = false;
    for (const QString& key : QStringList{"DS0", "DS1", "DS2"}) { m_operatingDisplayBatches[key] = std::max(m_operatingDisplayBatches.value(key), m_controlIngressSequence); }
    m_operatingDisplayHanded = true; const bool sent = send(command);
    if (!guard || generation != m_generation) { return true; }
    if (ticket == m_operatingDisplayWriteTicket) {
        if (sent) {
            m_operatingDisplayLastWrite.insert("state", m_operatingDisplayOutcomeRetired ? QStringLiteral("indeterminate") : QStringLiteral("sent_unverified"));
            m_operatingDisplayLastWrite.insert("reason", QStringLiteral("Action sent; no causal acknowledgement exists.")); startOperatingDisplayRead(true);
        } else { m_operatingDisplayHanded = false; retireOperatingDisplayWrite(QStringLiteral("Action transport refused.")); }
        publishOperatingDisplay(); if (!guard || generation != m_generation) { return true; }
    }
    if (sent) { emit extensionResult(requestId, QVariantMap{{"sent", true}, {"verified", false}, {"token", token}}); }
    else { emit extensionError(requestId, QStringLiteral("Operating display action could not be sent.")); }
    return true;
}
}
