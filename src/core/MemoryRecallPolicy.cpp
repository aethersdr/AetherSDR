#include "core/MemoryRecallPolicy.h"

#include <QStringList>

#include <cmath>

namespace AetherSDR {

namespace {

QString normalizedRepeaterDirection(const QString& direction)
{
    const QString normalized = direction.trimmed().toLower();
    if (normalized == "up" || normalized == "down" || normalized == "simplex")
        return normalized;
    return {};
}

} // namespace

double memoryRepeaterTxOffsetFreq(const MemoryEntry& memory)
{
    const QString direction = normalizedRepeaterDirection(memory.offsetDir);
    const double offsetMhz = std::abs(memory.repeaterOffset);
    if (offsetMhz == 0.0)
        return 0.0;
    if (direction == "up")
        return offsetMhz;
    if (direction == "down")
        return -offsetMhz;
    return 0.0;
}

QString buildMemoryRecallRetuneCommand(int sliceId, const MemoryEntry& memory)
{
    if (sliceId < 0 || memory.freq <= 0.0)
        return {};
    return QString("slice tune %1 %2 autopan=0").arg(sliceId).arg(memory.freq, 0, 'f', 6);
}

QString buildMemoryRecallSliceFixupCommand(int sliceId, const MemoryEntry& memory)
{
    QStringList fields;

    const QString direction = normalizedRepeaterDirection(memory.offsetDir);
    if (!direction.isEmpty()) {
        const double offsetMhz = std::abs(memory.repeaterOffset);
        fields << QString("repeater_offset_dir=%1").arg(direction);
        fields << QString("fm_repeater_offset_freq=%1").arg(offsetMhz, 0, 'f', 6);
        fields << QString("tx_offset_freq=%1")
            .arg(memoryRepeaterTxOffsetFreq(memory), 0, 'f', 6);
    }

    const QString toneMode = memory.toneMode.trimmed().toLower();
    if (!toneMode.isEmpty())
        fields << QString("fm_tone_mode=%1").arg(toneMode);
    if (!toneMode.isEmpty() || memory.toneValue > 0.0)
        fields << QString("fm_tone_value=%1").arg(memory.toneValue, 0, 'f', 1);

    if (fields.isEmpty())
        return {};
    return QString("slice set %1 %2").arg(sliceId).arg(fields.join(' '));
}

} // namespace AetherSDR
