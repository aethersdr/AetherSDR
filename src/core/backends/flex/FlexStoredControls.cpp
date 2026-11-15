#include "core/backends/flex/FlexBackend.h"

#include <QStringList>
#include <QPointer>
#include <cmath>
#include <optional>

namespace AetherSDR {

ReceiveDispatch FlexBackend::requestProfile(const ProfileRequest& request,
                                           const RadioControlCompletion& completion,
                                           const RadioControlFence& current)
{
    using Action = ProfileRequest::Action;
    using Kind = ProfileRequest::Kind;
    if (!request.valid() || !m_setupSink || (current && !current())) {
        return ReceiveDispatch::Unsupported;
    }
    if (request.kind == Kind::Cw && !capabilities().cwProfileControl.has_value()) {
        return ReceiveDispatch::Unsupported;
    }
    const QString kind = request.kind == Kind::Global ? QStringLiteral("global")
        : request.kind == Kind::Mic ? QStringLiteral("mic")
        : request.kind == Kind::Cw ? QStringLiteral("cw") : QStringLiteral("tx");
    QString command;
    switch (request.action) {
    case Action::Refresh: command = QStringLiteral("profile %1 info").arg(kind); break;
    case Action::Load: command = QStringLiteral("profile %1 load \"%2\"").arg(kind, request.name); break;
    case Action::Save: command = QStringLiteral("profile global save \"%1\"").arg(request.name); break;
    case Action::Create: case Action::Remove: case Action::Reset: {
        // FlexLib 4.2.18 uses transmit for mutations, tx for load/info.
        const QString mutationKind = request.kind == Kind::Transmit ? QStringLiteral("transmit") : kind;
        const QString name = request.kind == Kind::Global ? request.name
            : QString(request.name).remove(QLatin1Char('*'));
        if (name.trimmed().isEmpty()) { return ReceiveDispatch::Unsupported; }
        const QString verb = request.action == Action::Create ? QStringLiteral("create")
            : request.action == Action::Reset ? QStringLiteral("reset") : QStringLiteral("delete");
        command = QStringLiteral("profile %1 %2 \"%3\"").arg(mutationKind, verb, name);
        break;
    }
    case Action::AutoSave: command = QStringLiteral("profile autosave %1")
        .arg(request.enabled ? QStringLiteral("on") : QStringLiteral("off")); break;
    }
    const auto sink = m_setupSink;
    return sink(command, completion) ? ReceiveDispatch::Dispatched : ReceiveDispatch::Unsupported;
}

ReceiveDispatch FlexBackend::requestMemory(const MemoryRequest& request,
                                          const RadioControlCompletion& completion,
                                          const RadioControlFence& current)
{
    using Action = MemoryRequest::Action;
    if (!request.valid() || !m_setupSink || (current && !current())) {
        return ReceiveDispatch::Unsupported;
    }
    QString command;
    switch (request.action) {
    case Action::Create: command = QStringLiteral("memory create"); break;
    case Action::Remove: command = QStringLiteral("memory remove %1").arg(request.index); break;
    case Action::Recall: command = QStringLiteral("memory apply %1").arg(request.index); break;
    case Action::Update: {
        QStringList fields;
        const MemoryPatch& patch = request.patch;
        const auto text = [&fields](const QString& key, const std::optional<QString>& value) {
            if (value) { fields.append(key + QLatin1Char('=') + QString(*value).replace(' ', QChar(0x7f))); }
        };
        const auto integer = [&fields](const QString& key, const std::optional<int>& value) {
            if (value) { fields.append(key + QLatin1Char('=') + QString::number(*value)); }
        };
        const auto number = [&fields](const QString& key, const std::optional<double>& value, int precision) {
            if (value) { fields.append(key + QLatin1Char('=') + QString::number(*value, 'f', precision)); }
        };
        text(QStringLiteral("group"), patch.group); text(QStringLiteral("owner"), patch.owner);
        number(QStringLiteral("freq"), patch.freq, 6); text(QStringLiteral("name"), patch.name);
        if (patch.mode) { text(QStringLiteral("mode"), MemoryFields::modeToWire(*patch.mode)); }
        integer(QStringLiteral("step"), patch.step);
        if (patch.offsetDir) { text(QStringLiteral("repeater"), MemoryFields::offsetDirToWire(*patch.offsetDir)); }
        number(QStringLiteral("repeater_offset"), patch.repeaterOffset, 6);
        if (patch.toneMode) { text(QStringLiteral("tone_mode"), MemoryFields::toneModeToWire(*patch.toneMode)); }
        number(QStringLiteral("tone_value"), patch.toneValue, 1);
        if (patch.squelch) { fields.append(QStringLiteral("squelch=%1").arg(int(*patch.squelch))); }
        integer(QStringLiteral("squelch_level"), patch.squelchLevel);
        integer(QStringLiteral("rx_filter_low"), patch.rxFilterLow); integer(QStringLiteral("rx_filter_high"), patch.rxFilterHigh);
        integer(QStringLiteral("rtty_mark"), patch.rttyMark); integer(QStringLiteral("rtty_shift"), patch.rttyShift);
        integer(QStringLiteral("digl_offset"), patch.diglOffset); integer(QStringLiteral("digu_offset"), patch.diguOffset);
        command = QStringLiteral("memory set %1 %2").arg(request.index).arg(fields.join(QLatin1Char(' ')));
        break;
    }
    }
    const auto sink = m_setupSink;
    const QPointer<FlexBackend> alive(this);
    if (!sink(command, completion)) { return ReceiveDispatch::Unsupported; }
    if (request.action != Action::Recall || !request.recall) { return ReceiveDispatch::Dispatched; }
    const MemoryEntry& memory = *request.recall;
    QStringList following;
    if (memory.freq > 0.0) {
        following.append(QStringLiteral("slice tune %1 %2 autopan=0")
            .arg(request.sliceId).arg(memory.freq, 0, 'f', 6));
    }
    if (memory.step > 0) {
        following.append(QStringLiteral("slice set %1 step=%2").arg(request.sliceId).arg(memory.step));
    }
    QStringList fields;
    const QString direction = MemoryFields::offsetDirToWire(memory.offsetDir);
    if (!direction.isEmpty()) {
        const double offset = std::abs(memory.repeaterOffset);
        fields.append(QStringLiteral("repeater_offset_dir=%1").arg(direction));
        fields.append(QStringLiteral("fm_repeater_offset_freq=%1").arg(offset, 0, 'f', 6));
        fields.append(QStringLiteral("tx_offset_freq=%1")
            .arg(direction == QLatin1String("up") ? offset : direction == QLatin1String("down") ? -offset : 0.0, 0, 'f', 6));
    }
    const QString toneMode = MemoryFields::toneModeToWire(memory.toneMode);
    if (!toneMode.isEmpty()) { fields.append(QStringLiteral("fm_tone_mode=%1").arg(toneMode)); }
    if (!toneMode.isEmpty() || memory.toneValue > 0.0) {
        fields.append(QStringLiteral("fm_tone_value=%1").arg(memory.toneValue, 0, 'f', 1));
    }
    if (!fields.isEmpty()) {
        following.append(QStringLiteral("slice set %1 %2").arg(request.sliceId).arg(fields.join(QLatin1Char(' '))));
    }
    for (const QString& next : following) {
        if (!alive || (current && !current())) { break; }
        if (!sink(next, {})) { break; }
    }
    return ReceiveDispatch::Dispatched;
}

} // namespace AetherSDR
