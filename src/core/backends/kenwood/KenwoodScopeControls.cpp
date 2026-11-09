#include "KenwoodBackend.h"

#include <QMetaType>
#include <QPointer>
#include <QScopeGuard>
#include <array>

namespace AetherSDR::kenwood {
namespace {
struct ScopeControl {
    QString id;
    QByteArray command;
    QString label;
    QString unit;
    int minimum;
    int maximum;
    QStringList labels;
};
const QList<ScopeControl>& scopeControls()
{
    static const QList<ScopeControl> controls{
        {"display", "BS0", "Radio scope display", "", 0, 1, {"Off", "On"}},
        {"display_type", "BS1", "Radio scope display type", "", 0, 4, {"Standard bandscope + waterfall", "Expanded bandscope + waterfall", "Bandscope", "Audio scope", "Multi-scope"}},
        {"mode", "BS3", "Bandscope mode", "", 0, 2, {"Center", "Fixed", "Auto-scroll"}},
        {"span", "BS4", "Bandscope span", "Hz", 0, 7, {"5 kHz", "10 kHz", "20 kHz", "30 kHz", "50 kHz", "100 kHz", "200 kHz", "500 kHz"}},
        {"fixed_range", "BS5", "Fixed range", "", 0, 3, {"Temporary/current", "1", "2", "3"}},
        {"pause", "BS6", "Bandscope pause", "", 0, 1, {"Off", "On"}},
        {"marker", "BS7", "Radio bandscope markers", "", 0, 1, {"RX only", "RX and TX"}},
        {"attenuation", "BS8", "Bandscope attenuation", "dB", 0, 3, {"0 dB", "10 dB", "20 dB", "30 dB"}},
        {"max_hold", "BS9", "Bandscope maximum hold", "", 0, 1, {"Off", "On"}},
        {"average", "BSA", "Bandscope averaging", "", 0, 3, {"Off", "Level 1", "Level 2", "Level 3"}},
        {"waterfall_speed", "BSB", "Radio waterfall speed", "index", 1, 4, {}},
        {"reference", "BSC", "Bandscope reference", "0.5 dB index", 0, 60, {}},
        {"audio_attenuation", "BSG", "Radio audio scope attenuation", "dB", 0, 3, {"0 dB", "10 dB", "20 dB", "30 dB"}},
        {"audio_span", "BSH", "Radio audio scope span", "Hz", 0, 1, {"3 kHz", "8 kHz"}},
        {"oscilloscope_level", "BSI", "Radio oscilloscope level", "dB", 0, 3, {"0 dB", "-10 dB", "-20 dB", "-30 dB"}},
        {"oscilloscope_sweep", "BSJ", "Radio oscilloscope sweep", "ms", 0, 5, {"1 ms", "3 ms", "10 ms", "30 ms", "100 ms", "300 ms"}},
        {"shift", "BSK", "Bandscope shift", "grid", 0, 8, {"4 left", "3 left", "2 left", "1 left", "Center", "1 right", "2 right", "3 right", "4 right"}},
        {"audio_pause", "BSN", "Radio audio scope pause", "", 0, 1, {"Off", "On"}},
        {"expand", "BSO", "Spectrum analysis expansion", "", 0, 1, {"Off", "On"}},
        {"overflow", "BSL", "Receive circuit overflow", "", 0, 1, {"Normal", "Overflow"}},
        {"bandscope_output", "DD0", "Bandscope data output", "", 0, 5, {"Off", "LAN high", "LAN medium", "LAN low", "COM/USB AI linked", "COM/USB not AI linked"}},
        {"filter_output", "DD1", "Filter AF spectrum output", "", 0, 2, {"Off", "LAN", "COM/USB"}}
    };
    return controls;
}
const ScopeControl* scopeControl(const QString& id)
{
    for (const ScopeControl& control : scopeControls()) { if (control.id == id) { return &control; } }
    return nullptr;
}
bool geometryControl(const QString& id)
{
    return id == "mode" || id == "span" || id == "fixed_range" || id == "shift" || id == "expand";
}
std::optional<qint64> scopeDigits(const QByteArray& text)
{
    if (text.isEmpty() || text.size() > 8) { return {}; }
    qint64 value = 0;
    for (char digit : text) { if (digit < '0' || digit > '9') { return {}; } value = value * 10 + digit - '0'; }
    return value;
}
bool strictScopeInt(const QVariant& value, int minimum, int maximum)
{
    return value.typeId() == QMetaType::Int && value.toInt() >= minimum && value.toInt() <= maximum;
}
bool strictScopeHz(const QVariant& value)
{
    return value.typeId() == QMetaType::LongLong && value.toLongLong() >= 0 && value.toLongLong() <= 99999999;
}
QString rangeKey(int slot) { return QStringLiteral("range%1").arg(slot); }
}

QString KenwoodBackend::scopeControlsContextKey() const
{
    return QStringLiteral("%1:%2:%3").arg(receiveControlContext()).arg(m_tune.has_value()).arg(m_tuneSerial);
}

bool KenwoodBackend::scopeControlsWritable() const
{
    return isConnected() && m_identityConfirmed && m_receiveVfo >= 0 && m_receiveVfo <= 1
        && m_frequency[m_receiveVfo] > 0 && encodeMode(m_mode).has_value()
        && m_txStateKnown && !m_txObserved && m_tfSet && !*m_tfSet
        && !m_keyIntent && !m_cwIntent && !m_atuIntent && !m_tune
        && m_nativeScopeContext == scopeControlsContextKey();
}

bool KenwoodBackend::scopeRfProjectionAvailable() const
{
    if (!scopeControlsWritable() || m_nativeScopeProjectionPending || !m_nativeScopeAi
        || (*m_nativeScopeAi != 2 && *m_nativeScopeAi != 4)) { return false; }
    const int output = m_nativeScopeValues.value("bandscope_output", -1);
    if (output < 1 || output > 3 || m_nativeScopeValues.value("expand", -1) != 0) { return false; }
    const int mode = m_nativeScopeValues.value("mode", -1);
    if (mode == 0) { return m_nativeScopeValues.contains("span") && m_nativeScopeValues.value("shift", -1) == 4; }
    return (mode == 1 || mode == 2) && m_nativeScopeCurrentBounds.has_value();
}

QString KenwoodBackend::scopeFilterFrameReason() const
{
    if (!scopeControlsWritable()) { return QStringLiteral("Requires confirmed receive source, frequency, mode, RX and TF-SET off; no local transmit intent."); }
    if (!m_nativeScopeAi || (*m_nativeScopeAi != 2 && *m_nativeScopeAi != 4)) { return QStringLiteral("Requires confirmed AI On for LAN filter spectrum."); }
    if (m_nativeScopeValues.value("filter_output", -1) != 1) { return QStringLiteral("Requires confirmed filter spectrum output to LAN."); }
    if (!m_nativeScopeValues.contains("display_type")) { return QStringLiteral("Waiting for radio scope display type."); }
    if (m_nativeScopeValues.value("display_type") == 3) { return QStringLiteral("The radio does not output filter spectrum while its Audio Scope is selected."); }
    return {};
}

void KenwoodBackend::resetScopeControls()
{
    ++m_nativeScopeRevision;
    m_nativeScopeContext.clear(); m_nativeScopeFrequencyContext.clear();
    m_nativeScopeValues.clear(); m_nativeScopeRanges.clear(); m_nativeScopeCurrentBounds.reset(); m_nativeScopeAi.reset();
    m_nativeScopePending.clear(); m_nativeScopePredispatch.clear(); m_nativeScopeFloors.clear();
    m_nativeScopeUnverifiedRanges.clear(); m_nativeScopeGeometryConfirmed.clear(); m_nativeScopeGeometryNeeded.clear();
    m_nativeScopeInterested = m_nativeScopeQuerying = m_nativeScopeProjectionPending = false;
    m_nativeScopeMinimumBatch = m_controlIngressSequence;
    m_nativeScopeBoundsFloor = 0; m_nativeScopeBoundsRefreshPending = false;
    m_nativeScopeGeometryFloor = m_nativeScopeOwnedGeometryFloor = m_nativeScopeGeometryTicket = 0;
    m_nativeScopeFrameFloor = m_controlIngressSequence;
    m_nativeScopeLastFrameMs = -1; m_nativeScopeFrameKnown = false;
}

bool KenwoodBackend::refreshScopeControlsContext()
{
    const QString context = scopeControlsContextKey();
    const qint64 frequency = m_receiveVfo >= 0 && m_receiveVfo <= 1 ? m_frequency[m_receiveVfo] : 0;
    const QString frequencyContext = context + QLatin1Char(':') + QString::number(frequency);
    const bool changed = context != m_nativeScopeContext;
    const bool frequencyChanged = frequencyContext != m_nativeScopeFrequencyContext;
    if (!changed && !frequencyChanged) { return false; }
    m_nativeScopeFrequencyContext = frequencyContext;
    const bool fixedInWindow = !changed && m_scopeMode == 1 && m_scopeHighHz > m_scopeLowHz
        && frequency >= m_scopeLowHz && frequency <= m_scopeHighHz;
    if (frequencyChanged && !fixedInWindow) { m_nativeScopeGeometryFloor = m_controlIngressSequence; }
    if (changed) {
        m_nativeScopeContext = context; ++m_nativeScopeRevision;
        m_nativeScopeValues.clear(); m_nativeScopeAi.reset();
        m_nativeScopePending.clear(); m_nativeScopePredispatch.clear(); m_nativeScopeFloors.clear();
        m_nativeScopeMinimumBatch = m_controlIngressSequence;
        m_nativeScopeBoundsFloor = m_controlIngressSequence;
        m_nativeScopeBoundsRefreshPending = false;
    }
    const bool preserveFixedBounds = !changed && m_scopeMode == 1 && m_nativeScopeCurrentBounds
        && frequency >= m_nativeScopeCurrentBounds->first && frequency <= m_nativeScopeCurrentBounds->second;
    m_nativeScopeRanges.clear(); m_nativeScopeUnverifiedRanges.clear();
    if (!preserveFixedBounds) { m_nativeScopeCurrentBounds.reset(); }
    m_nativeScopeGeometryConfirmed.clear();
    m_nativeScopeFrameFloor = m_controlIngressSequence;
    for (const QString& id : {QStringLiteral("mode"), QStringLiteral("span"), QStringLiteral("fixed_range"), QStringLiteral("shift"), QStringLiteral("expand"),
        QStringLiteral("range1"), QStringLiteral("range2"), QStringLiteral("range3")}) {
        ++m_nativeScopeRowRevisions[id];
        m_nativeScopePending.remove(id); m_nativeScopePredispatch.remove(id);
        m_nativeScopeFloors[id] = m_controlIngressSequence;
    }
    const QPointer<KenwoodBackend> guard(this);
    const quint64 generation = m_generation, revision = m_nativeScopeRevision;
    publishScopeControls();
    if (!guard || generation != m_generation || revision != m_nativeScopeRevision) { return true; }
    publishScopeFilterUnavailable(QStringLiteral("Receive context changed; waiting for a fresh native AF frame."));
    if (!guard || generation != m_generation || revision != m_nativeScopeRevision) { return true; }
    if (changed) { queryScopeControls(); }
    return true;
}

QVariantMap KenwoodBackend::scopeControlsStatus() const
{
    const bool ready = scopeControlsWritable();
    const QString reason = ready ? QString{} : QStringLiteral("Requires confirmed receive source, frequency, mode, RX and TF-SET off; no local transmit intent.");
    QVariantList rows;
    for (const ScopeControl& control : scopeControls()) {
        QString unavailable = reason;
        if (unavailable.isEmpty() && control.id == "overflow") { unavailable = QStringLiteral("Receive overflow is read-only."); }
        if (unavailable.isEmpty() && !m_nativeScopeValues.contains(control.id)) { unavailable = QStringLiteral("Waiting for current radio readback."); }
        if (unavailable.isEmpty() && geometryControl(control.id) && !m_nativeScopeGeometryConfirmed.contains(control.id)) { unavailable = QStringLiteral("Read scope settings after a receive frequency change."); }
        if (unavailable.isEmpty() && geometryControl(control.id) && m_nativeScopeProjectionPending) { unavailable = QStringLiteral("Waiting for fresh geometry after a native scope command."); }
        if (unavailable.isEmpty() && m_nativeScopePending.contains(control.id)) { unavailable = QStringLiteral("Waiting for radio scope readback; last confirmed value retained."); }
        if (unavailable.isEmpty() && control.id == "fixed_range" && m_nativeScopeValues.value("mode", -1) != 1) { unavailable = QStringLiteral("Fixed range selection requires confirmed Fixed mode."); }
        if (unavailable.isEmpty() && control.id == "expand" && (m_nativeScopeValues.value("mode", -1) == 1
            || !m_nativeScopeValues.contains("mode") || !m_nativeScopeValues.contains("span"))) { unavailable = QStringLiteral("Expansion requires confirmed Center or Auto-scroll mode and span."); }
        QVariantList choices;
        for (int value = control.minimum; value <= control.maximum; ++value) {
            QString label = control.labels.isEmpty() ? QString::number(value) : control.labels[value - control.minimum];
            if (control.id == "reference") { label = QStringLiteral("%1 dB").arg(value * 0.5 - 20, 0, 'f', 1); }
            QString choiceReason;
            if ((control.id == "bandscope_output" && value >= 4) || (control.id == "filter_output" && value == 2)) { choiceReason = QStringLiteral("COM/USB output is unavailable on this Ethernet connection."); }
            if (control.id == "expand" && value == 1 && m_nativeScopeValues.value("span", -1) == 7) { choiceReason = QStringLiteral("Expansion is always Off at the 500 kHz span."); }
            choices.append(QVariantMap{{"value", value}, {"label", label}, {"editable", choiceReason.isEmpty()}, {"reason", choiceReason}});
        }
        QVariantMap row{{"id", control.id}, {"label", control.label}, {"unit", control.unit},
            {"revision", QString::number(m_nativeScopeRowRevisions.value(control.id))}, {"editable", unavailable.isEmpty()},
            {"pending", m_nativeScopePending.contains(control.id)}, {"reason", unavailable}, {"choices", choices}};
        if (m_nativeScopeValues.contains(control.id)) { row.insert("value", m_nativeScopeValues.value(control.id)); }
        rows.append(row);
    }
    const bool rangesReady = ready && !m_nativeScopeProjectionPending && m_nativeScopeValues.value("mode", -1) == 1 && m_nativeScopeGeometryConfirmed.contains("mode");
    QVariantList ranges;
    for (int slot = 1; slot <= 3; ++slot) {
        const QString key = rangeKey(slot);
        const QString why = !rangesReady ? QStringLiteral("Requires current confirmed Fixed mode and receive context; inactive stored readback may be unavailable.")
            : m_nativeScopePending.contains(key) ? QStringLiteral("Stored range sent; waiting for a tagged readback. No query syntax for inactive slots is assumed.") : QString{};
        QVariantMap range{{"slot", slot}, {"revision", QString::number(m_nativeScopeRowRevisions.value(key))},
            {"editable", why.isEmpty()}, {"resettable", why.isEmpty()}, {"pending", m_nativeScopePending.contains(key)},
            {"reason", why.isEmpty() && m_nativeScopeUnverifiedRanges.contains(key) ? QStringLiteral("Request sent; stored slot not verified.") : why}};
        if (m_nativeScopeRanges.contains(slot)) { range.insert("lowHz", m_nativeScopeRanges[slot].first); range.insert("highHz", m_nativeScopeRanges[slot].second); }
        ranges.append(range);
    }
    QVariantMap bounds{{"reason", m_nativeScopeCurrentBounds ? QString{} : QStringLiteral("Waiting for current tagged scope bounds.")}};
    if (m_nativeScopeCurrentBounds) { bounds.insert("lowHz", m_nativeScopeCurrentBounds->first); bounds.insert("highHz", m_nativeScopeCurrentBounds->second); }
    const bool marker = ready && !m_nativeScopeProjectionPending && (m_nativeScopeValues.value("mode", -1) == 1 || m_nativeScopeValues.value("mode", -1) == 2);
    const auto action = [this](const QString& id, bool enabled, const QString& why) {
        const bool pending = m_nativeScopePending.contains(id);
        return QVariantMap{{"editable", enabled && !pending}, {"pending", pending},
            {"reason", pending ? QStringLiteral("Scope action dispatch is in progress.") : enabled ? QString{} : why}};
    };
    QVariantMap actions{{"clear", action("clear", ready, reason)}, {"marker_center", action("marker_center", marker, QStringLiteral("Marker center/shift requires Fixed or Auto-scroll mode."))},
        {"span_initial", action("span", ready && !m_nativeScopeProjectionPending && m_nativeScopeGeometryConfirmed.contains("span"), QStringLiteral("Requires current confirmed span readback."))}};
    const QString frameReason = scopeFilterFrameReason();
    QVariantMap transport{{"rfProjectionAvailable", scopeRfProjectionAvailable()},
        {"rfProjectionReason", scopeRfProjectionAvailable() ? QString{} : QStringLiteral("Requires fresh LAN/AI state and supported current RF geometry; shifted/expanded capture mapping is unverified.")},
        {"filterFrameAvailable", frameReason.isEmpty()}, {"filterFrameReason", frameReason}};
    if (m_nativeScopeValues.contains("bandscope_output")) { transport.insert("bandscopeOutput", m_nativeScopeValues.value("bandscope_output")); }
    if (m_nativeScopeValues.contains("filter_output")) { transport.insert("filterOutput", m_nativeScopeValues.value("filter_output")); }
    return {{"statusRevision", QString::number(m_nativeScopeStatusRevision)}, {"context", QStringLiteral("%1:%2").arg(m_generation).arg(m_nativeScopeRevision)},
        {"mode", m_mode}, {"editable", ready}, {"reason", reason}, {"controls", rows}, {"ranges", ranges}, {"currentBounds", bounds}, {"actions", actions}, {"transport", transport}};
}

void KenwoodBackend::publishScopeControls()
{
    ++m_nativeScopeStatusRevision;
    emit extensionStatus(QStringLiteral("kenwood"), QStringLiteral("scopeControls"), scopeControlsStatus());
}

void KenwoodBackend::publishScopeFilterUnavailable(const QString& reason)
{
    m_nativeScopeFrameKnown = false; m_nativeScopeLastFrameMs = -1;
    emit extensionStatus(QStringLiteral("kenwood"), QStringLiteral("scopeFilterFrame"),
        {{"context", QStringLiteral("%1:%2").arg(m_generation).arg(m_nativeScopeRevision)}, {"sequence", QString::number(++m_nativeScopeFrameSequence)},
         {"values", QVariantList{}}, {"unit", QStringLiteral("nativeInverseDisplayIndex")}, {"known", false}, {"reason", reason}});
}

void KenwoodBackend::pollScopeFrameFreshness(qint64 nowMs)
{
    if (m_nativeScopeFrameKnown && m_nativeScopeLastFrameMs >= 0 && nowMs - m_nativeScopeLastFrameMs > 1500) {
        publishScopeFilterUnavailable(QStringLiteral("No fresh native AF spectrum received within 1500 ms."));
    }
}

void KenwoodBackend::queryScopeControls(bool geometryOnly)
{
    if (!m_nativeScopeInterested || m_nativeScopeQuerying || !m_nativeScopePredispatch.isEmpty() || !scopeControlsWritable()) { return; }
    const QPointer<KenwoodBackend> guard(this);
    const quint64 generation = m_generation, revision = m_nativeScopeRevision;
    m_nativeScopeQuerying = true;
    const auto cleanup = qScopeGuard([guard, generation] { if (guard && generation == guard->m_generation) { guard->m_nativeScopeQuerying = false; } });
    if (!geometryOnly) { if (!send("AI") || !guard || generation != m_generation || revision != m_nativeScopeRevision) { return; } }
    for (const ScopeControl& control : scopeControls()) {
        if (geometryOnly && !geometryControl(control.id)) { continue; }
        if (!send(control.command) || !guard || generation != m_generation || revision != m_nativeScopeRevision || !scopeControlsWritable()) { return; }
    }
    m_nativeScopeBoundsRefreshPending = false;
    send("BSM");
}

bool KenwoodBackend::scopeSharedGeometryConsistent() const
{
    if (m_scopeMode != m_nativeScopeValues.value("mode", -1)
        || m_spanHz != scopeSpanHzForIndex(m_nativeScopeValues.value("span", -1))
        || m_scopeShift != m_nativeScopeValues.value("shift", -1)
        || m_scopeExpand != m_nativeScopeValues.value("expand", -1)) { return false; }
    if (m_scopeMode == 0) { return true; }
    return (m_scopeMode == 1 || m_scopeMode == 2) && m_nativeScopeCurrentBounds
        && m_scopeLowHz == m_nativeScopeCurrentBounds->first && m_scopeHighHz == m_nativeScopeCurrentBounds->second
        && m_fixedRange == m_nativeScopeValues.value("fixed_range", -1);
}

void KenwoodBackend::adoptScopeControlsContext()
{
    const QPointer<KenwoodBackend> guard(this);
    const quint64 generation = m_generation;
    refreshScopeControlsContext();
    if (!guard || generation != m_generation) { return; }
    if (m_nativeScopeBoundsRefreshPending && !m_nativeScopeQuerying && m_nativeScopePredispatch.isEmpty()
        && m_nativeScopeInterested && scopeControlsWritable()) {
        m_nativeScopeBoundsRefreshPending = false;
        const int mode = m_nativeScopeValues.value("mode", -1);
        if (mode == 1 || mode == 2) {
            const quint64 revision = m_nativeScopeRevision;
            const QString frequencyContext = m_nativeScopeFrequencyContext;
            m_nativeScopeQuerying = true;
            const bool sent = send("BSM");
            if (!guard || generation != m_generation) { return; }
            m_nativeScopeQuerying = false;
            if (!sent || revision != m_nativeScopeRevision || frequencyContext != m_nativeScopeFrequencyContext) { return; }
        }
    }
    if (m_nativeScopeProjectionPending && m_nativeScopeGeometryNeeded.isEmpty()) {
        if (!scopeSharedGeometryConsistent()) {
            m_nativeScopeGeometryNeeded = {QStringLiteral("mode"), QStringLiteral("span"), QStringLiteral("shift"), QStringLiteral("expand")};
            if (m_scopeMode != 0) { m_nativeScopeGeometryNeeded.insert("fixed_range"); m_nativeScopeGeometryNeeded.insert("bounds"); }
            queryScopeControls(true);
            return;
        }
        const quint64 ticket = m_nativeScopeGeometryTicket, revision = m_nativeScopeRevision;
        const QString frequencyContext = m_nativeScopeFrequencyContext;
        m_nativeScopeProjectionPending = false;
        publishScopeControls();
        if (guard && generation == m_generation && revision == m_nativeScopeRevision && ticket == m_nativeScopeGeometryTicket
            && frequencyContext == m_nativeScopeFrequencyContext && !m_nativeScopeProjectionPending
            && scopeSharedGeometryConsistent()) { publishGeometry(); }
    }
}

bool KenwoodBackend::adoptScopeControl(const QByteArray& message, quint64 ingressBatch)
{
    const QPointer<KenwoodBackend> guard(this);
    const quint64 generation = m_generation;
    refreshScopeControlsContext();
    if (!guard || generation != m_generation) { return false; }
    if (message.startsWith("AI")) {
        if (message.size() != 3 || (message[2] != '0' && message[2] != '2' && message[2] != '4')) { return false; }
        if (ingressBatch <= m_nativeScopeMinimumBatch) { return true; }
        const bool changedAi = !m_nativeScopeAi || *m_nativeScopeAi != message[2] - '0';
        m_nativeScopeAi = message[2] - '0';
        if (changedAi) { m_nativeScopeFrameFloor = ingressBatch; }
        const quint64 observation = ++m_nativeScopeObservation;
        publishScopeControls();
        if (!guard || generation != m_generation || observation != m_nativeScopeObservation) { return false; }
        const QString reason = scopeFilterFrameReason();
        if (changedAi || !reason.isEmpty()) { publishScopeFilterUnavailable(reason.isEmpty() ? QStringLiteral("Waiting for a fresh native AF frame after AI changed.") : reason); }
        return guard && generation == m_generation && observation == m_nativeScopeObservation;
    }
    QString id;
    std::optional<QPair<qint64, qint64>> pair;
    int value = -1, slot = -1;
    if (message.startsWith("BSM")) {
        if (message.size() != 20 || message[3] < '0' || message[3] > '3') { return false; }
        const std::optional<qint64> low = scopeDigits(message.mid(4, 8));
        const std::optional<qint64> high = scopeDigits(message.mid(12, 8));
        if (!low || !high || *low >= *high) { return false; }
        slot = message[3] - '0'; id = slot ? rangeKey(slot) : QStringLiteral("bounds");
        pair = qMakePair(*low, *high);
    } else {
        const ScopeControl* matched = nullptr;
        for (const ScopeControl& control : scopeControls()) { if (message.startsWith(control.command)) { matched = &control; break; } }
        if (!matched) { return true; }
        const qsizetype digits = matched->id == "reference" ? 3 : 1;
        if (message.size() != matched->command.size() + digits) { return false; }
        const std::optional<qint64> raw = scopeDigits(message.sliced(matched->command.size()));
        if (!raw || *raw < matched->minimum || *raw > matched->maximum) { return false; }
        id = matched->id; value = static_cast<int>(*raw);
    }
    if ((geometryControl(id) || pair.has_value()) && m_nativeScopeOwnedGeometryFloor > 0
        && ingressBatch <= m_nativeScopeOwnedGeometryFloor) { return false; }
    if (id == "bounds" && ingressBatch <= m_nativeScopeBoundsFloor
        && (m_nativeScopeInterested || m_nativeScopeGeometryTicket != 0)) { return false; }
    if (ingressBatch <= m_nativeScopeMinimumBatch || ingressBatch <= m_nativeScopeFloors.value(id, 0)) { return true; }
    const bool same = pair ? (slot == 0 ? m_nativeScopeCurrentBounds == pair : m_nativeScopeRanges.contains(slot) && m_nativeScopeRanges.value(slot) == *pair)
        : m_nativeScopeValues.contains(id) && m_nativeScopeValues.value(id) == value;
    const bool predispatch = m_nativeScopePending.contains(id)
        && m_nativeScopePredispatch.value(id, 0) == m_nativeScopePending.value(id);
    if (predispatch && same) { return true; }
    if (m_nativeScopeProjectionPending && !m_nativeScopePredispatch.isEmpty()
        && (geometryControl(id) || id == "bounds") && !predispatch) { return true; }
    if (!same && (id == "mode" || id == "fixed_range")) {
        m_nativeScopeCurrentBounds.reset();
        m_nativeScopeBoundsFloor = ingressBatch;
        m_nativeScopeBoundsRefreshPending = m_nativeScopeInterested;
    }
    if (pair) {
        if (slot == 0) { m_nativeScopeCurrentBounds = pair; m_nativeScopeBoundsRefreshPending = false; }
        else { m_nativeScopeRanges[slot] = *pair; m_nativeScopeUnverifiedRanges.remove(id); }
    } else { m_nativeScopeValues[id] = value; }
    if (!same) { ++m_nativeScopeRowRevisions[id]; }
    if (!same && (id == "filter_output" || id == "display_type")) { m_nativeScopeFrameFloor = ingressBatch; }
    if (!same && m_nativeScopeInterested && !m_nativeScopeProjectionPending && geometryControl(id)) { m_nativeScopeGeometryFloor = ingressBatch; }
    m_nativeScopePending.remove(id); m_nativeScopePredispatch.remove(id);
    if (geometryControl(id)) {
        m_nativeScopeGeometryConfirmed.insert(id);
        if (ingressBatch > m_nativeScopeGeometryFloor) {
            m_nativeScopeGeometryNeeded.remove(id);
            if (id == "mode" && value == 0) { m_nativeScopeGeometryNeeded.remove("bounds"); m_nativeScopeGeometryNeeded.remove("fixed_range"); }
            if (id == "mode" && value != 0) { m_nativeScopeGeometryNeeded.insert("bounds"); m_nativeScopeGeometryNeeded.insert("fixed_range"); }
        }
    }
    if (id == "bounds" && ingressBatch > m_nativeScopeGeometryFloor) { m_nativeScopeGeometryNeeded.remove("bounds"); }
    const quint64 observation = ++m_nativeScopeObservation, revision = m_nativeScopeRevision;
    publishScopeControls();
    if (!guard || generation != m_generation || revision != m_nativeScopeRevision || observation != m_nativeScopeObservation) { return false; }
    if (!same && (id == "filter_output" || id == "display_type")) {
        const QString reason = scopeFilterFrameReason();
        publishScopeFilterUnavailable(reason.isEmpty() ? QStringLiteral("Waiting for a fresh native AF spectrum after display/transport change.") : reason);
    }
    return guard && generation == m_generation && revision == m_nativeScopeRevision && observation == m_nativeScopeObservation;
}

void KenwoodBackend::adoptScopeFilterFrame(const QByteArray& message, quint64 ingressBatch)
{
    if (message.size() != 431 || !message.startsWith("##DD3")) { return; }
    QVariantList values; values.reserve(213);
    const auto hex = [](char digit) {
        if (digit >= '0' && digit <= '9') { return digit - '0'; }
        if (digit >= 'a' && digit <= 'f') { return digit - 'a' + 10; }
        if (digit >= 'A' && digit <= 'F') { return digit - 'A' + 10; }
        return -1;
    };
    for (int index = 5; index < 431; index += 2) {
        const int high = hex(message[index]), low = hex(message[index + 1]);
        if (high < 0 || low < 0 || high * 16 + low > 50) { return; }
        values.append(high * 16 + low);
    }
    const QPointer<KenwoodBackend> guard(this);
    const quint64 generation = m_generation;
    refreshScopeControlsContext();
    if (!guard || generation != m_generation || ingressBatch <= m_nativeScopeMinimumBatch || ingressBatch <= m_nativeScopeFrameFloor) { return; }
    const QString reason = scopeFilterFrameReason();
    if (!reason.isEmpty()) { if (m_nativeScopeFrameKnown) { publishScopeFilterUnavailable(reason); } return; }
    m_nativeScopeFrameKnown = true; m_nativeScopeLastFrameMs = m_clock.elapsed();
    emit extensionStatus(QStringLiteral("kenwood"), QStringLiteral("scopeFilterFrame"),
        {{"context", QStringLiteral("%1:%2").arg(m_generation).arg(m_nativeScopeRevision)}, {"sequence", QString::number(++m_nativeScopeFrameSequence)},
         {"values", values}, {"unit", QStringLiteral("nativeInverseDisplayIndex")}, {"known", true}, {"reason", QString{}}});
}

bool KenwoodBackend::handleScopeControlsExtension(const QString& verb, quint64 requestId, const QVariant& argument)
{
    const QStringList verbs{QStringLiteral("scope.controls.get"), QStringLiteral("scope.controls.set"), QStringLiteral("scope.controls.reset"),
        QStringLiteral("scope.controls.action"), QStringLiteral("scope.ranges.set"), QStringLiteral("scope.ranges.reset")};
    if (!verbs.contains(verb)) { return false; }
    const QPointer<KenwoodBackend> guard(this);
    const quint64 generation = m_generation;
    const bool wasInterested = m_nativeScopeInterested;
    const bool changed = refreshScopeControlsContext();
    if (!guard || generation != m_generation) { return true; }
    if (verb == "scope.controls.get") {
        if (argument.isValid() && (argument.typeId() != QMetaType::QVariantMap || !argument.toMap().isEmpty())) {
            emit extensionError(requestId, QStringLiteral("Scope query accepts no argument.")); return true;
        }
        m_nativeScopeInterested = true;
        if (!changed || !wasInterested) { queryScopeControls(); }
        if (guard && generation == m_generation) { emit extensionResult(requestId, scopeControlsStatus()); }
        return true;
    }
    const QVariantMap fields = argument.toMap();
    const QVariantMap status = scopeControlsStatus();
    const bool range = verb.startsWith("scope.ranges.");
    const bool reset = verb.endsWith(".reset");
    const bool action = verb == "scope.controls.action";
    const int slot = fields.value("slot").toInt();
    const QString id = range ? rangeKey(slot) : fields.value("id").toString();
    const ScopeControl* control = scopeControl(id);
    const int expected = action ? 2 : range ? (reset ? 3 : 5) : reset ? 3 : 4;
    bool valid = argument.typeId() == QMetaType::QVariantMap && fields.size() == expected
        && fields.value("context").typeId() == QMetaType::QString && fields.value("context") == status.value("context")
        && scopeControlsWritable();
    QByteArray command, query;
    bool geometry = false;
    if (range) {
        valid = valid && strictScopeInt(fields.value("slot"), 1, 3) && fields.value("revision").typeId() == QMetaType::QString
            && fields.value("revision").toString() == QString::number(m_nativeScopeRowRevisions.value(id));
        const QVariantMap row = slot >= 1 && slot <= 3 ? status.value("ranges").toList()[slot - 1].toMap() : QVariantMap{};
        valid = valid && row.value(reset ? "resettable" : "editable").toBool();
        if (!reset) { valid = valid && strictScopeHz(fields.value("lowHz")) && strictScopeHz(fields.value("highHz"))
            && fields.value("lowHz").toLongLong() < fields.value("highHz").toLongLong(); }
        if (valid) {
            command = "BSM" + QByteArray::number(slot);
            command += reset ? QByteArray(16, '9') : QByteArray::number(fields.value("lowHz").toLongLong()).rightJustified(8, '0')
                + QByteArray::number(fields.value("highHz").toLongLong()).rightJustified(8, '0');
        }
        query = "BSM";
        geometry = slot == m_nativeScopeValues.value("fixed_range", -1);
    } else if (action) {
        valid = valid && fields.value("id").typeId() == QMetaType::QString
            && (id == "clear" || id == "marker_center") && status.value("actions").toMap().value(id).toMap().value("editable").toBool();
        command = id == "clear" ? "BSD" : "BSE"; geometry = id == "marker_center";
    } else {
        valid = valid && fields.value("id").typeId() == QMetaType::QString && control && id != "overflow"
            && fields.value("revision").typeId() == QMetaType::QString
            && fields.value("revision").toString() == QString::number(m_nativeScopeRowRevisions.value(id));
        QVariantMap row;
        for (const QVariant& item : status.value("controls").toList()) { if (item.toMap().value("id") == id) { row = item.toMap(); break; } }
        valid = valid && row.value("editable").toBool();
        if (reset) { valid = valid && id == "span"; }
        else {
            valid = valid && control && strictScopeInt(fields.value("value"), control ? control->minimum : 0, control ? control->maximum : 0);
            bool allowed = false;
            for (const QVariant& item : row.value("choices").toList()) { if (item.toMap().value("value") == fields.value("value") && item.toMap().value("editable").toBool()) { allowed = true; } }
            valid = valid && allowed;
        }
        if (valid) { query = control->command; command = query + QByteArray::number(reset ? 9 : fields.value("value").toInt()).rightJustified(id == "reference" ? 3 : 1, '0'); }
        geometry = geometryControl(id);
    }
    if (!valid) { emit extensionError(requestId, QStringLiteral("Scope request requires exact typed fields and current editable readback ownership.")); return true; }
    const QString context = scopeControlsContextKey(), frequencyContext = m_nativeScopeFrequencyContext;
    const quint64 revision = m_nativeScopeRevision, rowRevision = m_nativeScopeRowRevisions.value(id), ticket = ++m_nativeScopeTicket;
    m_nativeScopePending[id] = ticket; m_nativeScopePredispatch[id] = ticket; m_nativeScopeFloors[id] = m_controlIngressSequence;
    if (geometry) {
        m_nativeScopeProjectionPending = true;
        m_nativeScopeGeometryTicket = ticket;
        m_nativeScopeOwnedGeometryFloor = m_nativeScopeGeometryFloor = m_controlIngressSequence;
        m_nativeScopeGeometryNeeded = {QStringLiteral("mode"), QStringLiteral("span"), QStringLiteral("shift"), QStringLiteral("expand")};
        if (m_nativeScopeValues.value("mode", -1) != 0) { m_nativeScopeGeometryNeeded.insert("bounds"); m_nativeScopeGeometryNeeded.insert("fixed_range"); }
        m_nativeScopeCurrentBounds.reset();
    }
    const auto owns = [guard, generation, revision, rowRevision, context, frequencyContext, id, ticket, geometry, range] {
        return guard && generation == guard->m_generation && revision == guard->m_nativeScopeRevision
            && context == guard->scopeControlsContextKey() && guard->scopeControlsWritable()
            && ((!geometry && !range) || frequencyContext == guard->m_nativeScopeFrequencyContext)
            && rowRevision == guard->m_nativeScopeRowRevisions.value(id) && guard->m_nativeScopePending.value(id, 0) == ticket;
    };
    const auto abandon = [guard, id, ticket](bool invalidate) {
        if (guard && guard->m_nativeScopePending.value(id, 0) == ticket) {
            guard->m_nativeScopePending.remove(id); guard->m_nativeScopePredispatch.remove(id);
            if (invalidate) {
                guard->m_nativeScopeValues.remove(id);
                if (id.startsWith("range")) { guard->m_nativeScopeRanges.remove(id.sliced(5).toInt()); }
                ++guard->m_nativeScopeRowRevisions[id];
            }
            guard->publishScopeControls();
        }
    };
    publishScopeControls();
    if (!owns()) { abandon(false); if (guard && generation == m_generation) { emit extensionError(requestId, QStringLiteral("Scope ownership changed before dispatch.")); } return true; }
    m_nativeScopePredispatch.remove(id);
    m_nativeScopeFloors[id] = m_controlIngressSequence;
    if (geometry) { m_nativeScopeOwnedGeometryFloor = m_nativeScopeGeometryFloor = m_controlIngressSequence; }
    const bool sent = send(command);
    if (!guard || generation != m_generation) { return true; }
    if (!sent) { abandon(true); if (guard && generation == m_generation) { emit extensionError(requestId, QStringLiteral("Scope command was not dispatched; read settings again.")); } return true; }
    if (m_nativeScopePending.value(id, 0) == ticket && !owns()) {
        abandon(false); if (guard && generation == m_generation) { emit extensionError(requestId, QStringLiteral("Scope ownership changed during dispatch; outcome unconfirmed.")); } return true;
    }
    if (action) {
        if (m_nativeScopePending.value(id, 0) == ticket) { m_nativeScopePending.remove(id); }
        publishScopeControls();
        if (!guard || generation != m_generation || revision != m_nativeScopeRevision) { return true; }
        if (geometry) { queryScopeControls(true); }
        if (!guard || generation != m_generation || revision != m_nativeScopeRevision) { return true; }
        emit extensionResult(requestId, QVariantMap{{"sent", true}, {"verified", false}}); return true;
    }
    bool queried = true;
    if (m_nativeScopePending.value(id, 0) == ticket) { queried = send(query); }
    if (!guard || generation != m_generation) { return true; }
    if (revision != m_nativeScopeRevision || context != scopeControlsContextKey()
        || ((geometry || range) && frequencyContext != m_nativeScopeFrequencyContext)) {
        abandon(false);
        if (guard && generation == m_generation) { emit extensionError(requestId, QStringLiteral("Scope context changed during readback; outcome unconfirmed.")); }
        return true;
    }
    if (!queried) { abandon(true); if (guard && generation == m_generation) { emit extensionError(requestId, QStringLiteral("Scope readback request failed; outcome unconfirmed.")); } return true; }
    if (geometry) { queryScopeControls(true); }
    if (!guard || generation != m_generation || revision != m_nativeScopeRevision || context != scopeControlsContextKey()) { return true; }
    if (range && m_nativeScopePending.value(id, 0) == ticket) {
        m_nativeScopePending.remove(id);
        m_nativeScopeRanges.remove(slot);
        m_nativeScopeUnverifiedRanges.insert(id);
        publishScopeControls();
        if (!guard || generation != m_generation || revision != m_nativeScopeRevision || context != scopeControlsContextKey()) { return true; }
    }
    emit extensionResult(requestId, range ? QVariant(QVariantMap{{"sent", true}, {"verified", false}}) : QVariant(true));
    return true;
}
} // namespace AetherSDR::kenwood
